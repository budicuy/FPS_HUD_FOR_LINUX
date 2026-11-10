#pragma once

#include <cstdint>
#include <string>

namespace fpshud {

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

struct TelemetryData;
using HardwareTelemetryData = TelemetryData;

class LevelZeroGpuProvider {
public:
  LevelZeroGpuProvider();
  ~LevelZeroGpuProvider();

  LevelZeroGpuProvider(const LevelZeroGpuProvider&) = delete;
  LevelZeroGpuProvider& operator=(const LevelZeroGpuProvider&) = delete;

  bool init(const std::string& root_prefix, const LevelZeroDispatch* mock_dispatch,
            uint32_t bound_device_id = 0);
  void poll(HardwareTelemetryData& out_data);
  void shutdown() noexcept;

  void update_api_vram_info(uint64_t used_bytes, uint64_t total_bytes) noexcept {
    api_vram_used_bytes_ = used_bytes;
    api_vram_total_bytes_ = total_bytes;
  }
  [[nodiscard]] bool has_vram_total() const noexcept { return has_vram_total_; }

private:
  void discover_hybrid_sysfs(const std::string& root_prefix);

  uint32_t bound_device_id_ = 0;
  bool has_vram_total_ = false;
  uint64_t api_vram_used_bytes_ = 0;
  uint64_t api_vram_total_bytes_ = 0;

  void* ze_lib_ = nullptr;
  void* ze_driver_ = nullptr;
  void* ze_device_ = nullptr;
  void* ze_freq_ = nullptr;
  void* ze_temp_ = nullptr;
  void* ze_pwr_ = nullptr;
  void* ze_mem_ = nullptr;
  void* ze_eng_ = nullptr;
  LevelZeroDispatch ze_dispatch_{};

  char gpu_name_[48] = "Intel Arc / Xe Graphics";
  char gpu_driver_version_[32] = "Level Zero Sysman";

  uint64_t prev_ze_eng_active_ = 0;
  uint64_t prev_ze_eng_ts_ = 0;
  uint64_t prev_ze_pwr_energy_ = 0;
  uint64_t prev_ze_pwr_ts_ = 0;

  std::string act_freq_path_;
  bool act_freq_in_hz_ = false;
  std::string temp_input_path_;
  std::string energy_input_path_;
  uint64_t prev_sysfs_energy_uj_ = 0;
  uint64_t prev_sysfs_energy_ts_ = 0;

  std::string drm_mclk_freq_path_;
  std::string drm_fan_pwm_path_;
  std::string drm_fan_rpm_path_;
  std::string drm_fan_max_path_;
};

}  // namespace fpshud
