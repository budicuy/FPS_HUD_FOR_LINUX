#include "providers/system_provider.hpp"
#include "parse_util.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "sysfs_util.hpp"
#include "telemetry.hpp"

namespace fs = std::filesystem;

namespace fpshud {

namespace {

bool is_whole_disk(const char* dev) noexcept {
  if (!dev || !*dev) return false;
  if (std::strncmp(dev, "nvme", 4) == 0) {
    const char* p = dev + 4;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') ++p;
    if (*p != 'n') return false;
    ++p;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') ++p;
    return *p == '\0';
  }
  if (std::strncmp(dev, "mmcblk", 6) == 0) {
    const char* p = dev + 6;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') ++p;
    return *p == '\0';
  }
  if ((dev[0] == 's' || dev[0] == 'v') && dev[1] == 'd') {
    const char* p = dev + 2;
    if (*p < 'a' || *p > 'z') return false;
    while (*p >= 'a' && *p <= 'z') ++p;
    return *p == '\0';
  }
  return false;
}

}  // namespace

SystemProvider::SystemProvider(std::string root_prefix) : root_prefix_(std::move(root_prefix)) {}

void SystemProvider::init() {
  proc_diskstats_path_ = root_prefix_ + "/proc/diskstats";
  proc_net_dev_path_ = root_prefix_ + "/proc/net/dev";
  init_battery_sensors();
  init_storage_sensors();
}

void SystemProvider::poll(HardwareTelemetryData& data) {
  poll_battery(data);
  poll_disk_io(data);
  poll_disk_temp(data);
  poll_network(data);
  poll_local_clock(data);
}

void SystemProvider::init_battery_sensors() {
  std::error_code ec;
  battery_capacity_path_.clear();
  battery_status_path_.clear();
  battery_power_uw_path_.clear();
  battery_current_ua_path_.clear();
  battery_voltage_uv_path_.clear();
  battery_discovered_ = false;

  std::string power_dir = root_prefix_ + "/sys/class/power_supply";
  if (!fs::exists(power_dir, ec)) {
    return;
  }

  for (const auto& entry : fs::directory_iterator(power_dir, ec)) {
    std::string fname = entry.path().filename().string();
    char type_buf[32] = {0};
    fs::path type_path = entry.path() / "type";
    bool is_bat = false;

    if (fname.rfind("BAT", 0) == 0) {
      is_bat = true;
    } else if (read_sysfs_buf(type_path.c_str(), type_buf, sizeof(type_buf))) {
      while (type_buf[0] && (type_buf[std::strlen(type_buf) - 1] == '\n' ||
                             type_buf[std::strlen(type_buf) - 1] == '\r')) {
        type_buf[std::strlen(type_buf) - 1] = '\0';
      }
      if (std::strcmp(type_buf, "Battery") == 0) {
        is_bat = true;
      }
    }

    if (is_bat) {
      fs::path cap_path = entry.path() / "capacity";
      if (fs::exists(cap_path, ec)) {
        battery_capacity_path_ = cap_path.string();
        battery_status_path_ = (entry.path() / "status").string();

        fs::path p_now = entry.path() / "power_now";
        if (fs::exists(p_now, ec)) {
          battery_power_uw_path_ = p_now.string();
        }
        fs::path c_now = entry.path() / "current_now";
        if (fs::exists(c_now, ec)) {
          battery_current_ua_path_ = c_now.string();
        }
        fs::path v_now = entry.path() / "voltage_now";
        if (fs::exists(v_now, ec)) {
          battery_voltage_uv_path_ = v_now.string();
        }

        battery_discovered_ = true;
        break;
      }
    }
  }
}

void SystemProvider::poll_battery(HardwareTelemetryData& data) {
  if (!battery_discovered_) {
    data.battery_available = false;
    return;
  }

  char buf[64];
  float cap_pct = 0.0f;
  float power_w = 0.0f;
  char status_str[16] = "Discharging";
  bool is_charging = false;

  if (!battery_capacity_path_.empty() &&
      read_sysfs_buf(battery_capacity_path_.c_str(), buf, sizeof(buf))) {
    cap_pct = std::clamp(std::strtof(buf, nullptr), 0.0f, 100.0f);
  }

  if (!battery_status_path_.empty() &&
      read_sysfs_buf(battery_status_path_.c_str(), buf, sizeof(buf))) {
    while (buf[0] && (buf[std::strlen(buf) - 1] == '\n' || buf[std::strlen(buf) - 1] == '\r')) {
      buf[std::strlen(buf) - 1] = '\0';
    }
    std::snprintf(status_str, sizeof(status_str), "%.15s", buf);
    is_charging = (std::strcmp(status_str, "Charging") == 0);
  }

  if (!battery_power_uw_path_.empty() &&
      read_sysfs_buf(battery_power_uw_path_.c_str(), buf, sizeof(buf))) {
    long long uw = std::strtoll(buf, nullptr, 10);
    power_w = static_cast<float>(std::abs(uw)) / 1000000.0f;
  } else if (!battery_current_ua_path_.empty() && !battery_voltage_uv_path_.empty()) {
    char c_buf[64] = {0};
    char v_buf[64] = {0};
    if (read_sysfs_buf(battery_current_ua_path_.c_str(), c_buf, sizeof(c_buf)) &&
        read_sysfs_buf(battery_voltage_uv_path_.c_str(), v_buf, sizeof(v_buf))) {
      double ua = std::abs(std::strtod(c_buf, nullptr));
      double uv = std::abs(std::strtod(v_buf, nullptr));
      power_w = static_cast<float>((ua * uv) / 1e12);
    }
  }

  data.battery_available = true;
  data.battery_pct = cap_pct;
  data.battery_power_w = power_w;
  data.battery_charging = is_charging;
  std::snprintf(data.battery_status, sizeof(data.battery_status), "%s", status_str);
}

void SystemProvider::init_storage_sensors() {
  std::error_code ec;
  disk_temp_path_.clear();

  std::string hwmon_dir = root_prefix_ + "/sys/class/hwmon";
  if (fs::exists(hwmon_dir, ec)) {
    std::string fallback_path;
    for (const auto& entry : fs::directory_iterator(hwmon_dir, ec)) {
      char buf[64] = {0};
      std::string name_path = (entry.path() / "name").string();
      if (!read_sysfs_buf(name_path.c_str(), buf, sizeof(buf))) continue;

      fs::path t1 = entry.path() / "temp1_input";
      if (!fs::exists(t1, ec)) continue;

      if (std::strncmp(buf, "nvme", 4) == 0) {
        disk_temp_path_ = t1.string();
        break;
      }
      if (fallback_path.empty() &&
          (std::strncmp(buf, "drivetemp", 9) == 0 || std::strncmp(buf, "ata", 3) == 0)) {
        fallback_path = t1.string();
      }
    }
    if (disk_temp_path_.empty() && !fallback_path.empty()) {
      disk_temp_path_ = std::move(fallback_path);
    }
  }
}

void SystemProvider::poll_disk_temp(HardwareTelemetryData& data) {
  if (disk_temp_path_.empty()) {
    data.disk_temp_available = false;
    return;
  }
  char buf[32] = {0};
  if (!read_sysfs_buf(disk_temp_path_.c_str(), buf, sizeof(buf))) return;

  long millideg = std::strtol(buf, nullptr, 10);
  int temp_c = static_cast<int>((millideg + 500) / 1000);
  if (temp_c > -20 && temp_c < 130) {
    data.disk_temp_available = true;
    data.disk_temp_c = temp_c;
  }
}

void SystemProvider::poll_disk_io(HardwareTelemetryData& data) {
  int fd = open(proc_diskstats_path_.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  std::array<char, 65536> buf;
  ssize_t total_read = 0;
  while (total_read < static_cast<ssize_t>(buf.size() - 1)) {
    ssize_t n = read(fd, buf.data() + total_read, buf.size() - 1 - total_read);
    if (n <= 0) break;
    total_read += n;
  }
  close(fd);
  if (total_read <= 0) return;
  buf[total_read] = '\0';

  uint64_t total_read_sectors = 0;
  uint64_t total_write_sectors = 0;

  const char* line = buf.data();
  while (*line) {
    const char* next_line = std::strchr(line, '\n');
    unsigned int maj = 0, min_id = 0;
    char dev_name[64] = {0};
    unsigned long long r_ios = 0, r_merges = 0, r_sec = 0, r_ticks = 0;
    unsigned long long w_ios = 0, w_merges = 0, w_sec = 0;
    std::string_view sv(line);
    if (next_line) {
      sv = std::string_view(line, next_line - line);
    }
    bool parsed_disk = false;
    if (fpshud::parse::parse_int_prefix(sv, maj, &sv) &&
        fpshud::parse::parse_int_prefix(sv, min_id, &sv)) {
      sv = fpshud::parse::trim_leading_whitespace(sv);
      if (!sv.empty()) {
        size_t name_len = 0;
        while (name_len < sv.size() && !fpshud::parse::is_whitespace(sv[name_len])) {
          ++name_len;
        }
        if (name_len > 0 && name_len < sizeof(dev_name)) {
          std::memcpy(dev_name, sv.data(), name_len);
          dev_name[name_len] = '\0';
          sv.remove_prefix(name_len);
          unsigned long long* nums[] = {&r_ios, &r_merges, &r_sec, &r_ticks,
                                        &w_ios, &w_merges, &w_sec};
          int parsed = 0;
          for (auto* n : nums) {
            if (fpshud::parse::parse_int_prefix(sv, *n, &sv)) {
              ++parsed;
            } else {
              break;
            }
          }
          if (parsed == 7) {
            parsed_disk = true;
          }
        }
      }
    }
    if (parsed_disk) {
      if (is_whole_disk(dev_name)) {
        total_read_sectors += static_cast<uint64_t>(r_sec);
        total_write_sectors += static_cast<uint64_t>(w_sec);
      }
    }
    if (!next_line) break;
    line = next_line + 1;
  }

  auto now = std::chrono::steady_clock::now();
  float read_mbs = 0.0f;
  float write_mbs = 0.0f;
  if (prev_disk_poll_time_.time_since_epoch().count() != 0) {
    double dt_sec = std::chrono::duration<double>(now - prev_disk_poll_time_).count();
    if (dt_sec >= 0.01) {
      uint64_t d_read = (total_read_sectors >= prev_disk_read_sectors_)
                            ? (total_read_sectors - prev_disk_read_sectors_)
                            : 0;
      uint64_t d_write = (total_write_sectors >= prev_disk_write_sectors_)
                             ? (total_write_sectors - prev_disk_write_sectors_)
                             : 0;
      constexpr double kSectorToMb = 512.0 / (1024.0 * 1024.0);
      read_mbs = static_cast<float>((static_cast<double>(d_read) * kSectorToMb) / dt_sec);
      write_mbs = static_cast<float>((static_cast<double>(d_write) * kSectorToMb) / dt_sec);
    }
  }
  prev_disk_read_sectors_ = total_read_sectors;
  prev_disk_write_sectors_ = total_write_sectors;
  prev_disk_poll_time_ = now;

  data.disk_read_mbs = read_mbs;
  data.disk_write_mbs = write_mbs;
}

void SystemProvider::poll_network(HardwareTelemetryData& data) {
  int fd = open(proc_net_dev_path_.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  char buf[8192];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return;
  buf[n] = '\0';

  uint64_t total_rx = 0;
  uint64_t total_tx = 0;

  const char* line = buf;
  while (*line) {
    const char* next_line = std::strchr(line, '\n');
    const char* colon = std::strchr(line, ':');
    if (colon && (!next_line || colon < next_line)) {
      const char* p = line;
      while (p < colon && (*p == ' ' || *p == '\t')) ++p;
      size_t name_len = static_cast<size_t>(colon - p);
      while (name_len > 0 && (p[name_len - 1] == ' ' || p[name_len - 1] == '\t')) --name_len;

      bool skip = (name_len == 0) || (name_len == 2 && std::strncmp(p, "lo", 2) == 0) ||
                  (name_len >= 6 && std::strncmp(p, "docker", 6) == 0) ||
                  (name_len >= 4 && std::strncmp(p, "veth", 4) == 0) ||
                  (name_len >= 3 && std::strncmp(p, "br-", 3) == 0) ||
                  (name_len >= 5 && std::strncmp(p, "virbr", 5) == 0);
      if (!skip) {
        unsigned long long rx_b = 0, rx_p = 0, rx_e = 0, rx_d = 0, rx_f = 0, rx_fr = 0, rx_c = 0,
                           rx_m = 0, tx_b = 0;
        std::string_view sv(colon + 1);
        unsigned long long* nums[] = {&rx_b,  &rx_p, &rx_e, &rx_d, &rx_f,
                                      &rx_fr, &rx_c, &rx_m, &tx_b};
        int parsed = 0;
        for (auto* n : nums) {
          if (fpshud::parse::parse_int_prefix(sv, *n, &sv)) {
            ++parsed;
          } else {
            break;
          }
        }
        if (parsed == 9) {
          total_rx += static_cast<uint64_t>(rx_b);
          total_tx += static_cast<uint64_t>(tx_b);
        }
      }
    }
    if (!next_line) break;
    line = next_line + 1;
  }

  auto now = std::chrono::steady_clock::now();
  float rx_mbs = 0.0f;
  float tx_mbs = 0.0f;
  if (prev_net_poll_time_.time_since_epoch().count() != 0) {
    double dt_sec = std::chrono::duration<double>(now - prev_net_poll_time_).count();
    if (dt_sec >= 0.01) {
      uint64_t d_rx = (total_rx >= prev_net_rx_bytes_) ? (total_rx - prev_net_rx_bytes_) : 0;
      uint64_t d_tx = (total_tx >= prev_net_tx_bytes_) ? (total_tx - prev_net_tx_bytes_) : 0;
      constexpr double kBytesToMb = 1.0 / (1024.0 * 1024.0);
      rx_mbs = static_cast<float>((static_cast<double>(d_rx) * kBytesToMb) / dt_sec);
      tx_mbs = static_cast<float>((static_cast<double>(d_tx) * kBytesToMb) / dt_sec);
    }
  }
  prev_net_rx_bytes_ = total_rx;
  prev_net_tx_bytes_ = total_tx;
  prev_net_poll_time_ = now;

  data.net_rx_mbs = rx_mbs;
  data.net_tx_mbs = tx_mbs;
}

void SystemProvider::poll_local_clock(HardwareTelemetryData& data) {
  std::time_t t = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char buf[8];
  std::strftime(buf, sizeof(buf), "%H:%M", &tm_buf);
  std::memcpy(data.local_time_str, buf, sizeof(data.local_time_str));
}

}  // namespace fpshud
