#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace fpshud {

struct TelemetryData;
using HardwareTelemetryData = TelemetryData;

class SystemProvider {
public:
  explicit SystemProvider(std::string root_prefix = "");

  void init();
  void poll(HardwareTelemetryData& data);

  void poll_disk_io(HardwareTelemetryData& data);
  void poll_disk_temp(HardwareTelemetryData& data);
  void poll_network(HardwareTelemetryData& data);
  void poll_battery(HardwareTelemetryData& data);
  void poll_local_clock(HardwareTelemetryData& data);

private:
  void init_battery_sensors();
  void init_storage_sensors();

  std::string root_prefix_;

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
};

}  // namespace fpshud
