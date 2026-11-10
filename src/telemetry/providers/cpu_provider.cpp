#include "providers/cpu_provider.hpp"
#include "logger.hpp"
#include "telemetry.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace fpshud {

CpuProvider::CpuProvider(std::string root_prefix) : root_prefix_(std::move(root_prefix)) {
  std::snprintf(cpu_name_, sizeof(cpu_name_), "CPU");
}

void CpuProvider::init() {
  proc_stat_path_ = root_prefix_ + "/proc/stat";
  proc_cpuinfo_path_ = root_prefix_ + "/proc/cpuinfo";
  cpuinfo_has_mhz_ = false;

  std::error_code ec;
  char buf[128];

  // 1. Detect CPU Model Name from /proc/cpuinfo (Works on Intel, AMD, & ARM)
  {
    if (FILE* fp = std::fopen(proc_cpuinfo_path_.c_str(), "r")) {
      char line_buf[256];
      bool model_found = false;
      while (std::fgets(line_buf, sizeof(line_buf), fp)) {
        std::string line(line_buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (!model_found && (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0)) {
          auto colon = line.find(':');
          if (colon != std::string::npos) {
            std::string raw_name = line.substr(colon + 1);
            clean_hardware_name(raw_name.c_str(), cpu_name_, sizeof(cpu_name_));
            model_found = true;
          }
        }
        if (line.rfind("cpu MHz", 0) == 0) {
          cpuinfo_has_mhz_ = true;
        }
      }
      std::fclose(fp);
    }
  }

  // 2. Discover CPU core frequency sysfs paths
  long max_cpus = sysconf(_SC_NPROCESSORS_CONF);
  if (max_cpus <= 0) max_cpus = 128;
  if (!root_prefix_.empty()) {
    max_cpus = std::max(max_cpus, 512L);
  }
  max_cpus = std::clamp(max_cpus, 1L, 512L);
  cpu_freq_sensors_.clear();
  char path_buf[512];
  for (long cpu_idx = 0; cpu_idx < max_cpus; ++cpu_idx) {
    int n_len = std::snprintf(path_buf, sizeof(path_buf),
                              "%s/sys/devices/system/cpu/cpu%ld/cpufreq/scaling_cur_freq",
                              root_prefix_.c_str(), cpu_idx);
    if (n_len > 0 && static_cast<size_t>(n_len) < sizeof(path_buf) &&
        ::access(path_buf, R_OK) == 0) {
      if (!cpu_freq_sensors_.try_push_back(CpuFreqSensor{static_cast<uint16_t>(cpu_idx), false}))
        break;
      continue;
    }
    n_len = std::snprintf(path_buf, sizeof(path_buf),
                          "%s/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_cur_freq",
                          root_prefix_.c_str(), cpu_idx);
    if (n_len > 0 && static_cast<size_t>(n_len) < sizeof(path_buf) &&
        ::access(path_buf, R_OK) == 0) {
      if (!cpu_freq_sensors_.try_push_back(CpuFreqSensor{static_cast<uint16_t>(cpu_idx), true}))
        break;
    }
    if (cpu_idx == 0) {
      n_len = std::snprintf(path_buf, sizeof(path_buf),
                            "%s/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",
                            root_prefix_.c_str());
      uint64_t max_khz = 0;
      if (n_len > 0 && static_cast<size_t>(n_len) < sizeof(path_buf) &&
          read_sysfs_u64(path_buf, max_khz) && max_khz > 0) {
        cpu_max_freq_mhz_ = static_cast<int>(max_khz / 1000ULL);
      }
    }
  }

  // 3. Discover CPU temperature & hwmon power sensors
  std::string hwmon_dir = root_prefix_ + "/sys/class/hwmon";
  if (fs::exists(hwmon_dir, ec)) {
    for (const auto& entry : fs::directory_iterator(hwmon_dir, ec)) {
      std::string name_path = (entry.path() / "name").string();
      if (!read_sysfs_buf(name_path.c_str(), buf, sizeof(buf))) continue;

      bool is_cpu_hwmon =
          (std::strcmp(buf, "coretemp") == 0 || std::strcmp(buf, "k10temp") == 0 ||
           std::strcmp(buf, "zenpower") == 0 || std::strcmp(buf, "zenpower3") == 0 ||
           std::strcmp(buf, "cpu_thermal") == 0 || std::strcmp(buf, "acpitz") == 0);

      if (is_cpu_hwmon && cpu_temp_path_.empty()) {
        std::string fallback_t1;
        for (int ti = 1; ti <= 8; ++ti) {
          char t_in_name[32];
          char t_lbl_name[32];
          std::snprintf(t_in_name, sizeof(t_in_name), "temp%d_input", ti);
          std::snprintf(t_lbl_name, sizeof(t_lbl_name), "temp%d_label", ti);
          fs::path t_in = entry.path() / t_in_name;
          if (!fs::exists(t_in, ec)) continue;
          if (fallback_t1.empty()) fallback_t1 = t_in.string();

          fs::path t_lbl = entry.path() / t_lbl_name;
          char lbl_buf[64]{};
          if (read_sysfs_buf(t_lbl.c_str(), lbl_buf, sizeof(lbl_buf))) {
            if (std::strstr(lbl_buf, "Tdie") != nullptr ||
                std::strstr(lbl_buf, "Package id 0") != nullptr) {
              cpu_temp_path_ = t_in.string();
              break;
            }
          }
        }
        if (cpu_temp_path_.empty() && !fallback_t1.empty()) {
          cpu_temp_path_ = std::move(fallback_t1);
        }
      }

      if (is_cpu_hwmon || std::strcmp(buf, "amd_energy") == 0 || std::strcmp(buf, "zenergy") == 0) {
        if (cpu_hwmon_power_uw_path_.empty()) {
          fs::path p_avg = entry.path() / "power1_average";
          fs::path p_inp = entry.path() / "power1_input";
          if (is_sysfs_readable(p_avg.c_str()))
            cpu_hwmon_power_uw_path_ = p_avg.string();
          else if (is_sysfs_readable(p_inp.c_str()))
            cpu_hwmon_power_uw_path_ = p_inp.string();
        }
        if (cpu_hwmon_energy_uj_path_.empty()) {
          fs::path e_inp = entry.path() / "energy1_input";
          if (is_sysfs_readable(e_inp.c_str())) cpu_hwmon_energy_uj_path_ = e_inp.string();
        }
      }
    }
  }

  // 3b. Fallback CPU temperature via thermal_zone0
  if (cpu_temp_path_.empty()) {
    std::string tz0 = root_prefix_ + "/sys/class/thermal/thermal_zone0/temp";
    if (fs::exists(tz0, ec)) {
      cpu_temp_path_ = std::move(tz0);
    }
  }

  // 4. Discover RAPL package energy counter
  const char* rapl_rel_candidates[] = {"/sys/class/powercap/intel-rapl:0/energy_uj",
                                       "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj",
                                       "/sys/class/powercap/amd-rapl:0/energy_uj"};
  for (const char* rel : rapl_rel_candidates) {
    std::string candidate = root_prefix_ + rel;
    fs::path parent_dir = fs::path(candidate).parent_path();

    uint64_t tdp_uw = 0;
    if (read_sysfs_u64((parent_dir / "constraint_0_max_power_uw").c_str(), tdp_uw) &&
        tdp_uw >= 5'000'000ULL && tdp_uw <= 500'000'000ULL) {
      cpu_tdp_watts_ = static_cast<float>(tdp_uw) / 1'000'000.0f;
    } else if (read_sysfs_u64((parent_dir / "constraint_0_power_limit_uw").c_str(), tdp_uw) &&
               tdp_uw >= 5'000'000ULL && tdp_uw <= 500'000'000ULL) {
      cpu_tdp_watts_ = static_cast<float>(tdp_uw) / 1'000'000.0f;
    }

    if (is_sysfs_readable(candidate.c_str())) {
      rapl_energy_path_ = candidate;
      fs::path max_path = parent_dir / "max_energy_range_uj";
      if (read_sysfs_buf(max_path.c_str(), buf, sizeof(buf))) {
        rapl_max_range_uj_ = std::strtoull(buf, nullptr, 10);
      }
      break;
    }
  }

  FPSHUD_LOG_DEBUG(fpshud::log::Subsystem::Telemetry, "CpuProvider initialized: %s (tdp=%.1fW)",
                   cpu_name_, cpu_tdp_watts_);
}

void CpuProvider::poll(HardwareTelemetryData& out_data) {
  if (cpu_name_[0] != '\0') {
    std::memcpy(out_data.cpu_name, cpu_name_, sizeof(out_data.cpu_name));
  }
  poll_cpu_load(out_data);
  poll_cpu_clock(out_data);
  poll_cpu_temp(out_data);
  poll_cpu_power(out_data);
}

void CpuProvider::poll_cpu_load(HardwareTelemetryData& out_data) {
  if (out_data.cpu_name[0] == '\0' && cpu_name_[0] != '\0') {
    std::memcpy(out_data.cpu_name, cpu_name_, sizeof(out_data.cpu_name));
  }

  int fd = open(proc_stat_path_.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  char buf[16384];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return;
  buf[n] = '\0';

  int max_core_seen = 0;
  std::array<uint8_t, TelemetryData::kMaxCpuCores> core_loads{};
  std::bitset<TelemetryData::kMaxCpuCores> core_updated{};

  const char* line = buf;
  while (*line) {
    const char* next_line = std::strchr(line, '\n');

    if (line[0] == 'c' && line[1] == 'p' && line[2] == 'u') {
      if (line[3] == ' ') {
        // Overall CPU line
        uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0,
                 steal = 0;
        if (std::sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu", &user, &nice, &system, &idle,
                        &iowait, &irq, &softirq, &steal) >= 4) {
          uint64_t idle_all = idle + iowait;
          uint64_t total = user + nice + system + idle_all + irq + softirq + steal;
          if (prev_cpu_total_ != 0 && total > prev_cpu_total_) {
            uint64_t total_d = total - prev_cpu_total_;
            uint64_t idle_d = (idle_all > prev_cpu_idle_) ? (idle_all - prev_cpu_idle_) : 0;
            uint64_t busy_d = (total_d > idle_d) ? (total_d - idle_d) : 0;
            float usage = std::clamp(
                100.0f * static_cast<float>(busy_d) / static_cast<float>(total_d), 0.0f, 100.0f);
            out_data.cpu_load_pct = usage;
          }
          prev_cpu_total_ = total;
          prev_cpu_idle_ = idle_all;
        }
      } else if (line[3] >= '0' && line[3] <= '9') {
        // Per-core line: cpu<N>
        char* endp = nullptr;
        long cpu_idx = std::strtol(line + 3, &endp, 10);
        if (endp && endp != line + 3 && cpu_idx >= 0 && cpu_idx < 512) {
          uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0,
                   steal = 0;
          if (std::sscanf(endp, "%lu %lu %lu %lu %lu %lu %lu %lu", &user, &nice, &system, &idle,
                          &iowait, &irq, &softirq, &steal) >= 4) {
            uint64_t idle_all = idle + iowait;
            uint64_t total = user + nice + system + idle_all + irq + softirq + steal;

            if (cpu_idx < static_cast<long>(TelemetryData::kMaxCpuCores)) {
              size_t cidx = static_cast<size_t>(cpu_idx);
              max_core_seen = std::max(max_core_seen, static_cast<int>(cpu_idx) + 1);
              if (prev_core_total_[cidx] != 0 && total > prev_core_total_[cidx]) {
                uint64_t tot_d = total - prev_core_total_[cidx];
                uint64_t idle_d =
                    (idle_all > prev_core_idle_[cidx]) ? (idle_all - prev_core_idle_[cidx]) : 0;
                uint64_t busy_d = (tot_d > idle_d) ? (tot_d - idle_d) : 0;
                int pct = std::clamp(static_cast<int>((busy_d * 100 + tot_d / 2) / tot_d), 0, 100);
                core_loads[cidx] = static_cast<uint8_t>(pct);
                core_updated.set(cidx);
              }
              prev_core_total_[cidx] = total;
              prev_core_idle_[cidx] = idle_all;
            }
          }
        }
      }
    }

    if (!next_line) break;
    line = next_line + 1;
  }

  if (max_core_seen > 0) {
    out_data.cpu_core_count = std::max(out_data.cpu_core_count, max_core_seen);
    for (size_t i = 0; i < TelemetryData::kMaxCpuCores; ++i) {
      if (core_updated.test(i)) {
        out_data.cpu_core_loads_pct[i] = core_loads[i];
      }
    }
  }
}

void CpuProvider::poll_cpu_clock(HardwareTelemetryData& out_data) {
  int max_mhz = 0;
  int max_core_seen = 0;
  std::array<uint16_t, TelemetryData::kMaxCpuCores> core_clocks{};
  std::bitset<TelemetryData::kMaxCpuCores> core_updated{};

  // 1. Fast path: parse /proc/cpuinfo in a single file read (zero IPI interrupts to CPU cores)
  if (cpuinfo_has_mhz_) {
    int fd = open(proc_cpuinfo_path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      std::array<char, 65536> buf;
      ssize_t total_read = 0;
      while (total_read < static_cast<ssize_t>(buf.size() - 1)) {
        ssize_t n = read(fd, buf.data() + total_read, buf.size() - 1 - total_read);
        if (n <= 0) break;
        total_read += n;
      }
      close(fd);
      if (total_read > 0) {
        buf[total_read] = '\0';
        int cur_processor = -1;
        int mhz_entries_found = 0;
        const char* p = buf.data();
        while (*p) {
          if (std::strncmp(p, "processor", 9) == 0) {
            const char* colon = std::strchr(p, ':');
            if (colon) {
              cur_processor = std::atoi(colon + 1);
            }
          } else if (std::strncmp(p, "cpu MHz", 7) == 0 && cur_processor >= 0) {
            const char* colon = std::strchr(p, ':');
            if (colon) {
              float mhz_f = std::strtof(colon + 1, nullptr);
              int mhz = static_cast<int>(mhz_f + 0.5f);
              if (mhz > max_mhz) max_mhz = mhz;
              if (cur_processor < static_cast<int>(TelemetryData::kMaxCpuCores)) {
                core_clocks[cur_processor] = static_cast<uint16_t>(std::clamp(mhz, 0, 65535));
                core_updated.set(cur_processor);
                max_core_seen = std::max(max_core_seen, cur_processor + 1);
              }
              ++mhz_entries_found;
            }
          }
          const char* next = std::strchr(p, '\n');
          if (!next) break;
          p = next + 1;
        }
        if (mhz_entries_found == 0) {
          cpuinfo_has_mhz_ = false;
        }
      }
    }
  }

  // 2. Fallback to sysfs if cpu MHz was not in /proc/cpuinfo
  if (max_mhz <= 0 && max_core_seen <= 0) {
    char buf[32];
    char path_buf[512];
    for (const auto& sensor : cpu_freq_sensors_) {
      int n_len = std::snprintf(path_buf, sizeof(path_buf),
                                sensor.use_cpuinfo
                                    ? "%s/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_cur_freq"
                                    : "%s/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq",
                                root_prefix_.c_str(), static_cast<unsigned>(sensor.cpu_id));
      if (n_len <= 0 || static_cast<size_t>(n_len) >= sizeof(path_buf)) continue;
      if (read_sysfs_buf(path_buf, buf, sizeof(buf))) {
        long khz = std::strtol(buf, nullptr, 10);
        int mhz = static_cast<int>((khz + 500) / 1000);
        if (mhz > max_mhz) max_mhz = mhz;
        if (sensor.cpu_id < TelemetryData::kMaxCpuCores) {
          core_clocks[sensor.cpu_id] = static_cast<uint16_t>(std::clamp(mhz, 0, 65535));
          core_updated.set(sensor.cpu_id);
          max_core_seen = std::max(max_core_seen, static_cast<int>(sensor.cpu_id) + 1);
        }
      }
    }
  }

  if (max_mhz > 0 || max_core_seen > 0) {
    if (max_mhz > 0) out_data.cpu_clock_mhz = max_mhz;
    out_data.cpu_core_count = std::max(out_data.cpu_core_count, max_core_seen);
    for (size_t i = 0; i < TelemetryData::kMaxCpuCores; ++i) {
      if (core_updated.test(i)) {
        out_data.cpu_core_clocks_mhz[i] = core_clocks[i];
      }
    }
  }
}

void CpuProvider::poll_cpu_temp(HardwareTelemetryData& out_data) {
  if (cpu_temp_path_.empty()) return;
  char buf[32];
  if (!read_sysfs_buf(cpu_temp_path_.c_str(), buf, sizeof(buf))) return;

  long millideg = std::strtol(buf, nullptr, 10);
  if (millideg > 0 && millideg < 130000) {
    int temp_c = static_cast<int>((millideg + 500) / 1000);
    out_data.cpu_temp_c = temp_c;
  }
}

void CpuProvider::poll_cpu_power(HardwareTelemetryData& out_data) {
  char buf[32];
  float watts = -1.0f;

  // 1. Primary: Intel / AMD RAPL energy counter
  const std::string& energy_path =
      !rapl_energy_path_.empty() ? rapl_energy_path_ : cpu_hwmon_energy_uj_path_;
  uint64_t curr_uj = 0;
  if (!energy_path.empty() && read_sysfs_u64(energy_path.c_str(), curr_uj) && curr_uj > 0) {
    auto now = std::chrono::steady_clock::now();

    if (prev_rapl_time_.time_since_epoch().count() != 0) {
      double dt_sec = std::chrono::duration<double>(now - prev_rapl_time_).count();
      if (dt_sec > 0.01) {
        uint64_t delta_uj = 0;
        if (curr_uj >= prev_rapl_uj_) {
          delta_uj = curr_uj - prev_rapl_uj_;
        } else if (rapl_max_range_uj_ > prev_rapl_uj_) {
          delta_uj = (rapl_max_range_uj_ - prev_rapl_uj_) + curr_uj;
        }
        watts = static_cast<float>((static_cast<double>(delta_uj) / 1e6) / dt_sec);
      }
    }
    prev_rapl_uj_ = curr_uj;
    prev_rapl_time_ = now;
  }

  // 2. Fallback: Direct hwmon power1_average / power1_input in microwatts (AMD zenpower / k10temp)
  if (watts < 0.0f && !cpu_hwmon_power_uw_path_.empty() &&
      read_sysfs_buf(cpu_hwmon_power_uw_path_.c_str(), buf, sizeof(buf))) {
    uint64_t uw = std::strtoull(buf, nullptr, 10);
    watts = static_cast<float>(uw) / 1e6f;
  }

  // 3. Tier-2 / Tier-3 Fallback: Sudo-free Dynamic TDP estimation (constraint_0_max_power_uw + load
  // & boost ratio)
  if (watts < 0.0f) {
    float load_ratio = std::clamp(out_data.cpu_load_pct / 100.0f, 0.0f, 1.0f);
    int cur_mhz = out_data.cpu_clock_mhz;
    float freq_ratio = 1.0f;
    if (cur_mhz > 0 && cpu_max_freq_mhz_ > 0) {
      freq_ratio = std::clamp(static_cast<float>(cur_mhz) / static_cast<float>(cpu_max_freq_mhz_),
                              0.35f, 1.25f);
    }
    float idle_w = std::clamp(cpu_tdp_watts_ * 0.12f, 4.0f, 18.0f);
    float dyn_w = (cpu_tdp_watts_ - idle_w) * load_ratio * std::pow(freq_ratio, 1.5f);
    watts = std::clamp(idle_w + dyn_w, idle_w, cpu_tdp_watts_ * 1.35f);
  }

  if (watts >= 0.0f && watts < 500.0f) {
    out_data.cpu_power_w = watts;
  }
}

}  // namespace fpshud
