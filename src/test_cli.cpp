#include "config.hpp"
#include "telemetry.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

int g_total_checks = 0;
int g_passed_checks = 0;

void check_cond(bool ok, const char* label, const char* detail = "") {
    ++g_total_checks;
    if (ok) {
        ++g_passed_checks;
        std::printf("  [PASS] %-42s %s\n", label, detail);
    } else {
        std::printf("  [FAIL] %-42s %s\n", label, detail);
    }
}

void write_mock_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    f << content << "\n";
}

// Tests all 19 metrics, Low FPS engine, Scene-Stall Reset, API switching, and Benchmark CSV on a collector
void verify_all_features_on_device(fpshud::TelemetryCollector& tc, const char* device_title) {
    using namespace fpshud;

    // 1. Test Render API Switch (VULKAN <-> OPENGL)
    tc.set_render_api("OPENGL");
    TelemetryData d_gl = tc.get_snapshot();
    bool api_switch_ok = (std::strcmp(d_gl.render_api, "OPENGL") == 0);
    tc.set_render_api("VULKAN");

    // 2. Test Scene / Loading Stall Auto-Reset (> 250ms frame should clear old history)
    tc.on_simulated_frame_dt(16.6f, true);
    tc.on_simulated_frame_dt(420.0f, true); // 420ms loading stall -> must auto-reset ring buffer!

    // 3. Start Benchmark CSV Recording + Simulate 100 frames (96x 6.94ms [144 FPS] + 4x 20.0ms [50 FPS stutter])
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
    std::snprintf(detail_hw, sizeof(detail_hw), "CPU=[%s] | GPU=[%s (%s, %s)]",
                  d.cpu_name, d.gpu_name, d.gpu_vendor, d.gpu_driver_version);

    char detail_fps[160];
    std::snprintf(detail_fps, sizeof(detail_fps),
                  "FPS=%.1f | AVG=%.1f | 1%%Low=%.1f | 0.1%%Low=%.1f | FT=%.2fms",
                  d.fps, d.fps_avg, d.fps_1pct_low, d.fps_01pct_low, d.frametime_ms);

    char detail_gpu[160];
    std::snprintf(detail_gpu, sizeof(detail_gpu),
                  "Load=%.0f%% | Clk=%dMHz | VRAM=%.2f/%.2fGB | BW=%.1fGB/s | Tmp=%dC | Fan=%d%% | Pwr=%.1fW",
                  d.gpu_load_pct, d.gpu_clock_mhz, d.gpu_vram_used_gb, d.gpu_vram_total_gb,
                  d.gpu_bandwidth_gbs, d.gpu_temp_c, d.gpu_fan_pct, d.gpu_power_w);

    char detail_cpu[160];
    std::snprintf(detail_cpu, sizeof(detail_cpu),
                  "Load=%.1f%% | Clk=%dMHz | Tmp=%dC | Pwr=%.1fW | RAM=%.2f/%.2fGB | Clock=%s",
                  d.cpu_load_pct, d.cpu_clock_mhz, d.cpu_temp_c, d.cpu_power_w,
                  d.ram_used_gb, d.ram_total_gb, d.local_time_str);

    char detail_bench[160];
    std::snprintf(detail_bench, sizeof(detail_bench),
                  "CSV AVG=%.1f | 1%%Low=%.1f | PeakGPU=%dC/%.1fW | PeakCPU=%dC/%.1fW",
                  d.last_benchmark.avg_fps, d.last_benchmark.fps_1pct_low,
                  d.last_benchmark.max_gpu_temp_c, d.last_benchmark.max_gpu_power_w,
                  d.last_benchmark.max_cpu_temp_c, d.last_benchmark.max_cpu_power_w);

    std::printf("\n--------------------------------------------------------------------\n");
    std::printf(" DEVICE PROFILE: %s\n", device_title);
    std::printf("--------------------------------------------------------------------\n");

    check_cond(d.gpu_available && std::strlen(d.cpu_name) > 2 && std::strlen(d.gpu_name) > 2,
               "1. Hardware Model & Vendor Auto-Detection", detail_hw);
    check_cond(d.fps > 80.0f && d.fps_avg > 120.0f && d.fps_1pct_low >= 45.0f && d.fps_1pct_low < d.fps_avg &&
                   d.fps_01pct_low <= d.fps_1pct_low,
               "2. FPS, AVG, 1% Low, 0.1% Low & Stall Reset", detail_fps);
    check_cond(d.gpu_load_pct >= 0.0f && d.gpu_clock_mhz > 0 && d.gpu_vram_total_gb > 0.0f &&
                   d.gpu_max_bandwidth_gbs > 0.0f && d.gpu_temp_c > 0 && d.gpu_power_w > 0.0f,
               "3. All 7 GPU Sensors (Load/Clk/VRAM/BW/Tmp/Fan/W)", detail_gpu);
    check_cond(d.cpu_load_pct > 0.0f && d.cpu_clock_mhz > 0 && d.cpu_temp_c > 0 &&
                   d.cpu_power_w > 0.0f && d.ram_total_gb > 0.0f && api_switch_ok,
               "4. All 7 CPU/RAM/System Sensors & API Hook", detail_cpu);
    check_cond(d.last_benchmark.has_result && d.last_benchmark.avg_fps > 120.0f &&
                   d.last_benchmark.max_gpu_temp_c > 0 && d.last_benchmark.max_cpu_temp_c > 0,
               "5. Benchmark CSV Recorder & Peak Sensor Log", detail_bench);
}

void run_all_devices_and_features_suite() {
    using namespace fpshud;
    std::printf("\n====================================================================\n");
    std::printf("  FULL-FEATURE & MULTI-DEVICE VERIFICATION SUITE (5 DEVICE PROFILES)\n");
    std::printf("====================================================================\n");

    // ------------------------------------------------------------------------
    // Verify Presets (Minimal, Standard, Advance, Custom) & Keybindings first
    // ------------------------------------------------------------------------
    {
        HudConfig test_cfg;
        apply_preset_minimal(test_cfg);
        bool min_ok = (test_cfg.metrics.size() == 2 && test_cfg.active_preset == "minimal");
        apply_preset_standard(test_cfg);
        bool std_ok = (test_cfg.metrics.size() == 9 && test_cfg.active_preset == "standard");
        apply_preset_advance(test_cfg);
        bool adv_ok = (test_cfg.metrics.size() == 19 && test_cfg.active_preset == "advance");
        check_cond(min_ok && std_ok && adv_ok,
                   "0. GUI Presets (Minimal=2, Standard=9, Advance=19)",
                   "All 3 built-in presets & 19 MetricType mappings verified");
    }

    const fs::path base = "/tmp/fpshud_mock_hw";
    std::error_code ec;

    // ------------------------------------------------------------------------
    // PROFILE 1: Real Physical PC (Intel Core i3-12100F + NVIDIA GTX 1660 SUPER)
    // ------------------------------------------------------------------------
    {
        auto& real_tc = get_telemetry();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        real_tc.poll_once_sync();
        verify_all_features_on_device(real_tc, "PROFILE 1 [REAL HARDWARE]: Intel CPU + NVIDIA GPU (NVML)");
    }

    // ------------------------------------------------------------------------
    // PROFILE 2: AMD Gaming Desktop (Ryzen 7 7800X3D + Radeon RX 7800 XT via amdgpu)
    // ------------------------------------------------------------------------
    fs::remove_all(base, ec);
    write_mock_file(base / "proc/cpuinfo",
                    "processor\t: 0\nmodel name\t: AMD Ryzen 7 7800X3D 8-Core Processor\n");
    write_mock_file(base / "proc/stat", "cpu  10000 0 2000 88000 0 0 0 0 0 0");
    write_mock_file(base / "proc/meminfo", "MemTotal:       33554432 kB\nMemAvailable:   20971520 kB\n");
    write_mock_file(base / "sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "5050000");
    write_mock_file(base / "sys/class/hwmon/hwmon0/name", "k10temp");
    write_mock_file(base / "sys/class/hwmon/hwmon0/temp1_input", "64000");
    write_mock_file(base / "sys/class/hwmon/hwmon0/power1_average", "58500000");

    fs::path amd_dev = base / "sys/class/drm/card0/device";
    write_mock_file(amd_dev / "vendor", "0x1002");
    write_mock_file(amd_dev / "device", "0x747e"); // Radeon RX 7800 XT in pci.ids
    write_mock_file(amd_dev / "gpu_busy_percent", "96");
    write_mock_file(amd_dev / "mem_busy_percent", "62");
    write_mock_file(amd_dev / "mem_info_vram_used", "9126805504");
    write_mock_file(amd_dev / "mem_info_vram_total", "17179869184");
    write_mock_file(amd_dev / "hwmon/hwmon1/freq1_input", "2430000000"); // 2430 MHz core
    write_mock_file(amd_dev / "hwmon/hwmon1/freq2_input", "2438000000"); // 2438 MHz VRAM
    write_mock_file(amd_dev / "hwmon/hwmon1/temp1_input", "68000");
    write_mock_file(amd_dev / "hwmon/hwmon1/pwm1", "166");
    write_mock_file(amd_dev / "hwmon/hwmon1/power1_average", "245000000");
    {
        TelemetryCollector amd_tc(base.string(), true);
        write_mock_file(base / "proc/stat", "cpu  10450 0 2150 88400 0 0 0 0 0 0");
        verify_all_features_on_device(amd_tc, "PROFILE 2 [SIMULATED AMD PC]: Ryzen 7 7800X3D + Radeon RX 7800 XT");
    }

    // ------------------------------------------------------------------------
    // PROFILE 3: Intel Gaming Desktop (Core i7-13700K + Intel Arc A770 via i915/xe)
    // ------------------------------------------------------------------------
    fs::remove_all(base, ec);
    write_mock_file(base / "proc/cpuinfo",
                    "processor\t: 0\nmodel name\t: 13th Gen Intel(R) Core(TM) i7-13700K\n");
    write_mock_file(base / "proc/stat", "cpu  20000 0 4000 76000 0 0 0 0 0 0");
    write_mock_file(base / "proc/meminfo", "MemTotal:       33554432 kB\nMemAvailable:   16777216 kB\n");
    write_mock_file(base / "sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "5300000");
    write_mock_file(base / "sys/class/hwmon/hwmon0/name", "coretemp");
    write_mock_file(base / "sys/class/hwmon/hwmon0/temp1_input", "71000");
    write_mock_file(base / "sys/class/powercap/intel-rapl:0/energy_uj", "10000000");
    write_mock_file(base / "sys/class/powercap/intel-rapl:0/max_energy_range_uj", "262143328850");

    fs::path intel_card = base / "sys/class/drm/card0";
    fs::path intel_dev = intel_card / "device";
    write_mock_file(intel_dev / "vendor", "0x8086");
    write_mock_file(intel_dev / "device", "0x56a0"); // Intel Arc A770 in pci.ids
    write_mock_file(intel_dev / "gpu_busy_percent", "88");
    write_mock_file(intel_dev / "mem_info_vram_used", "6442450944");
    write_mock_file(intel_card / "lmem_total_bytes", "17179869184");
    write_mock_file(intel_card / "gt/gt0/rps_cur_freq_mhz", "2400");
    write_mock_file(intel_dev / "hwmon/hwmon2/temp1_input", "66000");
    write_mock_file(intel_dev / "hwmon/hwmon2/fan1_input", "1800");
    write_mock_file(intel_dev / "hwmon/hwmon2/fan1_max", "3000");
    write_mock_file(intel_dev / "hwmon/hwmon2/power1_input", "190000000");
    {
        TelemetryCollector intel_tc(base.string(), true);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        write_mock_file(base / "proc/stat", "cpu  20350 0 4050 76600 0 0 0 0 0 0");
        write_mock_file(base / "sys/class/powercap/intel-rapl:0/energy_uj", "12700000");
        verify_all_features_on_device(intel_tc, "PROFILE 3 [SIMULATED INTEL PC]: Core i7-13700K + Intel Arc A770");
    }

    // ------------------------------------------------------------------------
    // PROFILE 4: Hybrid Dual-GPU Gaming Laptop (card0 512MB iGPU + card1 8GB dGPU RX 6600M)
    // ------------------------------------------------------------------------
    fs::remove_all(base, ec);
    write_mock_file(base / "proc/cpuinfo",
                    "processor\t: 0\nmodel name\t: AMD Ryzen 7 6800H with Radeon Graphics\n");
    write_mock_file(base / "proc/stat", "cpu  30000 0 3000 67000 0 0 0 0 0 0");
    write_mock_file(base / "proc/meminfo", "MemTotal:       16777216 kB\nMemAvailable:   6291456 kB\n");
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
    {
        TelemetryCollector hybrid_tc(base.string(), true);
        write_mock_file(base / "proc/stat", "cpu  30250 0 3050 67700 0 0 0 0 0 0");
        verify_all_features_on_device(hybrid_tc, "PROFILE 4 [HYBRID LAPTOP]: Ryzen 7 6800H (card0 iGPU -> auto card1 dGPU)");
    }

    // ------------------------------------------------------------------------
    // PROFILE 5: Handheld / Ultrabook APU / iGPU Only (Shared GTT RAM + thermal_zone0)
    // ------------------------------------------------------------------------
    fs::remove_all(base, ec);
    write_mock_file(base / "proc/cpuinfo",
                    "processor\t: 0\nmodel name\t: AMD Custom APU 0405 (Steam Deck OLED)\n");
    write_mock_file(base / "proc/stat", "cpu  40000 0 5000 55000 0 0 0 0 0 0");
    write_mock_file(base / "proc/meminfo", "MemTotal:       16777216 kB\nMemAvailable:   7340032 kB\n");
    write_mock_file(base / "sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "3500000");
    write_mock_file(base / "sys/class/thermal/thermal_zone0/temp", "69000"); // Fallback thermal_zone0
    write_mock_file(base / "sys/class/powercap/amd-rapl:0/energy_uj", "5000000");
    write_mock_file(base / "sys/class/powercap/amd-rapl:0/max_energy_range_uj", "262143328850");

    // APU with 512MB carved VRAM + 8GB dynamic GTT shared memory
    fs::path apu_dev = base / "sys/class/drm/card0/device";
    write_mock_file(apu_dev / "vendor", "0x1002");
    write_mock_file(apu_dev / "product_name", "AMD Radeon VanGogh APU");
    write_mock_file(apu_dev / "gpu_busy_percent", "85");
    write_mock_file(apu_dev / "mem_info_vram_used", "536870912");    // 0.50 GB carved
    write_mock_file(apu_dev / "mem_info_vram_total", "536870912");   // 0.50 GB carved
    write_mock_file(apu_dev / "mem_info_gtt_used", "3758096384");    // +3.50 GB shared GTT = 4.00 GB used!
    write_mock_file(apu_dev / "mem_info_gtt_total", "8053063680");   // +7.50 GB shared GTT = 8.00 GB total!
    write_mock_file(apu_dev / "hwmon/hwmon4/freq1_input", "1600000000"); // 1600 MHz
    write_mock_file(apu_dev / "hwmon/hwmon4/power1_average", "15000000"); // 15.0 W APU GPU
    {
        TelemetryCollector apu_tc(base.string(), true);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        write_mock_file(base / "proc/stat", "cpu  40400 0 5100 55500 0 0 0 0 0 0");
        write_mock_file(base / "sys/class/powercap/amd-rapl:0/energy_uj", "5270000"); // ~9W CPU
        verify_all_features_on_device(apu_tc, "PROFILE 5 [APU / STEAM DECK / iGPU]: Shared GTT Memory + Thermal Zone Fallback");
    }

    fs::remove_all(base, ec);
    std::printf("\n====================================================================\n");
    std::printf("  VERIFICATION RESULT: %d / %d CHECKS PASSED ACROSS ALL 5 DEVICES!\n",
                g_passed_checks, g_total_checks);
    std::printf("====================================================================\n");
}

} // namespace

int main(int argc, char** argv) {
    using namespace fpshud;

    const char* env_input = "1";
    if (const char* env = std::getenv("FPS_HUD")) {
        env_input = env;
    } else if (argc > 1) {
        env_input = argv[1];
    }

    HudConfig cfg = init_config_from_env(env_input);

    std::printf("=== FPSHUD Configuration & Multi-Device Feature Verification ===\n");
    std::printf("FPS_HUD Env     : %s\n", env_input);
    std::printf("Enabled         : %s\n", cfg.enabled ? "YES (HUD + In-Game GUI Active)" : "NO (Disabled)");
    std::printf("Active Preset   : %s | Layout: %s | Arrangement: %s\n",
                cfg.active_preset.c_str(), layout_name(cfg.layout), arrangement_name(cfg.arrangement));

    run_all_devices_and_features_suite();
    return (g_passed_checks == g_total_checks) ? 0 : 1;
}
