#include "providers/ram_provider.hpp"
#include "logger.hpp"
#include "telemetry.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace fpshud {

RamProvider::RamProvider(std::string root_prefix) : root_prefix_(std::move(root_prefix)) {}

void RamProvider::init() {
  FPSHUD_LOG_DEBUG(fpshud::log::Subsystem::Telemetry, "RamProvider initialized (prefix: %s)",
                   root_prefix_.c_str());
}

void RamProvider::poll(HardwareTelemetryData& out_data) {
  poll_ram(out_data);
  poll_game_ram(out_data);
  poll_mem_pressure(out_data);
}

void RamProvider::poll_ram(HardwareTelemetryData& out_data) {
  std::string mem_path = root_prefix_ + "/proc/meminfo";
  int fd = open(mem_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  char buf[4096];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return;
  buf[n] = '\0';

  uint64_t mem_total_kb = 0;
  uint64_t mem_avail_kb = 0;
  uint64_t swap_total_kb = 0;
  uint64_t swap_free_kb = 0;

  if (const char* p = std::strstr(buf, "MemTotal:")) {
    mem_total_kb = std::strtoull(p + 9, nullptr, 10);
  }
  if (const char* p = std::strstr(buf, "MemAvailable:")) {
    mem_avail_kb = std::strtoull(p + 13, nullptr, 10);
  }
  if (const char* p = std::strstr(buf, "SwapTotal:")) {
    swap_total_kb = std::strtoull(p + 10, nullptr, 10);
  }
  if (const char* p = std::strstr(buf, "SwapFree:")) {
    swap_free_kb = std::strtoull(p + 9, nullptr, 10);
  }

  if (mem_total_kb > 0) {
    uint64_t used_kb = (mem_total_kb > mem_avail_kb) ? (mem_total_kb - mem_avail_kb) : 0;
    uint64_t swap_used_kb = (swap_total_kb > swap_free_kb) ? (swap_total_kb - swap_free_kb) : 0;
    constexpr float kKbPerGb = 1024.0f * 1024.0f;
    out_data.ram_total_gb = static_cast<float>(mem_total_kb) / kKbPerGb;
    out_data.ram_used_gb = static_cast<float>(used_kb) / kKbPerGb;
    out_data.ram_used_pct = 100.0f * static_cast<float>(used_kb) / static_cast<float>(mem_total_kb);
    out_data.swap_total_gb = static_cast<float>(swap_total_kb) / kKbPerGb;
    out_data.swap_used_gb = static_cast<float>(swap_used_kb) / kKbPerGb;
  }
}

void RamProvider::poll_game_ram(HardwareTelemetryData& out_data) {
  // Fast path: /proc/self/statm (single line: size resident shared text lib data dirty)
  std::string statm_path = root_prefix_ + "/proc/self/statm";
  char buf[128];
  if (read_sysfs_buf(statm_path.c_str(), buf, sizeof(buf))) {
    unsigned long size = 0;
    unsigned long resident = 0;
    if (std::sscanf(buf, "%lu %lu", &size, &resident) == 2) {
      static const long s_page_size = sysconf(_SC_PAGESIZE);
      double bytes = static_cast<double>(resident) * (s_page_size > 0 ? s_page_size : 4096);
      out_data.game_ram_gb = static_cast<float>(bytes / (1024.0 * 1024.0 * 1024.0));
      return;
    }
  }

  // Fallback: /proc/self/status VmRSS
  std::string status_path = root_prefix_ + "/proc/self/status";
  int fd = open(status_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  char sbuf[4096];
  ssize_t n = read(fd, sbuf, sizeof(sbuf) - 1);
  close(fd);
  if (n <= 0) return;
  sbuf[n] = '\0';

  if (const char* p = std::strstr(sbuf, "VmRSS:")) {
    uint64_t rss_kb = std::strtoull(p + 6, nullptr, 10);
    out_data.game_ram_gb = static_cast<float>(rss_kb) / (1024.0f * 1024.0f);
  }
}

void RamProvider::poll_mem_pressure(HardwareTelemetryData& out_data) {
  // Linux PSI: share of time (last 10s) in which at least one task stalled waiting for memory
  char buf[256];
  std::string psi_path = root_prefix_ + "/proc/pressure/memory";
  float some_avg10 = -1.0f;
  if (read_sysfs_buf(psi_path.c_str(), buf, sizeof(buf))) {
    float v = 0.0f;
    if (std::sscanf(buf, "some avg10=%f", &v) == 1) {
      some_avg10 = std::clamp(v, 0.0f, 100.0f);
    }
  }
  out_data.mem_psi_some_pct = some_avg10;
}

}  // namespace fpshud
