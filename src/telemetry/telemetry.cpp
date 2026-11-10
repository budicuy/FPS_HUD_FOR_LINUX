#include "telemetry.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
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

namespace fs = std::filesystem;

namespace fpshud {

TelemetryCollector::TelemetryCollector()
    : cpu_provider_(""), ram_provider_(""), system_provider_("") {
  init_sensors();
  poll_once_sync();
  running_.store(true, std::memory_order_relaxed);
  worker_thread_ = std::thread(&TelemetryCollector::worker_loop, this);
}

TelemetryCollector::TelemetryCollector(std::string mock_root_prefix, bool disable_nvml,
                                       bool disable_level_zero)
    : root_prefix_(std::move(mock_root_prefix)),
      disable_nvml_(disable_nvml),
      disable_level_zero_(disable_level_zero),
      cpu_provider_(root_prefix_),
      ram_provider_(root_prefix_),
      system_provider_(root_prefix_) {
  init_sensors();
  poll_once_sync();
}

void TelemetryCollector::inject_mock_level_zero(const LevelZeroDispatch& dispatch) noexcept {
  mock_ze_dispatch_ = dispatch;
  has_mock_ze_ = true;
  std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
  shutdown_sensors_locked();
  init_gpu_sensors_locked();
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
  system_provider_.init();

  // Prime initial counters for delta metrics
  poll_cpu_load();
  poll_cpu_clock();
  poll_cpu_power();
  poll_battery();
  poll_disk_temp();
  poll_local_clock();
}

void TelemetryCollector::init_cpu_sensors() {
  cpu_provider_.init();
  ram_provider_.init();
  cpu_provider_.poll(staging_hw_data_);
}

void TelemetryCollector::init_gpu_sensors() {
  std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
  init_gpu_sensors_locked();
}

void TelemetryCollector::init_gpu_sensors_locked() {
  shutdown_sensors_locked();

  // 1. Try NVIDIA NVML first unless disabled
  if (!disable_nvml_) {
    if (nvidia_provider_.init(root_prefix_, bound_vendor_id_, bound_device_id_)) {
      gpu_backend_ = GpuBackendType::NvidiaNvml;
      return;
    }
  }

  // 2. Try Universal Linux DRM sysfs
  bool drm_ok = drm_provider_.init(root_prefix_, bound_vendor_id_, bound_device_id_);
  if (drm_ok) {
    gpu_backend_ = drm_provider_.get_backend_type();

    // Upgrade Intel GPU to Level Zero Sysman if available
    if (drm_provider_.detected_vendor_id() == 0x8086 && !disable_level_zero_) {
      const LevelZeroDispatch* mock_ptr = has_mock_ze_ ? &mock_ze_dispatch_ : nullptr;
      if (levelzero_provider_.init(root_prefix_, mock_ptr, bound_device_id_)) {
        gpu_backend_ = GpuBackendType::IntelLevelZero;
      }
    }
    return;
  }

  // Fallback if DRM not detected but Level Zero is injected/available
  if (!disable_level_zero_ && has_mock_ze_) {
    if (levelzero_provider_.init(root_prefix_, &mock_ze_dispatch_, bound_device_id_)) {
      gpu_backend_ = GpuBackendType::IntelLevelZero;
      return;
    }
  }

  gpu_backend_ = GpuBackendType::None;
}

void TelemetryCollector::shutdown_sensors() {
  std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
  shutdown_sensors_locked();
}

void TelemetryCollector::shutdown_sensors_locked() {
  nvidia_provider_.shutdown();
  drm_provider_.shutdown();
  levelzero_provider_.shutdown();
  gpu_backend_ = GpuBackendType::None;
}

void TelemetryCollector::on_frame() {
  auto now = std::chrono::steady_clock::now();
  if (last_frame_time_.time_since_epoch().count() == 0) {
    last_frame_time_ = now;
    return;
  }

  float dt_ms = std::chrono::duration<float, std::milli>(now - last_frame_time_).count();
  last_frame_time_ = now;
  on_simulated_frame_dt(dt_ms, false);
}

void TelemetryCollector::on_simulated_frame_dt(float dt_ms, bool force_display_update) {
  frametime_analyzer_.on_frame(dt_ms, force_display_update);

  if (benchmark_recorder_.is_active()) {
    uint32_t cur_idx = active_hw_buf_.load(std::memory_order_acquire);
    benchmark_recorder_.on_frame(dt_ms, hw_buffers_[cur_idx]);
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
    if (first_frame_time_.time_since_epoch().count() == 0) {
      first_frame_time_ = now;
    }

    if (!bench_started_ && !bench_finished_) {
      double warmup = std::chrono::duration<double>(now - first_frame_time_).count();
      if (warmup >= 1.0) {
        start_benchmark();
        bench_started_ = true;
        std::fprintf(stderr, "[FPSHUD] Automated Benchmark started (duration: %d seconds)...\n",
                     s_auto_bench_secs);
      }
    } else if (bench_started_ && !bench_finished_) {
      float elapsed = benchmark_recorder_.get_elapsed_sec();
      if (elapsed >= static_cast<float>(s_auto_bench_secs)) {
        stop_benchmark();
        bench_finished_ = true;
        std::fprintf(stderr, "[FPSHUD] Automated Benchmark complete! Results saved to CSV.\n");
        if (const char* exit_env = std::getenv("FPSHUD_BENCHMARK_EXIT");
            exit_env && std::atoi(exit_env) != 0) {
          std::exit(0);
        }
      }
    }
  }
}

void TelemetryCollector::start_benchmark() { benchmark_recorder_.start(); }

void TelemetryCollector::stop_benchmark() { benchmark_recorder_.stop(); }

void TelemetryCollector::toggle_benchmark() { benchmark_recorder_.toggle(); }

namespace {

bool contains_icase(std::string_view haystack, std::string_view needle) noexcept {
  if (needle.empty()) return false;
  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                        [](char ch1, char ch2) {
                          return std::tolower(static_cast<unsigned char>(ch1)) ==
                                 std::tolower(static_cast<unsigned char>(ch2));
                        });
  return it != haystack.end();
}

}  // namespace

const char* TelemetryCollector::classify_maps_line(std::string_view line) noexcept {
  if (contains_icase(line, "vkd3d") || contains_icase(line, "d3d12.dll") ||
      contains_icase(line, "d3d12core.dll")) {
    return "DX12";
  }
  // Check DX9 before generic DXVK / DX11 so /path/to/dxvk/d3d9.dll correctly identifies as DX9
  if (contains_icase(line, "d3d9.dll")) {
    return "DX9";
  }
  if (contains_icase(line, "dxvk") || contains_icase(line, "d3d11.dll")) {
    return "DX11";
  }
  return "";
}

const char* detect_effective_render_api(const char* base_api, const char* maps_path) noexcept {
  if (!base_api || !*base_api) return "VULKAN";
  if (std::strcmp(base_api, "VULKAN") != 0) return base_api;
  if (!maps_path || !*maps_path) return "VULKAN";

  FILE* fp = std::fopen(maps_path, "re");
  if (!fp) return "VULKAN";

  bool has_dx12 = false;
  bool has_dx11 = false;
  bool has_dx9 = false;
  char line[1024];
  while (std::fgets(line, sizeof(line), fp)) {
    const char* classified = TelemetryCollector::classify_maps_line(line);
    if (std::strcmp(classified, "DX12") == 0) {
      has_dx12 = true;
      break;
    }
    if (std::strcmp(classified, "DX9") == 0) {
      has_dx9 = true;
      break;
    }
    if (std::strcmp(classified, "DX11") == 0) {
      has_dx11 = true;
      break;
    }
  }
  std::fclose(fp);

  if (has_dx12) return "DX12";
  if (has_dx11) return "DX11";
  if (has_dx9) return "DX9";
  return "VULKAN";
}

TelemetryData TelemetryCollector::get_snapshot() const {
  TelemetryData out;
  {
    uint32_t idx = active_hw_buf_.load(std::memory_order_acquire);
    out = hw_buffers_[idx];
  }
  const char* base_api = active_api_.load(std::memory_order_relaxed);
  const char* trans_api = translated_api_.load(std::memory_order_relaxed);
  if (base_api && std::strcmp(base_api, "VULKAN") == 0 && trans_api) {
    out.render_api = trans_api;
  } else {
    out.render_api = base_api ? base_api : "VULKAN";
  }
  out.fps = frametime_analyzer_.get_fps();
  out.fps_avg = frametime_analyzer_.get_fps_avg();
  out.fps_1pct_low = frametime_analyzer_.get_1pct_low();
  out.fps_01pct_low = frametime_analyzer_.get_01pct_low();
  out.frametime_ms = frametime_analyzer_.get_frametime_ms();

  out.benchmark_active = benchmark_recorder_.is_active();
  out.benchmark_elapsed_sec = benchmark_recorder_.get_elapsed_sec();
  if (benchmark_recorder_.is_active() || benchmark_recorder_.has_result()) {
    out.last_benchmark = benchmark_recorder_.get_summary();
  }
  return out;
}

void TelemetryCollector::poll_once_sync() {
  std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
  poll_cpu_load();
  poll_cpu_clock();
  poll_cpu_temp();
  poll_cpu_power();
  poll_ram();
  poll_game_ram();
  poll_mem_pressure();
  poll_gpu();
  poll_battery();
  poll_disk_io();
  poll_disk_temp();
  poll_net_speed();
  poll_local_clock();

  benchmark_recorder_.update_peaks(staging_hw_data_.cpu_temp_c, staging_hw_data_.cpu_power_w,
                                   staging_hw_data_.gpu_temp_c, staging_hw_data_.gpu_power_w);

  uint32_t next_idx = 1 - active_hw_buf_.load(std::memory_order_relaxed);
  hw_buffers_[next_idx] = staging_hw_data_;
  active_hw_buf_.store(next_idx, std::memory_order_release);
}

void TelemetryCollector::notify_active_gpu(uint32_t vendor_id, uint32_t device_id) {
  std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
  if (bound_vendor_id_ == vendor_id && bound_device_id_ == device_id &&
      gpu_backend_ != GpuBackendType::None) {
    return;
  }
  bound_vendor_id_ = vendor_id;
  bound_device_id_ = device_id;
  shutdown_sensors_locked();

  // Clear stale GPU telemetry fields before probing/polling newly bound GPU
  staging_hw_data_.gpu_load_pct = 0.0f;
  staging_hw_data_.gpu_clock_mhz = 0;
  staging_hw_data_.gpu_vram_clock_mhz = 0;
  staging_hw_data_.gpu_temp_c = 0;
  staging_hw_data_.gpu_power_w = 0.0f;
  staging_hw_data_.gpu_vram_used_gb = 0.0f;
  staging_hw_data_.gpu_vram_total_gb = 0.0f;
  staging_hw_data_.gpu_fan_pct = 0;
  staging_hw_data_.gpu_bandwidth_gbs = 0.0f;
  staging_hw_data_.gpu_available = false;

  init_gpu_sensors_locked();
  if (gpu_backend_ == GpuBackendType::NvidiaNvml) {
    poll_nvidia_gpu();
  } else if (gpu_backend_ == GpuBackendType::IntelLevelZero) {
    poll_intel_level_zero_gpu();
  } else if (gpu_backend_ == GpuBackendType::AmdSysfs ||
             gpu_backend_ == GpuBackendType::IntelSysfs) {
    poll_drm_sysfs_gpu();
  }
}

void TelemetryCollector::update_api_vram_info(uint64_t used_bytes, uint64_t total_bytes) noexcept {
  drm_provider_.update_api_vram_info(used_bytes, total_bytes);
  levelzero_provider_.update_api_vram_info(used_bytes, total_bytes);
}

void TelemetryCollector::worker_loop() {
#if defined(__linux__)
  pthread_setname_np(pthread_self(), "fpshud_telem");
  // Set nice +10 to guarantee game engine and render threads take scheduling priority
  setpriority(PRIO_PROCESS, 0, 10);
#endif
  while (running_.load(std::memory_order_relaxed)) {
    poll_once_sync();
    for (int i = 0; i < 10 && running_.load(std::memory_order_relaxed); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

void TelemetryCollector::poll_local_clock() {
  system_provider_.poll_local_clock(staging_hw_data_);

  if (!translated_api_cached_.load(std::memory_order_relaxed)) {
    std::string maps_path =
        root_prefix_.empty() ? "/proc/self/maps" : (root_prefix_ + "/proc/self/maps");
    const char* detected = detect_effective_render_api("VULKAN", maps_path.c_str());
    if (detected && std::strcmp(detected, "VULKAN") != 0) {
      translated_api_.store(detected, std::memory_order_relaxed);
      translated_api_cached_.store(true, std::memory_order_relaxed);
    } else {
      translated_api_.store("VULKAN", std::memory_order_relaxed);
      if (++map_scan_attempts_ >= 3) {
        // After 3 initial attempts (~1.5s), permanently freeze and stop reading /proc/self/maps
        translated_api_cached_.store(true, std::memory_order_relaxed);
      }
    }
  }
}

void TelemetryCollector::poll_battery() { system_provider_.poll_battery(staging_hw_data_); }

void TelemetryCollector::poll_disk_temp() { system_provider_.poll_disk_temp(staging_hw_data_); }

void TelemetryCollector::poll_disk_io() { system_provider_.poll_disk_io(staging_hw_data_); }

void TelemetryCollector::poll_net_speed() { system_provider_.poll_network(staging_hw_data_); }

void TelemetryCollector::poll_cpu_load() { cpu_provider_.poll_cpu_load(staging_hw_data_); }

void TelemetryCollector::poll_cpu_clock() { cpu_provider_.poll_cpu_clock(staging_hw_data_); }

void TelemetryCollector::poll_cpu_temp() { cpu_provider_.poll_cpu_temp(staging_hw_data_); }

void TelemetryCollector::poll_cpu_power() { cpu_provider_.poll_cpu_power(staging_hw_data_); }

void TelemetryCollector::poll_ram() { ram_provider_.poll_ram(staging_hw_data_); }

void TelemetryCollector::poll_game_ram() { ram_provider_.poll_game_ram(staging_hw_data_); }

void TelemetryCollector::poll_mem_pressure() { ram_provider_.poll_mem_pressure(staging_hw_data_); }

void TelemetryCollector::poll_gpu() {
  if (gpu_backend_ == GpuBackendType::NvidiaNvml) {
    poll_nvidia_gpu();
  } else if (gpu_backend_ == GpuBackendType::IntelLevelZero) {
    poll_intel_level_zero_gpu();
  } else if (gpu_backend_ == GpuBackendType::AmdSysfs ||
             gpu_backend_ == GpuBackendType::IntelSysfs) {
    poll_drm_sysfs_gpu();
  }
}

void TelemetryCollector::poll_nvidia_gpu() { nvidia_provider_.poll(staging_hw_data_); }

void TelemetryCollector::poll_intel_level_zero_gpu() { levelzero_provider_.poll(staging_hw_data_); }

void TelemetryCollector::poll_drm_sysfs_gpu() { drm_provider_.poll(staging_hw_data_); }

TelemetryCollector& get_telemetry() {
  static TelemetryCollector instance;
  return instance;
}

}  // namespace fpshud
