#pragma once

#include <string>

namespace fpshud {

struct TelemetryData;
using HardwareTelemetryData = TelemetryData;

class RamProvider {
public:
  explicit RamProvider(std::string root_prefix = "");
  void init();
  void poll(HardwareTelemetryData& out_data);

  void poll_ram(HardwareTelemetryData& out_data);
  void poll_game_ram(HardwareTelemetryData& out_data);
  void poll_mem_pressure(HardwareTelemetryData& out_data);

private:
  std::string root_prefix_;
  std::string proc_meminfo_path_;
  std::string proc_statm_path_;
  std::string proc_status_path_;
  std::string proc_pressure_mem_path_;
};

}  // namespace fpshud
