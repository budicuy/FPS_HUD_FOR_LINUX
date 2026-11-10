#pragma once

#include <cstdint>
#include <string>

namespace fpshud {

struct TelemetryData;
using HardwareTelemetryData = TelemetryData;

class NvidiaGpuProvider {
public:
  NvidiaGpuProvider();
  ~NvidiaGpuProvider();

  NvidiaGpuProvider(const NvidiaGpuProvider&) = delete;
  NvidiaGpuProvider& operator=(const NvidiaGpuProvider&) = delete;

  bool init(const std::string& root_prefix, uint32_t vendor_id, uint32_t device_id);
  void poll(HardwareTelemetryData& out_data);
  void shutdown() noexcept;

  [[nodiscard]] bool is_available() const noexcept { return nvml_device_ != nullptr; }

private:
  void* nvml_lib_ = nullptr;
  void* nvml_device_ = nullptr;
  bool nvml_initialized_ = false;
  unsigned int gpu_bus_width_bits_ = 192;
  char gpu_name_[48] = "NVIDIA GPU";
  char gpu_driver_version_[32] = "N/A";

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
};

}  // namespace fpshud
