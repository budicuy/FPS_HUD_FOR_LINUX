#include "doctest.h"
#include "test_common.hpp"

#include "analytics/benchmark_recorder.hpp"
#include "analytics/frametime_analyzer.hpp"
#include "providers/system_provider.hpp"
#include "telemetry.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace fpshud;

TEST_CASE("Telemetry: Analytics & System Provider") {
  SUBCASE("Suite 15.1 & 15.2: FrametimeAnalyzer 100-Frame Simulation & Stall Reset") {
    FrametimeAnalyzer fta;

    // 1. Initial State Verification
    CHECK(fta.get_fps() == 0.0f);
    CHECK(fta.get_fps_avg() == 0.0f);
    CHECK(fta.get_1pct_low() == 0.0f);
    CHECK(fta.get_01pct_low() == 0.0f);
    CHECK(fta.get_frametime_ms() == 0.0f);
    CHECK(fta.get_graph_offset() == 0);

    // 2. 96 frames of 6.944ms (~144 FPS)
    for (int i = 0; i < 96; ++i) {
      fta.on_frame(6.944f, true);
    }
    float pre_stutter_fps = fta.get_fps();
    bool pre_fps_ok = (pre_stutter_fps >= 143.0f && pre_stutter_fps <= 145.0f);
    CHECK_MESSAGE(pre_fps_ok, "Pre-stutter FPS must accurately track ~144 FPS (6.944ms)");

    // 3. Stutter: 4 frames of 20.0ms (50 FPS)
    for (int i = 0; i < 4; ++i) {
      fta.on_frame(20.0f, true);
    }

    float fps = fta.get_fps();
    float fps_avg = fta.get_fps_avg();
    float p1_low = fta.get_1pct_low();
    float p01_low = fta.get_01pct_low();
    float ft_ms = fta.get_frametime_ms();
    const float* ft_hist = fta.get_ft_history();
    const float* fps_hist = fta.get_fps_history();
    int g_offset = fta.get_graph_offset();

    bool ema_fps_ok = (fps >= 85.0f && fps <= 90.0f);
    bool avg_ok = (fps_avg >= 130.0f && fps_avg <= 138.0f);
    bool p1_ok = (p1_low >= 48.0f && p1_low <= 52.0f);
    bool p01_ok = (p01_low >= 48.0f && p01_low <= 52.0f);
    bool ft_ms_ok = (ft_ms >= 11.0f && ft_ms <= 12.0f);

    CHECK_MESSAGE(ema_fps_ok, "EMA smoothed FPS after stutter must be in [85.0, 90.0]");
    CHECK_MESSAGE(avg_ok, "Average FPS must reflect rolling window [130.0, 138.0]");
    CHECK_MESSAGE(p1_ok, "1% Low FPS percentile must identify stutter [48.0, 52.0]");
    CHECK_MESSAGE(p01_ok, "0.1% Low FPS percentile must identify worst tail [48.0, 52.0]");
    CHECK_MESSAGE(p01_low <= p1_low, "0.1% Low must never exceed 1% Low");
    CHECK_MESSAGE(p1_low < fps_avg, "1% Low must be lower than rolling average FPS");
    CHECK_MESSAGE(ft_ms_ok, "Frametime EMA must match corresponding ~87 FPS (~11.4ms)");
    CHECK(ft_hist != nullptr);
    CHECK(fps_hist != nullptr);
    CHECK(g_offset == 100);

    // 4. Ring Buffer Wrap-Around:
    // Feed additional 520 frames to exceed both kGraphHistorySize (120) and kFrameWindowSize (512)
    for (int i = 0; i < 520; ++i) {
      fta.on_frame(10.0f, true);  // 100 FPS
    }
    CHECK(fta.get_graph_offset() == (620 % static_cast<int>(FrametimeAnalyzer::kGraphHistorySize)));
    bool post_wrap_fps_ok = (fta.get_fps() >= 99.0f && fta.get_fps() <= 101.0f);
    bool post_wrap_avg_ok = (fta.get_fps_avg() >= 99.0f && fta.get_fps_avg() <= 101.0f);
    bool post_wrap_p1_ok = (fta.get_1pct_low() >= 99.0f && fta.get_1pct_low() <= 101.0f);
    CHECK_MESSAGE(post_wrap_fps_ok, "Post-wrap EMA FPS must converge to 100 FPS");
    CHECK_MESSAGE(post_wrap_avg_ok, "Post-wrap rolling average FPS must converge to 100 FPS");
    CHECK_MESSAGE(post_wrap_p1_ok, "Post-wrap 1% Low must converge to 100 FPS");

    // 5. Scene Stall Auto-Reset (> 250ms frame) & History Purge
    FrametimeAnalyzer fta_stall;
    for (int i = 0; i < 30; ++i) {
      fta_stall.on_frame(16.6f, true);
    }
    float pre_stall_fps = fta_stall.get_fps();
    CHECK_MESSAGE(pre_stall_fps > 55.0f, "Pre-stall FPS must be > 55 FPS for 16.6ms cadence");

    // 400ms loading stall -> resets rolling ring buffer
    fta_stall.on_frame(400.0f, true);

    // Manual reset clears all displayed metrics
    fta_stall.reset();
    CHECK(fta_stall.get_fps() == 0.0f);
    CHECK(fta_stall.get_fps_avg() == 0.0f);
    CHECK(fta_stall.get_1pct_low() == 0.0f);
    CHECK(fta_stall.get_01pct_low() == 0.0f);
    CHECK(fta_stall.get_frametime_ms() == 0.0f);
    CHECK(fta_stall.get_graph_offset() == 0);

    // 6. Anomalous / Out-of-bounds frame time rejection
    fta_stall.on_frame(0.0001f, true);  // <= 0.001 ms ignored
    fta_stall.on_frame(-10.0f, true);   // negative dt ignored
    fta_stall.on_frame(6000.0f, true);  // > 5000 ms ignored
    fta_stall.on_frame(std::numeric_limits<float>::quiet_NaN(), true);
    fta_stall.on_frame(std::numeric_limits<float>::infinity(), true);
    CHECK(fta_stall.get_fps() == 0.0f);

    // 7. Accelerated SIMD frametime reduction verification
    std::vector<float> simd_test_data(256, 16.666667f);
    SimdStatsResult simd_res = compute_frametime_stats_simd(simd_test_data);
    CHECK(simd_res.total_ms == doctest::Approx(256 * 16.666667).epsilon(0.01));
    CHECK(simd_res.min_val == doctest::Approx(16.666667f));
    CHECK(simd_res.max_val == doctest::Approx(16.666667f));

    // Empty vector handling
    std::vector<float> empty_data;
    SimdStatsResult empty_res = compute_frametime_stats_simd(empty_data);
    CHECK(empty_res.total_ms == 0.0);
    CHECK(empty_res.min_val == 0.0f);
    CHECK(empty_res.max_val == 0.0f);

    // Multi-element vector with non-VSize count (11 elements)
    std::vector<float> multi_vec = {16.6f, 8.33f, 33.3f, 12.5f, 20.0f, 6.94f,
                                    14.2f, 18.1f, 22.0f, 10.0f, 15.0f};
    SimdStatsResult r_multi = compute_frametime_stats_simd(multi_vec);
    CHECK(r_multi.min_val == 6.94f);
    CHECK(r_multi.max_val == 33.3f);

    // All non-finite vector returns clean zeros
    std::vector<float> nan_only_vec = {NAN, INFINITY, -INFINITY};
    SimdStatsResult r_nan = compute_frametime_stats_simd(nan_only_vec);
    CHECK(r_nan.total_ms == 0.0);
    CHECK(r_nan.min_val == 0.0f);
    CHECK(r_nan.max_val == 0.0f);

    // Large vector with >512 SIMD chunks (5,003 elements) mixed with NaN/+Inf/-Inf
    std::vector<float> large_vec(5003, 10.0f);
    large_vec[0] = NAN;
    large_vec[100] = INFINITY;
    large_vec[2049] = -INFINITY;
    large_vec[2500] = 2.5f;
    large_vec[5002] = 30.0f;
    SimdStatsResult r_large = compute_frametime_stats_simd(large_vec);
    CHECK(std::fabs(r_large.total_ms - 50012.5) < 0.01);
    CHECK(r_large.min_val == 2.5f);
    CHECK(r_large.max_val == 30.0f);
  }

  SUBCASE("Suite 15.3: BenchmarkRecorder Lifecycle, Peak Sensors & CSV Output") {
    ScopedTempDir bench_dir("fpshud_test_bench_");
    fs::path csv_file = bench_dir / "test_benchmark.csv";

    BenchmarkRecorder recorder(csv_file.string());
    CHECK(!recorder.is_active());
    CHECK(!recorder.has_result());
    CHECK(recorder.get_elapsed_sec() == 0.0f);
    CHECK(recorder.get_csv_path() == csv_file.string());

    recorder.start();
    CHECK(recorder.is_active());
    CHECK(recorder.get_elapsed_sec() >= 0.0f);

    TelemetryData mock_snap{};
    mock_snap.gpu_temp_c = 68;
    mock_snap.gpu_power_w = 210.5f;
    mock_snap.cpu_temp_c = 72;
    mock_snap.cpu_power_w = 65.2f;

    for (int i = 0; i < 96; ++i) {
      recorder.on_frame(6.944f, mock_snap);
    }
    for (int i = 0; i < 4; ++i) {
      recorder.on_frame(20.0f, mock_snap);
    }

    recorder.stop();
    CHECK(!recorder.is_active());
    CHECK(recorder.has_result());

    BenchmarkSummary summary = recorder.get_summary();
    CHECK(summary.has_result);
    bool bench_avg_ok = (summary.avg_fps >= 130.0f && summary.avg_fps <= 138.0f);
    bool bench_1pct_ok = (summary.fps_1pct_low >= 48.0f && summary.fps_1pct_low <= 52.0f);
    bool bench_gpu_w_ok = (summary.max_gpu_power_w >= 210.0f && summary.max_gpu_power_w <= 211.0f);
    bool bench_cpu_w_ok = (summary.max_cpu_power_w >= 65.0f && summary.max_cpu_power_w <= 66.0f);

    CHECK_MESSAGE(bench_avg_ok, "Benchmark average FPS must be in [130.0, 138.0]");
    CHECK_MESSAGE(bench_1pct_ok, "Benchmark 1% low FPS must be in [48.0, 52.0]");
    CHECK_MESSAGE(summary.fps_01pct_low <= summary.fps_1pct_low,
                  "Benchmark 0.1% low must not exceed 1% low");
    CHECK(summary.max_gpu_temp_c == 68);
    CHECK_MESSAGE(bench_gpu_w_ok, "Peak GPU power must be tracked");
    CHECK(summary.max_cpu_temp_c == 72);
    CHECK_MESSAGE(bench_cpu_w_ok, "Peak CPU power must be tracked");
    CHECK(summary.duration_sec >= 0.0);
    CHECK(summary.min_fps > 0.0f);
    CHECK(summary.max_fps >= summary.avg_fps);

    // Verify CSV file generation & schema header
    std::error_code ec;
    CHECK(fs::exists(csv_file, ec));
    CHECK(fs::file_size(csv_file, ec) > 0);

    std::string csv_content = read_text_file(csv_file);
    CHECK(csv_content.find("Timestamp") != std::string::npos);
    CHECK(csv_content.find("Avg_FPS") != std::string::npos);
    CHECK(csv_content.find("1Pct_Low_FPS") != std::string::npos);
    CHECK(csv_content.find("0.1Pct_Low_FPS") != std::string::npos);
    CHECK(csv_content.find("Max_GPU_Temp_C") != std::string::npos);
    CHECK(csv_content.find("Max_CPU_Watt") != std::string::npos);

    // Toggle API verification
    recorder.toggle();
    CHECK(recorder.is_active());
    recorder.toggle();
    CHECK(!recorder.is_active());

    // Symlink / O_NOFOLLOW security verification
    fs::path target_file = bench_dir / "protected_target.txt";
    fs::path symlink_file = bench_dir / "symlink_exploit.csv";
    write_mock_file(target_file, "SENSITIVE_DATA_DO_NOT_CORRUPT");

    std::error_code sym_ec;
    fs::create_symlink(target_file, symlink_file, sym_ec);
    CHECK(!sym_ec);

    // Direct POSIX check for O_NOFOLLOW rejection
    int nofollow_fd =
        ::open(symlink_file.c_str(), O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    CHECK(nofollow_fd < 0);
    CHECK(errno == ELOOP);

    // BenchmarkRecorder symlink attack neutralization
    BenchmarkRecorder rogue_recorder(symlink_file.string());
    rogue_recorder.start();
    rogue_recorder.on_frame(16.6f, mock_snap);
    rogue_recorder.stop();

    // Verify target file was untouched
    std::string target_content = read_text_file(target_file);
    CHECK(target_content.find("SENSITIVE_DATA_DO_NOT_CORRUPT") != std::string::npos);
  }

  SUBCASE("Suite 15.4: SystemProvider Sysfs & Proc Polling") {
    ScopedTempDir sys_dir("fpshud_test_sys_");

    // 1. Mock /proc/diskstats
    write_mock_file(sys_dir / "proc/diskstats",
                    " 259       0 nvme0n1 1000 0 20000 100 2000 0 40000 200 0 300 300 0 0 0 0\n"
                    " 259       1 nvme0n1p1 500 0 10000 50 1000 0 20000 100 0 150 150 0 0 0 0\n");

    // 2. Mock /sys/class/hwmon for NVMe temp
    fs::path hm_dir = sys_dir / "sys/class/hwmon/hwmon0";
    write_mock_file(hm_dir / "name", "nvme\n");
    write_mock_file(hm_dir / "temp1_input", "42000\n");

    // 3. Mock /proc/net/dev
    write_mock_file(
        sys_dir / "proc/net/dev",
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets "
        "errs drop fifo colls carrier compressed\n"
        "    lo: 100000       0    0    0    0     0          0         0   100000       0    "
        "0    0    0     0       0          0\n"
        "eth0: 10485760       0    0    0    0     0          0         0  5242880       0    "
        "0    0    0     0       0          0\n");

    // 4. Mock /sys/class/power_supply/BAT0
    fs::path bat_dir = sys_dir / "sys/class/power_supply/BAT0";
    write_mock_file(bat_dir / "type", "Battery\n");
    write_mock_file(bat_dir / "capacity", "85\n");
    write_mock_file(bat_dir / "status", "Discharging\n");
    write_mock_file(bat_dir / "power_now", "15000000\n");

    SystemProvider sys_prov(sys_dir.string());
    sys_prov.init();
    HardwareTelemetryData hdata{};
    sys_prov.poll(hdata);  // initial sample

    // Delta sample after sleep
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    write_mock_file(sys_dir / "proc/diskstats",
                    " 259       0 nvme0n1 1100 0 40000 150 2100 0 60000 250 0 400 400 0 0 0 0\n");
    write_mock_file(
        sys_dir / "proc/net/dev",
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets "
        "errs drop fifo colls carrier compressed\n"
        "    lo: 100000       0    0    0    0     0          0         0   100000       0    "
        "0    0    0     0       0          0\n"
        "eth0: 20971520       0    0    0    0     0          0         0 10485760       0    "
        "0    0    0     0       0          0\n");

    sys_prov.poll(hdata);  // delta sample

    CHECK_MESSAGE(hdata.disk_read_mbs > 0.0f, "Disk read MB/s must be calculated from delta");
    CHECK_MESSAGE(hdata.disk_write_mbs > 0.0f, "Disk write MB/s must be calculated from delta");
    CHECK(hdata.disk_temp_available);
    CHECK(hdata.disk_temp_c == 42);
    CHECK_MESSAGE(hdata.net_rx_mbs > 0.0f, "Network download MB/s must be calculated from delta");
    CHECK_MESSAGE(hdata.net_tx_mbs > 0.0f, "Network upload MB/s must be calculated from delta");
    CHECK(hdata.battery_available);
    CHECK(hdata.battery_pct == 85.0f);
    CHECK_MESSAGE(std::fabs(hdata.battery_power_w - 15.0f) < 0.1f,
                  "Battery power must parse 15000000 uW to 15.0 W");
    CHECK(std::strcmp(hdata.battery_status, "Discharging") == 0);
    CHECK(!hdata.battery_charging);
    CHECK(std::strlen(hdata.local_time_str) >= 4);
    CHECK(std::strchr(hdata.local_time_str, ':') != nullptr);

    // 5. Desktop PC Profile (No battery / power_supply)
    ScopedTempDir desktop_dir("fpshud_test_desktop_");
    SystemProvider desktop_prov(desktop_dir.string());
    desktop_prov.init();
    HardwareTelemetryData desktop_data{};
    desktop_prov.poll(desktop_data);
    CHECK(!desktop_data.battery_available);
    CHECK(!desktop_data.disk_temp_available);

    // 6. Alternative Battery: Charging & current_now + voltage_now calculation
    ScopedTempDir bat_alt_dir("fpshud_test_bat_alt_");
    fs::path bat1_dir = bat_alt_dir / "sys/class/power_supply/BAT1";
    write_mock_file(bat1_dir / "type", "Battery\n");
    write_mock_file(bat1_dir / "capacity", "92\n");
    write_mock_file(bat1_dir / "status", "Charging\n");
    write_mock_file(bat1_dir / "current_now", "2500000\n");   // 2.5 A
    write_mock_file(bat1_dir / "voltage_now", "12000000\n");  // 12.0 V -> 30.0 W

    SystemProvider bat_alt_prov(bat_alt_dir.string());
    bat_alt_prov.init();
    HardwareTelemetryData bat_alt_data{};
    bat_alt_prov.poll(bat_alt_data);
    CHECK(bat_alt_data.battery_available);
    CHECK(bat_alt_data.battery_pct == 92.0f);
    CHECK(bat_alt_data.battery_charging);
    CHECK(std::strcmp(bat_alt_data.battery_status, "Charging") == 0);
    CHECK_MESSAGE(std::fabs(bat_alt_data.battery_power_w - 30.0f) < 0.2f,
                  "Battery power from current*voltage (2.5A * 12V = 30W)");

    // 7. SATA / drivetemp Hwmon Fallback
    ScopedTempDir sata_dir("fpshud_test_sata_");
    fs::path sata_hm_dir = sata_dir / "sys/class/hwmon/hwmon1";
    write_mock_file(sata_hm_dir / "name", "drivetemp\n");
    write_mock_file(sata_hm_dir / "temp1_input", "38000\n");  // 38°C

    SystemProvider sata_prov(sata_dir.string());
    sata_prov.init();
    HardwareTelemetryData sata_data{};
    sata_prov.poll(sata_data);
    CHECK(sata_data.disk_temp_available);
    CHECK(sata_data.disk_temp_c == 38);
  }

  SUBCASE("Suite 15.5: BenchmarkRecorder Concurrency & Zero-Stall Stop") {
    // 1. Basic race & frame counting test
    {
      ScopedTempDir tmp_bench("fpshud_bench_race");
      fpshud::BenchmarkRecorder rec;
      rec.set_csv_path((tmp_bench / "race_bench.csv").string());
      fpshud::TelemetryData dummy{};
      rec.start(dummy);
      for (int i = 0; i < 1000; ++i) {
        rec.on_frame(6.94f, dummy);
        CHECK(rec.get_elapsed_sec() >= 0.0f);
      }
      rec.stop(dummy);
      CHECK(rec.has_result());
      CHECK(rec.get_last_summary().total_frames == 1000);
    }

    // 2. High-contention concurrent render & reader threads stress test
    {
      ScopedTempDir tmp_bench("fpshud_bench_stress");
      fpshud::BenchmarkRecorder rec((tmp_bench / "concurrent_bench.csv").string());
      fpshud::TelemetryData dummy{};
      dummy.cpu_temp_c = 55;
      dummy.cpu_power_w = 45.0f;
      dummy.gpu_temp_c = 65;
      dummy.gpu_power_w = 120.0f;

      rec.start(dummy);
      std::atomic<bool> stop_flag{false};
      std::atomic<int> frames_rendered{0};

      // Thread 1: Render thread invoking on_frame and reading elapsed time
      std::thread render_thread([&]() {
        while (!stop_flag.load(std::memory_order_relaxed)) {
          rec.on_frame(6.944f, dummy);
          float el = rec.get_elapsed_sec();
          CHECK(el >= 0.0f);
          frames_rendered.fetch_add(1, std::memory_order_relaxed);
          std::this_thread::yield();
        }
      });

      // Thread 2: UI / Polling thread reading status & last summary
      std::thread ui_thread([&]() {
        while (!stop_flag.load(std::memory_order_relaxed)) {
          (void)rec.is_active();
          (void)rec.has_result();
          (void)rec.get_elapsed_sec();
          (void)rec.get_last_summary();
          std::this_thread::yield();
        }
      });

      // Let threads run concurrently for 50 milliseconds
      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      // Stop recorder while render and UI threads are actively running
      rec.stop(dummy);
      stop_flag.store(true, std::memory_order_relaxed);

      render_thread.join();
      ui_thread.join();

      CHECK(!rec.is_active());
      CHECK(rec.has_result());
      BenchmarkSummary sum = rec.get_last_summary();
      CHECK(sum.has_result);
      CHECK(sum.total_frames > 0);
      CHECK(sum.total_frames <= static_cast<size_t>(frames_rendered.load()));
      CHECK(sum.avg_fps > 0.0f);
      CHECK(sum.duration_sec >= 0.0);
    }
  }
}
