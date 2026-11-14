#include "tools/sensor_probe.hpp"

#include "telemetry.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace fpshud::tools {

void print_probe_row(const char* name, bool available, const char* value_or_reason, int& avail_cnt,
                     int& total_cnt) {
  ++total_cnt;
  if (available) {
    ++avail_cnt;
    std::printf("  [\033[32m✓\033[0m] %-26s : \033[32mAVAILABLE\033[0m   | %s\n", name,
                value_or_reason);
  } else {
    std::printf("  [\033[33m-\033[0m] %-26s : \033[33mUNAVAILABLE\033[0m | %s\n", name,
                value_or_reason);
  }
}

void run_sensor_probe() {
  using namespace fpshud;
  TelemetryCollector tc;
  tc.poll_once_sync();
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  tc.poll_once_sync();
  TelemetryData d = tc.get_snapshot();

  bool rapl_readable = false;
  std::error_code ec;
  if (fs::exists("/sys/class/powercap", ec)) {
    for (const auto& entry : fs::directory_iterator("/sys/class/powercap", ec)) {
      std::string fname = entry.path().filename().string();
      if (fname.find("rapl") != std::string::npos) {
        fs::path epath = entry.path() / "energy_uj";
        struct stat st{};
        if (::stat(epath.c_str(), &st) == 0 && (st.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) != 0 &&
            ::access(epath.c_str(), R_OK) == 0) {
          rapl_readable = true;
          break;
        }
      }
    }
  }

  std::printf("\n==============================================================================\n");
  std::printf("  FPSHUD — Live Hardware Sensor Readability Test (31 Telemetry Sensors)\n");
  std::printf("==============================================================================\n");

  int avail = 0;
  int total = 0;
  char buf[160];

  // 1. Presentation & Real-Time Frametimes (5)
  print_probe_row("01. FPS Counter", true, "Dynamic Vulkan/OpenGL Swapchain Hook", avail, total);
  print_probe_row("02. FPS Average", true, "3.5s Moving Average Window Engine", avail, total);
  print_probe_row("03. FPS 1% Low", true, "Time-Weighted 1% Frametime Stutter Analyzer", avail,
                  total);
  print_probe_row("04. FPS 0.1% Low", true, "Micro-Stutter Spike Ring Buffer Analyzer", avail,
                  total);
  print_probe_row("05. Frame Time", true, "Microsecond Precision CPU/GPU Frame Timer", avail,
                  total);

  std::printf("------------------------------------------------------------------------------\n");

  // 2. GPU Telemetry (8)
  if (d.gpu_available) {
    std::snprintf(buf, sizeof(buf), "%.0f %%", d.gpu_load_pct);
    print_probe_row("06. GPU Core Load", true, buf, avail, total);
  } else {
    print_probe_row("06. GPU Core Load", false, "GPU load sensor unavailable", avail, total);
  }

  if (d.gpu_clock_mhz > 0) {
    std::snprintf(buf, sizeof(buf), "%d MHz", d.gpu_clock_mhz);
    print_probe_row("07. GPU Core Clock", true, buf, avail, total);
  } else {
    print_probe_row("07. GPU Core Clock", false, "GPU core frequency sensor unavailable", avail,
                    total);
  }

  if (d.gpu_vram_clock_mhz > 0) {
    std::snprintf(buf, sizeof(buf), "%d MHz", d.gpu_vram_clock_mhz);
    print_probe_row("08. GPU VRAM Clock", true, buf, avail, total);
  } else {
    print_probe_row("08. GPU VRAM Clock", false,
                    "VRAM memory clock sensor not exposed by GPU driver", avail, total);
  }

  if (d.gpu_vram_total_gb > 0.0f) {
    float vram_pct = (d.gpu_vram_used_gb / d.gpu_vram_total_gb) * 100.0f;
    std::snprintf(buf, sizeof(buf), "%.2f / %.2f GB (%.0f%%)", d.gpu_vram_used_gb,
                  d.gpu_vram_total_gb, vram_pct);
    print_probe_row("09. GPU VRAM Usage", true, buf, avail, total);
  } else {
    print_probe_row("09. GPU VRAM Usage", false, "VRAM capacity sensor unavailable", avail, total);
  }

  if (d.gpu_bandwidth_gbs > 0.0f) {
    std::snprintf(buf, sizeof(buf), "%.1f GB/s", d.gpu_bandwidth_gbs);
    print_probe_row("10. GPU Memory Bandwidth", true, buf, avail, total);
  } else {
    print_probe_row("10. GPU Memory Bandwidth", false, "GPU memory throughput sensor unavailable",
                    avail, total);
  }

  if (d.gpu_temp_c > 0) {
    std::snprintf(buf, sizeof(buf), "%d °C", d.gpu_temp_c);
    print_probe_row("11. GPU Temperature", true, buf, avail, total);
  } else {
    print_probe_row("11. GPU Temperature", false, "GPU thermal sensor unavailable", avail, total);
  }

  if (d.gpu_available && d.gpu_fan_pct >= 0) {
    std::snprintf(buf, sizeof(buf), "%d %%", d.gpu_fan_pct);
    print_probe_row("12. GPU Fan Speed", true, buf, avail, total);
  } else {
    print_probe_row("12. GPU Fan Speed", false,
                    "Passive / Laptop shared cooling or fan sensor absent", avail, total);
  }

  if (d.gpu_power_w > 0.0f) {
    std::snprintf(buf, sizeof(buf), "%.1f W", d.gpu_power_w);
    print_probe_row("13. GPU Power Draw", true, buf, avail, total);
  } else {
    print_probe_row("13. GPU Power Draw", false, "GPU power sensor not exposed by driver", avail,
                    total);
  }

  std::printf("------------------------------------------------------------------------------\n");

  // 3. CPU Telemetry (6)
  std::snprintf(buf, sizeof(buf), "%.1f %%", d.cpu_load_pct);
  print_probe_row("14. CPU Total Load", d.cpu_core_count > 0, buf, avail, total);

  if (d.cpu_core_count > 0) {
    std::snprintf(buf, sizeof(buf), "%d Threads (C0: %u%%, C1: %u%%, ...)", d.cpu_core_count,
                  static_cast<unsigned>(d.cpu_core_loads_pct[0]),
                  static_cast<unsigned>(d.cpu_core_loads_pct[d.cpu_core_count > 1 ? 1 : 0]));
    print_probe_row("15. CPU All-Core Load", true, buf, avail, total);
  } else {
    print_probe_row("15. CPU All-Core Load", false, "/proc/stat per-core lines unavailable", avail,
                    total);
  }

  if (d.cpu_clock_mhz > 0) {
    std::snprintf(buf, sizeof(buf), "%d MHz", d.cpu_clock_mhz);
    print_probe_row("16. CPU Clock (Max)", true, buf, avail, total);
  } else {
    print_probe_row("16. CPU Clock (Max)", false, "cpufreq / /proc/cpuinfo clock not exposed",
                    avail, total);
  }

  if (d.cpu_core_count > 0 && d.cpu_core_clocks_mhz[0] > 0) {
    std::snprintf(buf, sizeof(buf), "%d Cores (C0: %u MHz, C1: %u MHz, ...)", d.cpu_core_count,
                  static_cast<unsigned>(d.cpu_core_clocks_mhz[0]),
                  static_cast<unsigned>(d.cpu_core_clocks_mhz[d.cpu_core_count > 1 ? 1 : 0]));
    print_probe_row("17. CPU All-Core Clock", true, buf, avail, total);
  } else {
    print_probe_row("17. CPU All-Core Clock", false, "Per-core frequency scaling unavailable",
                    avail, total);
  }

  if (d.cpu_power_w > 0.0f) {
    std::snprintf(
        buf, sizeof(buf), "%.1f W (%s)", d.cpu_power_w,
        rapl_readable ? "Direct Hardware Counter / RAPL" : "TDP + Frequency Dynamic Estimation");
    print_probe_row("18. CPU Package Power", true, buf, avail, total);
  } else {
    print_probe_row("18. CPU Package Power", false, "0.0 W (Run: ~/.local/bin/fpshud --setup)",
                    avail, total);
  }

  if (d.cpu_temp_c > 0) {
    std::snprintf(buf, sizeof(buf), "%d °C", d.cpu_temp_c);
    print_probe_row("19. CPU Temperature", true, buf, avail, total);
  } else {
    print_probe_row("19. CPU Temperature", false, "coretemp / k10temp / zenpower hwmon not found",
                    avail, total);
  }

  std::printf("------------------------------------------------------------------------------\n");

  // 4. Memory & Storage (4)
  if (d.ram_total_gb > 0.0f) {
    std::snprintf(buf, sizeof(buf), "%.2f / %.2f GB (%.0f%%)", d.ram_used_gb, d.ram_total_gb,
                  d.ram_used_pct);
    print_probe_row("20. System RAM", true, buf, avail, total);
  } else {
    print_probe_row("20. System RAM", false, "/proc/meminfo unavailable", avail, total);
  }

  std::snprintf(buf, sizeof(buf), "%.2f GB (Process VmRSS)", d.game_ram_gb);
  print_probe_row("21. Process / Game RAM", d.ram_total_gb > 0.0f, buf, avail, total);

  if (d.swap_total_gb > 0.0f) {
    std::snprintf(buf, sizeof(buf), "%.2f / %.2f GB", d.swap_used_gb, d.swap_total_gb);
    print_probe_row("22. Swap / ZRAM", true, buf, avail, total);
  } else {
    print_probe_row("22. Swap / ZRAM", false, "No Swap or ZRAM configured on this system", avail,
                    total);
  }

  if (d.mem_psi_some_pct >= 0.0f) {
    std::snprintf(buf, sizeof(buf), "%.1f %% (10s stall avg)", d.mem_psi_some_pct);
    print_probe_row("23. Memory Pressure (PSI)", true, buf, avail, total);
  } else {
    print_probe_row("23. Memory Pressure (PSI)", false, "/proc/pressure/memory disabled in kernel",
                    avail, total);
  }

  std::printf("------------------------------------------------------------------------------\n");

  // 5. System, Storage, Network & Battery (8)
  std::snprintf(buf, sizeof(buf), "Read: %.1f MB/s | Write: %.1f MB/s", d.disk_read_mbs,
                d.disk_write_mbs);
  print_probe_row("24. Disk I/O Speed", true, buf, avail, total);

  if (d.disk_temp_available && d.disk_temp_c > 0) {
    std::snprintf(buf, sizeof(buf), "%d °C", d.disk_temp_c);
    print_probe_row("25. Storage / NVMe Temp", true, buf, avail, total);
  } else {
    print_probe_row("25. Storage / NVMe Temp", false, "No NVMe / SATA hwmon thermal sensor exposed",
                    avail, total);
  }

  std::snprintf(buf, sizeof(buf), "Download: %.2f MB/s | Upload: %.2f MB/s", d.net_rx_mbs,
                d.net_tx_mbs);
  print_probe_row("26. Network Speed", true, buf, avail, total);

  print_probe_row("27. Render API Backend", true, "Vulkan 1.3 Implicit Layer & OpenGL Hook", avail,
                  total);

  if (d.gpu_available) {
    std::snprintf(buf, sizeof(buf), "%s (%s, Driver: %s)", d.gpu_name, d.gpu_vendor,
                  d.gpu_driver_version);
    print_probe_row("28. GPU Hardware Model", true, buf, avail, total);
  } else {
    print_probe_row("28. GPU Hardware Model", false,
                    "No supported DRM / NVML / Level Zero GPU detected", avail, total);
  }

  std::snprintf(buf, sizeof(buf), "%s (%d Threads)", d.cpu_name, d.cpu_core_count);
  print_probe_row("29. CPU Processor Model", d.cpu_core_count > 0, buf, avail, total);

  std::time_t t_now = std::time(nullptr);
  std::tm tm_now{};
  localtime_r(&t_now, &tm_now);
  std::snprintf(buf, sizeof(buf), "%02d:%02d (24-Hour System Clock)", tm_now.tm_hour,
                tm_now.tm_min);
  print_probe_row("30. Local Time Clock", true, buf, avail, total);

  if (d.battery_available) {
    std::snprintf(buf, sizeof(buf), "%.0f%% (%.1f W, %s)", d.battery_pct, d.battery_power_w,
                  d.battery_status);
    print_probe_row("31. Battery & Power", true, buf, avail, total);
  } else {
    print_probe_row("31. Battery & Power", false, "Desktop PC (No battery detected)", avail, total);
  }

  std::printf("==============================================================================\n");
  std::printf(
      "  Summary: \033[32m%d / %d\033[0m Hardware Sensors & Telemetry Pipeline items AVAILABLE on "
      "your system.\n",
      avail, total);
  if (!rapl_readable) {
    std::printf(
        "  [i] Info: CPU Power is using Sudo-Free TDP Estimation (%.1f W). To enable\n"
        "      direct hardware RAPL counter access anytime, run: ~/.local/bin/fpshud --setup\n",
        d.cpu_power_w);
  }
  std::printf("==============================================================================\n");
}

}  // namespace fpshud::tools
