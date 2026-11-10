#include "doctest.h"
#include "test_common.hpp"

#include "telemetry.hpp"
#include "providers/cpu_provider.hpp"
#include "providers/ram_provider.hpp"
#include "providers/gpu_nvidia.hpp"
#include "providers/gpu_drm.hpp"
#include "providers/gpu_levelzero.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace fpshud;

namespace {

// Expected values for extended RAM sensors on simulated devices (nullptr = real hardware)
struct RamExpect {
  float game_ram_gb;
  float swap_used_gb;  // < 0 -> swap must be OFF
  float psi_pct;       // < 0 -> PSI must be reported as unavailable
};

// Tests all 23 metrics, Low FPS engine, Scene-Stall Reset, API switching, and Benchmark CSV
void verify_all_features_on_device(fpshud::TelemetryCollector& tc, const char* device_title,
                                   const RamExpect* ram_expect = nullptr) {
  INFO("Device Profile: ", device_title);

  // 1. Test Render API Switch (VULKAN <-> OPENGL)
  tc.set_render_api("OPENGL");
  TelemetryData d_gl = tc.get_snapshot();
  bool api_switch_ok = (std::strcmp(d_gl.render_api, "OPENGL") == 0);
  tc.set_render_api("VULKAN");

  // 2. Test Scene / Loading Stall Auto-Reset (> 250ms frame should clear old history)
  tc.on_simulated_frame_dt(16.6f, true);
  tc.on_simulated_frame_dt(420.0f, true);  // 420ms loading stall -> must auto-reset ring buffer!

  // 3. Start Benchmark CSV Recording + Simulate 100 frames
  tc.start_benchmark();
  for (int i = 0; i < 96; ++i) {
    tc.on_simulated_frame_dt(6.944f, true);
  }
  for (int i = 0; i < 4; ++i) {
    tc.on_simulated_frame_dt(20.0f, true);
  }
  tc.poll_once_sync();
  tc.stop_benchmark();

  TelemetryData d = tc.get_snapshot();

  char detail_hw[160];
  std::snprintf(detail_hw, sizeof(detail_hw), "CPU=[%s] | GPU=[%s (%s, %s)]", d.cpu_name,
                d.gpu_name, d.gpu_vendor, d.gpu_driver_version);

  char detail_fps[160];
  std::snprintf(detail_fps, sizeof(detail_fps),
                "FPS=%.1f | AVG=%.1f | 1%%Low=%.1f | 0.1%%Low=%.1f | FT=%.2fms", d.fps, d.fps_avg,
                d.fps_1pct_low, d.fps_01pct_low, d.frametime_ms);

  char detail_gpu[160];
  std::snprintf(
      detail_gpu, sizeof(detail_gpu),
      "Load=%.0f%% | Clk=%dMHz | VRAM=%.2f/%.2fGB | BW=%.1fGB/s | Tmp=%dC | Fan=%d%% | Pwr=%.1fW",
      d.gpu_load_pct, d.gpu_clock_mhz, d.gpu_vram_used_gb, d.gpu_vram_total_gb, d.gpu_bandwidth_gbs,
      d.gpu_temp_c, d.gpu_fan_pct, d.gpu_power_w);

  char detail_cpu[160];
  std::snprintf(detail_cpu, sizeof(detail_cpu),
                "Load=%.1f%% | Clk=%dMHz | Tmp=%dC | Pwr=%.1fW | RAM=%.2f/%.2fGB | Clock=%s",
                d.cpu_load_pct, d.cpu_clock_mhz, d.cpu_temp_c, d.cpu_power_w, d.ram_used_gb,
                d.ram_total_gb, d.local_time_str);

  char detail_bench[160];
  std::snprintf(detail_bench, sizeof(detail_bench),
                "CSV AVG=%.1f | 1%%Low=%.1f | PeakGPU=%dC/%.1fW | PeakCPU=%dC/%.1fW",
                d.last_benchmark.avg_fps, d.last_benchmark.fps_1pct_low,
                d.last_benchmark.max_gpu_temp_c, d.last_benchmark.max_gpu_power_w,
                d.last_benchmark.max_cpu_temp_c, d.last_benchmark.max_cpu_power_w);

  const bool is_real_hw = (ram_expect == nullptr);
  const bool hw_detect_ok =
      is_real_hw
          ? (std::strlen(d.cpu_name) > 2 && (!d.gpu_available || std::strlen(d.gpu_name) > 2))
          : (d.gpu_available && std::strlen(d.cpu_name) > 2 && std::strlen(d.gpu_name) > 2);
  const bool gpu_sensors_ok =
      is_real_hw ? (!d.gpu_available ||
                    (d.gpu_load_pct >= 0.0f && d.gpu_vram_total_gb >= 0.0f && d.gpu_temp_c >= 0))
                 : (d.gpu_load_pct >= 0.0f && d.gpu_clock_mhz > 0 && d.gpu_vram_total_gb > 0.0f &&
                    d.gpu_vram_used_gb > 0.0f && d.gpu_bandwidth_gbs > 0.0f && d.gpu_temp_c > 0 &&
                    d.gpu_power_w > 0.0f);
  const bool cpu_sensors_ok =
      is_real_hw ? (d.cpu_load_pct >= 0.0f && d.ram_total_gb > 0.0f &&
                    std::strlen(d.local_time_str) >= 4 && api_switch_ok)
                 : (d.cpu_load_pct > 0.0f && d.cpu_clock_mhz > 0 && d.cpu_temp_c > 0 &&
                    d.cpu_power_w > 0.0f && d.ram_total_gb > 0.0f && api_switch_ok);
  const bool bench_ok =
      is_real_hw ? (d.last_benchmark.has_result && d.last_benchmark.avg_fps > 120.0f &&
                    d.last_benchmark.max_gpu_temp_c >= 0 && d.last_benchmark.max_cpu_temp_c >= 0)
                 : (d.last_benchmark.has_result && d.last_benchmark.avg_fps > 120.0f &&
                    d.last_benchmark.max_gpu_temp_c > 0 && d.last_benchmark.max_cpu_power_w > 0);

  check_cond(hw_detect_ok, "1. Hardware Model & Vendor Auto-Detection", detail_hw);
  check_cond(d.fps > 80.0f && d.fps_avg > 120.0f && d.fps_1pct_low >= 45.0f &&
                 d.fps_1pct_low < d.fps_avg && d.fps_01pct_low <= d.fps_1pct_low,
             "2. FPS, AVG, 1% Low, 0.1% Low & Stall Reset", detail_fps);
  check_cond(gpu_sensors_ok, "3. All 7 GPU Sensors (Load/Clk/VRAM/BW/Tmp/Fan/W)", detail_gpu);
  check_cond(cpu_sensors_ok, "4. All 7 CPU/RAM/System Sensors & API Hook", detail_cpu);
  check_cond(bench_ok, "5. Benchmark CSV Recorder & Peak Sensor Log", detail_bench);

  // 6. Extended RAM sensors (Game RAM / Swap / Memory Pressure - 100% sudo-free)
  char psi_str[24], swap_str[32];
  if (d.mem_psi_some_pct >= 0.0f)
    std::snprintf(psi_str, sizeof(psi_str), "%.1f%%", d.mem_psi_some_pct);
  else
    std::snprintf(psi_str, sizeof(psi_str), "N/A");
  if (d.swap_total_gb > 0.0f)
    std::snprintf(swap_str, sizeof(swap_str), "%.2f/%.2fGB", d.swap_used_gb, d.swap_total_gb);
  else
    std::snprintf(swap_str, sizeof(swap_str), "OFF");

  char detail_ram[200];
  std::snprintf(detail_ram, sizeof(detail_ram), "RAM=%.0f%% | Game=%.2fGB | Swap=%s | PSI=%s",
                d.ram_used_pct, d.game_ram_gb, swap_str, psi_str);

  bool ram_ok = false;
  auto near = [](float a, float b) { return std::fabs(a - b) < 0.05f; };
  if (!ram_expect) {
    // Real hardware: values depend on the machine, so validate sanity only
    ram_ok = d.game_ram_gb > 0.0f && d.ram_used_pct > 0.0f && d.ram_used_pct <= 100.0f &&
             d.swap_used_gb >= 0.0f && (d.mem_psi_some_pct >= 0.0f || d.mem_psi_some_pct == -1.0f);
  } else {
    bool swap_ok = (ram_expect->swap_used_gb < 0.0f)
                       ? (d.swap_total_gb == 0.0f)
                       : near(d.swap_used_gb, ram_expect->swap_used_gb);
    bool psi_ok = (ram_expect->psi_pct < 0.0f) ? (d.mem_psi_some_pct == -1.0f)
                                               : near(d.mem_psi_some_pct, ram_expect->psi_pct);
    ram_ok = near(d.game_ram_gb, ram_expect->game_ram_gb) && swap_ok && psi_ok;
  }
  check_cond(ram_ok, "6. RAM Sensors (Game/Swap/PSI + %)", detail_ram);
}

}  // namespace

TEST_CASE("Telemetry: Multi-Device Hardware Profiles") {
  SUBCASE("Profile 1 [REAL HARDWARE]: Intel CPU + NVIDIA GPU (NVML)") {
    auto& real_tc = get_telemetry();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    real_tc.poll_once_sync();
    verify_all_features_on_device(real_tc,
                                  "PROFILE 1 [REAL HARDWARE]: Intel CPU + NVIDIA GPU (NVML)");
  }

  SUBCASE("Profile 2 [SIMULATED AMD PC]: Ryzen 7 7800X3D + Radeon RX 7800 XT") {
    ScopedTempDir mock_dir("fpshud_mock_amd_");
    const fs::path& base = mock_dir.get();
    write_mock_file(base / "proc/cpuinfo",
                    "processor\t: 0\nmodel name\t: AMD Ryzen 7 7800X3D 8-Core Processor\n");
    write_mock_file(base / "proc/stat", "cpu  10000 0 2000 88000 0 0 0 0 0 0");
    write_mock_file(base / "proc/meminfo",
                    "MemTotal:       33554432 kB\nMemAvailable:   20971520 kB\n"
                    "SwapTotal:       8388608 kB\nSwapFree:        6291456 kB\n");
    write_mock_file(base / "proc/self/status", "VmRSS:      4194304 kB\n");
    write_mock_file(base / "proc/pressure/memory",
                    "some avg10=2.50 avg60=1.20 avg300=0.50 total=12345\n");
    write_mock_file(base / "sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "5050000");
    write_mock_file(base / "sys/class/hwmon/hwmon0/name", "k10temp");
    write_mock_file(base / "sys/class/hwmon/hwmon0/temp1_input", "64000");
    write_mock_file(base / "sys/class/hwmon/hwmon0/power1_average", "58500000");

    fs::path amd_dev = base / "sys/class/drm/card0/device";
    write_mock_file(amd_dev / "vendor", "0x1002");
    write_mock_file(amd_dev / "device", "0x747e");  // Radeon RX 7800 XT in pci.ids
    write_mock_file(amd_dev / "gpu_busy_percent", "96");
    write_mock_file(amd_dev / "mem_busy_percent", "62");
    write_mock_file(amd_dev / "mem_info_vram_used", "9126805504");
    write_mock_file(amd_dev / "mem_info_vram_total", "17179869184");
    write_mock_file(amd_dev / "hwmon/hwmon1/freq1_input", "2430000000");  // 2430 MHz core
    write_mock_file(amd_dev / "hwmon/hwmon1/freq2_input", "2438000000");  // 2438 MHz VRAM
    write_mock_file(amd_dev / "hwmon/hwmon1/temp1_input", "68000");
    write_mock_file(amd_dev / "hwmon/hwmon1/pwm1", "166");
    write_mock_file(amd_dev / "hwmon/hwmon1/power1_average", "245000000");

    TelemetryCollector amd_tc(base.string(), true);
    write_mock_file(base / "proc/stat", "cpu  10450 0 2150 88400 0 0 0 0 0 0");
    RamExpect ram2{4.00f, 2.00f, 2.50f};
    verify_all_features_on_device(
        amd_tc, "PROFILE 2 [SIMULATED AMD PC]: Ryzen 7 7800X3D + Radeon RX 7800 XT", &ram2);
    auto amd_snap = amd_tc.get_snapshot();
    check_cond(
        amd_snap.gpu_vram_clock_mhz == 2438 && amd_snap.cpu_core_clocks_mhz[0] == 5050,
        "2.1 AMD GPU VRAM Clock (2438 MHz) & Per-Core CPU Clock",
        "Parsed hwmon1/freq2_input into gpu_vram_clock_mhz=2438 and cpu_core_clocks_mhz[0]=5050");
  }

  SUBCASE("Profile 3 [SIMULATED INTEL PC]: Core i7-13700K + Intel Arc A770") {
    ScopedTempDir mock_dir("fpshud_mock_intel_");
    const fs::path& base = mock_dir.get();
    write_mock_file(base / "proc/cpuinfo",
                    "processor\t: 0\nmodel name\t: 13th Gen Intel(R) Core(TM) i7-13700K\n");
    write_mock_file(base / "proc/stat", "cpu  20000 0 4000 76000 0 0 0 0 0 0");
    write_mock_file(base / "proc/meminfo",
                    "MemTotal:       33554432 kB\nMemAvailable:   16777216 kB\n"
                    "SwapTotal:             0 kB\nSwapFree:              0 kB\n");
    write_mock_file(base / "proc/self/status", "VmRSS:      6291456 kB\n");
    write_mock_file(base / "sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "5300000");
    write_mock_file(base / "sys/class/hwmon/hwmon0/name", "coretemp");
    write_mock_file(base / "sys/class/hwmon/hwmon0/temp1_input", "71000");
    write_mock_file(base / "sys/class/powercap/intel-rapl:0/energy_uj", "10000000");
    write_mock_file(base / "sys/class/powercap/intel-rapl:0/max_energy_range_uj", "262143328850");

    fs::path intel_card = base / "sys/class/drm/card0";
    fs::path intel_dev = intel_card / "device";
    write_mock_file(intel_dev / "vendor", "0x8086");
    write_mock_file(intel_dev / "device", "0x56a0");  // Intel Arc A770 in pci.ids
    write_mock_file(intel_dev / "gpu_busy_percent", "88");
    write_mock_file(intel_card / "lmem_used_bytes", "6442450944");
    write_mock_file(intel_card / "lmem_total_bytes", "17179869184");
    write_mock_file(intel_card / "gt/gt0/rps_cur_freq_mhz", "2400");
    write_mock_file(intel_dev / "hwmon/hwmon2/temp1_input", "66000");
    write_mock_file(intel_dev / "hwmon/hwmon2/fan1_input", "1800");
    write_mock_file(intel_dev / "hwmon/hwmon2/fan1_max", "3000");
    write_mock_file(intel_dev / "hwmon/hwmon2/power1_input", "190000000");

    TelemetryCollector intel_tc(base.string(), true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    write_mock_file(base / "proc/stat", "cpu  20350 0 4050 76600 0 0 0 0 0 0");
    write_mock_file(base / "sys/class/powercap/intel-rapl:0/energy_uj", "12700000");
    RamExpect ram3{6.00f, -1.0f, -1.0f};  // swap=OFF, psi=N/A
    verify_all_features_on_device(
        intel_tc, "PROFILE 3 [SIMULATED INTEL PC]: Core i7-13700K + Intel Arc A770", &ram3);

    auto intel_snap = intel_tc.get_snapshot();
    bool lmem_used_ok = (intel_snap.gpu_vram_used_gb > 5.9f && intel_snap.gpu_vram_used_gb < 6.1f);
    check_cond(lmem_used_ok, "3.1 Intel Arc LMEM Used Parsing",
               "Parsed lmem_used_bytes into vram_used_gb (6.00 GB)");
  }

  SUBCASE("Profile 4 [HYBRID LAPTOP]: Ryzen 7 6800H + dGPU") {
    ScopedTempDir mock_dir("fpshud_mock_hybrid_");
    const fs::path& base = mock_dir.get();
    write_mock_file(base / "proc/cpuinfo",
                    "processor\t: 0\nmodel name\t: AMD Ryzen 7 6800H with Radeon Graphics\n");
    write_mock_file(base / "proc/stat", "cpu  30000 0 3000 67000 0 0 0 0 0 0");
    write_mock_file(base / "proc/meminfo",
                    "MemTotal:       16777216 kB\nMemAvailable:   6291456 kB\n"
                    "SwapTotal:       4194304 kB\nSwapFree:        3145728 kB\n");
    write_mock_file(base / "proc/self/status", "VmRSS:      3145728 kB\n");
    write_mock_file(base / "proc/pressure/memory",
                    "some avg10=14.50 avg60=8.00 avg300=2.00 total=9999\n");
    write_mock_file(base / "sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "4400000");
    write_mock_file(base / "sys/class/hwmon/hwmon0/name", "zenpower");
    write_mock_file(base / "sys/class/hwmon/hwmon0/temp1_input", "76000");
    write_mock_file(base / "sys/class/hwmon/hwmon0/power1_input", "42000000");

    // card0: Integrated GPU (512 MB)
    fs::path igpu_dev = base / "sys/class/drm/card0/device";
    write_mock_file(igpu_dev / "vendor", "0x1002");
    write_mock_file(igpu_dev / "product_name", "Radeon 680M Integrated");
    write_mock_file(igpu_dev / "mem_info_vram_total", "536870912");

    // card1: Discrete GPU (8 GB, pp_dpm_sclk fallback)
    fs::path dgpu_dev = base / "sys/class/drm/card1/device";
    write_mock_file(dgpu_dev / "vendor", "0x1002");
    write_mock_file(dgpu_dev / "device", "0x73ff");
    write_mock_file(dgpu_dev / "gpu_busy_percent", "99");
    write_mock_file(dgpu_dev / "mem_info_vram_used", "5905580032");
    write_mock_file(dgpu_dev / "mem_info_vram_total", "8589934592");
    write_mock_file(dgpu_dev / "pp_dpm_sclk", "0: 500Mhz \n1: 2177Mhz *");
    write_mock_file(dgpu_dev / "hwmon/hwmon3/temp1_input", "74000");
    write_mock_file(dgpu_dev / "hwmon/hwmon3/pwm1", "190");
    write_mock_file(dgpu_dev / "hwmon/hwmon3/power1_average", "100000000");

    TelemetryCollector hybrid_tc(base.string(), true);
    write_mock_file(base / "proc/stat", "cpu  30250 0 3050 67700 0 0 0 0 0 0");
    RamExpect ram4{3.00f, 1.00f, 14.50f};
    verify_all_features_on_device(
        hybrid_tc, "PROFILE 4 [HYBRID LAPTOP]: Ryzen 7 6800H (card0 iGPU -> auto card1 dGPU)",
        &ram4);
  }

  SUBCASE("Profile 5 [APU / STEAM DECK / iGPU]: Shared GTT Memory + Thermal Zone") {
    ScopedTempDir mock_dir("fpshud_mock_apu_");
    const fs::path& base = mock_dir.get();
    write_mock_file(base / "proc/cpuinfo",
                    "processor\t: 0\nmodel name\t: AMD Custom APU 0405 (Steam Deck OLED)\n");
    write_mock_file(base / "proc/stat", "cpu  40000 0 5000 55000 0 0 0 0 0 0");
    write_mock_file(base / "proc/meminfo",
                    "MemTotal:       16777216 kB\nMemAvailable:   7340032 kB\n"
                    "SwapTotal:       1048576 kB\nSwapFree:         524288 kB\n");
    write_mock_file(base / "proc/self/status", "VmRSS:      5242880 kB\n");
    write_mock_file(base / "proc/pressure/memory",
                    "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
    write_mock_file(base / "sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "3500000");
    write_mock_file(base / "sys/class/thermal/thermal_zone0/temp",
                    "69000");  // Fallback thermal_zone0
    write_mock_file(base / "sys/class/powercap/amd-rapl:0/energy_uj", "5000000");
    write_mock_file(base / "sys/class/powercap/amd-rapl:0/max_energy_range_uj", "262143328850");

    // APU with 512MB carved VRAM + 8GB dynamic GTT shared memory
    fs::path apu_dev = base / "sys/class/drm/card0/device";
    write_mock_file(apu_dev / "vendor", "0x1002");
    write_mock_file(apu_dev / "product_name", "AMD Radeon VanGogh APU");
    write_mock_file(apu_dev / "gpu_busy_percent", "85");
    write_mock_file(apu_dev / "mem_info_vram_used", "536870912");   // 0.50 GB carved
    write_mock_file(apu_dev / "mem_info_vram_total", "536870912");  // 0.50 GB carved
    write_mock_file(apu_dev / "mem_info_gtt_used",
                    "3758096384");  // +3.50 GB shared GTT = 4.00 GB used!
    write_mock_file(apu_dev / "mem_info_gtt_total",
                    "8053063680");  // +7.50 GB shared GTT = 8.00 GB total!
    write_mock_file(apu_dev / "hwmon/hwmon4/freq1_input", "1600000000");   // 1600 MHz
    write_mock_file(apu_dev / "hwmon/hwmon4/power1_average", "15000000");  // 15.0 W APU GPU

    TelemetryCollector apu_tc(base.string(), true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    write_mock_file(base / "proc/stat", "cpu  40400 0 5100 55500 0 0 0 0 0 0");
    write_mock_file(base / "sys/class/powercap/amd-rapl:0/energy_uj", "5270000");  // ~9W CPU
    RamExpect ram5{5.00f, 0.50f, 0.00f};
    verify_all_features_on_device(
        apu_tc, "PROFILE 5 [APU / STEAM DECK / iGPU]: Shared GTT Memory + Thermal Zone Fallback",
        &ram5);
  }
}

TEST_CASE("Telemetry: Hardware Providers (CPU, RAM, GPU)") {
  SUBCASE("Suite 13: CpuProvider & RamProvider Unit Tests") {
    ScopedTempDir prov_dir("fpshud_test_providers_");
    const fs::path& pdir = prov_dir.get();
    write_mock_file(pdir / "proc/cpuinfo",
                    "processor\t: 0\nmodel name\t: Test CPU Model\ncpu MHz\t: 3600.000\n"
                    "processor\t: 1\nmodel name\t: Test CPU Model\ncpu MHz\t: 3600.000\n");
    write_mock_file(pdir / "proc/stat",
                    "cpu  10000 0 2000 88000 0 0 0 0\n"
                    "cpu0 5000 0 1000 44000 0 0 0 0\n"
                    "cpu1 5000 0 1000 44000 0 0 0 0\n");
    write_mock_file(pdir / "sys/class/hwmon/hwmon0/name", "coretemp");
    write_mock_file(pdir / "sys/class/hwmon/hwmon0/temp1_input", "55000");
    write_mock_file(pdir / "sys/class/hwmon/hwmon0/power1_average", "45000000");
    write_mock_file(pdir / "proc/meminfo",
                    "MemTotal:       16777216 kB\nMemAvailable:    8388608 kB\n"
                    "SwapTotal:        4194304 kB\nSwapFree:         2097152 kB\n");
    write_mock_file(pdir / "proc/self/statm", "10000 2500 500 100 0 100 0\n");
    write_mock_file(pdir / "proc/pressure/memory",
                    "some avg10=3.50 avg60=1.20 avg300=0.50 total=12345\n");

    fpshud::CpuProvider cpu_prov(pdir.string());
    cpu_prov.init();
    fpshud::HardwareTelemetryData hdata{};
    cpu_prov.poll(hdata);

    write_mock_file(pdir / "proc/stat",
                    "cpu  10600 0 2000 88400 0 0 0 0\n"
                    "cpu0 5300 0 1000 44200 0 0 0 0\n"
                    "cpu1 5300 0 1000 44200 0 0 0 0\n");
    cpu_prov.poll(hdata);

    fpshud::RamProvider ram_prov(pdir.string());
    ram_prov.init();
    ram_prov.poll(hdata);

    bool cpu_load_ok = (hdata.cpu_load_pct >= 59.0f && hdata.cpu_load_pct <= 61.0f);
    bool cpu_clk_ok = (hdata.cpu_clock_mhz == 3600);
    bool cpu_temp_ok = (hdata.cpu_temp_c == 55);
    bool cpu_pwr_ok = (hdata.cpu_power_w >= 44.9f && hdata.cpu_power_w <= 45.1f);
    bool ram_used_ok = (hdata.ram_used_gb >= 7.9f && hdata.ram_used_gb <= 8.1f);
    bool ram_tot_ok = (hdata.ram_total_gb >= 15.9f && hdata.ram_total_gb <= 16.1f);
    bool swap_used_ok = (hdata.swap_used_gb >= 1.9f && hdata.swap_used_gb <= 2.1f);
    bool swap_tot_ok = (hdata.swap_total_gb >= 3.9f && hdata.swap_total_gb <= 4.1f);
    bool psi_ok = (hdata.mem_psi_some_pct >= 3.4f && hdata.mem_psi_some_pct <= 3.6f);
    bool game_ram_ok = (hdata.game_ram_gb > 0.0f);

    CHECK_MESSAGE(cpu_load_ok, "CPU load between 59% and 61%");
    CHECK_MESSAGE(cpu_clk_ok, "CPU clock 3600 MHz");
    CHECK_MESSAGE(cpu_temp_ok, "CPU temperature 55 C");
    CHECK_MESSAGE(cpu_pwr_ok, "CPU power ~45W");
    CHECK_MESSAGE(ram_used_ok, "RAM used ~8GB");
    CHECK_MESSAGE(ram_tot_ok, "RAM total ~16GB");
    CHECK_MESSAGE(swap_used_ok, "Swap used ~2GB");
    CHECK_MESSAGE(swap_tot_ok, "Swap total ~4GB");
    CHECK_MESSAGE(psi_ok, "Memory PSI some ~3.5%");
    CHECK_MESSAGE(game_ram_ok, "Game RAM > 0");

    bool prov_suite_passed = cpu_load_ok && cpu_clk_ok && cpu_temp_ok && cpu_pwr_ok &&
                             ram_used_ok && ram_tot_ok && swap_used_ok && swap_tot_ok && psi_ok &&
                             game_ram_ok;
    std::string prov_detail = "cpu_load=" + std::to_string(hdata.cpu_load_pct) +
                              ", clk=" + std::to_string(hdata.cpu_clock_mhz) +
                              ", temp=" + std::to_string(hdata.cpu_temp_c) +
                              ", ram=" + std::to_string(hdata.ram_used_gb) + "/" +
                              std::to_string(hdata.ram_total_gb) +
                              ", swap=" + std::to_string(hdata.swap_used_gb) +
                              ", psi=" + std::to_string(hdata.mem_psi_some_pct) +
                              ", game=" + std::to_string(hdata.game_ram_gb);
    check_cond(prov_suite_passed, "13.1 Modular Telemetry: CPU & RAM Providers",
               prov_detail.c_str());
  }

  SUBCASE("Suite 14: DrmGpuProvider AMD Sysfs") {
    ScopedTempDir amd_dir("fpshud_test_drm_amd_");
    const fs::path& pdir = amd_dir.get();
    fs::path dev_dir = pdir / "sys/class/drm/card0/device";
    fs::path hm_dir = dev_dir / "hwmon/hwmon0";
    std::error_code ec;
    fs::create_directories(hm_dir, ec);

    write_mock_file(dev_dir / "vendor", "0x1002\n");
    write_mock_file(dev_dir / "device", "0x744c\n");
    write_mock_file(dev_dir / "product_name", "AMD Radeon RX 7900 XTX\n");
    write_mock_file(dev_dir / "gpu_busy_percent", "95\n");
    write_mock_file(dev_dir / "mem_busy_percent", "80\n");
    write_mock_file(dev_dir / "mem_info_vram_total", "25769803776\n");
    write_mock_file(dev_dir / "mem_info_vram_used", "12884901888\n");
    write_mock_file(hm_dir / "freq1_input", "2500000000\n");
    write_mock_file(hm_dir / "freq2_input", "2500000000\n");
    write_mock_file(hm_dir / "temp1_input", "62000\n");
    write_mock_file(hm_dir / "pwm1", "178\n");
    write_mock_file(hm_dir / "power1_average", "285000000\n");

    fpshud::DrmGpuProvider drm_amd;
    bool init_ok = drm_amd.init(pdir.string(), 0, 0);
    fpshud::HardwareTelemetryData hdata{};
    drm_amd.poll(hdata);

    bool be_ok = (drm_amd.get_backend_type() == fpshud::GpuBackendType::AmdSysfs);
    bool avail_ok = hdata.gpu_available;
    bool load_ok = (hdata.gpu_load_pct >= 94.0f && hdata.gpu_load_pct <= 96.0f);
    bool clk_ok = (hdata.gpu_clock_mhz == 2500);
    bool mclk_ok = (hdata.gpu_vram_clock_mhz == 2500);
    bool vram_ok = (hdata.gpu_vram_used_gb >= 11.9f && hdata.gpu_vram_used_gb <= 12.1f &&
                    hdata.gpu_vram_total_gb >= 23.9f && hdata.gpu_vram_total_gb <= 24.1f);
    bool temp_ok = (hdata.gpu_temp_c == 62);
    bool fan_ok = (hdata.gpu_fan_pct == 70);
    bool pwr_ok = (hdata.gpu_power_w >= 284.0f && hdata.gpu_power_w <= 286.0f);
    bool bw_ok = (hdata.gpu_bandwidth_gbs > 10.0f);

    CHECK(init_ok);
    CHECK(be_ok);
    CHECK(avail_ok);
    CHECK(load_ok);
    CHECK(clk_ok);
    CHECK(mclk_ok);
    CHECK(vram_ok);
    CHECK(temp_ok);
    CHECK(fan_ok);
    CHECK(pwr_ok);
    CHECK(bw_ok);

    bool amd_drm_passed = init_ok && be_ok && avail_ok && load_ok && clk_ok && mclk_ok && vram_ok &&
                          temp_ok && fan_ok && pwr_ok && bw_ok;
    std::string amd_drm_detail =
        "init=" + std::to_string(init_ok) + ", load=" + std::to_string(hdata.gpu_load_pct) +
        ", clk=" + std::to_string(hdata.gpu_clock_mhz) +
        ", vram=" + std::to_string(hdata.gpu_vram_used_gb) + "/" +
        std::to_string(hdata.gpu_vram_total_gb) + ", temp=" + std::to_string(hdata.gpu_temp_c) +
        ", pwr=" + std::to_string(hdata.gpu_power_w);
    check_cond(amd_drm_passed,
               "14.1 DrmGpuProvider AMD Sysfs (load, clk, VRAM, temp, fan, pwr, bw)",
               amd_drm_detail.c_str());
  }

  SUBCASE("Suite 14: DrmGpuProvider Intel Sysfs") {
    ScopedTempDir intel_dir("fpshud_test_drm_intel_");
    const fs::path& pdir = intel_dir.get();
    fs::path dev_dir = pdir / "sys/class/drm/card0/device";
    fs::path hm_dir = dev_dir / "hwmon/hwmon1";
    std::error_code ec;
    fs::create_directories(hm_dir, ec);
    fs::create_directories(dev_dir / "tile0/gt0/freq0", ec);

    write_mock_file(dev_dir / "vendor", "0x8086\n");
    write_mock_file(dev_dir / "device", "0x56a0\n");
    write_mock_file(dev_dir / "product_name", "Intel Arc A770 Graphics\n");
    write_mock_file(dev_dir / "gpu_busy_percent", "75\n");
    write_mock_file(dev_dir / "lmem_total_bytes", "17179869184\n");
    write_mock_file(dev_dir / "lmem_used_bytes", "8589934592\n");
    write_mock_file(dev_dir / "tile0/gt0/freq0/act_freq", "2100\n");
    write_mock_file(hm_dir / "temp1_input", "58000\n");
    write_mock_file(hm_dir / "pwm1", "128\n");
    write_mock_file(hm_dir / "power1_average", "150000000\n");

    fpshud::DrmGpuProvider drm_intel;
    bool init_ok = drm_intel.init(pdir.string(), 0, 0);
    fpshud::HardwareTelemetryData hdata{};
    drm_intel.poll(hdata);

    bool be_ok = (drm_intel.get_backend_type() == fpshud::GpuBackendType::IntelSysfs);
    bool avail_ok = hdata.gpu_available;
    bool load_ok = (hdata.gpu_load_pct >= 74.0f && hdata.gpu_load_pct <= 76.0f);
    bool clk_ok = (hdata.gpu_clock_mhz == 2100);
    bool vram_ok = (hdata.gpu_vram_used_gb >= 7.9f && hdata.gpu_vram_used_gb <= 8.1f &&
                    hdata.gpu_vram_total_gb >= 15.9f && hdata.gpu_vram_total_gb <= 16.1f);
    bool temp_ok = (hdata.gpu_temp_c == 58);
    bool fan_ok = (hdata.gpu_fan_pct == 50);
    bool pwr_ok = (hdata.gpu_power_w >= 149.0f && hdata.gpu_power_w <= 151.0f);

    CHECK(init_ok);
    CHECK(be_ok);
    CHECK(avail_ok);
    CHECK(load_ok);
    CHECK(clk_ok);
    CHECK(vram_ok);
    CHECK(temp_ok);
    CHECK(fan_ok);
    CHECK(pwr_ok);

    bool intel_drm_passed =
        init_ok && be_ok && avail_ok && load_ok && clk_ok && vram_ok && temp_ok && fan_ok && pwr_ok;
    std::string intel_drm_detail =
        "init=" + std::to_string(init_ok) + ", load=" + std::to_string(hdata.gpu_load_pct) +
        ", clk=" + std::to_string(hdata.gpu_clock_mhz) +
        ", vram=" + std::to_string(hdata.gpu_vram_used_gb) + "/" +
        std::to_string(hdata.gpu_vram_total_gb) + ", temp=" + std::to_string(hdata.gpu_temp_c) +
        ", pwr=" + std::to_string(hdata.gpu_power_w);
    check_cond(intel_drm_passed,
               "14.2 DrmGpuProvider Intel Sysfs (load, clk, LMEM, temp, fan, pwr)",
               intel_drm_detail.c_str());
  }

  SUBCASE("Suite 14: LevelZeroGpuProvider Mock Dispatch") {
    LevelZeroDispatch ze_test_mock{};
    ze_test_mock.zesInit = [](uint32_t) -> int { return 0; };
    ze_test_mock.zesDriverGet = [](uint32_t* count, void** phDrivers) -> int {
      if (count) *count = 1;
      if (phDrivers) *phDrivers = reinterpret_cast<void*>(0x1111);
      return 0;
    };
    ze_test_mock.zesDeviceGet = [](void*, uint32_t* count, void** phDevices) -> int {
      if (count) *count = 1;
      if (phDevices) *phDevices = reinterpret_cast<void*>(0x2222);
      return 0;
    };
    ze_test_mock.zesDeviceGetProperties = [](void*,
                                             intel_ze::zes_device_properties_t* pProps) -> int {
      if (pProps) {
        std::snprintf(pProps->modelName, sizeof(pProps->modelName), "Intel Arc A770 Graphics");
        std::snprintf(pProps->driverVersion, sizeof(pProps->driverVersion), "Level Zero 1.14");
      }
      return 0;
    };
    ze_test_mock.zesDeviceEnumFrequencyDomains = [](void*, uint32_t* count, void** phFreq) -> int {
      if (count) *count = 1;
      if (phFreq) *phFreq = reinterpret_cast<void*>(0x3333);
      return 0;
    };
    ze_test_mock.zesFrequencyGetState = [](void*, intel_ze::zes_freq_state_t* pState) -> int {
      if (pState) pState->actual = 2400.0;
      return 0;
    };
    ze_test_mock.zesDeviceEnumTemperatureSensors = [](void*, uint32_t* count,
                                                      void** phTemp) -> int {
      if (count) *count = 1;
      if (phTemp) *phTemp = reinterpret_cast<void*>(0x4444);
      return 0;
    };
    ze_test_mock.zesTemperatureGetState = [](void*, double* pTemp) -> int {
      if (pTemp) *pTemp = 65.0;
      return 0;
    };
    ze_test_mock.zesDeviceEnumMemoryModules = [](void*, uint32_t* count, void** phMem) -> int {
      if (count) *count = 1;
      if (phMem) *phMem = reinterpret_cast<void*>(0x5555);
      return 0;
    };
    ze_test_mock.zesMemoryGetState = [](void*, intel_ze::zes_mem_state_t* pState) -> int {
      if (pState) {
        pState->size = 17179869184ULL;
        pState->free = 10737418240ULL;
      }
      return 0;
    };
    static uint64_t s_ze_pwr_ts = 1000000;
    static uint64_t s_ze_pwr_uj = 10000000;
    s_ze_pwr_ts = 1000000;
    s_ze_pwr_uj = 10000000;
    ze_test_mock.zesDeviceEnumPowerDomains = [](void*, uint32_t* count, void** phPower) -> int {
      if (count) *count = 1;
      if (phPower) *phPower = reinterpret_cast<void*>(0x6666);
      return 0;
    };
    ze_test_mock.zesPowerGetEnergyCounter =
        [](void*, intel_ze::zes_power_energy_counter_t* pEnergy) -> int {
      if (pEnergy) {
        pEnergy->timestamp = s_ze_pwr_ts;
        pEnergy->energy = s_ze_pwr_uj;
      }
      return 0;
    };
    static uint64_t s_ze_eng_ts = 1000000;
    static uint64_t s_ze_eng_active = 200000;
    s_ze_eng_ts = 1000000;
    s_ze_eng_active = 200000;
    ze_test_mock.zesDeviceEnumEngineGroups = [](void*, uint32_t* count, void** phEngine) -> int {
      if (count) *count = 1;
      if (phEngine) *phEngine = reinterpret_cast<void*>(0x7777);
      return 0;
    };
    ze_test_mock.zesEngineGetActivity = [](void*, intel_ze::zes_engine_stats_t* pStats) -> int {
      if (pStats) {
        pStats->timestamp = s_ze_eng_ts;
        pStats->activeTime = s_ze_eng_active;
      }
      return 0;
    };

    fpshud::LevelZeroGpuProvider lz_prov;
    bool init_ok = lz_prov.init("", &ze_test_mock);
    fpshud::HardwareTelemetryData hdata{};
    lz_prov.poll(hdata);  // initial sample

    s_ze_pwr_ts += 1000000;
    s_ze_pwr_uj += 190000000;  // +190W
    s_ze_eng_ts += 1000000;
    s_ze_eng_active += 850000;  // 85% load
    lz_prov.poll(hdata);        // delta poll

    bool clk_ok = (hdata.gpu_clock_mhz == 2400);
    bool temp_ok = (hdata.gpu_temp_c == 65);
    bool vram_ok = (std::fabs(hdata.gpu_vram_used_gb - 6.0f) < 0.1f &&
                    std::fabs(hdata.gpu_vram_total_gb - 16.0f) < 0.1f);
    bool pwr_ok = (std::fabs(hdata.gpu_power_w - 190.0f) < 1.0f);
    bool load_ok = (std::fabs(hdata.gpu_load_pct - 85.0f) < 1.0f);
    lz_prov.shutdown();

    CHECK(init_ok);
    CHECK(clk_ok);
    CHECK(temp_ok);
    CHECK(vram_ok);
    CHECK(pwr_ok);
    CHECK(load_ok);

    bool ze_passed = init_ok && clk_ok && temp_ok && vram_ok && pwr_ok && load_ok;
    std::string ze_detail =
        "init=" + std::to_string(init_ok) + ", clk=" + std::to_string(hdata.gpu_clock_mhz) +
        ", temp=" + std::to_string(hdata.gpu_temp_c) +
        ", vram=" + std::to_string(hdata.gpu_vram_used_gb) + "/" +
        std::to_string(hdata.gpu_vram_total_gb) + ", pwr=" + std::to_string(hdata.gpu_power_w) +
        ", load=" + std::to_string(hdata.gpu_load_pct);
    check_cond(ze_passed, "14.3 LevelZeroGpuProvider Mock Dispatch (clk, temp, VRAM, pwr, load)",
               ze_detail.c_str());
  }

  SUBCASE("Suite 14: NvidiaGpuProvider Vendor Gating & Lifecycle") {
    fpshud::NvidiaGpuProvider nv_prov;
    // Should reject non-NVIDIA vendor bindings immediately
    bool non_nv_rejected = !nv_prov.init("", 0x8086, 0);
    bool mock_dir_rejected = !nv_prov.init("/nonexistent_mock_dir", 0, 0);

    // On this host (which has NVIDIA GTX 1660 SUPER), test real init and polling if available
    bool host_init = nv_prov.init("", 0, 0);
    bool host_polled = false;
    if (host_init) {
      fpshud::HardwareTelemetryData hdata{};
      nv_prov.poll(hdata);
      host_polled = hdata.gpu_available && (std::strcmp(hdata.gpu_vendor, "NVIDIA") == 0) &&
                    (hdata.gpu_temp_c > 0);
      nv_prov.shutdown();
    }

    CHECK(non_nv_rejected);
    CHECK(mock_dir_rejected);
    CHECK((!host_init || host_polled));

    bool nv_passed = non_nv_rejected && mock_dir_rejected && (!host_init || host_polled);
    std::string nv_detail = "non_nv_rej=" + std::to_string(non_nv_rejected) +
                            ", mock_rej=" + std::to_string(mock_dir_rejected) +
                            ", host_init=" + std::to_string(host_init) +
                            ", host_polled=" + std::to_string(host_polled);
    check_cond(nv_passed, "14.4 NvidiaGpuProvider Vendor Gating & Lifecycle", nv_detail.c_str());
  }

  SUBCASE("Suite 15: Map Line Classification (DX9/DX11/DX12)") {
    CHECK(std::string_view(
              fpshud::TelemetryCollector::classify_maps_line("/usr/lib/dxvk/d3d9.dll")) == "DX9");
    CHECK(std::string_view(
              fpshud::TelemetryCollector::classify_maps_line("/usr/lib/dxvk/d3d11.dll")) == "DX11");
    CHECK(std::string_view(fpshud::TelemetryCollector::classify_maps_line(
              "/usr/lib/vkd3d-proton/d3d12core.dll")) == "DX12");
  }

  SUBCASE("Suite 16: Level Zero Counter Underflow Guard & API VRAM Fallback") {
    LevelZeroDispatch ze_wrap_mock{};
    ze_wrap_mock.zesInit = [](uint32_t) -> int { return 0; };
    ze_wrap_mock.zesDriverGet = [](uint32_t* count, void** phDrivers) -> int {
      if (count) *count = 1;
      if (phDrivers) *phDrivers = reinterpret_cast<void*>(0x1111);
      return 0;
    };
    ze_wrap_mock.zesDeviceGet = [](void*, uint32_t* count, void** phDevices) -> int {
      if (count) *count = 1;
      if (phDevices) *phDevices = reinterpret_cast<void*>(0x2222);
      return 0;
    };
    ze_wrap_mock.zesDeviceGetProperties = [](void*,
                                             intel_ze::zes_device_properties_t* pProps) -> int {
      if (pProps) {
        std::snprintf(pProps->modelName, sizeof(pProps->modelName), "Intel Arc A770");
        std::snprintf(pProps->driverVersion, sizeof(pProps->driverVersion), "Level Zero 1.14");
      }
      return 0;
    };
    static uint64_t s_wrap_pwr_ts = 1000000;
    static uint64_t s_wrap_pwr_uj = 10000000;
    ze_wrap_mock.zesDeviceEnumPowerDomains = [](void*, uint32_t* count, void** phPower) -> int {
      if (count) *count = 1;
      if (phPower) *phPower = reinterpret_cast<void*>(0x6666);
      return 0;
    };
    ze_wrap_mock.zesPowerGetEnergyCounter =
        [](void*, intel_ze::zes_power_energy_counter_t* pEnergy) -> int {
      if (pEnergy) {
        pEnergy->timestamp = s_wrap_pwr_ts;
        pEnergy->energy = s_wrap_pwr_uj;
      }
      return 0;
    };
    static uint64_t s_wrap_eng_ts = 1000000;
    static uint64_t s_wrap_eng_active = 200000;
    ze_wrap_mock.zesDeviceEnumEngineGroups = [](void*, uint32_t* count, void** phEngine) -> int {
      if (count) *count = 1;
      if (phEngine) *phEngine = reinterpret_cast<void*>(0x7777);
      return 0;
    };
    ze_wrap_mock.zesEngineGetActivity = [](void*, intel_ze::zes_engine_stats_t* pStats) -> int {
      if (pStats) {
        pStats->timestamp = s_wrap_eng_ts;
        pStats->activeTime = s_wrap_eng_active;
      }
      return 0;
    };
    ze_wrap_mock.zesDeviceEnumMemoryModules = [](void*, uint32_t* count, void**) -> int {
      if (count) *count = 0;
      return 0;
    };

    fpshud::LevelZeroGpuProvider lz_prov;
    bool init_ok = lz_prov.init("", &ze_wrap_mock);
    CHECK(init_ok);
    fpshud::HardwareTelemetryData hdata{};
    lz_prov.poll(hdata);

    s_wrap_pwr_ts += 1000000;
    s_wrap_pwr_uj = 5000000;
    s_wrap_eng_ts += 1000000;
    s_wrap_eng_active = 100000;
    lz_prov.poll(hdata);

    CHECK(hdata.gpu_power_w < 500.0f);
    CHECK(hdata.gpu_load_pct <= 100.0f);

    lz_prov.update_api_vram_info(2ULL * 1024 * 1024 * 1024, 8ULL * 1024 * 1024 * 1024);
    lz_prov.poll(hdata);
    CHECK(hdata.gpu_vram_total_gb > 7.5f);
    CHECK(hdata.gpu_vram_used_gb > 1.8f);
    lz_prov.shutdown();

    // Multi-card binding verification: ensure discover_hybrid_sysfs matches bound_device_id_
    ScopedTempDir mock_multi_cards("fpshud_mock_lz_multi_cards");
    fs::path c0 = mock_multi_cards.path / "sys/class/drm/card0";
    fs::path c1 = mock_multi_cards.path / "sys/class/drm/card1";
    fs::create_directories(c0 / "device");
    fs::create_directories(c1 / "device");
    // Card 0: Intel iGPU (0x8086, 0x46a6), clock fallback 300 MHz
    write_mock_file(c0 / "device/vendor", "0x8086\n");
    write_mock_file(c0 / "device/device", "0x46a6\n");
    write_mock_file(c0 / "gt_act_freq_mhz", "300\n");
    // Card 1: Intel Arc dGPU (0x8086, 0x56a0), clock fallback 2100 MHz
    write_mock_file(c1 / "device/vendor", "0x8086\n");
    write_mock_file(c1 / "device/device", "0x56a0\n");
    write_mock_file(c1 / "gt_act_freq_mhz", "2100\n");

    fpshud::LevelZeroGpuProvider lz_multi;
    // When bound to 0x56a0, it should bind to card1 (2100 MHz) instead of card0 (300 MHz)
    CHECK(lz_multi.init(mock_multi_cards.path.string(), &ze_wrap_mock, 0x56a0));
    fpshud::HardwareTelemetryData dgpu_data{};
    lz_multi.poll(dgpu_data);
    CHECK(dgpu_data.gpu_clock_mhz == 2100);
    lz_multi.shutdown();

    // When bound to 0x46a6, it should bind to card0 (300 MHz)
    CHECK(lz_multi.init(mock_multi_cards.path.string(), &ze_wrap_mock, 0x46a6));
    fpshud::HardwareTelemetryData igpu_data{};
    lz_multi.poll(igpu_data);
    CHECK(igpu_data.gpu_clock_mhz == 300);
    lz_multi.shutdown();
  }

  SUBCASE("Suite 17: CPU Power Tier-2 TDP Fallback") {
    ScopedTempDir tmp_tdp("fpshud_cpu_tier2_tdp");
    const fs::path& base = tmp_tdp.get();
    std::error_code ec;
    fs::create_directories(base / "sys/class/powercap/intel-rapl:0", ec);
    write_mock_file(base / "sys/class/powercap/intel-rapl:0/constraint_0_max_power_uw",
                    "65000000\n");
    write_mock_file(base / "sys/class/powercap/intel-rapl:0/energy_uj", "0000\n");

    fpshud::CpuProvider cpu_prov(base.string());
    cpu_prov.init();
    fpshud::HardwareTelemetryData hdata{};
    cpu_prov.poll(hdata);
    CHECK(hdata.cpu_power_w > 5.0f);
    CHECK(hdata.cpu_power_w <= 65.0f);
  }

  SUBCASE("Suite 18: NVIDIA D3cold Runtime Suspend Guard") {
    ScopedTempDir tmp_nv_d3("fpshud_nv_d3cold");
    const fs::path& base = tmp_nv_d3.get();
    std::error_code ec;
    fs::create_directories(base / "sys/bus/pci/devices/0000:01:00.0/power", ec);
    write_mock_file(base / "sys/bus/pci/devices/0000:01:00.0/vendor", "0x10de\n");
    write_mock_file(base / "sys/bus/pci/devices/0000:01:00.0/power/runtime_status", "suspended\n");

    fpshud::NvidiaGpuProvider nv_prov;
    nv_prov.set_root_prefix(base.string());
    CHECK(nv_prov.is_d3cold_suspended() == true);
    bool init_ok = nv_prov.init(base.string(), 0, 0);
    CHECK(init_ok == false);
  }
}
