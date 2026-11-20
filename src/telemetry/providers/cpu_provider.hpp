#pragma once

#include <inplace_vector>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace fpshud {

struct TelemetryData;
using HardwareTelemetryData = TelemetryData;

class CpuProvider {
public:
  static constexpr size_t kMaxCpuCores = 64;

  explicit CpuProvider(std::string root_prefix = "");
  void init();
  void poll(HardwareTelemetryData& out_data);

  void poll_cpu_load(HardwareTelemetryData& out_data);
  void poll_cpu_clock(HardwareTelemetryData& out_data);
  void poll_cpu_temp(HardwareTelemetryData& out_data);
  void poll_cpu_power(HardwareTelemetryData& out_data);

private:
  std::string root_prefix_;
  std::string proc_stat_path_;
  std::string proc_cpuinfo_path_;
  bool cpuinfo_has_mhz_ = true;
  char cpu_name_[48] = "CPU";

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
  std::string cpu_hwmon_power_uw_path_;
  std::string cpu_hwmon_energy_uj_path_;

  uint64_t prev_cpu_total_ = 0;
  uint64_t prev_cpu_idle_ = 0;
  std::array<uint64_t, kMaxCpuCores> prev_core_total_{};
  std::array<uint64_t, kMaxCpuCores> prev_core_idle_{};
  uint64_t prev_rapl_uj_ = 0;
  std::chrono::steady_clock::time_point prev_rapl_time_{};
};

}  // namespace fpshud
