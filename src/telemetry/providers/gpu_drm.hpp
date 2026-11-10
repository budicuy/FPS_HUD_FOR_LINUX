#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace fpshud {

enum class GpuBackendType { None = 0, NvidiaNvml, AmdSysfs, IntelSysfs, IntelLevelZero };

struct TelemetryData;
using HardwareTelemetryData = TelemetryData;

class DrmGpuProvider {
public:
  DrmGpuProvider();
  ~DrmGpuProvider() = default;

  bool init(const std::string& root_prefix, uint32_t vendor_id, uint32_t device_id);
  void poll(HardwareTelemetryData& out_data);
  void shutdown() noexcept;

  void update_api_vram_info(uint64_t used_bytes, uint64_t total_bytes) noexcept {
    api_vram_used_bytes_.store(used_bytes, std::memory_order_relaxed);
    api_vram_total_bytes_.store(total_bytes, std::memory_order_relaxed);
  }

  [[nodiscard]] GpuBackendType get_backend_type() const noexcept { return backend_type_; }
  [[nodiscard]] uint32_t detected_vendor_id() const noexcept { return detected_vendor_id_; }
  [[nodiscard]] uint32_t detected_device_id() const noexcept { return detected_device_id_; }

  [[nodiscard]] const std::string& mclk_freq_path() const noexcept { return drm_mclk_freq_path_; }
  [[nodiscard]] const std::string& fan_pwm_path() const noexcept { return drm_fan_pwm_path_; }
  [[nodiscard]] const std::string& fan_rpm_path() const noexcept { return drm_fan_rpm_path_; }
  [[nodiscard]] const std::string& fan_max_path() const noexcept { return drm_fan_max_path_; }

private:
  std::string root_prefix_;
  GpuBackendType backend_type_{GpuBackendType::None};
  uint32_t detected_vendor_id_ = 0;
  uint32_t detected_device_id_ = 0;

  std::atomic<uint64_t> api_vram_used_bytes_{0};
  std::atomic<uint64_t> api_vram_total_bytes_{0};

  std::string drm_gpu_busy_path_;
  std::string drm_vram_used_path_;
  std::string drm_vram_total_path_;
  std::string drm_gtt_used_path_;
  std::string drm_gtt_total_path_;
  std::string drm_mem_busy_path_;
  std::string drm_sclk_freq_path_;
  std::string drm_mclk_freq_path_;
  std::string drm_max_freq_path_;
  std::string drm_pp_dpm_sclk_path_;
  std::string drm_pp_dpm_mclk_path_;
  bool drm_sclk_in_hz_ = true;
  std::string drm_temp_path_;
  std::string drm_temp_fallback_path_;
  std::string drm_fan_pwm_path_;
  std::string drm_fan_rpm_path_;
  std::string drm_fan_max_path_;
  std::string drm_power_uw_path_;
  std::string drm_energy_uj_path_;

  char gpu_vendor_[16] = "Generic";
  char gpu_name_[48] = "GPU";
  char gpu_driver_version_[32] = "N/A";

  uint64_t prev_gpu_energy_uj_ = 0;
  std::chrono::steady_clock::time_point prev_gpu_energy_time_{};
  uint64_t prev_drm_render_ns_ = 0;
  std::chrono::steady_clock::time_point prev_drm_render_time_{};
};

}  // namespace fpshud
