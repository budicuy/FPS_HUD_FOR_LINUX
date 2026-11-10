#include "providers/gpu_levelzero.hpp"
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

LevelZeroGpuProvider::LevelZeroGpuProvider() = default;

LevelZeroGpuProvider::~LevelZeroGpuProvider() { shutdown(); }

void LevelZeroGpuProvider::discover_hybrid_sysfs(const std::string& root_prefix) {
  std::error_code ec;
  std::string drm_base = root_prefix + "/sys/class/drm";
  if (!fs::exists(drm_base, ec)) return;

  char buf[128];
  for (int card_idx = 0; card_idx < 8; ++card_idx) {
    fs::path card_root = fs::path(drm_base) / ("card" + std::to_string(card_idx));
    fs::path dev_dir = card_root / "device";
    if (!fs::exists(dev_dir, ec)) continue;

    if (read_sysfs_buf((dev_dir / "vendor").c_str(), buf, sizeof(buf))) {
      uint32_t vendor = static_cast<uint32_t>(std::strtoul(buf, nullptr, 16));
      if (vendor != 0x8086) continue;
    } else {
      continue;
    }

    if (bound_device_id_ != 0) {
      if (read_sysfs_buf((dev_dir / "device").c_str(), buf, sizeof(buf))) {
        uint32_t dev_id = static_cast<uint32_t>(std::strtoul(buf, nullptr, 16));
        if (dev_id != bound_device_id_) continue;
      } else {
        continue;
      }
    }

    // Core clock fallback discovery
    if (fs::exists(card_root / "gt_act_freq_mhz", ec)) {
      act_freq_path_ = (card_root / "gt_act_freq_mhz").string();
      act_freq_in_hz_ = false;
    } else if (fs::exists(card_root / "gt/gt0/rps_cur_freq_mhz", ec)) {
      act_freq_path_ = (card_root / "gt/gt0/rps_cur_freq_mhz").string();
      act_freq_in_hz_ = false;
    } else if (fs::exists(dev_dir / "tile0/gt0/freq0/act_freq", ec)) {
      act_freq_path_ = (dev_dir / "tile0/gt0/freq0/act_freq").string();
      act_freq_in_hz_ = false;
    } else if (fs::exists(card_root / "gt_cur_freq_mhz", ec)) {
      act_freq_path_ = (card_root / "gt_cur_freq_mhz").string();
      act_freq_in_hz_ = false;
    }

    fs::path hwmon_root = dev_dir / "hwmon";
    if (fs::exists(hwmon_root, ec)) {
      for (const auto& hm : fs::directory_iterator(hwmon_root, ec)) {
        fs::path hp = hm.path();
        if (act_freq_path_.empty() && fs::exists(hp / "freq1_input", ec)) {
          act_freq_path_ = (hp / "freq1_input").string();
          act_freq_in_hz_ = true;
        }
        if (fs::exists(hp / "freq2_input", ec)) {
          drm_mclk_freq_path_ = (hp / "freq2_input").string();
        }
        if (fs::exists(hp / "temp1_input", ec)) {
          temp_input_path_ = (hp / "temp1_input").string();
        }
        if (fs::exists(hp / "energy1_input", ec)) {
          energy_input_path_ = (hp / "energy1_input").string();
        }
        if (fs::exists(hp / "pwm1", ec)) {
          drm_fan_pwm_path_ = (hp / "pwm1").string();
        }
        if (fs::exists(hp / "fan1_input", ec) && fs::exists(hp / "fan1_max", ec)) {
          drm_fan_rpm_path_ = (hp / "fan1_input").string();
          drm_fan_max_path_ = (hp / "fan1_max").string();
        }
        break;
      }
    }
    break;
  }
}

bool LevelZeroGpuProvider::init(const std::string& root_prefix,
                                const LevelZeroDispatch* mock_dispatch, uint32_t bound_device_id) {
  shutdown();
  bound_device_id_ = bound_device_id;
  discover_hybrid_sysfs(root_prefix);

  if (mock_dispatch) {
    ze_dispatch_ = *mock_dispatch;
  } else {
    if (!root_prefix.empty()) return false;

    const char* ze_candidates[] = {"libze_loader.so.1", "libze_loader.so",
                                   "/run/host/usr/lib/x86_64-linux-gnu/libze_loader.so.1",
                                   "/run/host/usr/lib64/libze_loader.so.1"};
    for (const char* path : ze_candidates) {
      ze_lib_ = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
      if (ze_lib_) break;
    }
    if (!ze_lib_) return false;

    ze_dispatch_.zeInit = reinterpret_cast<int (*)(uint32_t)>(dlsym(ze_lib_, "zeInit"));
    ze_dispatch_.zesInit = reinterpret_cast<int (*)(uint32_t)>(dlsym(ze_lib_, "zesInit"));
    ze_dispatch_.zesDriverGet =
        reinterpret_cast<int (*)(uint32_t*, void**)>(dlsym(ze_lib_, "zesDriverGet"));
    ze_dispatch_.zesDeviceGet =
        reinterpret_cast<int (*)(void*, uint32_t*, void**)>(dlsym(ze_lib_, "zesDeviceGet"));
    ze_dispatch_.zesDeviceGetProperties =
        reinterpret_cast<int (*)(void*, intel_ze::zes_device_properties_t*)>(
            dlsym(ze_lib_, "zesDeviceGetProperties"));
    ze_dispatch_.zesDeviceEnumFrequencyDomains =
        reinterpret_cast<int (*)(void*, uint32_t*, void**)>(
            dlsym(ze_lib_, "zesDeviceEnumFrequencyDomains"));
    ze_dispatch_.zesFrequencyGetState =
        reinterpret_cast<int (*)(void*, intel_ze::zes_freq_state_t*)>(
            dlsym(ze_lib_, "zesFrequencyGetState"));
    ze_dispatch_.zesDeviceEnumTemperatureSensors =
        reinterpret_cast<int (*)(void*, uint32_t*, void**)>(
            dlsym(ze_lib_, "zesDeviceEnumTemperatureSensors"));
    ze_dispatch_.zesTemperatureGetState =
        reinterpret_cast<int (*)(void*, double*)>(dlsym(ze_lib_, "zesTemperatureGetState"));
    ze_dispatch_.zesDeviceEnumPowerDomains = reinterpret_cast<int (*)(void*, uint32_t*, void**)>(
        dlsym(ze_lib_, "zesDeviceEnumPowerDomains"));
    ze_dispatch_.zesPowerGetEnergyCounter =
        reinterpret_cast<int (*)(void*, intel_ze::zes_power_energy_counter_t*)>(
            dlsym(ze_lib_, "zesPowerGetEnergyCounter"));
    ze_dispatch_.zesDeviceEnumMemoryModules = reinterpret_cast<int (*)(void*, uint32_t*, void**)>(
        dlsym(ze_lib_, "zesDeviceEnumMemoryModules"));
    ze_dispatch_.zesMemoryGetState = reinterpret_cast<int (*)(void*, intel_ze::zes_mem_state_t*)>(
        dlsym(ze_lib_, "zesMemoryGetState"));
    ze_dispatch_.zesDeviceEnumEngineGroups = reinterpret_cast<int (*)(void*, uint32_t*, void**)>(
        dlsym(ze_lib_, "zesDeviceEnumEngineGroups"));
    ze_dispatch_.zesEngineGetActivity =
        reinterpret_cast<int (*)(void*, intel_ze::zes_engine_stats_t*)>(
            dlsym(ze_lib_, "zesEngineGetActivity"));
  }

  // Initialize Level Zero / Sysman (ensure ZES_ENABLE_SYSMAN=1 for older Compute Runtime drivers)
  ::setenv("ZES_ENABLE_SYSMAN", "1", 0);
  int init_res = 1;
  if (ze_dispatch_.zesInit) {
    init_res = ze_dispatch_.zesInit(0);
  } else if (ze_dispatch_.zeInit) {
    init_res = ze_dispatch_.zeInit(0);
  }
  if (init_res != intel_ze::ZE_RESULT_SUCCESS) {
    shutdown();
    return false;
  }

  // Enumerate Sysman drivers & devices
  if (!ze_dispatch_.zesDriverGet || !ze_dispatch_.zesDeviceGet) {
    shutdown();
    return false;
  }

  uint32_t drv_count = 1;
  if (ze_dispatch_.zesDriverGet(&drv_count, &ze_driver_) != intel_ze::ZE_RESULT_SUCCESS ||
      drv_count == 0 || !ze_driver_) {
    shutdown();
    return false;
  }

  uint32_t dev_count = 1;
  if (ze_dispatch_.zesDeviceGet(ze_driver_, &dev_count, &ze_device_) !=
          intel_ze::ZE_RESULT_SUCCESS ||
      dev_count == 0 || !ze_device_) {
    shutdown();
    return false;
  }

  // Cache domain handles
  uint32_t count = 1;
  if (ze_dispatch_.zesDeviceEnumFrequencyDomains) {
    ze_dispatch_.zesDeviceEnumFrequencyDomains(ze_device_, &count, &ze_freq_);
  }
  count = 1;
  if (ze_dispatch_.zesDeviceEnumTemperatureSensors) {
    ze_dispatch_.zesDeviceEnumTemperatureSensors(ze_device_, &count, &ze_temp_);
  }
  count = 1;
  if (ze_dispatch_.zesDeviceEnumPowerDomains) {
    ze_dispatch_.zesDeviceEnumPowerDomains(ze_device_, &count, &ze_pwr_);
  }
  count = 1;
  if (ze_dispatch_.zesDeviceEnumMemoryModules) {
    ze_dispatch_.zesDeviceEnumMemoryModules(ze_device_, &count, &ze_mem_);
  }
  has_vram_total_.store(ze_mem_ != nullptr, std::memory_order_relaxed);
  count = 1;
  if (ze_dispatch_.zesDeviceEnumEngineGroups) {
    ze_dispatch_.zesDeviceEnumEngineGroups(ze_device_, &count, &ze_eng_);
  }

  std::snprintf(gpu_name_, sizeof(gpu_name_), "Intel Arc / Xe Graphics");
  std::snprintf(gpu_driver_version_, sizeof(gpu_driver_version_), "Level Zero Sysman");

  if (ze_dispatch_.zesDeviceGetProperties) {
    intel_ze::zes_device_properties_t props{};
    props.stype = 0x1;
    if (ze_dispatch_.zesDeviceGetProperties(ze_device_, &props) == intel_ze::ZE_RESULT_SUCCESS) {
      if (props.modelName[0] != '\0') {
        clean_hardware_name(props.modelName, gpu_name_, sizeof(gpu_name_));
      }
      if (props.driverVersion[0] != '\0') {
        std::snprintf(gpu_driver_version_, sizeof(gpu_driver_version_), "%s", props.driverVersion);
      }
    }
  }

  return true;
}

void LevelZeroGpuProvider::poll(HardwareTelemetryData& out_data) {
  if (!ze_device_) return;

  // 1. Clock (Frequency)
  if (ze_freq_ && ze_dispatch_.zesFrequencyGetState) {
    intel_ze::zes_freq_state_t fstate{};
    if (ze_dispatch_.zesFrequencyGetState(ze_freq_, &fstate) == intel_ze::ZE_RESULT_SUCCESS) {
      out_data.gpu_clock_mhz = static_cast<int>(fstate.actual);
    }
  }
  if (out_data.gpu_clock_mhz <= 0 && !act_freq_path_.empty()) {
    char clk_buf[64]{};
    if (read_sysfs_buf(act_freq_path_.c_str(), clk_buf, sizeof(clk_buf))) {
      uint64_t val = std::strtoull(clk_buf, nullptr, 10);
      out_data.gpu_clock_mhz =
          act_freq_in_hz_ ? static_cast<int>((val + 500000) / 1000000) : static_cast<int>(val);
    }
  }

  // 2. Temperature
  if (ze_temp_ && ze_dispatch_.zesTemperatureGetState) {
    double temp_val = 0.0;
    if (ze_dispatch_.zesTemperatureGetState(ze_temp_, &temp_val) == intel_ze::ZE_RESULT_SUCCESS) {
      out_data.gpu_temp_c = static_cast<int>(temp_val + 0.5);
    }
  }
  if (out_data.gpu_temp_c <= 0 && !temp_input_path_.empty()) {
    char tbuf[64]{};
    if (read_sysfs_buf(temp_input_path_.c_str(), tbuf, sizeof(tbuf))) {
      long millideg = std::strtol(tbuf, nullptr, 10);
      if (millideg > 0 && millideg < 130000) {
        out_data.gpu_temp_c = static_cast<int>((millideg + 500) / 1000);
      }
    }
  }

  // 3. VRAM Memory
  bool ze_vram_ok = false;
  constexpr float kBytesToGb = 1.0f / (1024.0f * 1024.0f * 1024.0f);
  if (ze_mem_ && ze_dispatch_.zesMemoryGetState) {
    intel_ze::zes_mem_state_t mstate{};
    if (ze_dispatch_.zesMemoryGetState(ze_mem_, &mstate) == intel_ze::ZE_RESULT_SUCCESS &&
        mstate.size > 0) {
      float total_gb = static_cast<float>(mstate.size) * kBytesToGb;
      uint64_t used_bytes = (mstate.size > mstate.free) ? (mstate.size - mstate.free) : 0;
      float used_gb = static_cast<float>(used_bytes) * kBytesToGb;
      out_data.gpu_vram_total_gb = total_gb;
      out_data.gpu_vram_used_gb = used_gb;
      has_vram_total_.store(true, std::memory_order_relaxed);
      ze_vram_ok = true;
    }
  }
  if (!ze_vram_ok) {
    has_vram_total_.store(false, std::memory_order_relaxed);
    uint64_t api_total = api_vram_total_bytes_.load(std::memory_order_relaxed);
    if (api_total > 0) {
      uint64_t api_used = api_vram_used_bytes_.load(std::memory_order_relaxed);
      out_data.gpu_vram_total_gb = static_cast<float>(api_total) * kBytesToGb;
      out_data.gpu_vram_used_gb = static_cast<float>(api_used) * kBytesToGb;
    }
  }

  // 4. Power (Energy Counter Delta)
  if (ze_pwr_ && ze_dispatch_.zesPowerGetEnergyCounter) {
    intel_ze::zes_power_energy_counter_t pwr{};
    if (ze_dispatch_.zesPowerGetEnergyCounter(ze_pwr_, &pwr) == intel_ze::ZE_RESULT_SUCCESS) {
      if (prev_ze_pwr_ts_ != 0 && pwr.timestamp > prev_ze_pwr_ts_ &&
          pwr.energy >= prev_ze_pwr_energy_) {
        uint64_t d_energy = pwr.energy - prev_ze_pwr_energy_;  // microjoules
        uint64_t d_time = pwr.timestamp - prev_ze_pwr_ts_;     // microseconds
        if (d_time > 0) {
          float watts = static_cast<float>(d_energy) / static_cast<float>(d_time);
          out_data.gpu_power_w = std::clamp(watts, 0.0f, 1000.0f);
        }
      }
      prev_ze_pwr_energy_ = pwr.energy;
      prev_ze_pwr_ts_ = pwr.timestamp;
    }
  }
  if (out_data.gpu_power_w <= 0.0f && !energy_input_path_.empty()) {
    char pbuf[64]{};
    if (read_sysfs_buf(energy_input_path_.c_str(), pbuf, sizeof(pbuf))) {
      uint64_t curr_uj = std::strtoull(pbuf, nullptr, 10);
      auto now = std::chrono::steady_clock::now();
      uint64_t now_us = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
      if (prev_sysfs_energy_ts_ != 0 && now_us > prev_sysfs_energy_ts_ &&
          curr_uj >= prev_sysfs_energy_uj_) {
        uint64_t d_energy = curr_uj - prev_sysfs_energy_uj_;
        uint64_t d_time = now_us - prev_sysfs_energy_ts_;
        if (d_time > 10000) {
          float watts = static_cast<float>(d_energy) / static_cast<float>(d_time);
          out_data.gpu_power_w = std::clamp(watts, 0.0f, 1000.0f);
        }
      }
      prev_sysfs_energy_uj_ = curr_uj;
      prev_sysfs_energy_ts_ = now_us;
    }
  }

  // 5. Engine Activity (Load %)
  if (ze_eng_ && ze_dispatch_.zesEngineGetActivity) {
    intel_ze::zes_engine_stats_t stats{};
    if (ze_dispatch_.zesEngineGetActivity(ze_eng_, &stats) == intel_ze::ZE_RESULT_SUCCESS) {
      if (prev_ze_eng_ts_ != 0 && stats.timestamp > prev_ze_eng_ts_ &&
          stats.activeTime >= prev_ze_eng_active_) {
        uint64_t d_active = stats.activeTime - prev_ze_eng_active_;
        uint64_t d_time = stats.timestamp - prev_ze_eng_ts_;
        if (d_time > 0) {
          float load = 100.0f * static_cast<float>(d_active) / static_cast<float>(d_time);
          out_data.gpu_load_pct = std::clamp(load, 0.0f, 100.0f);
        }
      }
      prev_ze_eng_active_ = stats.activeTime;
      prev_ze_eng_ts_ = stats.timestamp;
    }
  }

  // 6. Hybrid sysfs / hwmon supplement (VRAM clock, bandwidth, fan speed)
  char buf[64]{};
  int mclk_mhz = 0;
  if (!drm_mclk_freq_path_.empty() &&
      read_sysfs_buf(drm_mclk_freq_path_.c_str(), buf, sizeof(buf))) {
    uint64_t raw_hz = std::strtoull(buf, nullptr, 10);
    mclk_mhz = static_cast<int>((raw_hz + 500000) / 1000000);
  }
  int fan_pct = -1;
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

  if (mclk_mhz > 0) out_data.gpu_vram_clock_mhz = mclk_mhz;
  if (fan_pct >= 0) out_data.gpu_fan_pct = fan_pct;

  int eff_clk =
      (out_data.gpu_vram_clock_mhz > 0) ? out_data.gpu_vram_clock_mhz : out_data.gpu_clock_mhz;
  if (eff_clk > 0) {
    float bus_bytes = (out_data.gpu_vram_total_gb >= 14.5f)
                          ? 32.0f
                          : ((out_data.gpu_vram_total_gb >= 10.5f) ? 24.0f : 16.0f);
    float mult = (out_data.gpu_vram_clock_mhz > 0) ? 2.0f : 1.0f;
    float max_bw_gbs = (static_cast<float>(eff_clk) * mult * bus_bytes) / 1000.0f;
    float util_ratio =
        (out_data.gpu_load_pct > 0.0f)
            ? (out_data.gpu_load_pct / 100.0f)
            : ((out_data.gpu_vram_total_gb > 0.0f)
                   ? std::clamp(out_data.gpu_vram_used_gb / out_data.gpu_vram_total_gb, 0.05f, 1.0f)
                   : 0.1f);
    out_data.gpu_bandwidth_gbs = max_bw_gbs * util_ratio;
  }

  out_data.gpu_available = true;
  std::snprintf(out_data.gpu_vendor, sizeof(out_data.gpu_vendor), "Intel");
  std::snprintf(out_data.gpu_name, sizeof(out_data.gpu_name), "%s", gpu_name_);
  std::snprintf(out_data.gpu_driver_version, sizeof(out_data.gpu_driver_version), "%s",
                gpu_driver_version_);
}

void LevelZeroGpuProvider::shutdown() noexcept {
  if (ze_lib_) {
    dlclose(ze_lib_);
    ze_lib_ = nullptr;
  }
  ze_driver_ = nullptr;
  ze_device_ = nullptr;
  ze_freq_ = nullptr;
  ze_temp_ = nullptr;
  ze_pwr_ = nullptr;
  ze_mem_ = nullptr;
  ze_eng_ = nullptr;
  ze_dispatch_ = {};

  prev_ze_eng_active_ = 0;
  prev_ze_eng_ts_ = 0;
  prev_ze_pwr_energy_ = 0;
  prev_ze_pwr_ts_ = 0;
  has_vram_total_.store(false, std::memory_order_relaxed);
  api_vram_used_bytes_.store(0, std::memory_order_relaxed);
  api_vram_total_bytes_.store(0, std::memory_order_relaxed);

  act_freq_path_.clear();
  act_freq_in_hz_ = false;
  temp_input_path_.clear();
  energy_input_path_.clear();
  prev_sysfs_energy_uj_ = 0;
  prev_sysfs_energy_ts_ = 0;

  drm_mclk_freq_path_.clear();
  drm_fan_pwm_path_.clear();
  drm_fan_rpm_path_.clear();
  drm_fan_max_path_.clear();
}

}  // namespace fpshud
