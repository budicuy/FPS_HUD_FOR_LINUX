#include "providers/gpu_nvidia.hpp"
#include "logger.hpp"
#include "sysfs_util.hpp"
#include "telemetry.hpp"

#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace fpshud {

namespace {

constexpr int NVML_SUCCESS = 0;
constexpr int NVML_TEMPERATURE_GPU = 0;
constexpr int NVML_CLOCK_GRAPHICS = 0;
constexpr int NVML_CLOCK_MEM = 2;

struct NvmlUtilization {
  unsigned int gpu;
  unsigned int memory;
};

struct NvmlMemory {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
};

struct NvmlPciInfo {
  char busIdLegacy[16];
  unsigned int domain;
  unsigned int bus;
  unsigned int device;
  unsigned int pciDeviceId;
  unsigned int pciSubSystemId;
  char busId[32];
};

}  // namespace

NvidiaGpuProvider::NvidiaGpuProvider() = default;

NvidiaGpuProvider::~NvidiaGpuProvider() { shutdown(); }

bool NvidiaGpuProvider::is_d3cold_suspended() const noexcept {
  std::error_code ec;
  bool has_nvidia_card = false;
  bool all_susp = true;

  // 1. Check /sys/bus/pci/devices
  std::string pci_base = root_prefix_ + "/sys/bus/pci/devices";
  if (fs::exists(pci_base, ec)) {
    for (const auto& entry : fs::directory_iterator(pci_base, ec)) {
      fs::path dev_dir = entry.path();
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
  }

  // 2. Check /sys/class/drm
  std::string drm_base = root_prefix_ + "/sys/class/drm";
  if (fs::exists(drm_base, ec)) {
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
  }

  return has_nvidia_card && all_susp;
}

bool NvidiaGpuProvider::init(const std::string& root_prefix, uint32_t vendor_id,
                             uint32_t device_id) {
  shutdown();
  if (!root_prefix.empty()) {
    root_prefix_ = root_prefix;
  }

  // Optimus D3cold Protection: Check if all NVIDIA cards are in D3cold (suspended)
  bool nvidia_all_suspended = is_d3cold_suspended();

  bool allow_nvml = root_prefix_.empty();
  if (vendor_id != 0 && vendor_id != 0x10de) {
    allow_nvml = false;
  }
  if (nvidia_all_suspended && vendor_id != 0x10de) {
    allow_nvml = false;
  }

  if (!allow_nvml) {
    return false;
  }

  const char* nvml_candidates[] = {"libnvidia-ml.so.1", "libnvidia-ml.so",
                                   "/run/host/usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1",
                                   "/run/host/usr/lib64/libnvidia-ml.so.1"};
  for (const char* path : nvml_candidates) {
    nvml_lib_ = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (nvml_lib_) break;
  }

  if (!nvml_lib_) {
    return false;
  }

  pfn_nvmlInit_v2_ = reinterpret_cast<int (*)()>(dlsym(nvml_lib_, "nvmlInit_v2"));
  pfn_nvmlShutdown_ = reinterpret_cast<int (*)()>(dlsym(nvml_lib_, "nvmlShutdown"));
  pfn_nvmlSystemGetDriverVersion_ = reinterpret_cast<int (*)(char*, unsigned int)>(
      dlsym(nvml_lib_, "nvmlSystemGetDriverVersion"));
  pfn_nvmlDeviceGetCount_v2_ =
      reinterpret_cast<int (*)(unsigned int*)>(dlsym(nvml_lib_, "nvmlDeviceGetCount_v2"));
  pfn_nvmlDeviceGetHandleByIndex_v2_ = reinterpret_cast<int (*)(unsigned int, void**)>(
      dlsym(nvml_lib_, "nvmlDeviceGetHandleByIndex_v2"));
  pfn_nvmlDeviceGetName_ =
      reinterpret_cast<int (*)(void*, char*, unsigned int)>(dlsym(nvml_lib_, "nvmlDeviceGetName"));
  pfn_nvmlDeviceGetUtilizationRates_ =
      reinterpret_cast<int (*)(void*, void*)>(dlsym(nvml_lib_, "nvmlDeviceGetUtilizationRates"));
  pfn_nvmlDeviceGetMemoryInfo_ =
      reinterpret_cast<int (*)(void*, void*)>(dlsym(nvml_lib_, "nvmlDeviceGetMemoryInfo"));
  pfn_nvmlDeviceGetTemperature_ = reinterpret_cast<int (*)(void*, int, unsigned int*)>(
      dlsym(nvml_lib_, "nvmlDeviceGetTemperature"));
  pfn_nvmlDeviceGetFanSpeed_ =
      reinterpret_cast<int (*)(void*, unsigned int*)>(dlsym(nvml_lib_, "nvmlDeviceGetFanSpeed"));
  pfn_nvmlDeviceGetPowerUsage_ =
      reinterpret_cast<int (*)(void*, unsigned int*)>(dlsym(nvml_lib_, "nvmlDeviceGetPowerUsage"));
  pfn_nvmlDeviceGetClockInfo_ = reinterpret_cast<int (*)(void*, int, unsigned int*)>(
      dlsym(nvml_lib_, "nvmlDeviceGetClockInfo"));
  pfn_nvmlDeviceGetMemoryBusWidth_ = reinterpret_cast<int (*)(void*, unsigned int*)>(
      dlsym(nvml_lib_, "nvmlDeviceGetMemoryBusWidth"));
  pfn_nvmlDeviceGetPciInfo_ =
      reinterpret_cast<int (*)(void*, void*)>(dlsym(nvml_lib_, "nvmlDeviceGetPciInfo_v3"));
  if (!pfn_nvmlDeviceGetPciInfo_) {
    pfn_nvmlDeviceGetPciInfo_ =
        reinterpret_cast<int (*)(void*, void*)>(dlsym(nvml_lib_, "nvmlDeviceGetPciInfo"));
  }

  if (!pfn_nvmlInit_v2_ || !pfn_nvmlDeviceGetHandleByIndex_v2_ ||
      pfn_nvmlInit_v2_() != NVML_SUCCESS) {
    shutdown();
    return false;
  }

  nvml_initialized_ = true;
  unsigned int dev_count = 1;
  if (pfn_nvmlDeviceGetCount_v2_) {
    unsigned int cnt = 0;
    if (pfn_nvmlDeviceGetCount_v2_(&cnt) == NVML_SUCCESS && cnt > 0) {
      dev_count = std::min(cnt, 8U);
    }
  }

  void* best_handle = nullptr;
  unsigned long long best_vram = 0;
  for (unsigned int idx = 0; idx < dev_count; ++idx) {
    void* h = nullptr;
    if (pfn_nvmlDeviceGetHandleByIndex_v2_(idx, &h) == NVML_SUCCESS && h) {
      if (device_id != 0 && pfn_nvmlDeviceGetPciInfo_) {
        NvmlPciInfo pci{};
        if (pfn_nvmlDeviceGetPciInfo_(h, &pci) == NVML_SUCCESS) {
          uint32_t dev_id_hi = (pci.pciDeviceId >> 16) & 0xFFFF;
          uint32_t dev_id_lo = pci.pciDeviceId & 0xFFFF;
          if (dev_id_hi == device_id || dev_id_lo == device_id) {
            best_handle = h;
            break;
          }
        }
      }
      unsigned long long total_vram = 0;
      if (pfn_nvmlDeviceGetMemoryInfo_) {
        NvmlMemory mem{};
        if (pfn_nvmlDeviceGetMemoryInfo_(h, &mem) == NVML_SUCCESS) {
          total_vram = mem.total;
        }
      }
      if (!best_handle || total_vram > best_vram) {
        best_handle = h;
        best_vram = total_vram;
      }
    }
  }

  nvml_device_ = best_handle;
  if (!nvml_device_) {
    shutdown();
    return false;
  }

  if (pfn_nvmlDeviceGetName_) {
    char raw_gpu[96] = {0};
    if (pfn_nvmlDeviceGetName_(nvml_device_, raw_gpu, sizeof(raw_gpu)) == NVML_SUCCESS) {
      clean_hardware_name(raw_gpu, gpu_name_, sizeof(gpu_name_));
    }
  }
  if (pfn_nvmlSystemGetDriverVersion_) {
    char drv[32] = {0};
    if (pfn_nvmlSystemGetDriverVersion_(drv, sizeof(drv)) == NVML_SUCCESS) {
      std::snprintf(gpu_driver_version_, sizeof(gpu_driver_version_), "%s", drv);
    }
  }
  unsigned int bw_bits = 0;
  if (pfn_nvmlDeviceGetMemoryBusWidth_ &&
      pfn_nvmlDeviceGetMemoryBusWidth_(nvml_device_, &bw_bits) == NVML_SUCCESS && bw_bits > 0) {
    gpu_bus_width_bits_ = bw_bits;
  }

  return true;
}

void NvidiaGpuProvider::poll(HardwareTelemetryData& out_data) {
  if (!nvml_initialized_ || !nvml_device_) return;

  NvmlUtilization util{};
  NvmlMemory mem{};
  unsigned int temp_c = 0;
  unsigned int fan_pct = 0;
  unsigned int power_mw = 0;
  unsigned int gr_clock_mhz = 0;
  unsigned int mem_clock_mhz = 0;

  bool ok_util = pfn_nvmlDeviceGetUtilizationRates_ &&
                 (pfn_nvmlDeviceGetUtilizationRates_(nvml_device_, &util) == NVML_SUCCESS);
  bool ok_mem = pfn_nvmlDeviceGetMemoryInfo_ &&
                (pfn_nvmlDeviceGetMemoryInfo_(nvml_device_, &mem) == NVML_SUCCESS);
  bool ok_temp =
      pfn_nvmlDeviceGetTemperature_ &&
      (pfn_nvmlDeviceGetTemperature_(nvml_device_, NVML_TEMPERATURE_GPU, &temp_c) == NVML_SUCCESS);
  bool ok_fan = pfn_nvmlDeviceGetFanSpeed_ &&
                (pfn_nvmlDeviceGetFanSpeed_(nvml_device_, &fan_pct) == NVML_SUCCESS);
  bool ok_pwr = pfn_nvmlDeviceGetPowerUsage_ &&
                (pfn_nvmlDeviceGetPowerUsage_(nvml_device_, &power_mw) == NVML_SUCCESS);
  bool ok_gr_clk = pfn_nvmlDeviceGetClockInfo_ &&
                   (pfn_nvmlDeviceGetClockInfo_(nvml_device_, NVML_CLOCK_GRAPHICS, &gr_clock_mhz) ==
                    NVML_SUCCESS);
  bool ok_mem_clk =
      pfn_nvmlDeviceGetClockInfo_ &&
      (pfn_nvmlDeviceGetClockInfo_(nvml_device_, NVML_CLOCK_MEM, &mem_clock_mhz) == NVML_SUCCESS);

  float watts = static_cast<float>(power_mw) / 1000.0f;

  out_data.gpu_available = true;
  std::snprintf(out_data.gpu_vendor, sizeof(out_data.gpu_vendor), "NVIDIA");
  if (gpu_name_[0] != '\0') {
    std::snprintf(out_data.gpu_name, sizeof(out_data.gpu_name), "%s", gpu_name_);
  }
  if (gpu_driver_version_[0] != '\0') {
    std::snprintf(out_data.gpu_driver_version, sizeof(out_data.gpu_driver_version), "%s",
                  gpu_driver_version_);
  }

  if (ok_util) {
    out_data.gpu_load_pct = static_cast<float>(util.gpu);
  }
  if (ok_gr_clk) {
    out_data.gpu_clock_mhz = static_cast<int>(gr_clock_mhz);
  }
  if (ok_mem) {
    out_data.gpu_vram_used_gb = static_cast<float>(mem.used) / (1024.0f * 1024.0f * 1024.0f);
    out_data.gpu_vram_total_gb = static_cast<float>(mem.total) / (1024.0f * 1024.0f * 1024.0f);
  }
  if (ok_mem_clk) {
    out_data.gpu_vram_clock_mhz = static_cast<int>(mem_clock_mhz);
    float max_bw_gbs = (static_cast<float>(mem_clock_mhz) * 2.0f *
                        (static_cast<float>(gpu_bus_width_bits_) / 8.0f)) /
                       1000.0f;
    float used_vram_bw = max_bw_gbs * (ok_util ? (static_cast<float>(util.memory) / 100.0f) : 0.0f);
    out_data.gpu_bandwidth_gbs = used_vram_bw;
  }
  if (ok_temp) {
    out_data.gpu_temp_c = static_cast<int>(temp_c);
  }
  if (ok_fan) {
    out_data.gpu_fan_pct = static_cast<int>(fan_pct);
  }
  if (ok_pwr) {
    out_data.gpu_power_w = watts;
  }
}

void NvidiaGpuProvider::shutdown() noexcept {
  if (nvml_initialized_ && pfn_nvmlShutdown_) {
    pfn_nvmlShutdown_();
    nvml_initialized_ = false;
  }
  if (nvml_lib_) {
    dlclose(nvml_lib_);
    nvml_lib_ = nullptr;
  }
  nvml_device_ = nullptr;
  pfn_nvmlInit_v2_ = nullptr;
  pfn_nvmlShutdown_ = nullptr;
  pfn_nvmlSystemGetDriverVersion_ = nullptr;
  pfn_nvmlDeviceGetCount_v2_ = nullptr;
  pfn_nvmlDeviceGetHandleByIndex_v2_ = nullptr;
  pfn_nvmlDeviceGetName_ = nullptr;
  pfn_nvmlDeviceGetUtilizationRates_ = nullptr;
  pfn_nvmlDeviceGetMemoryInfo_ = nullptr;
  pfn_nvmlDeviceGetTemperature_ = nullptr;
  pfn_nvmlDeviceGetFanSpeed_ = nullptr;
  pfn_nvmlDeviceGetPowerUsage_ = nullptr;
  pfn_nvmlDeviceGetClockInfo_ = nullptr;
  pfn_nvmlDeviceGetMemoryBusWidth_ = nullptr;
  pfn_nvmlDeviceGetPciInfo_ = nullptr;
}

}  // namespace fpshud
