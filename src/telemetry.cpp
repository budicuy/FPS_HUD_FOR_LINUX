#include "telemetry.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace fs = std::filesystem;

namespace fpshud {

namespace {

TelemetryCollector* g_active_telemetry = nullptr;

bool read_file_trimmed(const std::string& path, std::string& out) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;
    std::string line;
    if (std::getline(ifs, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(" \t");
        out = (start != std::string::npos) ? line.substr(start) : "";
        return true;
    }
    return false;
}

int read_int_from_file(const std::string& path, int fallback = 0) {
    std::string s;
    if (read_file_trimmed(path, s) && !s.empty()) {
        char* end = nullptr;
        long v = std::strtol(s.c_str(), &end, 10);
        if (end != s.c_str()) return static_cast<int>(v);
    }
    return fallback;
}

uint64_t read_u64_from_file(const std::string& path, uint64_t fallback = 0) {
    std::string s;
    if (read_file_trimmed(path, s) && !s.empty()) {
        char* end = nullptr;
        unsigned long long v = std::strtoull(s.c_str(), &end, 10);
        if (end != s.c_str()) return static_cast<uint64_t>(v);
    }
    return fallback;
}

// Cleans up verbose brand tags (e.g. "(R)", "(TM)", "12th Gen", "with Radeon Graphics") for clean HUD rendering
void clean_hardware_name(const std::string& in, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (in.empty()) return;

    std::string s = in;
    const char* filters[] = {
        "(R)", "(r)", "(TM)", "(tm)", "Processor", "processor",
        "CPU @", "with Radeon Graphics", "Graphics", "12th Gen ", "13th Gen ", "14th Gen "
    };
    for (const char* f : filters) {
        size_t pos = 0;
        size_t flen = std::strlen(f);
        while ((pos = s.find(f, pos)) != std::string::npos) {
            s.erase(pos, flen);
        }
    }

    // Collapse multi-spaces & strip leading/trailing whitespace
    std::string clean;
    clean.reserve(s.size());
    bool prev_space = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prev_space && !clean.empty()) {
                clean.push_back(' ');
                prev_space = true;
            }
        } else {
            clean.push_back(c);
            prev_space = false;
        }
    }
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();

    std::snprintf(out, out_sz, "%s", clean.c_str());
}

std::string get_benchmark_csv_path() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        std::error_code ec;
        fs::path docs_en = fs::path(home) / "Documents";
        if (fs::exists(docs_en, ec)) {
            return (docs_en / "fpshud_benchmark.csv").string();
        }
        fs::path docs_id = fs::path(home) / "Dokumen";
        if (fs::exists(docs_id, ec)) {
            return (docs_id / "fpshud_benchmark.csv").string();
        }
        return (fs::path(home) / "fpshud_benchmark.csv").string();
    }
    return "/tmp/fpshud_benchmark.csv";
}

} // namespace

TelemetryCollector::TelemetryCollector()
    : TelemetryCollector("", false) {}

TelemetryCollector::TelemetryCollector(std::string mock_root_prefix, bool disable_nvml)
    : root_prefix_(std::move(mock_root_prefix)), disable_nvml_(disable_nvml) {
    graph_ft_ms_.fill(16.67f);
    graph_fps_.fill(60.0f);
    ft_ring_.fill(16.67f);

    init_sensors();
    g_active_telemetry = this;

    worker_running_.store(true, std::memory_order_relaxed);
    worker_thread_ = std::thread(&TelemetryCollector::worker_loop, this);
}

TelemetryCollector::~TelemetryCollector() {
    worker_running_.store(false, std::memory_order_relaxed);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    shutdown_sensors();
    if (g_active_telemetry == this) {
        g_active_telemetry = nullptr;
    }
}

void TelemetryCollector::init_sensors() {
    init_cpu_sensors();
    init_gpu_sensors();
    init_ram_info();
}

void TelemetryCollector::init_cpu_sensors() {
    std::string cpuinfo_path = root_prefix_ + "/proc/cpuinfo";
    std::ifstream ifs(cpuinfo_path);
    if (ifs.is_open()) {
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.rfind("model name", 0) == 0) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    clean_hardware_name(line.substr(colon + 1), cpu_name_, sizeof(cpu_name_));
                    break;
                }
            }
        }
    }
    if (cpu_name_[0] == '\0') {
        std::snprintf(cpu_name_, sizeof(cpu_name_), "Generic CPU");
    }

    // Identify RAPL package energy sysfs paths for CPU power telemetry
    std::error_code ec;
    std::string rapl_base = root_prefix_ + "/sys/class/powercap/intel-rapl";
    if (fs::exists(rapl_base, ec)) {
        for (const auto& entry : fs::directory_iterator(rapl_base, ec)) {
            std::string name_path = entry.path().string() + "/name";
            std::string name;
            if (read_file_trimmed(name_path, name)) {
                if (name == "package-0" || name == "core") {
                    cpu_energy_path_ = entry.path().string() + "/energy_uj";
                    break;
                }
            }
        }
    }
    if (cpu_energy_path_.empty()) {
        std::string fallback = root_prefix_ + "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj";
        if (fs::exists(fallback, ec)) cpu_energy_path_ = fallback;
    }

    // Locate CPU Package / Core Temperature Hwmon
    std::string hwmon_base = root_prefix_ + "/sys/class/hwmon";
    if (fs::exists(hwmon_base, ec)) {
        for (const auto& entry : fs::directory_iterator(hwmon_base, ec)) {
            std::string name_path = entry.path().string() + "/name";
            std::string name;
            if (read_file_trimmed(name_path, name)) {
                if (name == "coretemp" || name == "k10temp" || name == "zenpower" || name == "cpu_thermal") {
                    // Search for package/die temp first, then input 1
                    std::string t1 = entry.path().string() + "/temp1_input";
                    std::string t2 = entry.path().string() + "/temp2_input";
                    if (fs::exists(t1, ec)) cpu_temp_path_ = t1;
                    if (name == "k10temp" && fs::exists(t2, ec)) cpu_temp_path_ = t2; // Tctl/Tdie
                    if (!cpu_temp_path_.empty()) break;
                }
            }
        }
    }
}

void TelemetryCollector::init_gpu_sensors() {
    // 1. Try NVIDIA NVML first (unless disabled)
    if (!disable_nvml_) {
        const char* nvml_paths[] = {
            "libnvidia-ml.so.1",
            "libnvidia-ml.so",
            "/usr/lib/libnvidia-ml.so.1",
            "/usr/lib64/libnvidia-ml.so.1",
            "/run/host/usr/lib/libnvidia-ml.so.1"
        };
        for (const char* path : nvml_paths) {
            nvml_lib_ = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
            if (nvml_lib_) break;
        }

        if (nvml_lib_) {
            pfn_nvmlInit_v2_ = reinterpret_cast<int(*)()>(dlsym(nvml_lib_, "nvmlInit_v2"));
            pfn_nvmlShutdown_ = reinterpret_cast<int(*)()>(dlsym(nvml_lib_, "nvmlShutdown"));
            pfn_nvmlSystemGetDriverVersion_ = reinterpret_cast<int(*)(char*, unsigned int)>(dlsym(nvml_lib_, "nvmlSystemGetDriverVersion"));
            pfn_nvmlDeviceGetHandleByIndex_v2_ = reinterpret_cast<int(*)(unsigned int, void**)>(dlsym(nvml_lib_, "nvmlDeviceGetHandleByIndex_v2"));
            pfn_nvmlDeviceGetName_ = reinterpret_cast<int(*)(void*, char*, unsigned int)>(dlsym(nvml_lib_, "nvmlDeviceGetName"));
            pfn_nvmlDeviceGetUtilizationRates_ = reinterpret_cast<int(*)(void*, void*)>(dlsym(nvml_lib_, "nvmlDeviceGetUtilizationRates"));
            pfn_nvmlDeviceGetMemoryInfo_ = reinterpret_cast<int(*)(void*, void*)>(dlsym(nvml_lib_, "nvmlDeviceGetMemoryInfo"));
            pfn_nvmlDeviceGetTemperature_ = reinterpret_cast<int(*)(void*, int, unsigned int*)>(dlsym(nvml_lib_, "nvmlDeviceGetTemperature"));
            pfn_nvmlDeviceGetFanSpeed_ = reinterpret_cast<int(*)(void*, unsigned int*)>(dlsym(nvml_lib_, "nvmlDeviceGetFanSpeed"));
            pfn_nvmlDeviceGetPowerUsage_ = reinterpret_cast<int(*)(void*, unsigned int*)>(dlsym(nvml_lib_, "nvmlDeviceGetPowerUsage"));
            pfn_nvmlDeviceGetClockInfo_ = reinterpret_cast<int(*)(void*, int, unsigned int*)>(dlsym(nvml_lib_, "nvmlDeviceGetClockInfo"));

            if (pfn_nvmlInit_v2_ && pfn_nvmlInit_v2_() == 0) {
                nvml_initialized_ = true;
                gpu_backend_ = GpuBackendType::NvidiaNvml;

                if (pfn_nvmlDeviceGetHandleByIndex_v2_(0, &nvml_device_) == 0 && nvml_device_) {
                    char raw_name[96] = {0};
                    if (pfn_nvmlDeviceGetName_(nvml_device_, raw_name, sizeof(raw_name)) == 0) {
                        clean_hardware_name(raw_name, gpu_name_, sizeof(gpu_name_));
                    }
                    std::snprintf(gpu_vendor_, sizeof(gpu_vendor_), "NVIDIA");

                    char driver_buf[64] = {0};
                    if (pfn_nvmlSystemGetDriverVersion_ && pfn_nvmlSystemGetDriverVersion_(driver_buf, sizeof(driver_buf)) == 0) {
                        std::snprintf(gpu_driver_version_, sizeof(gpu_driver_version_), "%s", driver_buf);
                    }
                    return;
                }
            }
        }
    }

    // 2. AMD / Intel DRM Sysfs Discovery (Fallback or Primary on AMD/Intel/Steam Deck/APUs)
    std::error_code ec;
    std::string drm_base = root_prefix_ + "/sys/class/drm";
    if (fs::exists(drm_base, ec)) {
        // Iterate card1 first (frequent dGPU on hybrid laptops), then card0
        std::vector<std::string> cards = {"card1", "card0"};
        for (const auto& c : cards) {
            std::string device_dir = drm_base + "/" + c + "/device";
            if (!fs::exists(device_dir, ec)) continue;

            std::string vendor_path = device_dir + "/vendor";
            std::string vendor_id_str;
            if (read_file_trimmed(vendor_path, vendor_id_str)) {
                uint32_t vid = static_cast<uint32_t>(std::strtoul(vendor_id_str.c_str(), nullptr, 16));
                if (vid == 0x1002) { // AMD
                    gpu_backend_ = GpuBackendType::AmdSysfs;
                    std::snprintf(gpu_vendor_, sizeof(gpu_vendor_), "AMD");
                    std::snprintf(gpu_driver_version_, sizeof(gpu_driver_version_), "amdgpu");

                    // Read human-readable GPU marketing name if exported by kernel
                    std::string prod_name;
                    if (read_file_trimmed(device_dir + "/product_name", prod_name) && !prod_name.empty()) {
                        clean_hardware_name(prod_name, gpu_name_, sizeof(gpu_name_));
                    } else {
                        std::snprintf(gpu_name_, sizeof(gpu_name_), "Radeon GPU (AMD)");
                    }

                    drm_card_device_dir_ = device_dir;
                    return;
                } else if (vid == 0x8086) { // Intel
                    gpu_backend_ = GpuBackendType::IntelSysfs;
                    std::snprintf(gpu_vendor_, sizeof(gpu_vendor_), "Intel");
                    std::snprintf(gpu_driver_version_, sizeof(gpu_driver_version_), "i915/xe");
                    std::snprintf(gpu_name_, sizeof(gpu_name_), "Intel Arc/HD Graphics");
                    drm_card_device_dir_ = device_dir;
                    return;
                }
            }
        }
    }

    if (gpu_name_[0] == '\0') {
        std::snprintf(gpu_name_, sizeof(gpu_name_), "Generic GPU");
    }
}

void TelemetryCollector::init_ram_info() {
    // 1. Try pre-generated cached info from installer (~/.local/share/fpshud/ram_info.txt)
    if (const char* home = std::getenv("HOME"); home && *home) {
        std::string cached = std::string(home) + "/.local/share/fpshud/ram_info.txt";
        if (load_ram_info_file(cached)) return;
    }

    // 2. Direct read of Linux DMI memory device entries
    std::string dmi_dir = root_prefix_ + "/sys/firmware/dmi/entries";
    if (load_ram_info_dmi(dmi_dir)) return;

    // 3. Fallback: try reading system DMI tables (/sys/class/dmi/id/)
    std::string dmi_id = root_prefix_ + "/sys/class/dmi/id";
    std::string prod;
    if (read_file_trimmed(dmi_id + "/product_name", prod) && !prod.empty()) {
        // Known devices with fixed RAM specs
        if (prod.find("Jupiter") != std::string::npos || prod.find("Galileo") != std::string::npos) {
            ram_speed_mts_ = 6400;
            ram_modules_ = 2;
            std::snprintf(ram_type_, sizeof(ram_type_), "LPDDR5");
            return;
        }
    }

    // If completely unreadable (no root / udev rules), mark as 0
    ram_speed_mts_ = 0;
    ram_modules_ = 0;
    std::snprintf(ram_type_, sizeof(ram_type_), "RAM");
}

bool TelemetryCollector::load_ram_info_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;

    std::string line;
    while (std::getline(ifs, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!key.empty() && std::isspace(key.back())) key.pop_back();
        while (!val.empty() && std::isspace(val.back())) val.pop_back();
        size_t s = val.find_first_not_of(" \t");
        if (s != std::string::npos) val = val.substr(s);

        if (key == "SPEED_MTS") {
            ram_speed_mts_ = std::atoi(val.c_str());
        } else if (key == "TYPE") {
            std::snprintf(ram_type_, sizeof(ram_type_), "%s", val.c_str());
        } else if (key == "MODULES") {
            ram_modules_ = std::atoi(val.c_str());
        }
    }
    return (ram_speed_mts_ > 0);
}

bool TelemetryCollector::load_ram_info_dmi(const std::string& entries_dir) {
    std::error_code ec;
    if (!fs::exists(entries_dir, ec)) return false;

    int max_speed = 0;
    int count = 0;
    char detected_type[16] = {0};

    // SMBIOS Type 17 is Memory Device: directory pattern "17-*"
    for (const auto& entry : fs::directory_iterator(entries_dir, ec)) {
        std::string fname = entry.path().filename().string();
        if (fname.rfind("17-", 0) != 0) continue;

        std::string raw_path = entry.path().string() + "/raw";
        std::ifstream ifs(raw_path, std::ios::binary);
        if (!ifs.is_open()) continue;

        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (raw.size() < 0x16) continue;

        uint8_t type_code = (raw.size() > 0x12) ? raw[0x12] : 0;
        uint16_t speed = 0;
        if (raw.size() >= 0x17) {
            speed = static_cast<uint16_t>(raw[0x15] | (raw[0x16] << 8));
        }
        // Extended configured speed at offset 0x54 (SMBIOS 3.3+)
        if (speed == 0xFFFF && raw.size() >= 0x58) {
            uint32_t ext_speed = static_cast<uint32_t>(raw[0x54] | (raw[0x55] << 8) | (raw[0x56] << 16) | (raw[0x57] << 24));
            if (ext_speed > 0) speed = static_cast<uint16_t>(ext_speed);
        }

        if (speed > 0 && speed < 20000) {
            count++;
            if (speed > max_speed) max_speed = speed;
            if (detected_type[0] == '\0') {
                switch (type_code) {
                    case 0x18: std::strcpy(detected_type, "DDR3"); break;
                    case 0x1A: std::strcpy(detected_type, "DDR4"); break;
                    case 0x1B: std::strcpy(detected_type, "LPDDR4"); break;
                    case 0x22: std::strcpy(detected_type, "DDR5"); break;
                    case 0x23: std::strcpy(detected_type, "LPDDR5"); break;
                    default:   std::strcpy(detected_type, "DDR"); break;
                }
            }
        }
    }

    if (max_speed > 0) {
        ram_speed_mts_ = max_speed;
        ram_modules_ = count;
        if (detected_type[0]) {
            std::snprintf(ram_type_, sizeof(ram_type_), "%s", detected_type);
        }
        return true;
    }
    return false;
}

void TelemetryCollector::shutdown_sensors() {
    if (nvml_initialized_ && pfn_nvmlShutdown_) {
        pfn_nvmlShutdown_();
        nvml_initialized_ = false;
    }
    if (nvml_lib_) {
        dlclose(nvml_lib_);
        nvml_lib_ = nullptr;
    }
}

void TelemetryCollector::on_frame() {
    auto now = std::chrono::steady_clock::now();
    if (last_frame_time_.time_since_epoch().count() == 0) {
        last_frame_time_ = now;
        last_fps_display_update_ = now;
        return;
    }

    float dt_ms = std::chrono::duration<float, std::milli>(now - last_frame_time_).count();
    last_frame_time_ = now;
    on_simulated_frame_dt(dt_ms, false);
}

void TelemetryCollector::on_simulated_frame_dt(float dt_ms, bool force_display_update) {
    if (dt_ms <= 0.001f || dt_ms > 5000.0f) {
        return;
    }

    // Scene / Loading stall reset: if a single frame took > 250ms (< 4 FPS), reset rolling buffer
    if (dt_ms > 250.0f) {
        ft_ring_pos_ = 0;
        ft_ring_count_ = 0;
        ema_frametime_ms_ = 0.0f;
        return;
    }

    if (ema_frametime_ms_ <= 0.0f) {
        ema_frametime_ms_ = dt_ms;
    } else {
        ema_frametime_ms_ = ema_frametime_ms_ * 0.90f + dt_ms * 0.10f;
    }

    ft_ring_[ft_ring_pos_] = dt_ms;
    ft_ring_pos_ = (ft_ring_pos_ + 1) % kFrameWindowSize;
    if (ft_ring_count_ < kFrameWindowSize) {
        ++ft_ring_count_;
    }

    // Update 120-frame graph ring buffer (render thread only, zero lock!)
    graph_ft_ms_[graph_offset_] = ema_frametime_ms_;
    graph_fps_[graph_offset_] = 1000.0f / ema_frametime_ms_;
    graph_offset_ = (graph_offset_ + 1) % static_cast<int>(kGraphHistorySize);

    // Record frame into Benchmark buffer only when benchmark is active
    if (bench_active_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> block(bench_mutex_);
        if (bench_frametimes_ms_.size() < 360000) {
            bench_frametimes_ms_.push_back(dt_ms);
        }
    }

    auto now = std::chrono::steady_clock::now();

    // Automated Benchmark Controller (via FPSHUD_BENCHMARK=seconds environment variable)
    static const int s_auto_bench_secs = []() {
        if (const char* env = std::getenv("FPSHUD_BENCHMARK")) {
            return std::max(1, std::atoi(env));
        }
        return 0;
    }();

    if (s_auto_bench_secs > 0) {
        static auto s_first_frame = now;
        static bool s_bench_started = false;
        static bool s_bench_finished = false;

        if (!s_bench_started && !s_bench_finished) {
            double warmup = std::chrono::duration<double>(now - s_first_frame).count();
            if (warmup >= 1.0) {
                start_benchmark();
                s_bench_started = true;
                std::fprintf(stderr, "[FPSHUD] Automated Benchmark started (duration: %d seconds)...\n", s_auto_bench_secs);
            }
        } else if (s_bench_started && !s_bench_finished) {
            double elapsed = std::chrono::duration<double>(now - bench_start_time_).count();
            if (elapsed >= static_cast<double>(s_auto_bench_secs)) {
                stop_benchmark();
                s_bench_finished = true;
                std::fprintf(stderr, "[FPSHUD] Automated Benchmark complete! Results saved to CSV.\n");
                if (const char* exit_env = std::getenv("FPSHUD_BENCHMARK_EXIT"); exit_env && std::atoi(exit_env) != 0) {
                    std::exit(0);
                }
            }
        }
    }
    auto since_update_ms = std::chrono::duration<float, std::milli>(now - last_fps_display_update_).count();
    if (since_update_ms >= 200.0f || displayed_fps_.load(std::memory_order_relaxed) == 0.0f || force_display_update) {
        last_fps_display_update_ = now;
        displayed_frametime_ms_.store(ema_frametime_ms_, std::memory_order_relaxed);
        displayed_fps_.store(1000.0f / ema_frametime_ms_, std::memory_order_relaxed);

        if (ft_ring_count_ > 0) {
            std::array<float, kFrameWindowSize> recent_ft{};
            size_t recent_count = 0;
            double total_window_ms = 0.0;

            for (size_t step = 0; step < ft_ring_count_; ++step) {
                size_t idx = (ft_ring_pos_ + kFrameWindowSize - 1 - step) % kFrameWindowSize;
                float v = ft_ring_[idx];
                recent_ft[recent_count++] = v;
                total_window_ms += static_cast<double>(v);
                if (total_window_ms >= static_cast<double>(kRollingWindowMaxMs) && recent_count >= 30) {
                    break;
                }
            }

            if (total_window_ms > 0.001) {
                float avg_fps = static_cast<float>((static_cast<double>(recent_count) * 1000.0) / total_window_ms);
                displayed_fps_avg_.store(avg_fps, std::memory_order_relaxed);
            }

            std::sort(recent_ft.begin(), recent_ft.begin() + recent_count, std::greater<float>());

            auto calc_time_weighted_low_fps = [&](double tail_ratio) -> float {
                double target_tail_ms = total_window_ms * tail_ratio;
                double accum_ms = 0.0;
                double weighted_ft_sum = 0.0;

                for (size_t i = 0; i < recent_count; ++i) {
                    double ft = static_cast<double>(recent_ft[i]);
                    if (accum_ms + ft >= target_tail_ms) {
                        double remaining = target_tail_ms - accum_ms;
                        weighted_ft_sum += remaining * ft;
                        accum_ms = target_tail_ms;
                        break;
                    }
                    weighted_ft_sum += ft * ft;
                    accum_ms += ft;
                }

                if (accum_ms <= 0.0001) return 1000.0f / recent_ft[0];
                double effective_tail_ft = weighted_ft_sum / accum_ms;
                return (effective_tail_ft > 0.001) ? static_cast<float>(1000.0 / effective_tail_ft) : 0.0f;
            };

            float low_1pct = calc_time_weighted_low_fps(0.01);
            float low_01pct = calc_time_weighted_low_fps(0.001);
            if (low_01pct > low_1pct) low_01pct = low_1pct;

            displayed_fps_1pct_low_.store(low_1pct, std::memory_order_relaxed);
            displayed_fps_01pct_low_.store(low_01pct, std::memory_order_relaxed);
        }
    }
}

void TelemetryCollector::start_benchmark() {
    std::lock_guard<std::mutex> block(bench_mutex_);
    bench_frametimes_ms_.clear();
    bench_frametimes_ms_.reserve(16384);
    bench_max_gpu_temp_c_ = 0;
    bench_max_gpu_power_w_ = 0.0f;
    bench_max_cpu_temp_c_ = 0;
    bench_max_cpu_power_w_ = 0.0f;
    bench_start_time_ = std::chrono::steady_clock::now();
    bench_active_.store(true, std::memory_order_relaxed);
}

void TelemetryCollector::stop_benchmark() {
    if (!bench_active_.exchange(false, std::memory_order_relaxed)) {
        return;
    }

    auto end_time = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> block(bench_mutex_);

    if (bench_frametimes_ms_.empty()) {
        return;
    }

    BenchmarkSummary sum{};
    sum.has_result = true;
    sum.duration_sec = std::chrono::duration<double>(end_time - bench_start_time_).count();
    sum.finished_time = end_time;
    sum.max_gpu_temp_c = bench_max_gpu_temp_c_;
    sum.max_gpu_power_w = bench_max_gpu_power_w_;
    sum.max_cpu_temp_c = bench_max_cpu_temp_c_;
    sum.max_cpu_power_w = bench_max_cpu_power_w_;

    size_t n = bench_frametimes_ms_.size();
    double total_ms = 0.0;
    float min_ft_ms = bench_frametimes_ms_[0];
    float max_ft_ms = bench_frametimes_ms_[0];

    for (float ft : bench_frametimes_ms_) {
        total_ms += static_cast<double>(ft);
        if (ft < min_ft_ms) min_ft_ms = ft;
        if (ft > max_ft_ms) max_ft_ms = ft;
    }

    float mean_ft_ms = static_cast<float>(total_ms / static_cast<double>(n));
    sum.avg_fps = (mean_ft_ms > 0.001f) ? (1000.0f / mean_ft_ms) : 0.0f;
    sum.max_fps = (min_ft_ms > 0.001f) ? (1000.0f / min_ft_ms) : 0.0f;
    sum.min_fps = (max_ft_ms > 0.001f) ? (1000.0f / max_ft_ms) : 0.0f;

    std::sort(bench_frametimes_ms_.begin(), bench_frametimes_ms_.end(), std::greater<float>());

    auto calc_bench_time_weighted_low = [&](double tail_ratio) -> float {
        double target_ms = total_ms * tail_ratio;
        double accum_ms = 0.0;
        double weighted_sum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double ft = static_cast<double>(bench_frametimes_ms_[i]);
            if (accum_ms + ft >= target_ms) {
                double rem = target_ms - accum_ms;
                weighted_sum += rem * ft;
                accum_ms = target_ms;
                break;
            }
            weighted_sum += ft * ft;
            accum_ms += ft;
        }
        if (accum_ms <= 0.0001) return 1000.0f / bench_frametimes_ms_[0];
        return static_cast<float>(1000.0 / (weighted_sum / accum_ms));
    };

    sum.fps_1pct_low = calc_bench_time_weighted_low(0.01);
    sum.fps_01pct_low = std::min(sum.fps_1pct_low, calc_bench_time_weighted_low(0.001));

    std::string csv_path = get_benchmark_csv_path();
    std::snprintf(sum.saved_csv_path, sizeof(sum.saved_csv_path), "%s", csv_path.c_str());

    std::error_code ec;
    bool file_exists = fs::exists(csv_path, ec) && fs::file_size(csv_path, ec) > 0;
    if (FILE* csv = std::fopen(csv_path.c_str(), "a")) {
        if (!file_exists) {
            std::fputs("Timestamp,Duration_Sec,Frames,Avg_FPS,1Pct_Low_FPS,0.1Pct_Low_FPS,Min_FPS,Max_FPS,Max_GPU_Temp_C,Max_GPU_Watt,Max_CPU_Temp_C,Max_CPU_Watt\n", csv);
        }
        std::time_t t = std::time(nullptr);
        std::tm tm_buf{};
        localtime_r(&t, &tm_buf);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

        std::fprintf(csv,
                     "%s,%.1f,%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%.1f,%d,%.1f\n",
                     ts, sum.duration_sec, n,
                     sum.avg_fps, sum.fps_1pct_low, sum.fps_01pct_low,
                     sum.min_fps, sum.max_fps,
                     sum.max_gpu_temp_c, sum.max_gpu_power_w,
                     sum.max_cpu_temp_c, sum.max_cpu_power_w);
        std::fflush(csv);
        std::fclose(csv);
    }

    last_bench_summary_ = sum;
}

void TelemetryCollector::toggle_benchmark() {
    if (is_benchmark_active()) {
        stop_benchmark();
    } else {
        start_benchmark();
    }
}

TelemetryData TelemetryCollector::get_snapshot() const {
    TelemetryData out{};
    out.fps = displayed_fps_.load(std::memory_order_relaxed);
    out.fps_avg = displayed_fps_avg_.load(std::memory_order_relaxed);
    out.fps_1pct_low = displayed_fps_1pct_low_.load(std::memory_order_relaxed);
    out.fps_01pct_low = displayed_fps_01pct_low_.load(std::memory_order_relaxed);
    out.frametime_ms = displayed_frametime_ms_.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(hw_mutex_);
        out.gpu_load_pct = gpu_load_pct_;
        out.gpu_clock_mhz = gpu_clock_mhz_;
        out.gpu_vram_used_gb = gpu_vram_used_gb_;
        out.gpu_vram_total_gb = gpu_vram_total_gb_;
        out.gpu_bandwidth_gbs = gpu_bandwidth_gbs_;
        out.gpu_max_bandwidth_gbs = gpu_max_bandwidth_gbs_;
        out.gpu_temp_c = gpu_temp_c_;
        out.gpu_fan_pct = gpu_fan_pct_;
        out.gpu_power_w = gpu_power_w_;
        out.gpu_available = gpu_available_;

        out.cpu_load_pct = cpu_load_pct_;
        out.cpu_clock_mhz = cpu_clock_mhz_;
        out.cpu_temp_c = cpu_temp_c_;
        out.cpu_power_w = cpu_power_w_;

        out.ram_used_gb = ram_used_gb_;
        out.ram_total_gb = ram_total_gb_;
        out.ram_used_pct = ram_used_pct_;

        out.game_ram_gb = game_ram_gb_;
        out.swap_used_gb = swap_used_gb_;
        out.swap_total_gb = swap_total_gb_;
        out.mem_psi_some_pct = mem_psi_some_pct_;
        out.ram_speed_mts = ram_speed_mts_;
        out.ram_modules = ram_modules_;
        std::memcpy(out.ram_type, ram_type_, sizeof(out.ram_type));

        out.benchmark_active = bench_active_.load(std::memory_order_relaxed);
        if (out.benchmark_active) {
            auto now = std::chrono::steady_clock::now();
            out.benchmark_elapsed_sec = std::chrono::duration<float>(now - bench_start_time_).count();
        }
        out.last_benchmark = last_bench_summary_;
    }

    out.render_api = active_api_.load(std::memory_order_relaxed);
    std::memcpy(out.cpu_name, cpu_name_, sizeof(out.cpu_name));
    std::memcpy(out.gpu_name, gpu_name_, sizeof(out.gpu_name));
    std::memcpy(out.gpu_vendor, gpu_vendor_, sizeof(out.gpu_vendor));
    std::memcpy(out.gpu_driver_version, gpu_driver_version_, sizeof(out.gpu_driver_version));
    std::memcpy(out.local_time_str, local_time_str_, sizeof(out.local_time_str));

    return out;
}

void TelemetryCollector::poll_once_sync() {
    poll_cpu_load();
    poll_cpu_clock();
    poll_cpu_temp();
    poll_cpu_power();
    poll_ram();
    poll_game_ram();
    poll_mem_pressure();
    poll_gpu();
    poll_local_clock();
}

void TelemetryCollector::worker_loop() {
    while (worker_running_.load(std::memory_order_relaxed)) {
        poll_once_sync();

        // Track peak values during benchmark
        if (bench_active_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> block(bench_mutex_);
            if (gpu_temp_c_ > bench_max_gpu_temp_c_) bench_max_gpu_temp_c_ = gpu_temp_c_;
            if (gpu_power_w_ > bench_max_gpu_power_w_) bench_max_gpu_power_w_ = gpu_power_w_;
            if (cpu_temp_c_ > bench_max_cpu_temp_c_) bench_max_cpu_temp_c_ = cpu_temp_c_;
            if (cpu_power_w_ > bench_max_cpu_power_w_) bench_max_cpu_power_w_ = cpu_power_w_;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250)); // 4 Hz smooth background polling
    }
}

void TelemetryCollector::poll_cpu_load() {
    std::string stat_path = root_prefix_ + "/proc/stat";
    std::ifstream ifs(stat_path);
    if (!ifs.is_open()) return;

    std::string line;
    if (std::getline(ifs, line) && line.rfind("cpu ", 0) == 0) {
        std::istringstream ss(line.substr(4));
        uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        if (ss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
            uint64_t active = user + nice + system + irq + softirq + steal;
            uint64_t total = active + idle + iowait;

            if (prev_cpu_total_ > 0 && total > prev_cpu_total_) {
                uint64_t delta_active = active - prev_cpu_active_;
                uint64_t delta_total = total - prev_cpu_total_;
                float pct = static_cast<float>(delta_active) * 100.0f / static_cast<float>(delta_total);
                std::lock_guard<std::mutex> lock(hw_mutex_);
                cpu_load_pct_ = std::clamp(pct, 0.0f, 100.0f);
            }
            prev_cpu_active_ = active;
            prev_cpu_total_ = total;
        }
    }
}

void TelemetryCollector::poll_cpu_clock() {
    int max_mhz = 0;
    std::error_code ec;
    std::string cpu_base = root_prefix_ + "/sys/devices/system/cpu";

    for (int i = 0; i < 64; ++i) {
        std::string cur_path = cpu_base + "/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq";
        if (!fs::exists(cur_path, ec)) break;
        int khz = read_int_from_file(cur_path, 0);
        int mhz = khz / 1000;
        if (mhz > max_mhz) max_mhz = mhz;
    }

    if (max_mhz == 0) {
        // Fallback: /proc/cpuinfo "cpu MHz"
        std::ifstream ifs(root_prefix_ + "/proc/cpuinfo");
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.rfind("cpu MHz", 0) == 0) {
                auto c = line.find(':');
                if (c != std::string::npos) {
                    int m = static_cast<int>(std::strtof(line.substr(c + 1).c_str(), nullptr));
                    if (m > max_mhz) max_mhz = m;
                }
            }
        }
    }

    if (max_mhz > 0) {
        std::lock_guard<std::mutex> lock(hw_mutex_);
        cpu_clock_mhz_ = max_mhz;
    }
}

void TelemetryCollector::poll_cpu_temp() {
    if (!cpu_temp_path_.empty()) {
        int millic = read_int_from_file(cpu_temp_path_, 0);
        if (millic > 0) {
            std::lock_guard<std::mutex> lock(hw_mutex_);
            cpu_temp_c_ = millic / 1000;
            return;
        }
    }

    // Thermal zone fallback
    std::error_code ec;
    std::string tz_base = root_prefix_ + "/sys/class/thermal";
    if (fs::exists(tz_base, ec)) {
        for (int i = 0; i < 8; ++i) {
            std::string z = tz_base + "/thermal_zone" + std::to_string(i);
            if (!fs::exists(z, ec)) break;
            std::string type;
            if (read_file_trimmed(z + "/type", type)) {
                if (type.find("cpu") != std::string::npos || type.find("x86_pkg_temp") != std::string::npos) {
                    int mc = read_int_from_file(z + "/temp", 0);
                    if (mc > 0) {
                        std::lock_guard<std::mutex> lock(hw_mutex_);
                        cpu_temp_c_ = mc / 1000;
                        return;
                    }
                }
            }
        }
    }
}

void TelemetryCollector::poll_cpu_power() {
    if (cpu_energy_path_.empty()) return;

    auto now = std::chrono::steady_clock::now();
    uint64_t energy_uj = read_u64_from_file(cpu_energy_path_, 0);
    if (energy_uj == 0) return;

    if (prev_rapl_energy_uj_ > 0 && now > prev_rapl_time_) {
        double dt_sec = std::chrono::duration<double>(now - prev_rapl_time_).count();
        if (dt_sec >= 0.15) {
            uint64_t delta_uj = 0;
            if (energy_uj >= prev_rapl_energy_uj_) {
                delta_uj = energy_uj - prev_rapl_energy_uj_;
            } else {
                delta_uj = energy_uj; // RAPL 32-bit wrap
            }
            float watts = static_cast<float>((static_cast<double>(delta_uj) / 1000000.0) / dt_sec);
            if (watts >= 0.0f && watts <= 600.0f) {
                std::lock_guard<std::mutex> lock(hw_mutex_);
                cpu_power_w_ = watts;
            }
            prev_rapl_energy_uj_ = energy_uj;
            prev_rapl_time_ = now;
        }
    } else {
        prev_rapl_energy_uj_ = energy_uj;
        prev_rapl_time_ = now;
    }
}

void TelemetryCollector::poll_ram() {
    std::string meminfo_path = root_prefix_ + "/proc/meminfo";
    std::ifstream ifs(meminfo_path);
    if (!ifs.is_open()) return;

    uint64_t mem_total_kb = 0;
    uint64_t mem_avail_kb = 0;
    uint64_t swap_total_kb = 0;
    uint64_t swap_free_kb = 0;

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            std::sscanf(line.c_str(), "MemTotal: %lu", &mem_total_kb);
        } else if (line.rfind("MemAvailable:", 0) == 0) {
            std::sscanf(line.c_str(), "MemAvailable: %lu", &mem_avail_kb);
        } else if (line.rfind("SwapTotal:", 0) == 0) {
            std::sscanf(line.c_str(), "SwapTotal: %lu", &swap_total_kb);
        } else if (line.rfind("SwapFree:", 0) == 0) {
            std::sscanf(line.c_str(), "SwapFree: %lu", &swap_free_kb);
        }
    }

    if (mem_total_kb > 0) {
        uint64_t used_kb = (mem_total_kb > mem_avail_kb) ? (mem_total_kb - mem_avail_kb) : 0;
        float total_gb = static_cast<float>(mem_total_kb) / 1048576.0f;
        float used_gb = static_cast<float>(used_kb) / 1048576.0f;
        float pct = (used_gb / total_gb) * 100.0f;

        float s_total_gb = static_cast<float>(swap_total_kb) / 1048576.0f;
        float s_used_gb = (swap_total_kb > swap_free_kb) ? (static_cast<float>(swap_total_kb - swap_free_kb) / 1048576.0f) : 0.0f;

        std::lock_guard<std::mutex> lock(hw_mutex_);
        ram_total_gb_ = total_gb;
        ram_used_gb_ = used_gb;
        ram_used_pct_ = std::clamp(pct, 0.0f, 100.0f);
        swap_total_gb_ = s_total_gb;
        swap_used_gb_ = s_used_gb;
    }
}

void TelemetryCollector::poll_game_ram() {
    std::string statm_path = root_prefix_ + "/proc/self/statm";
    std::ifstream ifs(statm_path);
    if (!ifs.is_open()) return;

    uint64_t size_pages = 0, resident_pages = 0;
    if (ifs >> size_pages >> resident_pages) {
        long page_size_bytes = sysconf(_SC_PAGESIZE);
        if (page_size_bytes <= 0) page_size_bytes = 4096;
        float rss_gb = static_cast<float>(static_cast<double>(resident_pages) * page_size_bytes / (1024.0 * 1024.0 * 1024.0));
        std::lock_guard<std::mutex> lock(hw_mutex_);
        game_ram_gb_ = rss_gb;
    }
}

void TelemetryCollector::poll_mem_pressure() {
    std::string psi_path = root_prefix_ + "/proc/pressure/memory";
    std::ifstream ifs(psi_path);
    if (!ifs.is_open()) {
        std::lock_guard<std::mutex> lock(hw_mutex_);
        mem_psi_some_pct_ = -1.0f;
        return;
    }

    std::string line;
    float avg10 = -1.0f;
    while (std::getline(ifs, line)) {
        if (line.rfind("some ", 0) == 0) {
            auto pos = line.find("avg10=");
            if (pos != std::string::npos) {
                avg10 = std::strtof(line.c_str() + pos + 6, nullptr);
                break;
            }
        }
    }

    std::lock_guard<std::mutex> lock(hw_mutex_);
    mem_psi_some_pct_ = avg10;
}

void TelemetryCollector::poll_gpu() {
    if (gpu_backend_ == GpuBackendType::NvidiaNvml) {
        poll_nvidia_gpu();
    } else if (gpu_backend_ == GpuBackendType::AmdSysfs || gpu_backend_ == GpuBackendType::IntelSysfs) {
        poll_drm_sysfs_gpu();
    }
}

void TelemetryCollector::poll_nvidia_gpu() {
    if (!nvml_device_) return;

    struct nvmlUtilization_t {
        unsigned int gpu;
        unsigned int memory;
    } util{};

    struct nvmlMemory_t {
        unsigned long long total;
        unsigned long long free;
        unsigned long long used;
    } mem{};

    unsigned int temp_c = 0;
    unsigned int fan_pct = 0;
    unsigned int power_mw = 0;
    unsigned int clock_mhz = 0;

    if (pfn_nvmlDeviceGetUtilizationRates_ && pfn_nvmlDeviceGetUtilizationRates_(nvml_device_, &util) == 0) {
        std::lock_guard<std::mutex> lock(hw_mutex_);
        gpu_load_pct_ = static_cast<float>(util.gpu);
        // Instantaneous bandwidth estimate: utilization % * theoretical peak
        gpu_bandwidth_gbs_ = (static_cast<float>(util.memory) / 100.0f) * 336.0f; // Approx 1660S / RTX GDDR6 peak
    }

    if (pfn_nvmlDeviceGetMemoryInfo_ && pfn_nvmlDeviceGetMemoryInfo_(nvml_device_, &mem) == 0) {
        std::lock_guard<std::mutex> lock(hw_mutex_);
        gpu_vram_total_gb_ = static_cast<float>(static_cast<double>(mem.total) / (1024.0 * 1024.0 * 1024.0));
        gpu_vram_used_gb_ = static_cast<float>(static_cast<double>(mem.used) / (1024.0 * 1024.0 * 1024.0));
    }

    if (pfn_nvmlDeviceGetTemperature_ && pfn_nvmlDeviceGetTemperature_(nvml_device_, 0, &temp_c) == 0) {
        std::lock_guard<std::mutex> lock(hw_mutex_);
        gpu_temp_c_ = static_cast<int>(temp_c);
    }

    if (pfn_nvmlDeviceGetFanSpeed_ && pfn_nvmlDeviceGetFanSpeed_(nvml_device_, &fan_pct) == 0) {
        std::lock_guard<std::mutex> lock(hw_mutex_);
        gpu_fan_pct_ = static_cast<int>(fan_pct);
    }

    if (pfn_nvmlDeviceGetPowerUsage_ && pfn_nvmlDeviceGetPowerUsage_(nvml_device_, &power_mw) == 0) {
        std::lock_guard<std::mutex> lock(hw_mutex_);
        gpu_power_w_ = static_cast<float>(power_mw) / 1000.0f;
    }

    if (pfn_nvmlDeviceGetClockInfo_ && pfn_nvmlDeviceGetClockInfo_(nvml_device_, 0, &clock_mhz) == 0) {
        std::lock_guard<std::mutex> lock(hw_mutex_);
        gpu_clock_mhz_ = static_cast<int>(clock_mhz);
    }

    gpu_available_ = true;
}

void TelemetryCollector::poll_drm_sysfs_gpu() {
    if (drm_card_device_dir_.empty()) return;

    std::error_code ec;

    // 1. AMD GPU Load
    std::string load_path = drm_card_device_dir_ + "/gpu_busy_percent";
    if (fs::exists(load_path, ec)) {
        int pct = read_int_from_file(load_path, -1);
        if (pct >= 0) {
            std::lock_guard<std::mutex> lock(hw_mutex_);
            gpu_load_pct_ = static_cast<float>(pct);
        }
    }

    // 2. AMD VRAM Usage
    std::string vram_used_path = drm_card_device_dir_ + "/mem_info_vram_used";
    std::string vram_tot_path = drm_card_device_dir_ + "/mem_info_vram_total";
    if (fs::exists(vram_used_path, ec)) {
        uint64_t used_b = read_u64_from_file(vram_used_path, 0);
        uint64_t tot_b = read_u64_from_file(vram_tot_path, 0);
        if (tot_b > 0) {
            std::lock_guard<std::mutex> lock(hw_mutex_);
            gpu_vram_used_gb_ = static_cast<float>(static_cast<double>(used_b) / (1024.0 * 1024.0 * 1024.0));
            gpu_vram_total_gb_ = static_cast<float>(static_cast<double>(tot_b) / (1024.0 * 1024.0 * 1024.0));
        }
    } else {
        // Fallback for APUs / GTT shared memory
        std::string gtt_used_path = drm_card_device_dir_ + "/mem_info_gtt_used";
        std::string gtt_tot_path = drm_card_device_dir_ + "/mem_info_gtt_total";
        if (fs::exists(gtt_used_path, ec)) {
            uint64_t used_b = read_u64_from_file(gtt_used_path, 0);
            uint64_t tot_b = read_u64_from_file(gtt_tot_path, 0);
            if (tot_b > 0) {
                std::lock_guard<std::mutex> lock(hw_mutex_);
                gpu_vram_used_gb_ = static_cast<float>(static_cast<double>(used_b) / (1024.0 * 1024.0 * 1024.0));
                gpu_vram_total_gb_ = static_cast<float>(static_cast<double>(tot_b) / (1024.0 * 1024.0 * 1024.0));
            }
        }
    }

    // 3. AMD / Intel Hwmon Telemetry (Temp, Fan, Power)
    std::string hwmon_dir = drm_card_device_dir_ + "/hwmon";
    if (fs::exists(hwmon_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(hwmon_dir, ec)) {
            std::string hp = entry.path().string();

            // Temperature (temp1_input or edge/junction)
            std::string t1 = hp + "/temp1_input";
            if (fs::exists(t1, ec)) {
                int mc = read_int_from_file(t1, 0);
                if (mc > 0) {
                    std::lock_guard<std::mutex> lock(hw_mutex_);
                    gpu_temp_c_ = mc / 1000;
                }
            }

            // Fan Speed
            std::string fan_in = hp + "/fan1_input";
            std::string fan_max = hp + "/fan1_max";
            std::string fan_pwm = hp + "/pwm1";
            if (fs::exists(fan_pwm, ec)) {
                int pwm = read_int_from_file(fan_pwm, 0);
                std::lock_guard<std::mutex> lock(hw_mutex_);
                gpu_fan_pct_ = static_cast<int>(std::round(static_cast<float>(pwm) * 100.0f / 255.0f));
            } else if (fs::exists(fan_in, ec) && fs::exists(fan_max, ec)) {
                int rpm = read_int_from_file(fan_in, 0);
                int max_rpm = read_int_from_file(fan_max, 1);
                if (max_rpm > 0) {
                    std::lock_guard<std::mutex> lock(hw_mutex_);
                    gpu_fan_pct_ = static_cast<int>(std::round(static_cast<float>(rpm) * 100.0f / static_cast<float>(max_rpm)));
                }
            }

            // Power (power1_average / power1_input in microwatts)
            std::string p_avg = hp + "/power1_average";
            std::string p_in = hp + "/power1_input";
            uint64_t uw = fs::exists(p_avg, ec) ? read_u64_from_file(p_avg, 0) : read_u64_from_file(p_in, 0);
            if (uw > 0) {
                std::lock_guard<std::mutex> lock(hw_mutex_);
                gpu_power_w_ = static_cast<float>(uw) / 1000000.0f;
            }
            break;
        }
    }

    // 4. Core Clock
    std::string clk_path = drm_card_device_dir_ + "/pp_dpm_sclk";
    if (fs::exists(clk_path, ec)) {
        std::ifstream ifs(clk_path);
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.find('*') != std::string::npos) {
                // e.g.: "1: 2430Mhz *"
                auto mhz_pos = line.find("Mhz");
                if (mhz_pos != std::string::npos) {
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        int m = std::atoi(line.substr(colon + 1).c_str());
                        std::lock_guard<std::mutex> lock(hw_mutex_);
                        gpu_clock_mhz_ = m;
                    }
                }
                break;
            }
        }
    }

    gpu_available_ = true;
}

void TelemetryCollector::poll_local_clock() {
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    std::lock_guard<std::mutex> lock(hw_mutex_);
    std::strftime(local_time_str_, sizeof(local_time_str_), "%H:%M", &tm_buf);
}

TelemetryCollector& get_telemetry_collector() {
    if (!g_active_telemetry) {
        static TelemetryCollector instance;
        return instance;
    }
    return *g_active_telemetry;
}

TelemetryCollector& get_telemetry() {
    return get_telemetry_collector();
}

} // namespace fpshud
