#pragma once

#include <inplace_vector>

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fpshud {

enum class GpuBackendType { None = 0, NvidiaNvml, AmdSysfs, IntelSysfs, IntelLevelZero };

namespace intel_ze {

constexpr int ZE_RESULT_SUCCESS = 0;

struct zes_device_properties_t {
  uint32_t stype = 0;
  void* pNext = nullptr;
  uint32_t type = 0;
  uint32_t flags = 0;
  uint32_t deviceId = 0;
  uint32_t vendorId = 0;
  char name[256] = {0};
  char serialNumber[32] = {0};
  char boardNumber[32] = {0};
  char brandName[256] = {0};
  char modelName[256] = {0};
  char vendorName[256] = {0};
  char driverVersion[256] = {0};
};

struct zes_freq_state_t {
  uint32_t stype = 0;
  const void* pNext = nullptr;
  double currentVal = 0.0;
  double request = 0.0;
  double tdp = 0.0;
  double efficient = 0.0;
  double actual = 0.0;
  uint32_t throttleReasons = 0;
};

struct zes_mem_state_t {
  uint32_t stype = 0;
  const void* pNext = nullptr;
  uint32_t health = 0;
  uint64_t free = 0;
  uint64_t size = 0;
};

struct zes_power_energy_counter_t {
  uint64_t energy = 0;     // in microjoules
  uint64_t timestamp = 0;  // in microseconds
};

struct zes_engine_stats_t {
  uint64_t activeTime = 0;  // in microseconds
  uint64_t timestamp = 0;   // in microseconds
};

}  // namespace intel_ze

struct LevelZeroDispatch {
  int (*zeInit)(uint32_t flags) = nullptr;
  int (*zesInit)(uint32_t flags) = nullptr;
  int (*zesDriverGet)(uint32_t* count, void** phDrivers) = nullptr;
  int (*zesDeviceGet)(void* hDriver, uint32_t* count, void** phDevices) = nullptr;
  int (*zesDeviceGetProperties)(void* hDevice,
                                intel_ze::zes_device_properties_t* pProperties) = nullptr;
  int (*zesDeviceEnumFrequencyDomains)(void* hDevice, uint32_t* count,
                                       void** phFrequency) = nullptr;
  int (*zesFrequencyGetState)(void* hFrequency, intel_ze::zes_freq_state_t* pState) = nullptr;
  int (*zesDeviceEnumTemperatureSensors)(void* hDevice, uint32_t* count,
                                         void** phTemperature) = nullptr;
  int (*zesTemperatureGetState)(void* hTemperature, double* pTemperature) = nullptr;
  int (*zesDeviceEnumPowerDomains)(void* hDevice, uint32_t* count, void** phPower) = nullptr;
  int (*zesPowerGetEnergyCounter)(void* hPower,
                                  intel_ze::zes_power_energy_counter_t* pEnergy) = nullptr;
  int (*zesDeviceEnumMemoryModules)(void* hDevice, uint32_t* count, void** phMemory) = nullptr;
  int (*zesMemoryGetState)(void* hMemory, intel_ze::zes_mem_state_t* pState) = nullptr;
  int (*zesDeviceEnumEngineGroups)(void* hDevice, uint32_t* count, void** phEngine) = nullptr;
  int (*zesEngineGetActivity)(void* hEngine, intel_ze::zes_engine_stats_t* pStats) = nullptr;
};

struct BenchmarkSummary {
  bool has_result = false;
  double duration_sec = 0.0;
  float avg_fps = 0.0f;
  float fps_1pct_low = 0.0f;
  float fps_01pct_low = 0.0f;
  float min_fps = 0.0f;
  float max_fps = 0.0f;
  int max_gpu_temp_c = 0;
  float max_gpu_power_w = 0.0f;
  int max_cpu_temp_c = 0;
  float max_cpu_power_w = 0.0f;
  char saved_csv_path[128] = {0};
  std::chrono::steady_clock::time_point finished_time{};
};

struct SimdStatsResult {
  double total_ms = 0.0;
  float min_val = 0.0f;
  float max_val = 0.0f;
};

constexpr bool has_simd_frametime_backend() noexcept {
#if __has_include(<experimental/simd>)
  return true;
#else
  return false;
#endif
}

// Accelerated frametime analytics (sum, min, max) using C++26 SIMD reduction
[[nodiscard]] SimdStatsResult compute_frametime_stats_simd(const std::vector<float>& data);

// Trivially copyable snapshot struct (zero heap allocations per frame!)
struct TelemetryData {
  float fps = 0.0f;
  float fps_avg = 0.0f;
  float fps_1pct_low = 0.0f;
  float fps_01pct_low = 0.0f;
  float frametime_ms = 0.0f;

  float gpu_load_pct = 0.0f;
  int gpu_clock_mhz = 0;
  int gpu_vram_clock_mhz = 0;
  float gpu_vram_used_gb = 0.0f;
  float gpu_vram_total_gb = 0.0f;
  float gpu_bandwidth_gbs = 0.0f;
  float gpu_max_bandwidth_gbs = 0.0f;
  int gpu_temp_c = 0;
  int gpu_fan_pct = 0;
  float gpu_power_w = 0.0f;
  bool gpu_available = false;

  float cpu_load_pct = 0.0f;
  int cpu_clock_mhz = 0;
  int cpu_temp_c = 0;
  float cpu_power_w = 0.0f;

  // Per-core CPU load (%) and clock (MHz) for All-Core indicators (zero-heap fixed arrays)
  static constexpr size_t kMaxCpuCores = 64;
  int cpu_core_count = 0;
  std::array<uint8_t, kMaxCpuCores> cpu_core_loads_pct{};
  std::array<uint16_t, kMaxCpuCores> cpu_core_clocks_mhz{};

  float ram_used_gb = 0.0f;
  float ram_total_gb = 0.0f;
  float ram_used_pct = 0.0f;

  // Extended memory sensors (100% sudo-free via /proc)
  float game_ram_gb = 0.0f;    // Resident memory (VmRSS) of the hooked game process
  float swap_used_gb = 0.0f;   // Swap / ZRAM in use
  float swap_total_gb = 0.0f;  // 0 = no swap configured
  float mem_psi_some_pct =
      -1.0f;  // /proc/pressure/memory "some avg10" (-1 = kernel PSI unavailable)

  // Laptop & Handheld battery / power sensors
  bool battery_available = false;
  float battery_pct = 0.0f;
  float battery_power_w = 0.0f;
  bool battery_charging = false;
  char battery_status[16] = "Discharging";

  // Storage & Network sensors (100% sudo-free via /proc and /sys/class/hwmon)
  float disk_read_mbs = 0.0f;
  float disk_write_mbs = 0.0f;
  int disk_temp_c = 0;
  bool disk_temp_available = false;
  float net_rx_mbs = 0.0f;
  float net_tx_mbs = 0.0f;

  const char* render_api = "VULKAN";
  char cpu_name[48] = "CPU";
  char gpu_name[48] = "GPU";
  char gpu_vendor[16] = "Generic";
  char gpu_driver_version[32] = "N/A";
  char local_time_str[8] = "00:00";

  // Benchmark status
  bool benchmark_active = false;
  float benchmark_elapsed_sec = 0.0f;
  BenchmarkSummary last_benchmark{};
};

class TelemetryCollector {
public:
  static constexpr size_t kFrameWindowSize = 512;
  static constexpr size_t kGraphHistorySize = 120;
  static constexpr float kRollingWindowMaxMs = 3500.0f;  // 3.5-second real-time window

  TelemetryCollector();
  explicit TelemetryCollector(std::string mock_root_prefix, bool disable_nvml = false,
                              bool disable_level_zero = false);
  ~TelemetryCollector();

  TelemetryCollector(const TelemetryCollector&) = delete;
  TelemetryCollector& operator=(const TelemetryCollector&) = delete;

  [[nodiscard]] GpuBackendType get_gpu_backend() const noexcept { return gpu_backend_; }

  void inject_mock_level_zero(const LevelZeroDispatch& dispatch) noexcept;

  // Set active graphics API ("VULKAN" or "OPENGL") — lock-free pointer store
  void set_render_api(const char* api_name) noexcept {
    active_api_.store(api_name, std::memory_order_relaxed);
  }

  // Call once per presented frame on the render thread (fast, lock-free)
  void on_frame();

  // Inject a specific frame delta in milliseconds (used by deterministic CLI test suite)
  void on_simulated_frame_dt(float dt_ms, bool force_display_update = false);

  // Get latest snapshot for rendering (trivially copyable, zero heap allocation)
  TelemetryData get_snapshot() const;

  // Access 120-frame frametime/FPS history directly on render thread
  const float* get_ft_history() const noexcept { return graph_ft_ms_.data(); }
  const float* get_fps_history() const noexcept { return graph_fps_.data(); }
  int get_graph_offset() const noexcept { return graph_offset_; }

  // Benchmark Recording Controls (F9 or GUI Button)
  void start_benchmark();
  void stop_benchmark();
  void toggle_benchmark();
  bool is_benchmark_active() const noexcept {
    return bench_active_.load(std::memory_order_relaxed);
  }

  // Force an immediate synchronous poll (useful for CLI test)
  void poll_once_sync();

  // Active GPU binding & rendering API VRAM reporting
  void notify_active_gpu(uint32_t vendor_id, uint32_t device_id);
  void update_api_vram_info(uint64_t used_bytes, uint64_t total_bytes) noexcept;

private:
  void worker_loop();
  void init_sensors();
  void init_cpu_sensors();
  void init_gpu_sensors();
  void init_gpu_sensors_locked();
  bool init_intel_level_zero();
  void init_battery_sensors();
  void init_storage_sensors();
  void shutdown_sensors();
  void shutdown_sensors_locked();

  void poll_cpu_load();
  void poll_cpu_clock();
  void poll_cpu_temp();
  void poll_cpu_power();
  void poll_ram();
  void poll_game_ram();
  void poll_mem_pressure();
  void poll_gpu();
  void poll_battery();
  void poll_disk_io();
  void poll_disk_temp();
  void poll_net_speed();
  void poll_nvidia_gpu();
  void poll_intel_level_zero_gpu();
  void poll_drm_sysfs_gpu();
  void poll_local_clock();

  mutable std::mutex gpu_sensor_mutex_;
  GpuBackendType gpu_backend_ = GpuBackendType::None;
  std::string root_prefix_;
  bool disable_nvml_ = false;
  uint32_t bound_vendor_id_ = 0;
  uint32_t bound_device_id_ = 0;
  std::atomic<uint64_t> api_vram_used_bytes_{0};
  std::atomic<uint64_t> api_vram_total_bytes_{0};

  // Dynamic NVML state (NVIDIA GPUs)
  void* nvml_lib_ = nullptr;
  void* nvml_device_ = nullptr;
  bool nvml_initialized_ = false;
  unsigned int gpu_bus_width_bits_ = 192;

  int (*pfn_nvmlInit_v2_)() = nullptr;
  int (*pfn_nvmlShutdown_)() = nullptr;
  int (*pfn_nvmlSystemGetDriverVersion_)(char*, unsigned int) = nullptr;
  int (*pfn_nvmlDeviceGetCount_v2_)(unsigned int*) = nullptr;
  int (*pfn_nvmlDeviceGetHandleByIndex_v2_)(unsigned int, void**) = nullptr;
  int (*pfn_nvmlDeviceGetName_)(void*, char*, unsigned int) = nullptr;
  int (*pfn_nvmlDeviceGetUtilizationRates_)(void*, void*) = nullptr;
  int (*pfn_nvmlDeviceGetMemoryInfo_)(void*, void*) = nullptr;
  int (*pfn_nvmlDeviceGetTemperature_)(void*, int, unsigned int*) = nullptr;
  int (*pfn_nvmlDeviceGetFanSpeed_)(void*, unsigned int*) = nullptr;
  int (*pfn_nvmlDeviceGetPowerUsage_)(void*, unsigned int*) = nullptr;
  int (*pfn_nvmlDeviceGetClockInfo_)(void*, int, unsigned int*) = nullptr;
  int (*pfn_nvmlDeviceGetMemoryBusWidth_)(void*, unsigned int*) = nullptr;
  int (*pfn_nvmlDeviceGetPcieThroughput_)(void*, int, unsigned int*) = nullptr;

  // Intel Level Zero / Sysman state & function pointers
  bool disable_level_zero_ = false;
  bool has_mock_ze_ = false;
  LevelZeroDispatch mock_ze_dispatch_{};
  void* ze_lib_ = nullptr;
  void* ze_driver_ = nullptr;
  void* ze_device_ = nullptr;
  void* ze_freq_ = nullptr;
  void* ze_temp_ = nullptr;
  void* ze_pwr_ = nullptr;
  void* ze_mem_ = nullptr;
  void* ze_eng_ = nullptr;
  LevelZeroDispatch ze_dispatch_{};
  uint64_t prev_ze_eng_active_ = 0;
  uint64_t prev_ze_eng_ts_ = 0;
  uint64_t prev_ze_pwr_energy_ = 0;
  uint64_t prev_ze_pwr_ts_ = 0;

  // Universal DRM / sysfs GPU paths (AMD amdgpu & Intel i915 / xe)
  std::string drm_gpu_busy_path_;
  std::string drm_vram_used_path_;
  std::string drm_vram_total_path_;
  std::string drm_gtt_used_path_;     // AMD APU shared GTT memory used
  std::string drm_gtt_total_path_;    // AMD APU shared GTT memory total
  std::string drm_mem_busy_path_;     // AMD mem_busy_percent (0..100)
  std::string drm_sclk_freq_path_;    // hwmon/freq1_input (Hz) or gt_cur_freq_mhz (MHz)
  std::string drm_mclk_freq_path_;    // hwmon/freq2_input (Hz, AMD VRAM clock)
  std::string drm_max_freq_path_;     // gt_max_freq_mhz or rps_max_freq_mhz (Intel i915)
  std::string drm_pp_dpm_sclk_path_;  // AMD fallback: device/pp_dpm_sclk ("1: 2400Mhz *")
  std::string drm_pp_dpm_mclk_path_;  // AMD fallback: device/pp_dpm_mclk ("1: 1200Mhz *")
  bool drm_sclk_in_hz_ = true;
  std::string drm_temp_path_;           // hwmon/temp1_input (millidegrees C)
  std::string drm_temp_fallback_path_;  // fallback edge temp if junction temp2_input used
  std::string drm_fan_pwm_path_;        // hwmon/pwm1 (0..255)
  std::string drm_fan_rpm_path_;        // hwmon/fan1_input
  std::string drm_fan_max_path_;        // hwmon/fan1_max
  std::string drm_power_uw_path_;       // hwmon/power1_average or power1_input (microwatts)
  std::string drm_energy_uj_path_;      // hwmon/energy1_input (microjoules)

  // CPU Sensor descriptors discovered at init (Intel & AMD, zero-heap trivial inline storage)
  struct CpuFreqSensor {
    uint16_t cpu_id = 0;
    bool use_cpuinfo = false;
  };
  std::inplace_vector<CpuFreqSensor, 512> cpu_freq_sensors_;
  std::string cpu_temp_path_;
  std::string rapl_energy_path_;
  uint64_t rapl_max_range_uj_ = 0;
  float cpu_tdp_watts_ = 65.0f;
  int cpu_max_freq_mhz_ = 0;
  std::string cpu_hwmon_power_uw_path_;   // Fallback direct microwatt sensor (AMD zenpower/k10temp)
  std::string cpu_hwmon_energy_uj_path_;  // Fallback microjoule counter (amd_energy)

  // Battery paths
  std::string battery_capacity_path_;
  std::string battery_status_path_;
  std::string battery_power_uw_path_;
  std::string battery_current_ua_path_;
  std::string battery_voltage_uv_path_;
  bool battery_discovered_ = false;

  // Storage & Network state
  std::string disk_temp_path_;
  uint64_t prev_disk_read_sectors_ = 0;
  uint64_t prev_disk_write_sectors_ = 0;
  std::chrono::steady_clock::time_point prev_disk_poll_time_{};
  uint64_t prev_net_rx_bytes_ = 0;
  uint64_t prev_net_tx_bytes_ = 0;
  std::chrono::steady_clock::time_point prev_net_poll_time_{};

  // Previous counters for delta calculation
  uint64_t prev_cpu_total_ = 0;
  uint64_t prev_cpu_idle_ = 0;
  std::array<uint64_t, TelemetryData::kMaxCpuCores> prev_core_total_{};
  std::array<uint64_t, TelemetryData::kMaxCpuCores> prev_core_idle_{};
  uint64_t prev_rapl_uj_ = 0;
  std::chrono::steady_clock::time_point prev_rapl_time_{};
  uint64_t prev_gpu_energy_uj_ = 0;
  std::chrono::steady_clock::time_point prev_gpu_energy_time_{};
  uint64_t prev_drm_render_ns_ = 0;
  std::chrono::steady_clock::time_point prev_drm_render_time_{};

  // Frame timing state (exclusively render thread — no mutex needed)
  std::chrono::steady_clock::time_point last_frame_time_{};
  std::chrono::steady_clock::time_point last_fps_display_update_{};
  float ema_frametime_ms_ = 0.0f;
  std::array<float, kFrameWindowSize> ft_ring_{};
  size_t ft_ring_pos_ = 0;
  size_t ft_ring_count_ = 0;

  std::array<float, kGraphHistorySize> graph_ft_ms_{};
  std::array<float, kGraphHistorySize> graph_fps_{};
  int graph_offset_ = 0;

  std::atomic<const char*> active_api_{"VULKAN"};
  std::atomic<const char*> translated_api_{nullptr};
  std::atomic<float> displayed_fps_{0.0f};
  std::atomic<float> displayed_fps_avg_{0.0f};
  std::atomic<float> displayed_fps_1low_{0.0f};
  std::atomic<float> displayed_fps_01low_{0.0f};
  std::atomic<float> displayed_frametime_ms_{0.0f};

  // Benchmark recording state
  mutable std::mutex bench_mutex_;
  std::atomic<bool> bench_active_{false};
  std::atomic<bool> bench_has_result_{false};
  std::chrono::steady_clock::time_point bench_start_time_{};
  std::vector<float> bench_frametimes_ms_;
  int bench_max_gpu_temp_c_ = 0;
  float bench_max_gpu_power_w_ = 0.0f;
  int bench_max_cpu_temp_c_ = 0;
  float bench_max_cpu_power_w_ = 0.0f;
  BenchmarkSummary last_bench_summary_{};

  // Hardware telemetry state (updated by background thread every 500ms)
  mutable std::mutex data_mutex_;
  TelemetryData hw_data_{};

  std::atomic<bool> running_{false};
  std::thread worker_thread_;
};

// Global singleton accessor
TelemetryCollector& get_telemetry();

// Detect translated graphics API (DX12 / DX11 / DX9) under Wine/Proton or native VULKAN / OPENGL
const char* detect_effective_render_api(const char* base_api,
                                        const char* maps_path = "/proc/self/maps") noexcept;

}  // namespace fpshud
