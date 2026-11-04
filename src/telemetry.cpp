#include "telemetry.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>

#if __has_include(<experimental/simd>)
#include <experimental/simd>
namespace stdx = std::experimental;
#define FPSHUD_HAS_SIMD 1
#endif

namespace fs = std::filesystem;

namespace fpshud {

namespace {

struct NvmlUtilization {
    unsigned int gpu;
    unsigned int memory;
};

struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

constexpr int NVML_TEMPERATURE_GPU = 0;
constexpr int NVML_CLOCK_GRAPHICS = 0;
constexpr int NVML_CLOCK_MEM = 2;
constexpr int NVML_PCIE_UTIL_TX_BYTES = 0;
constexpr int NVML_PCIE_UTIL_RX_BYTES = 1;

#ifndef SYS_getdents64
#ifdef __NR_getdents64
#define SYS_getdents64 __NR_getdents64
#endif
#endif

struct LinuxDirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[1];
};
constexpr int NVML_SUCCESS = 0;

// Fast zero-allocation sysfs reader using stack buffer
bool read_sysfs_buf(const char* path, char* buf, size_t buf_size) noexcept {
    if (!path || !*path || buf_size == 0) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n <= 0) {
        buf[0] = '\0';
        return false;
    }
    buf[n] = '\0';
    for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            buf[i] = '\0';
            break;
        }
    }
    return true;
}

// Shortens verbose CPU/GPU marketing strings so they fit cleanly in the GUI header
void clean_hardware_name(const char* raw, char* out, size_t out_size) noexcept {
    if (!raw || !*raw || out_size == 0) return;
    std::string s(raw);

    const char* strip_tokens[] = {
        "(R)", "(TM)", "(tm)", "CPU", "Processor",
        "12th Gen ", "13th Gen ", "14th Gen ", "11th Gen ", "10th Gen ",
        "Intel ", "NVIDIA ", "GeForce ", "AMD ", "Advanced Micro Devices, Inc. "
    };
    for (const char* tok : strip_tokens) {
        size_t pos = 0;
        size_t len = std::strlen(tok);
        while ((pos = s.find(tok, pos)) != std::string::npos) {
            s.erase(pos, len);
        }
    }
    // Strip trailing "@ 2.50GHz" or "with Radeon Graphics"
    if (size_t at = s.find('@'); at != std::string::npos) {
        s.erase(at);
    }
    if (size_t w = s.find("with "); w != std::string::npos) {
        s.erase(w);
    }
    // Collapse multiple spaces and trim
    std::string compact;
    bool prev_space = true;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!prev_space) compact.push_back(' ');
            prev_space = true;
        } else {
            compact.push_back(c);
            prev_space = false;
        }
    }
    while (!compact.empty() && compact.back() == ' ') compact.pop_back();
    std::snprintf(out, out_size, "%s", compact.empty() ? raw : compact.c_str());
}

// Resolves PCI (vendor_id, device_id) such as ("0x1002", "0x747e") against system pci.ids
bool lookup_pci_device_name(const char* vendor_hex, const char* device_hex, char* out, size_t out_size) noexcept {
    if (!vendor_hex || !device_hex || out_size == 0) return false;
    const char* v_id = (std::strncmp(vendor_hex, "0x", 2) == 0 || std::strncmp(vendor_hex, "0X", 2) == 0)
                           ? vendor_hex + 2
                           : vendor_hex;
    const char* d_id = (std::strncmp(device_hex, "0x", 2) == 0 || std::strncmp(device_hex, "0X", 2) == 0)
                           ? device_hex + 2
                           : device_hex;
    if (std::strlen(v_id) != 4 || std::strlen(d_id) != 4) return false;

    const char* pci_db_paths[] = {
        "/usr/share/hwdata/pci.ids",
        "/usr/share/misc/pci.ids",
        "/usr/share/pci.ids"
    };

    for (const char* db_path : pci_db_paths) {
        FILE* fp = std::fopen(db_path, "r");
        if (!fp) continue;

        bool in_vendor = false;
        char buf[512];
        while (std::fgets(buf, sizeof(buf), fp)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;

            if (line[0] != '\t') {
                if (in_vendor) break; // Reached next vendor section
                if (line.size() >= 4 &&
                    strncasecmp(line.c_str(), v_id, 4) == 0 &&
                    (line[4] == ' ' || line[4] == '\t')) {
                    in_vendor = true;
                }
            } else if (in_vendor && line.size() > 6 && line[0] == '\t' && line[1] != '\t') {
                if (strncasecmp(line.c_str() + 1, d_id, 4) == 0 &&
                    (line[5] == ' ' || line[5] == '\t')) {
                    std::string desc = line.substr(5);
                    while (!desc.empty() && (desc.front() == ' ' || desc.front() == '\t')) {
                        desc.erase(desc.begin());
                    }
                    // Prefer marketing name inside brackets [...] if present, e.g. "Navi 32 [Radeon RX 7700 XT / 7800 XT]"
                    auto lb = desc.rfind('[');
                    auto rb = desc.rfind(']');
                    if (lb != std::string::npos && rb != std::string::npos && rb > lb + 1) {
                        desc = desc.substr(lb + 1, rb - lb - 1);
                    }
                    clean_hardware_name(desc.c_str(), out, out_size);
                    std::fclose(fp);
                    return true;
                }
            }
        }
        std::fclose(fp);
    }
    return false;
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

TelemetryCollector::TelemetryCollector() {
    init_sensors();
    poll_once_sync();
    running_.store(true, std::memory_order_relaxed);
    worker_thread_ = std::thread(&TelemetryCollector::worker_loop, this);
}

TelemetryCollector::TelemetryCollector(std::string mock_root_prefix, bool disable_nvml)
    : root_prefix_(std::move(mock_root_prefix)), disable_nvml_(disable_nvml) {
    init_sensors();
    poll_once_sync();
}

TelemetryCollector::~TelemetryCollector() {
    running_.store(false, std::memory_order_relaxed);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    shutdown_sensors();
}

void TelemetryCollector::init_sensors() {
    init_cpu_sensors();
    init_gpu_sensors();
    init_ram_info();

    // Prime initial counters for delta metrics
    poll_cpu_load();
    poll_cpu_power();
    poll_local_clock();
}

// ============================================================================
// RAM speed / type detection
// Priority:
//   1. ~/.local/share/fpshud/ram_info   (generated by install.sh with sudo; visible
//                                         inside Steam pressure-vessel containers)
//   2. /run/fpshud/ram_info             (optional system-wide cache)
//   3. /sys/firmware/dmi/entries/17-*/raw (direct SMBIOS read, works only as root)
// ============================================================================
namespace {

const char* smbios_memory_type_name(unsigned int t) noexcept {
    switch (t) {
        case 0x12: return "DDR";
        case 0x13: return "DDR2";
        case 0x18: return "DDR3";
        case 0x1A: return "DDR4";
        case 0x1B: return "LPDDR";
        case 0x1C: return "LPDDR2";
        case 0x1D: return "LPDDR3";
        case 0x1E: return "LPDDR4";
        case 0x22: return "DDR5";
        case 0x23: return "LPDDR5";
    }
    return nullptr;
}

std::string get_user_data_dir() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/fpshud";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.local/share/fpshud";
    }
    return {};
}

} // namespace

bool TelemetryCollector::load_ram_info_file(const std::string& path) {
    if (path.empty()) return false;
    FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) return false;

    char type_buf[12] = {0};
    int speed = 0, rated = 0, modules = 0;
    char line[128];
    while (std::fgets(line, sizeof(line), fp)) {
        char key[32] = {0};
        char val[64] = {0};
        if (std::sscanf(line, " %31[^=]=%63s", key, val) != 2) continue;
        if (std::strcmp(key, "type") == 0) {
            std::snprintf(type_buf, sizeof(type_buf), "%s", val);
        } else if (std::strcmp(key, "speed_mts") == 0) {
            speed = std::atoi(val);
        } else if (std::strcmp(key, "rated_mts") == 0) {
            rated = std::atoi(val);
        } else if (std::strcmp(key, "modules") == 0) {
            modules = std::atoi(val);
        }
    }
    std::fclose(fp);

    if (speed <= 0) speed = rated;
    if (speed <= 0 && type_buf[0] == '\0') return false;

    std::lock_guard<std::mutex> lock(data_mutex_);
    hw_data_.ram_speed_mts = std::max(speed, 0);
    hw_data_.ram_modules = std::max(modules, 0);
    if (type_buf[0] != '\0' && std::strcmp(type_buf, "Unknown") != 0) {
        std::snprintf(hw_data_.ram_type, sizeof(hw_data_.ram_type), "%s", type_buf);
    }
    return true;
}

bool TelemetryCollector::load_ram_info_dmi(const std::string& entries_dir) {
    std::error_code ec;
    if (!fs::exists(entries_dir, ec)) return false;

    int best_speed = 0;
    int modules = 0;
    const char* type_name = nullptr;

    for (const auto& entry : fs::directory_iterator(entries_dir, ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("17-", 0) != 0) continue;

        std::string raw_path = (entry.path() / "raw").string();
        FILE* fp = std::fopen(raw_path.c_str(), "rb");
        if (!fp) continue; // Root-only on most distros -> silently skip
        unsigned char b[256] = {0};
        size_t n = std::fread(b, 1, sizeof(b), fp);
        std::fclose(fp);

        if (n < 0x17 || b[0] != 17) continue;
        size_t len = b[1];
        if (len > n) len = n;

        auto u16 = [&](size_t off) -> unsigned int {
            return (off + 1 < len) ? static_cast<unsigned int>(b[off] | (b[off + 1] << 8)) : 0u;
        };
        auto u32 = [&](size_t off) -> unsigned int {
            return (off + 3 < len)
                       ? static_cast<unsigned int>(b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) |
                                                   (static_cast<unsigned int>(b[off + 3]) << 24))
                       : 0u;
        };

        unsigned int size = u16(0x0C);
        if (size == 0 || size == 0xFFFF) continue; // Empty slot / unknown
        if (size & 0x8000) size = (size & 0x7FFF) / 1024; // in KB -> convert to MB

        ++modules;
        if (len > 0x12 && !type_name) {
            type_name = smbios_memory_type_name(b[0x12]);
        }

        unsigned int rated = u16(0x15);
        if (rated == 0xFFFF) rated = u32(0x54);
        unsigned int conf = u16(0x20);
        if (conf == 0xFFFF) conf = u32(0x58);

        int eff = static_cast<int>(conf > 0 ? conf : rated);
        if (eff > best_speed) best_speed = eff;
    }

    if (modules == 0) return false;

    std::lock_guard<std::mutex> lock(data_mutex_);
    hw_data_.ram_speed_mts = best_speed;
    hw_data_.ram_modules = modules;
    if (type_name) {
        std::snprintf(hw_data_.ram_type, sizeof(hw_data_.ram_type), "%s", type_name);
    }
    return best_speed > 0;
}

void TelemetryCollector::init_ram_info() {
    if (root_prefix_.empty()) {
        std::string user_dir = get_user_data_dir();
        if (!user_dir.empty() && load_ram_info_file(user_dir + "/ram_info")) return;
        if (load_ram_info_file("/etc/fpshud/ram_info")) return;
        if (load_ram_info_file("/run/fpshud/ram_info")) return;
        load_ram_info_dmi("/sys/firmware/dmi/entries");
    } else {
        if (load_ram_info_file(root_prefix_ + "/data/fpshud/ram_info")) return;
        if (load_ram_info_file(root_prefix_ + "/etc/fpshud/ram_info")) return;
        if (load_ram_info_file(root_prefix_ + "/run/fpshud/ram_info")) return;
        load_ram_info_dmi(root_prefix_ + "/sys/firmware/dmi/entries");
    }
}

void TelemetryCollector::init_cpu_sensors() {
    std::error_code ec;
    char buf[128];

    // 1. Detect CPU Model Name from /proc/cpuinfo (Works on Intel, AMD, & ARM)
    {
        std::string cpuinfo_path = root_prefix_ + "/proc/cpuinfo";
        if (FILE* fp = std::fopen(cpuinfo_path.c_str(), "r")) {
            char line_buf[256];
            while (std::fgets(line_buf, sizeof(line_buf), fp)) {
                std::string line(line_buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                if (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0) {
                    auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string raw_name = line.substr(colon + 1);
                        std::lock_guard<std::mutex> lock(data_mutex_);
                        clean_hardware_name(raw_name.c_str(), hw_data_.cpu_name, sizeof(hw_data_.cpu_name));
                        break;
                    }
                }
            }
            std::fclose(fp);
        }
    }

    // 2. Discover CPU core frequency sysfs paths (/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq)
    long max_cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (max_cpus <= 0) max_cpus = 128;
    if (!root_prefix_.empty()) {
        max_cpus = std::max(max_cpus, 512L);
    }
    max_cpus = std::clamp(max_cpus, 1L, 512L);
    cpu_freq_sensors_.clear();
    char path_buf[512];
    for (long cpu_idx = 0; cpu_idx < max_cpus; ++cpu_idx) {
        int n_len = std::snprintf(path_buf, sizeof(path_buf),
                                  "%s/sys/devices/system/cpu/cpu%ld/cpufreq/scaling_cur_freq",
                                  root_prefix_.c_str(), cpu_idx);
        if (n_len > 0 && static_cast<size_t>(n_len) < sizeof(path_buf) && ::access(path_buf, R_OK) == 0) {
            if (!cpu_freq_sensors_.try_push_back(CpuFreqSensor{static_cast<uint16_t>(cpu_idx), false})) break;
            continue;
        }
        n_len = std::snprintf(path_buf, sizeof(path_buf),
                              "%s/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_cur_freq",
                              root_prefix_.c_str(), cpu_idx);
        if (n_len > 0 && static_cast<size_t>(n_len) < sizeof(path_buf) && ::access(path_buf, R_OK) == 0) {
            if (!cpu_freq_sensors_.try_push_back(CpuFreqSensor{static_cast<uint16_t>(cpu_idx), true})) break;
        }
    }

    // 3. Discover CPU temperature & hwmon power sensors across Intel (coretemp), AMD (k10temp, zenpower, amd_energy), and ACPI/SoC
    std::string hwmon_dir = root_prefix_ + "/sys/class/hwmon";
    if (fs::exists(hwmon_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(hwmon_dir, ec)) {
            std::string name_path = (entry.path() / "name").string();
            if (!read_sysfs_buf(name_path.c_str(), buf, sizeof(buf))) continue;

            bool is_cpu_hwmon = (std::strcmp(buf, "coretemp") == 0 ||
                                 std::strcmp(buf, "k10temp") == 0 ||
                                 std::strcmp(buf, "zenpower") == 0 ||
                                 std::strcmp(buf, "zenpower3") == 0 ||
                                 std::strcmp(buf, "cpu_thermal") == 0 ||
                                 std::strcmp(buf, "acpitz") == 0);

            if (is_cpu_hwmon && cpu_temp_path_.empty()) {
                // Prefer Tctl/Tdie or Package temp1_input
                fs::path t1 = entry.path() / "temp1_input";
                if (fs::exists(t1, ec)) {
                    cpu_temp_path_ = t1.string();
                }
            }

            if (is_cpu_hwmon || std::strcmp(buf, "amd_energy") == 0) {
                if (cpu_hwmon_power_uw_path_.empty()) {
                    fs::path p_avg = entry.path() / "power1_average";
                    fs::path p_inp = entry.path() / "power1_input";
                    if (fs::exists(p_avg, ec)) cpu_hwmon_power_uw_path_ = p_avg.string();
                    else if (fs::exists(p_inp, ec)) cpu_hwmon_power_uw_path_ = p_inp.string();
                }
                if (cpu_hwmon_energy_uj_path_.empty()) {
                    fs::path e_inp = entry.path() / "energy1_input";
                    if (fs::exists(e_inp, ec)) cpu_hwmon_energy_uj_path_ = e_inp.string();
                }
            }
        }
    }

    // 3b. Fallback CPU temperature via /sys/class/thermal/thermal_zone0/temp (SoCs / Laptops / VMs)
    if (cpu_temp_path_.empty()) {
        std::string tz0 = root_prefix_ + "/sys/class/thermal/thermal_zone0/temp";
        if (fs::exists(tz0, ec)) {
            cpu_temp_path_ = std::move(tz0);
        }
    }

    // 4. Discover RAPL package energy counter (supported by both Intel & modern AMD Zen CPUs via powercap)
    const char* rapl_rel_candidates[] = {
        "/sys/class/powercap/intel-rapl:0/energy_uj",
        "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj",
        "/sys/class/powercap/amd-rapl:0/energy_uj"
    };
    for (const char* rel : rapl_rel_candidates) {
        std::string candidate = root_prefix_ + rel;
        if (fs::exists(candidate, ec)) {
            rapl_energy_path_ = candidate;
            fs::path max_path = fs::path(candidate).parent_path() / "max_energy_range_uj";
            if (read_sysfs_buf(max_path.c_str(), buf, sizeof(buf))) {
                rapl_max_range_uj_ = std::strtoull(buf, nullptr, 10);
            }
            break;
        }
    }
}

void TelemetryCollector::init_gpu_sensors() {
    std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
    init_gpu_sensors_locked();
}

void TelemetryCollector::init_gpu_sensors_locked() {
    std::error_code ec;
    char buf[128];

    // Clear DRM paths before discovery (crucial when switching bound GPU)
    drm_gpu_busy_path_.clear();
    drm_vram_used_path_.clear();
    drm_vram_total_path_.clear();
    drm_gtt_used_path_.clear();
    drm_gtt_total_path_.clear();
    drm_mem_busy_path_.clear();
    drm_sclk_freq_path_.clear();
    drm_mclk_freq_path_.clear();
    drm_max_freq_path_.clear();
    drm_pp_dpm_sclk_path_.clear();
    drm_temp_path_.clear();
    drm_temp_fallback_path_.clear();
    drm_fan_pwm_path_.clear();
    drm_fan_rpm_path_.clear();
    drm_fan_max_path_.clear();
    drm_power_uw_path_.clear();
    drm_energy_uj_path_.clear();
    drm_sclk_in_hz_ = true;
    gpu_backend_ = GpuBackendType::None;

    std::string drm_base = root_prefix_ + "/sys/class/drm";

    // ========================================================================
    // Optimus D3cold Protection: Check if all NVIDIA cards are in D3cold (suspended)
    // ========================================================================
    bool nvidia_all_suspended = false;
    bool has_nvidia_card = false;
    if (fs::exists(drm_base, ec)) {
        bool all_susp = true;
        for (int card_idx = 0; card_idx < 8; ++card_idx) {
            fs::path dev_dir = fs::path(drm_base) / ("card" + std::to_string(card_idx)) / "device";
            if (!fs::exists(dev_dir, ec)) continue;
            char vbuf[32] = {0};
            if (read_sysfs_buf((dev_dir / "vendor").c_str(), vbuf, sizeof(vbuf))) {
                if (std::strcmp(vbuf, "0x10de") == 0 || std::strtoul(vbuf, nullptr, 16) == 0x10de) {
                    has_nvidia_card = true;
                    char pbuf[32] = {0};
                    if (read_sysfs_buf((dev_dir / "power/runtime_status").c_str(), pbuf, sizeof(pbuf))) {
                        if (std::strcmp(pbuf, "suspended") != 0 && std::strstr(pbuf, "suspended") == nullptr) {
                            all_susp = false;
                        }
                    } else {
                        all_susp = false;
                    }
                }
            }
        }
        if (has_nvidia_card && all_susp) {
            nvidia_all_suspended = true;
        }
    }

    // ========================================================================
    // Backend 1: Try NVIDIA NVML first (libnvidia-ml.so.1) unless:
    // - disabled by config/mock
    // - bound explicitly to a non-NVIDIA GPU (vendor_id != 0x10de)
    // - all NVIDIA GPUs are currently suspended (Optimus D3cold protection) and not explicitly bound to 0x10de
    // ========================================================================
    bool allow_nvml = !disable_nvml_ && root_prefix_.empty();
    if (bound_vendor_id_ != 0 && bound_vendor_id_ != 0x10de) {
        allow_nvml = false;
    }
    if (nvidia_all_suspended && bound_vendor_id_ != 0x10de) {
        allow_nvml = false;
    }

    if (allow_nvml) {
        const char* nvml_candidates[] = {
            "libnvidia-ml.so.1",
            "libnvidia-ml.so",
            "/run/host/usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1",
            "/run/host/usr/lib64/libnvidia-ml.so.1"
        };
        for (const char* path : nvml_candidates) {
            nvml_lib_ = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
            if (nvml_lib_) break;
        }
    }

    if (nvml_lib_) {
        pfn_nvmlInit_v2_ = reinterpret_cast<int (*)()>(dlsym(nvml_lib_, "nvmlInit_v2"));
        pfn_nvmlShutdown_ = reinterpret_cast<int (*)()>(dlsym(nvml_lib_, "nvmlShutdown"));
        pfn_nvmlSystemGetDriverVersion_ = reinterpret_cast<int (*)(char*, unsigned int)>(
            dlsym(nvml_lib_, "nvmlSystemGetDriverVersion"));
        pfn_nvmlDeviceGetHandleByIndex_v2_ = reinterpret_cast<int (*)(unsigned int, void**)>(
            dlsym(nvml_lib_, "nvmlDeviceGetHandleByIndex_v2"));
        pfn_nvmlDeviceGetName_ = reinterpret_cast<int (*)(void*, char*, unsigned int)>(
            dlsym(nvml_lib_, "nvmlDeviceGetName"));
        pfn_nvmlDeviceGetUtilizationRates_ = reinterpret_cast<int (*)(void*, void*)>(
            dlsym(nvml_lib_, "nvmlDeviceGetUtilizationRates"));
        pfn_nvmlDeviceGetMemoryInfo_ = reinterpret_cast<int (*)(void*, void*)>(
            dlsym(nvml_lib_, "nvmlDeviceGetMemoryInfo"));
        pfn_nvmlDeviceGetTemperature_ = reinterpret_cast<int (*)(void*, int, unsigned int*)>(
            dlsym(nvml_lib_, "nvmlDeviceGetTemperature"));
        pfn_nvmlDeviceGetFanSpeed_ = reinterpret_cast<int (*)(void*, unsigned int*)>(
            dlsym(nvml_lib_, "nvmlDeviceGetFanSpeed"));
        pfn_nvmlDeviceGetPowerUsage_ = reinterpret_cast<int (*)(void*, unsigned int*)>(
            dlsym(nvml_lib_, "nvmlDeviceGetPowerUsage"));
        pfn_nvmlDeviceGetClockInfo_ = reinterpret_cast<int (*)(void*, int, unsigned int*)>(
            dlsym(nvml_lib_, "nvmlDeviceGetClockInfo"));
        pfn_nvmlDeviceGetMemoryBusWidth_ = reinterpret_cast<int (*)(void*, unsigned int*)>(
            dlsym(nvml_lib_, "nvmlDeviceGetMemoryBusWidth"));
        pfn_nvmlDeviceGetPcieThroughput_ = reinterpret_cast<int (*)(void*, int, unsigned int*)>(
            dlsym(nvml_lib_, "nvmlDeviceGetPcieThroughput"));

        if (pfn_nvmlInit_v2_ && pfn_nvmlDeviceGetHandleByIndex_v2_) {
            if (pfn_nvmlInit_v2_() == NVML_SUCCESS) {
                nvml_initialized_ = true;
                if (pfn_nvmlDeviceGetHandleByIndex_v2_(0, &nvml_device_) == NVML_SUCCESS && nvml_device_) {
                    gpu_backend_ = GpuBackendType::NvidiaNvml;
                    std::lock_guard<std::mutex> lock(data_mutex_);
                    hw_data_.gpu_available = true;
                    std::snprintf(hw_data_.gpu_vendor, sizeof(hw_data_.gpu_vendor), "NVIDIA");

                    if (pfn_nvmlDeviceGetName_) {
                        char raw_gpu[96] = {0};
                        if (pfn_nvmlDeviceGetName_(nvml_device_, raw_gpu, sizeof(raw_gpu)) == NVML_SUCCESS) {
                            clean_hardware_name(raw_gpu, hw_data_.gpu_name, sizeof(hw_data_.gpu_name));
                        }
                    }
                    if (pfn_nvmlSystemGetDriverVersion_) {
                        char drv[32] = {0};
                        if (pfn_nvmlSystemGetDriverVersion_(drv, sizeof(drv)) == NVML_SUCCESS) {
                            std::snprintf(hw_data_.gpu_driver_version, sizeof(hw_data_.gpu_driver_version), "%s", drv);
                        }
                    }
                    unsigned int bw_bits = 0;
                    if (pfn_nvmlDeviceGetMemoryBusWidth_ &&
                        pfn_nvmlDeviceGetMemoryBusWidth_(nvml_device_, &bw_bits) == NVML_SUCCESS &&
                        bw_bits > 0) {
                        gpu_bus_width_bits_ = bw_bits;
                    }
                    return;
                }
            }
        }
    }

    // ========================================================================
    // Backend 2 & 3: Universal Linux DRM sysfs for AMD (amdgpu/radeon) & Intel (i915/xe)
    // Automatically prioritizes Discrete GPU (largest VRAM) on Dual-GPU / Hybrid laptops!
    // ========================================================================
    if (!fs::exists(drm_base, ec)) return;

    int best_card_idx = -1;
    uint64_t best_vram_bytes = 0;
    std::string best_vendor_id;
    std::string best_device_id;

    for (int card_idx = 0; card_idx < 8; ++card_idx) {
        fs::path card_root = fs::path(drm_base) / ("card" + std::to_string(card_idx));
        fs::path dev_dir = card_root / "device";
        if (!fs::exists(dev_dir, ec)) continue;

        std::string vendor_id;
        if (read_sysfs_buf((dev_dir / "vendor").c_str(), buf, sizeof(buf))) {
            vendor_id = buf;
        }
        uint32_t card_vendor = static_cast<uint32_t>(std::strtoul(vendor_id.c_str(), nullptr, 16));
        bool is_amd    = (card_vendor == 0x1002);
        bool is_intel  = (card_vendor == 0x8086);
        bool is_nvidia = (card_vendor == 0x10de);
        if (!is_amd && !is_intel && !is_nvidia) continue;

        std::string device_id;
        if (read_sysfs_buf((dev_dir / "device").c_str(), buf, sizeof(buf))) {
            device_id = buf;
        }
        uint32_t card_device = static_cast<uint32_t>(std::strtoul(device_id.c_str(), nullptr, 16));

        // Filtering by bound GPU if specified
        if (bound_vendor_id_ != 0) {
            if (card_vendor != bound_vendor_id_) continue;
            if (bound_device_id_ != 0 && card_device != 0 && card_device != bound_device_id_) continue;
        } else {
            // Unbound: if NVIDIA card is suspended, do not pick it over an active iGPU!
            if (is_nvidia) {
                char pbuf[32] = {0};
                if (read_sysfs_buf((dev_dir / "power/runtime_status").c_str(), pbuf, sizeof(pbuf))) {
                    if (std::strcmp(pbuf, "suspended") == 0 || std::strstr(pbuf, "suspended") != nullptr) {
                        continue;
                    }
                }
            }
        }

        uint64_t card_vram = 0;
        if (read_sysfs_buf((dev_dir / "mem_info_vram_total").c_str(), buf, sizeof(buf))) {
            card_vram = std::strtoull(buf, nullptr, 10);
        } else if (read_sysfs_buf((card_root / "lmem_total_bytes").c_str(), buf, sizeof(buf))) {
            card_vram = std::strtoull(buf, nullptr, 10);
        } else if (read_sysfs_buf((dev_dir / "lmem_total_bytes").c_str(), buf, sizeof(buf))) {
            card_vram = std::strtoull(buf, nullptr, 10);
        }

        // If explicitly bound to this vendor/device, immediate preference
        if (bound_vendor_id_ != 0 && card_vendor == bound_vendor_id_) {
            if (best_card_idx < 0 || (bound_device_id_ != 0 && card_device == bound_device_id_) || card_vram > best_vram_bytes) {
                best_card_idx = card_idx;
                best_vram_bytes = card_vram;
                best_vendor_id = vendor_id;
                best_device_id = device_id;
                if (bound_device_id_ != 0 && card_device == bound_device_id_) break;
            }
        } else if (best_card_idx < 0 || card_vram > best_vram_bytes) {
            best_card_idx = card_idx;
            best_vram_bytes = card_vram;
            best_vendor_id = vendor_id;
            best_device_id = device_id;
        }
    }

    if (best_card_idx < 0) return;

    fs::path card_root = fs::path(drm_base) / ("card" + std::to_string(best_card_idx));
    fs::path dev_dir = card_root / "device";
    uint32_t best_vendor_num = static_cast<uint32_t>(std::strtoul(best_vendor_id.c_str(), nullptr, 16));
    bool is_amd    = (best_vendor_num == 0x1002);
    bool is_intel  = (best_vendor_num == 0x8086);
    bool is_nvidia = (best_vendor_num == 0x10de);

    if (is_amd) gpu_backend_ = GpuBackendType::AmdSysfs;
    else if (is_intel) gpu_backend_ = GpuBackendType::IntelSysfs;
    else gpu_backend_ = GpuBackendType::AmdSysfs; // generic DRM sysfs for Nouveau

    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        hw_data_.gpu_available = true;
        std::snprintf(hw_data_.gpu_vendor, sizeof(hw_data_.gpu_vendor), "%s",
                      is_amd ? "AMD" : (is_intel ? "Intel" : (is_nvidia ? "NVIDIA" : "Unknown")));
        std::snprintf(hw_data_.gpu_name, sizeof(hw_data_.gpu_name), "%s",
                      is_amd ? "Radeon GPU" : (is_intel ? "Intel Graphics" : (is_nvidia ? "NVIDIA GPU" : "Generic GPU")));
        std::snprintf(hw_data_.gpu_driver_version, sizeof(hw_data_.gpu_driver_version), "%s",
                      is_amd ? "amdgpu" : (is_intel ? "i915/xe" : (is_nvidia ? "nouveau" : "drm")));

        // 1. Check sysfs product_name first
        if (read_sysfs_buf((dev_dir / "product_name").c_str(), buf, sizeof(buf)) && buf[0] != '\0') {
            clean_hardware_name(buf, hw_data_.gpu_name, sizeof(hw_data_.gpu_name));
        } else if (!best_device_id.empty()) {
            // 2. Fallback: Resolve exact marketing name via PCI ID database (/usr/share/hwdata/pci.ids)
            lookup_pci_device_name(best_vendor_id.c_str(), best_device_id.c_str(),
                                   hw_data_.gpu_name, sizeof(hw_data_.gpu_name));
        }
    }

    // Common DRM busy & VRAM paths (AMD amdgpu & Intel xe)
    if (fs::exists(dev_dir / "gpu_busy_percent", ec)) {
        drm_gpu_busy_path_ = (dev_dir / "gpu_busy_percent").string();
    }
    if (fs::exists(dev_dir / "mem_busy_percent", ec)) {
        drm_mem_busy_path_ = (dev_dir / "mem_busy_percent").string();
    }
    if (fs::exists(dev_dir / "mem_info_vram_used", ec)) {
        drm_vram_used_path_ = (dev_dir / "mem_info_vram_used").string();
    } else if (fs::exists(card_root / "lmem_used_bytes", ec)) {
        drm_vram_used_path_ = (card_root / "lmem_used_bytes").string();
    } else if (fs::exists(dev_dir / "lmem_used_bytes", ec)) {
        drm_vram_used_path_ = (dev_dir / "lmem_used_bytes").string();
    } else if (fs::exists(card_root / "lmem0/lmem_used_bytes", ec)) {
        drm_vram_used_path_ = (card_root / "lmem0/lmem_used_bytes").string();
    }

    if (fs::exists(dev_dir / "mem_info_vram_total", ec)) {
        drm_vram_total_path_ = (dev_dir / "mem_info_vram_total").string();
    } else if (fs::exists(card_root / "lmem_total_bytes", ec)) {
        drm_vram_total_path_ = (card_root / "lmem_total_bytes").string();
    } else if (fs::exists(dev_dir / "lmem_total_bytes", ec)) {
        drm_vram_total_path_ = (dev_dir / "lmem_total_bytes").string();
    } else if (fs::exists(card_root / "lmem0/lmem_total_bytes", ec)) {
        drm_vram_total_path_ = (card_root / "lmem0/lmem_total_bytes").string();
    }
    if (fs::exists(dev_dir / "mem_info_gtt_used", ec)) {
        drm_gtt_used_path_ = (dev_dir / "mem_info_gtt_used").string();
    }
    if (fs::exists(dev_dir / "mem_info_gtt_total", ec)) {
        drm_gtt_total_path_ = (dev_dir / "mem_info_gtt_total").string();
    }
    if (fs::exists(dev_dir / "pp_dpm_sclk", ec)) {
        drm_pp_dpm_sclk_path_ = (dev_dir / "pp_dpm_sclk").string();
    }

    // Intel i915 / xe specific frequency paths
    if (fs::exists(dev_dir / "tile0/gt0/freq0/act_freq", ec)) {
        drm_sclk_freq_path_ = (dev_dir / "tile0/gt0/freq0/act_freq").string();
        drm_sclk_in_hz_ = false;
    } else if (fs::exists(dev_dir / "tile0/gt0/freq0/cur_freq", ec)) {
        drm_sclk_freq_path_ = (dev_dir / "tile0/gt0/freq0/cur_freq").string();
        drm_sclk_in_hz_ = false;
    } else if (fs::exists(card_root / "gt_cur_freq_mhz", ec)) {
        drm_sclk_freq_path_ = (card_root / "gt_cur_freq_mhz").string();
        drm_sclk_in_hz_ = false;
    } else if (fs::exists(card_root / "gt/gt0/rps_cur_freq_mhz", ec)) {
        drm_sclk_freq_path_ = (card_root / "gt/gt0/rps_cur_freq_mhz").string();
        drm_sclk_in_hz_ = false;
    }

    if (fs::exists(dev_dir / "tile0/gt0/freq0/max_freq", ec)) {
        drm_max_freq_path_ = (dev_dir / "tile0/gt0/freq0/max_freq").string();
    } else if (fs::exists(dev_dir / "tile0/gt0/freq0/rp0_freq", ec)) {
        drm_max_freq_path_ = (dev_dir / "tile0/gt0/freq0/rp0_freq").string();
    } else if (fs::exists(card_root / "gt_max_freq_mhz", ec)) {
        drm_max_freq_path_ = (card_root / "gt_max_freq_mhz").string();
    } else if (fs::exists(card_root / "gt/gt0/rps_max_freq_mhz", ec)) {
        drm_max_freq_path_ = (card_root / "gt/gt0/rps_max_freq_mhz").string();
    }

    // Discover GPU hwmon subfolder (temp, fan, clock, power/energy)
    fs::path hwmon_root = dev_dir / "hwmon";
    if (fs::exists(hwmon_root, ec)) {
        for (const auto& hm : fs::directory_iterator(hwmon_root, ec)) {
            fs::path hp = hm.path();
            if (fs::exists(hp / "freq1_input", ec)) {
                drm_sclk_freq_path_ = (hp / "freq1_input").string();
                drm_sclk_in_hz_ = true;
            }
            if (fs::exists(hp / "freq2_input", ec)) {
                drm_mclk_freq_path_ = (hp / "freq2_input").string();
            }

            // Junction / Hotspot (temp2_input) detection with fallback to temp1_input
            std::string temp1_path;
            std::string temp2_path;
            if (fs::exists(hp / "temp1_input", ec)) {
                temp1_path = (hp / "temp1_input").string();
            }
            if (fs::exists(hp / "temp2_input", ec)) {
                bool is_junction = false;
                if (fs::exists(hp / "temp2_label", ec)) {
                    char lbl[64] = {0};
                    if (read_sysfs_buf((hp / "temp2_label").c_str(), lbl, sizeof(lbl))) {
                        if (std::strstr(lbl, "junction") != nullptr) {
                            is_junction = true;
                        }
                    }
                }
                if (is_junction || is_amd) {
                    temp2_path = (hp / "temp2_input").string();
                }
            }
            if (!temp2_path.empty()) {
                drm_temp_path_ = temp2_path;
                drm_temp_fallback_path_ = temp1_path;
            } else if (!temp1_path.empty()) {
                drm_temp_path_ = temp1_path;
                drm_temp_fallback_path_.clear();
            }

            if (fs::exists(hp / "pwm1", ec)) {
                drm_fan_pwm_path_ = (hp / "pwm1").string();
            }
            if (fs::exists(hp / "fan1_input", ec) && fs::exists(hp / "fan1_max", ec)) {
                drm_fan_rpm_path_ = (hp / "fan1_input").string();
                drm_fan_max_path_ = (hp / "fan1_max").string();
            }
            if (fs::exists(hp / "power1_average", ec)) {
                drm_power_uw_path_ = (hp / "power1_average").string();
            } else if (fs::exists(hp / "power1_input", ec)) {
                drm_power_uw_path_ = (hp / "power1_input").string();
            }
            if (fs::exists(hp / "energy1_input", ec)) {
                drm_energy_uj_path_ = (hp / "energy1_input").string();
            }
            break;
        }
    }

    // Fallback for Intel integrated GPUs: check RAPL uncore/iGPU energy counter (intel-rapl:0:1)
    if (drm_power_uw_path_.empty() && drm_energy_uj_path_.empty() && is_intel) {
        std::string uncore_rapl = root_prefix_ + "/sys/class/powercap/intel-rapl:0:1/energy_uj";
        if (fs::exists(uncore_rapl, ec)) {
            drm_energy_uj_path_ = std::move(uncore_rapl);
        }
    }
}

void TelemetryCollector::shutdown_sensors() {
    std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
    shutdown_sensors_locked();
}

void TelemetryCollector::shutdown_sensors_locked() {
    if (nvml_initialized_ && pfn_nvmlShutdown_) {
        pfn_nvmlShutdown_();
        nvml_initialized_ = false;
    }
    if (nvml_lib_) {
        dlclose(nvml_lib_);
        nvml_lib_ = nullptr;
    }
    nvml_device_ = nullptr;
    gpu_backend_ = GpuBackendType::None;
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
    if (std::isnan(dt_ms) || std::isinf(dt_ms) || dt_ms <= 0.001f || dt_ms > 5000.0f) {
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
    graph_fps_[graph_offset_] = (ema_frametime_ms_ > 0.001f) ? (1000.0f / ema_frametime_ms_) : 0.0f;
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
        displayed_fps_.store((ema_frametime_ms_ > 0.001f) ? (1000.0f / ema_frametime_ms_) : 0.0f, std::memory_order_relaxed);

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
                        double rem = target_tail_ms - accum_ms;
                        weighted_ft_sum += rem * ft;
                        accum_ms = target_tail_ms;
                        break;
                    }
                    weighted_ft_sum += ft * ft;
                    accum_ms += ft;
                }

                if (accum_ms <= 0.0001) return (recent_count > 0 && recent_ft[0] > 0.0001f) ? (1000.0f / recent_ft[0]) : 0.0f;
                double effective_tail_ft_ms = weighted_ft_sum / accum_ms;
                return (effective_tail_ft_ms > 0.0001) ? static_cast<float>(1000.0 / effective_tail_ft_ms) : 0.0f;
            };

            float fps_1low = calc_time_weighted_low_fps(0.01);

            float fps_01low = fps_1low;
            if (recent_count >= 4) {
                float worst_0 = recent_ft[0];
                float worst_1 = recent_ft[1];
                float blended_01_ms = worst_0 * 0.65f + worst_1 * 0.35f;
                float tw_01 = calc_time_weighted_low_fps(0.002);
                fps_01low = std::min(fps_1low, (1000.0f / blended_01_ms) * 0.5f + tw_01 * 0.5f);
            } else {
                fps_01low = calc_time_weighted_low_fps(0.001);
            }

            displayed_fps_1low_.store(fps_1low, std::memory_order_relaxed);
            displayed_fps_01low_.store(fps_01low, std::memory_order_relaxed);
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

namespace {

#if defined(FPSHUD_HAS_SIMD)
template <size_t Width>
SimdStatsResult reduce_frametime_simd_impl(const float* data, size_t n) noexcept {
    using Abi = stdx::simd_abi::deduce_t<float, Width>;
    using V = stdx::simd<float, Abi>;
    constexpr size_t VSize = V::size();

    const V kMaxFinite(std::numeric_limits<float>::max());
    V sum_v(0.0f);
    V min_v(std::numeric_limits<float>::max());
    V max_v(std::numeric_limits<float>::lowest());

    double total = 0.0;
    float scalar_min = std::numeric_limits<float>::max();
    float scalar_max = std::numeric_limits<float>::lowest();
    bool any_valid = false;
    size_t chunks_accumulated = 0;
    size_t i = 0;

    for (; i + VSize <= n; i += VSize) {
        V chunk(&data[i], stdx::element_aligned);
        auto finite_mask = stdx::abs(chunk) <= kMaxFinite;
        if (stdx::all_of(finite_mask)) {
            any_valid = true;
            sum_v += chunk;
            min_v = stdx::min(min_v, chunk);
            max_v = stdx::max(max_v, chunk);

            // Periodically fold into double total to eliminate floating-point drift in ultra-long runs
            if (++chunks_accumulated == 512) {
                for (size_t lane = 0; lane < VSize; ++lane) {
                    total += static_cast<double>(sum_v[lane]);
                }
                sum_v = V(0.0f);
                chunks_accumulated = 0;
            }
        } else {
            for (size_t lane = 0; lane < VSize; ++lane) {
                float v = chunk[lane];
                if (std::isfinite(v)) {
                    any_valid = true;
                    total += static_cast<double>(v);
                    if (v < scalar_min) scalar_min = v;
                    if (v > scalar_max) scalar_max = v;
                }
            }
        }
    }

    for (size_t lane = 0; lane < VSize; ++lane) {
        total += static_cast<double>(sum_v[lane]);
        if (min_v[lane] < scalar_min) scalar_min = min_v[lane];
        if (max_v[lane] > scalar_max) scalar_max = max_v[lane];
    }

    for (; i < n; ++i) {
        float v = data[i];
        if (std::isfinite(v)) {
            any_valid = true;
            total += static_cast<double>(v);
            if (v < scalar_min) scalar_min = v;
            if (v > scalar_max) scalar_max = v;
        }
    }

    if (!any_valid) {
        return SimdStatsResult{0.0, 0.0f, 0.0f};
    }
    return SimdStatsResult{total, scalar_min, scalar_max};
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
[[gnu::target("avx2")]]
SimdStatsResult reduce_frametime_avx2(const float* data, size_t n) noexcept {
    return reduce_frametime_simd_impl<8>(data, n);
}

SimdStatsResult reduce_frametime_sse2(const float* data, size_t n) noexcept {
    return reduce_frametime_simd_impl<4>(data, n);
}
#endif
#endif

} // namespace

SimdStatsResult compute_frametime_stats_simd(const std::vector<float>& data) {
    if (data.empty()) return SimdStatsResult{};

#if defined(FPSHUD_HAS_SIMD)
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    static const bool s_has_avx2 = __builtin_cpu_supports("avx2");
    if (s_has_avx2) {
        return reduce_frametime_avx2(data.data(), data.size());
    }
    return reduce_frametime_sse2(data.data(), data.size());
#else
    return reduce_frametime_simd_impl<stdx::native_simd<float>::size()>(data.data(), data.size());
#endif
#else
    double total = 0.0;
    float min_val = std::numeric_limits<float>::max();
    float max_val = std::numeric_limits<float>::lowest();
    bool any_valid = false;
    for (float v : data) {
        if (std::isfinite(v)) {
            any_valid = true;
            total += static_cast<double>(v);
            if (v < min_val) min_val = v;
            if (v > max_val) max_val = v;
        }
    }
    if (!any_valid) return SimdStatsResult{0.0, 0.0f, 0.0f};
    return SimdStatsResult{total, min_val, max_val};
#endif
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
    SimdStatsResult stats = compute_frametime_stats_simd(bench_frametimes_ms_);
    double total_ms = stats.total_ms;
    float min_ft_ms = stats.min_val;
    float max_ft_ms = stats.max_val;

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
    bench_has_result_.store(true, std::memory_order_relaxed);
    std::vector<float>().swap(bench_frametimes_ms_);
}

void TelemetryCollector::toggle_benchmark() {
    if (is_benchmark_active()) {
        stop_benchmark();
    } else {
        start_benchmark();
    }
}

TelemetryData TelemetryCollector::get_snapshot() const {
    TelemetryData out;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        out = hw_data_;
    }
    out.render_api = active_api_.load(std::memory_order_relaxed);
    out.fps = displayed_fps_.load(std::memory_order_relaxed);
    out.fps_avg = displayed_fps_avg_.load(std::memory_order_relaxed);
    out.fps_1pct_low = displayed_fps_1low_.load(std::memory_order_relaxed);
    out.fps_01pct_low = displayed_fps_01low_.load(std::memory_order_relaxed);
    out.frametime_ms = displayed_frametime_ms_.load(std::memory_order_relaxed);

    if (bench_active_.load(std::memory_order_relaxed) || bench_has_result_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> block(bench_mutex_);
        out.benchmark_active = bench_active_.load(std::memory_order_relaxed);
        if (out.benchmark_active) {
            auto now = std::chrono::steady_clock::now();
            out.benchmark_elapsed_sec = std::chrono::duration<float>(now - bench_start_time_).count();
        }
        out.last_benchmark = last_bench_summary_;
    }
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

void TelemetryCollector::notify_active_gpu(uint32_t vendor_id, uint32_t device_id) {
    std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
    if (bound_vendor_id_ == vendor_id && bound_device_id_ == device_id && gpu_backend_ != GpuBackendType::None) {
        return;
    }
    bound_vendor_id_ = vendor_id;
    bound_device_id_ = device_id;
    shutdown_sensors_locked();
    init_gpu_sensors_locked();
    if (gpu_backend_ == GpuBackendType::NvidiaNvml) {
        poll_nvidia_gpu();
    } else if (gpu_backend_ == GpuBackendType::AmdSysfs || gpu_backend_ == GpuBackendType::IntelSysfs) {
        poll_drm_sysfs_gpu();
    }
}

void TelemetryCollector::update_api_vram_info(uint64_t used_bytes, uint64_t total_bytes) noexcept {
    api_vram_used_bytes_.store(used_bytes, std::memory_order_relaxed);
    api_vram_total_bytes_.store(total_bytes, std::memory_order_relaxed);
}

void TelemetryCollector::worker_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        poll_once_sync();
        for (int i = 0; i < 10 && running_.load(std::memory_order_relaxed); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

void TelemetryCollector::poll_local_clock() {
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[8];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm_buf);
    std::lock_guard<std::mutex> lock(data_mutex_);
    std::memcpy(hw_data_.local_time_str, buf, sizeof(hw_data_.local_time_str));
}

void TelemetryCollector::poll_cpu_load() {
    char buf[256];
    std::string stat_path = root_prefix_ + "/proc/stat";
    if (!read_sysfs_buf(stat_path.c_str(), buf, sizeof(buf))) return;

    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (std::sscanf(buf, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
                    &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4) {
        return;
    }

    uint64_t idle_all = idle + iowait;
    uint64_t total = user + nice + system + idle_all + irq + softirq + steal;

    if (prev_cpu_total_ != 0 && total > prev_cpu_total_) {
        uint64_t total_d = total - prev_cpu_total_;
        uint64_t idle_d = idle_all - prev_cpu_idle_;
        float usage = 100.0f * static_cast<float>(total_d - idle_d) / static_cast<float>(total_d);
        std::lock_guard<std::mutex> lock(data_mutex_);
        hw_data_.cpu_load_pct = usage;
    }

    prev_cpu_total_ = total;
    prev_cpu_idle_ = idle_all;
}

void TelemetryCollector::poll_cpu_clock() {
    int max_mhz = 0;
    char buf[32];
    char path_buf[512];
    for (const auto& sensor : cpu_freq_sensors_) {
        int n_len = std::snprintf(
            path_buf, sizeof(path_buf),
            sensor.use_cpuinfo ? "%s/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_cur_freq"
                               : "%s/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq",
            root_prefix_.c_str(), static_cast<unsigned>(sensor.cpu_id));
        if (n_len <= 0 || static_cast<size_t>(n_len) >= sizeof(path_buf)) continue;
        if (read_sysfs_buf(path_buf, buf, sizeof(buf))) {
            long khz = std::strtol(buf, nullptr, 10);
            int mhz = static_cast<int>((khz + 500) / 1000);
            if (mhz > max_mhz) max_mhz = mhz;
        }
    }

    if (max_mhz > 0) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        hw_data_.cpu_clock_mhz = max_mhz;
    }
}

void TelemetryCollector::poll_cpu_temp() {
    if (cpu_temp_path_.empty()) return;
    char buf[32];
    if (!read_sysfs_buf(cpu_temp_path_.c_str(), buf, sizeof(buf))) return;

    long millideg = std::strtol(buf, nullptr, 10);
    int temp_c = static_cast<int>((millideg + 500) / 1000);
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        hw_data_.cpu_temp_c = temp_c;
    }
    if (bench_active_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> block(bench_mutex_);
        if (temp_c > bench_max_cpu_temp_c_) bench_max_cpu_temp_c_ = temp_c;
    }
}

void TelemetryCollector::poll_cpu_power() {
    char buf[32];
    float watts = -1.0f;

    // 1. Primary: Intel / AMD RAPL energy counter
    const std::string& energy_path = !rapl_energy_path_.empty() ? rapl_energy_path_ : cpu_hwmon_energy_uj_path_;
    if (!energy_path.empty() && read_sysfs_buf(energy_path.c_str(), buf, sizeof(buf))) {
        uint64_t curr_uj = std::strtoull(buf, nullptr, 10);
        auto now = std::chrono::steady_clock::now();

        if (prev_rapl_time_.time_since_epoch().count() != 0) {
            double dt_sec = std::chrono::duration<double>(now - prev_rapl_time_).count();
            if (dt_sec > 0.01) {
                uint64_t delta_uj = 0;
                if (curr_uj >= prev_rapl_uj_) {
                    delta_uj = curr_uj - prev_rapl_uj_;
                } else if (rapl_max_range_uj_ > prev_rapl_uj_) {
                    delta_uj = (rapl_max_range_uj_ - prev_rapl_uj_) + curr_uj;
                }
                watts = static_cast<float>((static_cast<double>(delta_uj) / 1e6) / dt_sec);
            }
        }
        prev_rapl_uj_ = curr_uj;
        prev_rapl_time_ = now;
    }

    // 2. Fallback: Direct hwmon power1_average / power1_input in microwatts (AMD zenpower / k10temp)
    if (watts < 0.0f && !cpu_hwmon_power_uw_path_.empty() &&
        read_sysfs_buf(cpu_hwmon_power_uw_path_.c_str(), buf, sizeof(buf))) {
        uint64_t uw = std::strtoull(buf, nullptr, 10);
        watts = static_cast<float>(uw) / 1e6f;
    }

    if (watts >= 0.0f && watts < 500.0f) {
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            hw_data_.cpu_power_w = watts;
        }
        if (bench_active_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> block(bench_mutex_);
            if (watts > bench_max_cpu_power_w_) bench_max_cpu_power_w_ = watts;
        }
    }
}

void TelemetryCollector::poll_ram() {
    std::string mem_path = root_prefix_ + "/proc/meminfo";
    int fd = open(mem_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    uint64_t mem_total_kb = 0;
    uint64_t mem_avail_kb = 0;
    uint64_t swap_total_kb = 0;
    uint64_t swap_free_kb = 0;

    if (const char* p = std::strstr(buf, "MemTotal:")) {
        mem_total_kb = std::strtoull(p + 9, nullptr, 10);
    }
    if (const char* p = std::strstr(buf, "MemAvailable:")) {
        mem_avail_kb = std::strtoull(p + 13, nullptr, 10);
    }
    if (const char* p = std::strstr(buf, "SwapTotal:")) {
        swap_total_kb = std::strtoull(p + 10, nullptr, 10);
    }
    if (const char* p = std::strstr(buf, "SwapFree:")) {
        swap_free_kb = std::strtoull(p + 9, nullptr, 10);
    }

    if (mem_total_kb > 0) {
        uint64_t used_kb = (mem_total_kb > mem_avail_kb) ? (mem_total_kb - mem_avail_kb) : 0;
        uint64_t swap_used_kb = (swap_total_kb > swap_free_kb) ? (swap_total_kb - swap_free_kb) : 0;
        constexpr float kKbPerGb = 1024.0f * 1024.0f;
        std::lock_guard<std::mutex> lock(data_mutex_);
        hw_data_.ram_total_gb = static_cast<float>(mem_total_kb) / kKbPerGb;
        hw_data_.ram_used_gb = static_cast<float>(used_kb) / kKbPerGb;
        hw_data_.ram_used_pct = 100.0f * static_cast<float>(used_kb) / static_cast<float>(mem_total_kb);
        hw_data_.swap_total_gb = static_cast<float>(swap_total_kb) / kKbPerGb;
        hw_data_.swap_used_gb = static_cast<float>(swap_used_kb) / kKbPerGb;
    }
}

void TelemetryCollector::poll_game_ram() {
    // /proc/self/status of the hooked game process (VmRSS = resident physical memory)
    std::string status_path = root_prefix_ + "/proc/self/status";
    int fd = open(status_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    if (const char* p = std::strstr(buf, "VmRSS:")) {
        uint64_t rss_kb = std::strtoull(p + 6, nullptr, 10);
        std::lock_guard<std::mutex> lock(data_mutex_);
        hw_data_.game_ram_gb = static_cast<float>(rss_kb) / (1024.0f * 1024.0f);
    }
}

void TelemetryCollector::poll_mem_pressure() {
    // Linux PSI: share of time (last 10s) in which at least one task stalled waiting for memory
    char buf[256];
    std::string psi_path = root_prefix_ + "/proc/pressure/memory";
    float some_avg10 = -1.0f;
    if (read_sysfs_buf(psi_path.c_str(), buf, sizeof(buf))) {
        float v = 0.0f;
        if (std::sscanf(buf, "some avg10=%f", &v) == 1) {
            some_avg10 = std::clamp(v, 0.0f, 100.0f);
        }
    }
    std::lock_guard<std::mutex> lock(data_mutex_);
    hw_data_.mem_psi_some_pct = some_avg10;
}

void TelemetryCollector::poll_gpu() {
    std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
    if (gpu_backend_ == GpuBackendType::NvidiaNvml) {
        poll_nvidia_gpu();
    } else if (gpu_backend_ == GpuBackendType::AmdSysfs || gpu_backend_ == GpuBackendType::IntelSysfs) {
        poll_drm_sysfs_gpu();
    }
}

void TelemetryCollector::poll_nvidia_gpu() {
    if (!nvml_initialized_ || !nvml_device_) return;

    NvmlUtilization util{};
    NvmlMemory mem{};
    unsigned int temp_c = 0;
    unsigned int fan_pct = 0;
    unsigned int power_mw = 0;
    unsigned int gr_clock_mhz = 0;
    unsigned int mem_clock_mhz = 0;
    unsigned int pcie_tx_kb = 0;
    unsigned int pcie_rx_kb = 0;

    bool ok_util = pfn_nvmlDeviceGetUtilizationRates_ &&
                   (pfn_nvmlDeviceGetUtilizationRates_(nvml_device_, &util) == NVML_SUCCESS);
    bool ok_mem = pfn_nvmlDeviceGetMemoryInfo_ &&
                  (pfn_nvmlDeviceGetMemoryInfo_(nvml_device_, &mem) == NVML_SUCCESS);
    bool ok_temp = pfn_nvmlDeviceGetTemperature_ &&
                   (pfn_nvmlDeviceGetTemperature_(nvml_device_, NVML_TEMPERATURE_GPU, &temp_c) == NVML_SUCCESS);
    bool ok_fan = pfn_nvmlDeviceGetFanSpeed_ &&
                  (pfn_nvmlDeviceGetFanSpeed_(nvml_device_, &fan_pct) == NVML_SUCCESS);
    bool ok_pwr = pfn_nvmlDeviceGetPowerUsage_ &&
                  (pfn_nvmlDeviceGetPowerUsage_(nvml_device_, &power_mw) == NVML_SUCCESS);
    bool ok_gr_clk = pfn_nvmlDeviceGetClockInfo_ &&
                      (pfn_nvmlDeviceGetClockInfo_(nvml_device_, NVML_CLOCK_GRAPHICS, &gr_clock_mhz) == NVML_SUCCESS);
    bool ok_mem_clk = pfn_nvmlDeviceGetClockInfo_ &&
                      (pfn_nvmlDeviceGetClockInfo_(nvml_device_, NVML_CLOCK_MEM, &mem_clock_mhz) == NVML_SUCCESS);

    if (pfn_nvmlDeviceGetPcieThroughput_) {
        pfn_nvmlDeviceGetPcieThroughput_(nvml_device_, NVML_PCIE_UTIL_TX_BYTES, &pcie_tx_kb);
        pfn_nvmlDeviceGetPcieThroughput_(nvml_device_, NVML_PCIE_UTIL_RX_BYTES, &pcie_rx_kb);
    }

    float watts = static_cast<float>(power_mw) / 1000.0f;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        hw_data_.gpu_available = true;
        if (ok_util) {
            hw_data_.gpu_load_pct = static_cast<float>(util.gpu);
        }
        if (ok_gr_clk) {
            hw_data_.gpu_clock_mhz = static_cast<int>(gr_clock_mhz);
        }
        if (ok_mem) {
            hw_data_.gpu_vram_used_gb = static_cast<float>(mem.used) / (1024.0f * 1024.0f * 1024.0f);
            hw_data_.gpu_vram_total_gb = static_cast<float>(mem.total) / (1024.0f * 1024.0f * 1024.0f);
        }
        if (ok_mem_clk) {
            float max_bw_gbs = (static_cast<float>(mem_clock_mhz) * 2.0f * (static_cast<float>(gpu_bus_width_bits_) / 8.0f)) / 1000.0f;
            hw_data_.gpu_max_bandwidth_gbs = max_bw_gbs;
            float used_vram_bw = max_bw_gbs * (ok_util ? (static_cast<float>(util.memory) / 100.0f) : 0.0f);
            float pcie_bw_gbs = static_cast<float>(pcie_tx_kb + pcie_rx_kb) / (1024.0f * 1024.0f);
            hw_data_.gpu_bandwidth_gbs = used_vram_bw + pcie_bw_gbs;
        }
        if (ok_temp) {
            hw_data_.gpu_temp_c = static_cast<int>(temp_c);
        }
        if (ok_fan) {
            hw_data_.gpu_fan_pct = static_cast<int>(fan_pct);
        }
        if (ok_pwr) {
            hw_data_.gpu_power_w = watts;
        }
    }

    if (bench_active_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> block(bench_mutex_);
        if (ok_temp && static_cast<int>(temp_c) > bench_max_gpu_temp_c_) {
            bench_max_gpu_temp_c_ = static_cast<int>(temp_c);
        }
        if (ok_pwr && watts > bench_max_gpu_power_w_) {
            bench_max_gpu_power_w_ = watts;
        }
    }
}

void TelemetryCollector::poll_drm_sysfs_gpu() {
    char buf[64];
    float load_pct = -1.0f;
    int clock_mhz = 0;
    float vram_used_gb = 0.0f;
    float vram_total_gb = 0.0f;
    float bw_gbs = 0.0f;
    float max_bw_gbs = 0.0f;
    int temp_c = 0;
    int fan_pct = 0;
    float watts = 0.0f;

    // 1. Clock frequency
    if (!drm_sclk_freq_path_.empty() && read_sysfs_buf(drm_sclk_freq_path_.c_str(), buf, sizeof(buf))) {
        uint64_t raw = std::strtoull(buf, nullptr, 10);
        clock_mhz = drm_sclk_in_hz_ ? static_cast<int>((raw + 500000) / 1000000) : static_cast<int>(raw);
    } else if (!drm_pp_dpm_sclk_path_.empty()) {
        if (FILE* fp = std::fopen(drm_pp_dpm_sclk_path_.c_str(), "r")) {
            char line_buf[128];
            while (std::fgets(line_buf, sizeof(line_buf), fp)) {
                std::string line(line_buf);
                if (line.find('*') != std::string::npos) {
                    auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        clock_mhz = static_cast<int>(std::strtol(line.c_str() + colon + 1, nullptr, 10));
                        break;
                    }
                }
            }
            std::fclose(fp);
        }
    }

    // 2. GPU Utilization / Load
    if (!drm_gpu_busy_path_.empty() && read_sysfs_buf(drm_gpu_busy_path_.c_str(), buf, sizeof(buf))) {
        load_pct = std::strtof(buf, nullptr);
    }

    // Intel i915 / xe fallback utilization
    if (load_pct < 0.0f) {
        // Fallback 1: Parse /proc/self/fdinfo for drm-engine-render or drm-cycles-rcs (zero runtime heap)
        int dfd = open("/proc/self/fdinfo", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            uint64_t total_render_ns = 0;
            bool found_drm_fd = false;
            char d_buf[512];
            long nread = 0;
            while ((nread = syscall(SYS_getdents64, dfd, d_buf, sizeof(d_buf))) > 0) {
                for (long bpos = 0; bpos < nread; ) {
                    auto* d = reinterpret_cast<const LinuxDirent64*>(d_buf + bpos);
                    if (d->d_name[0] != '.') {
                        char fd_path[64];
                        std::snprintf(fd_path, sizeof(fd_path), "/proc/self/fdinfo/%s", d->d_name);
                        int fd = open(fd_path, O_RDONLY | O_CLOEXEC);
                        if (fd >= 0) {
                            char fdbuf[512];
                            ssize_t rn = read(fd, fdbuf, sizeof(fdbuf) - 1);
                            close(fd);
                            if (rn > 0) {
                                fdbuf[rn] = '\0';
                                const char* tag = std::strstr(fdbuf, "drm-engine-render:");
                                if (tag) {
                                    uint64_t ns = std::strtoull(tag + 18, nullptr, 10);
                                    total_render_ns += ns;
                                    found_drm_fd = true;
                                } else if ((tag = std::strstr(fdbuf, "drm-cycles-rcs:"))) {
                                    uint64_t ns = std::strtoull(tag + 15, nullptr, 10);
                                    total_render_ns += ns;
                                    found_drm_fd = true;
                                }
                            }
                        }
                    }
                    bpos += d->d_reclen;
                }
            }
            close(dfd);

            if (found_drm_fd) {
                auto now = std::chrono::steady_clock::now();
                if (prev_drm_render_time_.time_since_epoch().count() != 0 && total_render_ns >= prev_drm_render_ns_) {
                    auto dt_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - prev_drm_render_time_).count();
                    if (dt_ns > 1000000) {
                        load_pct = std::clamp(static_cast<float>((total_render_ns - prev_drm_render_ns_) * 100.0 / dt_ns), 0.0f, 100.0f);
                    }
                }
                prev_drm_render_ns_ = total_render_ns;
                prev_drm_render_time_ = now;
            }
        }
    }

    if (load_pct < 0.0f) {
        // Fallback 2: Frequency ratio (clock_mhz * 100) / max_mhz
        if (!drm_max_freq_path_.empty() && read_sysfs_buf(drm_max_freq_path_.c_str(), buf, sizeof(buf))) {
            int max_mhz = static_cast<int>(std::strtol(buf, nullptr, 10));
            if (max_mhz > 0 && clock_mhz > 0) {
                load_pct = std::clamp((static_cast<float>(clock_mhz) * 100.0f) / static_cast<float>(max_mhz), 0.0f, 100.0f);
            }
        }
        if (load_pct < 0.0f) load_pct = 0.0f;
    }

    // 3. VRAM and GTT Shared Memory
    if (!drm_vram_used_path_.empty() && read_sysfs_buf(drm_vram_used_path_.c_str(), buf, sizeof(buf))) {
        uint64_t bytes = std::strtoull(buf, nullptr, 10);
        vram_used_gb = static_cast<float>(bytes) / (1024.0f * 1024.0f * 1024.0f);
    }
    if (!drm_vram_total_path_.empty() && read_sysfs_buf(drm_vram_total_path_.c_str(), buf, sizeof(buf))) {
        uint64_t bytes = std::strtoull(buf, nullptr, 10);
        vram_total_gb = static_cast<float>(bytes) / (1024.0f * 1024.0f * 1024.0f);
    }

    // Fallback / combined shared GTT memory for AMD APUs & Steam Deck
    bool is_apu_gtt = (vram_total_gb <= 1.0f) ||
                      (!drm_gtt_used_path_.empty() && drm_mclk_freq_path_.empty() && vram_total_gb <= 8.5f);
    if (is_apu_gtt && !drm_gtt_used_path_.empty() && read_sysfs_buf(drm_gtt_used_path_.c_str(), buf, sizeof(buf))) {
        uint64_t gtt_used = std::strtoull(buf, nullptr, 10);
        vram_used_gb += static_cast<float>(gtt_used) / (1024.0f * 1024.0f * 1024.0f);
        if (!drm_gtt_total_path_.empty() && read_sysfs_buf(drm_gtt_total_path_.c_str(), buf, sizeof(buf))) {
            uint64_t gtt_total = std::strtoull(buf, nullptr, 10);
            vram_total_gb += static_cast<float>(gtt_total) / (1024.0f * 1024.0f * 1024.0f);
        }
    }

    // Intel iGPU without lmem or Nouveau fallback
    if (vram_total_gb <= 0.0f) {
        uint64_t api_tot = api_vram_total_bytes_.load(std::memory_order_relaxed);
        if (api_tot > 0) {
            uint64_t api_usd = api_vram_used_bytes_.load(std::memory_order_relaxed);
            vram_total_gb = static_cast<float>(api_tot) / (1024.0f * 1024.0f * 1024.0f);
            vram_used_gb = static_cast<float>(api_usd) / (1024.0f * 1024.0f * 1024.0f);
        } else {
            float sys_ram_total = 0.0f;
            float sys_game_ram = 0.0f;
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                sys_ram_total = hw_data_.ram_total_gb;
                sys_game_ram = hw_data_.game_ram_gb;
            }
            if (sys_ram_total <= 0.0f) {
                std::string meminfo_path = root_prefix_ + "/proc/meminfo";
                if (FILE* f = std::fopen(meminfo_path.c_str(), "r")) {
                    char mbuf[128];
                    while (std::fgets(mbuf, sizeof(mbuf), f)) {
                        unsigned long long kb = 0;
                        if (std::sscanf(mbuf, "MemTotal: %llu kB", &kb) == 1) {
                            sys_ram_total = static_cast<float>(kb) / (1024.0f * 1024.0f);
                            break;
                        }
                    }
                    std::fclose(f);
                }
            }
            if (sys_ram_total > 0.0f) {
                vram_total_gb = sys_ram_total * 0.5f;
                vram_used_gb = std::min(sys_game_ram > 0.0f ? sys_game_ram : 0.0f, vram_total_gb);
            }
        }
    }

    // 4. Memory Bandwidth (GB/s)
    int mclk_mhz = 0;
    if (!drm_mclk_freq_path_.empty() && read_sysfs_buf(drm_mclk_freq_path_.c_str(), buf, sizeof(buf))) {
        uint64_t raw_hz = std::strtoull(buf, nullptr, 10);
        mclk_mhz = static_cast<int>((raw_hz + 500000) / 1000000);
    }
    float mem_util_ratio = load_pct / 100.0f;
    if (!drm_mem_busy_path_.empty() && read_sysfs_buf(drm_mem_busy_path_.c_str(), buf, sizeof(buf))) {
        mem_util_ratio = std::clamp(std::strtof(buf, nullptr) / 100.0f, 0.0f, 1.0f);
    }
    if (mclk_mhz > 0) {
        float bus_bytes = (vram_total_gb >= 11.5f) ? 32.0f : ((vram_total_gb >= 3.5f) ? 16.0f : 8.0f);
        max_bw_gbs = (static_cast<float>(mclk_mhz) * 2.0f * bus_bytes) / 1000.0f;
        bw_gbs = max_bw_gbs * mem_util_ratio;
    } else if (clock_mhz > 0) {
        float bus_bytes = (vram_total_gb >= 7.5f) ? 32.0f : 16.0f;
        max_bw_gbs = (static_cast<float>(clock_mhz) * bus_bytes) / 1000.0f;
        bw_gbs = max_bw_gbs * mem_util_ratio;
    }

    // 5. Temperature (with RDNA Hotspot / Junction & Edge Fallback)
    if (!drm_temp_path_.empty() && read_sysfs_buf(drm_temp_path_.c_str(), buf, sizeof(buf))) {
        long millideg = std::strtol(buf, nullptr, 10);
        if (millideg > 0 && millideg < 130000) {
            temp_c = static_cast<int>((millideg + 500) / 1000);
        } else if (!drm_temp_fallback_path_.empty() &&
                   read_sysfs_buf(drm_temp_fallback_path_.c_str(), buf, sizeof(buf))) {
            long millideg_fb = std::strtol(buf, nullptr, 10);
            if (millideg_fb > 0 && millideg_fb < 130000) {
                temp_c = static_cast<int>((millideg_fb + 500) / 1000);
            }
        }
    } else if (!drm_temp_fallback_path_.empty() &&
               read_sysfs_buf(drm_temp_fallback_path_.c_str(), buf, sizeof(buf))) {
        long millideg_fb = std::strtol(buf, nullptr, 10);
        if (millideg_fb > 0 && millideg_fb < 130000) {
            temp_c = static_cast<int>((millideg_fb + 500) / 1000);
        }
    }

    if (!drm_fan_pwm_path_.empty() && read_sysfs_buf(drm_fan_pwm_path_.c_str(), buf, sizeof(buf))) {
        long pwm = std::strtol(buf, nullptr, 10);
        fan_pct = std::clamp(static_cast<int>((pwm * 100 + 127) / 255), 0, 100);
    } else if (!drm_fan_rpm_path_.empty() && !drm_fan_max_path_.empty() &&
               read_sysfs_buf(drm_fan_rpm_path_.c_str(), buf, sizeof(buf))) {
        long rpm = std::strtol(buf, nullptr, 10);
        if (read_sysfs_buf(drm_fan_max_path_.c_str(), buf, sizeof(buf))) {
            long max_rpm = std::strtol(buf, nullptr, 10);
            if (max_rpm > 0) fan_pct = std::clamp(static_cast<int>((rpm * 100) / max_rpm), 0, 100);
        }
    }

    if (!drm_power_uw_path_.empty() && read_sysfs_buf(drm_power_uw_path_.c_str(), buf, sizeof(buf))) {
        uint64_t uw = std::strtoull(buf, nullptr, 10);
        watts = static_cast<float>(uw) / 1e6f;
    } else if (!drm_energy_uj_path_.empty() && read_sysfs_buf(drm_energy_uj_path_.c_str(), buf, sizeof(buf))) {
        uint64_t curr_uj = std::strtoull(buf, nullptr, 10);
        auto now = std::chrono::steady_clock::now();
        if (prev_gpu_energy_time_.time_since_epoch().count() != 0 && curr_uj >= prev_gpu_energy_uj_) {
            double dt_sec = std::chrono::duration<double>(now - prev_gpu_energy_time_).count();
            if (dt_sec > 0.01) {
                watts = static_cast<float>((static_cast<double>(curr_uj - prev_gpu_energy_uj_) / 1e6) / dt_sec);
            }
        }
        prev_gpu_energy_uj_ = curr_uj;
        prev_gpu_energy_time_ = now;
    }

    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        hw_data_.gpu_available = true;
        hw_data_.gpu_load_pct = load_pct;
        hw_data_.gpu_clock_mhz = clock_mhz;
        hw_data_.gpu_vram_used_gb = vram_used_gb;
        hw_data_.gpu_vram_total_gb = vram_total_gb;
        hw_data_.gpu_bandwidth_gbs = bw_gbs;
        hw_data_.gpu_max_bandwidth_gbs = max_bw_gbs;
        // On integrated GPUs (same silicon die as CPU), fallback to CPU die temperature if GPU hwmon temp is absent
        hw_data_.gpu_temp_c = (temp_c > 0) ? temp_c : hw_data_.cpu_temp_c;
        hw_data_.gpu_fan_pct = fan_pct;
        hw_data_.gpu_power_w = watts;
    }

    if (bench_active_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> block(bench_mutex_);
        int eff_temp = (temp_c > 0) ? temp_c : hw_data_.cpu_temp_c;
        if (eff_temp > bench_max_gpu_temp_c_) bench_max_gpu_temp_c_ = eff_temp;
        if (watts > bench_max_gpu_power_w_) bench_max_gpu_power_w_ = watts;
    }
}

TelemetryCollector& get_telemetry() {
    static TelemetryCollector instance;
    return instance;
}

} // namespace fpshud