#pragma once

#include <inplace_vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "analytics/benchmark_recorder.hpp"
#include "analytics/frametime_analyzer.hpp"
#include "providers/cpu_provider.hpp"
#include "providers/gpu_drm.hpp"
#include "providers/gpu_levelzero.hpp"
#include "providers/gpu_nvidia.hpp"
#include "providers/ram_provider.hpp"
#include "providers/system_provider.hpp"
#include "sysfs_util.hpp"

namespace fpshud {

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

using HardwareTelemetryData = TelemetryData;

class TelemetryCollector {
public:
  static constexpr size_t kFrameWindowSize = FrametimeAnalyzer::kFrameWindowSize;
  static constexpr size_t kGraphHistorySize = FrametimeAnalyzer::kGraphHistorySize;
  static constexpr float kRollingWindowMaxMs = FrametimeAnalyzer::kRollingWindowMaxMs;

  TelemetryCollector();
  explicit TelemetryCollector(std::string mock_root_prefix, bool disable_nvml = false,
                              bool disable_level_zero = false);
  ~TelemetryCollector();

  TelemetryCollector(const TelemetryCollector&) = delete;
  TelemetryCollector& operator=(const TelemetryCollector&) = delete;

  [[nodiscard]] GpuBackendType get_gpu_backend() const noexcept {
    return gpu_backend_.load(std::memory_order_relaxed);
  }

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
  const float* get_ft_history() const noexcept { return frametime_analyzer_.get_ft_history(); }
  const float* get_fps_history() const noexcept { return frametime_analyzer_.get_fps_history(); }
  int get_graph_offset() const noexcept { return frametime_analyzer_.get_graph_offset(); }

  // Benchmark Recording Controls (F9 or GUI Button)
  void start_benchmark();
  void stop_benchmark();
  void toggle_benchmark();
  bool is_benchmark_active() const noexcept { return benchmark_recorder_.is_active(); }

  // Force an immediate synchronous poll (useful for CLI test)
  void poll_once_sync();

  // Active GPU binding & rendering API VRAM reporting
  void notify_active_gpu(uint32_t vendor_id, uint32_t device_id);
  void update_api_vram_info(uint64_t used_bytes, uint64_t total_bytes) noexcept;
  [[nodiscard]] bool needs_api_vram_query() const noexcept {
    uint32_t idx = active_hw_buf_.load(std::memory_order_relaxed);
    return hw_buffers_[idx].gpu_vram_total_gb <= 0.0f;
  }

private:
  void worker_loop();
  void init_sensors();
  void init_cpu_sensors();
  void init_gpu_sensors();
  void init_gpu_sensors_locked();
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
  std::atomic<GpuBackendType> gpu_backend_{GpuBackendType::None};
  std::string root_prefix_;
  bool disable_nvml_ = false;
  uint32_t bound_vendor_id_ = 0;
  uint32_t bound_device_id_ = 0;
  std::atomic<uint64_t> api_vram_used_bytes_{0};
  std::atomic<uint64_t> api_vram_total_bytes_{0};

  bool disable_level_zero_ = false;
  bool has_mock_ze_ = false;
  LevelZeroDispatch mock_ze_dispatch_{};

  NvidiaGpuProvider nvidia_provider_;
  DrmGpuProvider drm_provider_;
  LevelZeroGpuProvider levelzero_provider_;

  CpuProvider cpu_provider_;
  RamProvider ram_provider_;
  SystemProvider system_provider_;

  FrametimeAnalyzer frametime_analyzer_;
  BenchmarkRecorder benchmark_recorder_;

  // Frame timing state
  std::chrono::steady_clock::time_point last_frame_time_{};

  std::atomic<const char*> active_api_{"VULKAN"};
  std::atomic<const char*> translated_api_{nullptr};
  std::atomic<bool> translated_api_cached_{false};

  // Lock-free double-buffered hardware telemetry snapshot (zero render-thread mutex contention)
  std::array<TelemetryData, 2> hw_buffers_{};
  std::atomic<uint32_t> active_hw_buf_{0};
  TelemetryData staging_hw_data_{};

  std::atomic<bool> running_{false};
  std::thread worker_thread_;
};

// Global singleton accessor
TelemetryCollector& get_telemetry();

// Detect translated graphics API (DX12 / DX11 / DX9) under Wine/Proton or native VULKAN / OPENGL
const char* detect_effective_render_api(const char* base_api,
                                        const char* maps_path = "/proc/self/maps") noexcept;

}  // namespace fpshud
