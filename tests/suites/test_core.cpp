#include "doctest.h"
#include "test_common.hpp"

#include "adaptive_vfr.hpp"
#include "analytics/benchmark_recorder.hpp"
#include "analytics/frametime_analyzer.hpp"
#include "config.hpp"
#include "input_manager.hpp"
#include "logger.hpp"
#include "overlay_ui.hpp"
#include "pack_indexing.hpp"
#include "recorder_ipc.hpp"
#include "telemetry.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <inplace_vector>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace fpshud;

TEST_CASE("Core: ISO C++26 Modernization Suite") {
  SUBCASE("C++26 standard and compiler feature macros") {
    bool cpp26_standard_ok = (__cplusplus >= 202400L);
    CHECK_MESSAGE(cpp26_standard_ok, "ISO C++26 dialect requirement (__cplusplus >= 202400L)");

#if defined(__cpp_lib_inplace_vector)
    bool inplace_vec_macro_ok = (__cpp_lib_inplace_vector >= 202406L);
#else
    bool inplace_vec_macro_ok = false;
#endif
    CHECK_MESSAGE(inplace_vec_macro_ok,
                  "std::inplace_vector feature test macro (__cpp_lib_inplace_vector >= 202406L)");

#if defined(__cpp_pack_indexing)
    bool pack_indexing_ok = (__cpp_pack_indexing >= 202311L);
#else
    bool pack_indexing_ok = false;
#endif
    CHECK_MESSAGE(pack_indexing_ok,
                  "C++26 pack indexing feature test macro (__cpp_pack_indexing >= 202311L)");

    CHECK_MESSAGE(fpshud::has_simd_frametime_backend(),
                  "Hardware SIMD frametime acceleration backend must be active");
  }

  SUBCASE("std::inplace_vector container semantics and noexcept guarantees") {
    std::inplace_vector<int, 4> test_iv;
    CHECK(static_cast<bool>(test_iv.try_push_back(100)));
    CHECK(static_cast<bool>(test_iv.try_push_back(200)));
    CHECK(test_iv.size() == 2);
    CHECK(test_iv.capacity() == 4);
    CHECK(test_iv[0] == 100);
    CHECK(test_iv[1] == 200);

    std::inplace_vector<int, 2> bounded_iv;
    CHECK(static_cast<bool>(bounded_iv.try_push_back(1)));
    CHECK(static_cast<bool>(bounded_iv.try_push_back(2)));
    CHECK(!static_cast<bool>(bounded_iv.try_push_back(3)));
    CHECK(bounded_iv.size() == 2);

    static_assert(noexcept(std::declval<const HudConfig&>().has_metric(MetricType::FPS)));
    static_assert(noexcept(std::declval<HudConfig&>().set_metric(MetricType::FPS, true)));
  }

  SUBCASE("Pack indexing utility, type traits, and rvalue lifetime safety") {
    IpcPacketHeader hdr;
    [&]<typename T = TelemetryData>() {
      static_assert(
          !requires(T td) { td.ram_speed_mts; },
          "ram_speed_mts must be removed from TelemetryData");
      static_assert(
          !requires(T td) { td.ram_type; }, "ram_type must be removed from TelemetryData");
    }();
    CHECK(hdr.get_field<0>() == kRecorderIpcMagic);
    CHECK(hdr.is_valid());

    static_assert(
        std::is_same_v<decltype(fpshud::pack_element<2>("first", 123, 45.67f, true)), float>);
    int lv = 10;
    const int clv = 20;
    static_assert(std::is_same_v<decltype(fpshud::pack_element<0>(lv, clv)), int&>);
    static_assert(std::is_same_v<decltype(fpshud::pack_element<1>(lv, clv)), const int&>);
    fpshud::pack_element<0>(lv, clv) = 77;
    CHECK(lv == 77);

    const auto& safe_ref = fpshud::pack_element<1>(lv, std::string("safe_lifetime"));
    CHECK(safe_ref == "safe_lifetime");

    auto move_ptr = fpshud::pack_element<0>(std::make_unique<int>(99), 42);
    REQUIRE(move_ptr != nullptr);
    CHECK(*move_ptr == 99);

    auto val = fpshud::pack_element<2>("first", 123, 45.67f, true);
    static_assert(std::is_same_v<fpshud::PackType<2, const char*, int, float, bool>, float>);
    CHECK(val == 45.67f);
  }

  SUBCASE("SIMD frametime analytics parity and edge cases") {
    // 1. Empty vector returns total_ms == 0.0
    std::vector<float> empty_vec;
    SimdStatsResult r_empty = compute_frametime_stats_simd(empty_vec);
    CHECK(r_empty.total_ms == 0.0);
    CHECK(r_empty.min_val == 0.0f);
    CHECK(r_empty.max_val == 0.0f);

    // 2. Single element returns min_val == 16.6f && max_val == 16.6f
    std::vector<float> single_vec = {16.6f};
    SimdStatsResult r_single = compute_frametime_stats_simd(single_vec);
    CHECK(std::fabs(r_single.total_ms - 16.6) < 1e-4);
    CHECK(r_single.min_val == 16.6f);
    CHECK(r_single.max_val == 16.6f);

    // 3. Multi-element vector with non-VSize count (11 elements)
    std::vector<float> multi_vec = {16.6f, 8.33f, 33.3f, 12.5f, 20.0f, 6.94f,
                                    14.2f, 18.1f, 22.0f, 10.0f, 15.0f};
    SimdStatsResult r_multi = compute_frametime_stats_simd(multi_vec);
    double expected_total = 0.0;
    float expected_min = multi_vec[0];
    float expected_max = multi_vec[0];
    for (float v : multi_vec) {
      expected_total += static_cast<double>(v);
      if (v < expected_min) expected_min = v;
      if (v > expected_max) expected_max = v;
    }
    CHECK(std::fabs(r_multi.total_ms - expected_total) < 1e-4);
    CHECK(r_multi.min_val == expected_min);
    CHECK(r_multi.max_val == expected_max);

    // 4. All non-finite vector returns clean zeros
    std::vector<float> nan_only_vec = {NAN, INFINITY, -INFINITY};
    SimdStatsResult r_nan = compute_frametime_stats_simd(nan_only_vec);
    CHECK(r_nan.total_ms == 0.0);
    CHECK(r_nan.min_val == 0.0f);
    CHECK(r_nan.max_val == 0.0f);

    // 5. >512 SIMD chunks (5,003 elements) mixed with NaN/+Inf/-Inf
    std::vector<float> large_vec(5003, 10.0f);
    large_vec[0] = NAN;
    large_vec[100] = INFINITY;
    large_vec[2049] = -INFINITY;
    large_vec[2500] = 2.5f;
    large_vec[5002] = 30.0f;
    // Remaining finite elements: 4998 * 10.0 + 2.5 + 30.0 = 50012.5
    SimdStatsResult r_large = compute_frametime_stats_simd(large_vec);
    CHECK(std::fabs(r_large.total_ms - 50012.5) < 0.01);
    CHECK(r_large.min_val == 2.5f);
    CHECK(r_large.max_val == 30.0f);
  }
}

TEST_CASE("Core Config") {
  ScopedTempDir tmp("fpshud_keybind_neg1");
  const char* prev_xdg = std::getenv("XDG_CONFIG_HOME");
  std::string saved_xdg_env = prev_xdg ? prev_xdg : "";
  ::setenv("XDG_CONFIG_HOME", tmp.c_str(), 1);

  fpshud::HudConfig cfg{};
  CHECK(cfg.record_codec == 0);
  CHECK(cfg.record_bitrate_kbps == 20000);
  CHECK(cfg.record_cq_level == 22);
  cfg.key_toggle_gui = -1;
  cfg.key_toggle_hud = -1;
  cfg.key_benchmark = -1;
  cfg.key_record = -1;
  cfg.bind_toggle_gui = {29, 88};  // Ctrl (29) + F12 (88)
  fpshud::get_mutable_config() = cfg;
  fpshud::save_config_to_disk();

  fpshud::get_mutable_config().key_toggle_gui = 0;
  fpshud::load_config_from_disk();
  const auto& loaded = fpshud::get_global_config();
  CHECK(loaded.key_toggle_gui == -1);
  CHECK(loaded.key_toggle_hud == -1);
  CHECK(loaded.key_benchmark == -1);
  CHECK(loaded.key_record == -1);
  CHECK(loaded.bind_toggle_gui.main_key == 88);
  CHECK(loaded.bind_toggle_gui.mod_key == 29);

  if (prev_xdg) {
    ::setenv("XDG_CONFIG_HOME", saved_xdg_env.c_str(), 1);
  } else {
    ::unsetenv("XDG_CONFIG_HOME");
  }
}

TEST_CASE("Core: GUI Presets, INI Persistence & Video Encoders") {
  SUBCASE("GUI Presets and Metrics Configuration") {
    HudConfig test_cfg;
    apply_preset_minimal(test_cfg);
    CHECK(test_cfg.metrics.size() == 2);
    CHECK(test_cfg.active_preset == "minimal");

    apply_preset_standard(test_cfg);
    CHECK(test_cfg.metrics.size() == 9);
    CHECK(test_cfg.active_preset == "standard");

    apply_preset_advance(test_cfg);
    CHECK(test_cfg.metrics.size() == kTotalMetricCount);
    CHECK(kTotalMetricCount == 31);
    CHECK(test_cfg.active_preset == "advance");
    CHECK(test_cfg.layout == LayoutPosition::TopCenter);

    fs::path rroot = find_repo_root();
    if (!rroot.empty()) {
      std::string cfg_hpp = read_text_file(rroot / "src" / "core" / "config.hpp");
      std::string cfg_cpp = read_text_file(rroot / "src" / "core" / "config.cpp");
      CHECK(cfg_hpp.find("gui_keybind_name") == std::string::npos);
      CHECK(cfg_hpp.find("hud_keybind_name") == std::string::npos);
      CHECK(cfg_hpp.find("bench_keybind_name") == std::string::npos);
      CHECK(cfg_hpp.find("record_keybind_name") == std::string::npos);
      CHECK(cfg_hpp.find("metric_name(") == std::string::npos);
      CHECK(cfg_hpp.find("background_name(") == std::string::npos);
      CHECK(cfg_hpp.find("font_family_name(") == std::string::npos);
      CHECK(cfg_hpp.find("arrangement_name") == std::string::npos);
      CHECK(cfg_hpp.find("CenterTop") == std::string::npos);
      CHECK(cfg_hpp.find("CenterBottom") == std::string::npos);
      CHECK(cfg_cpp.find("LayoutPosition::CenterTop") == std::string::npos);
      CHECK(cfg_cpp.find("arrangement_name") == std::string::npos);
    }

    test_cfg.set_metric(MetricType::Battery, true);
    test_cfg.set_metric(MetricType::GpuVramClock, true);
    test_cfg.set_metric(MetricType::CpuAllCoreLoad, true);
    test_cfg.set_metric(MetricType::CpuAllCoreClock, true);
    test_cfg.set_metric(MetricType::GpuModel, true);
    test_cfg.set_metric(MetricType::CpuModel, true);
    CHECK(test_cfg.has_metric(MetricType::Battery));
    CHECK(test_cfg.has_metric(MetricType::GpuVramClock));
    CHECK(test_cfg.has_metric(MetricType::CpuAllCoreLoad));
    CHECK(test_cfg.has_metric(MetricType::CpuAllCoreClock));
    CHECK(test_cfg.has_metric(MetricType::GpuModel));
    CHECK(test_cfg.has_metric(MetricType::CpuModel));
    CHECK(std::strcmp(layout_name(LayoutPosition::TopCenter), "Top-Center") == 0);
    CHECK(std::strcmp(layout_name(LayoutPosition::BottomCenter), "Bottom-Center") == 0);

    TelemetryData td_check;
    td_check.battery_available = true;
    td_check.battery_pct = 75.5f;
    td_check.battery_power_w = 12.3f;
    td_check.battery_charging = true;
    std::strncpy(td_check.battery_status, "Charging", sizeof(td_check.battery_status));
    td_check.cpu_core_count = 16;
    td_check.cpu_core_loads_pct[0] = 45;
    td_check.cpu_core_clocks_mhz[0] = 5050;
    CHECK(td_check.battery_available);
    CHECK(td_check.battery_pct == 75.5f);
    CHECK(td_check.cpu_core_count == 16);
    CHECK(td_check.cpu_core_loads_pct[0] == 45);
  }

  SUBCASE("GUI Scale and Keybind INI Persistence") {
    ScopedTempDir tmp("fpshud_core_scale_");
    fs::path ini_path = tmp / "test_gui_scale.ini";

    HudConfig test_cfg;
    test_cfg.gui_scale = 1.35f;
    test_cfg.dim_labels_enabled = false;
    test_cfg.bind_record = Keybind{56, 33};  // LeftAlt + F
    CHECK(save_ini_file(ini_path, test_cfg, "Test Scale"));

    HudConfig loaded_cfg;
    CHECK(load_ini_file(ini_path, loaded_cfg));
    CHECK(std::fabs(loaded_cfg.gui_scale - 1.35f) < 0.01f);
    CHECK(loaded_cfg.dim_labels_enabled == false);
    CHECK(loaded_cfg.bind_record.mod_key == 56);
    CHECK(loaded_cfg.bind_record.main_key == 33);
    CHECK(format_keybind(loaded_cfg.bind_record) == "Alt + F");
  }

  SUBCASE("Custom Preset Persistence in Single Config File") {
    ScopedTempDir tmp_xdg("fpshud_xdg_");
    const char* prev_xdg = std::getenv("XDG_CONFIG_HOME");
    std::string saved_xdg_env = prev_xdg ? prev_xdg : "";
    ::setenv("XDG_CONFIG_HOME", tmp_xdg.c_str(), 1);

    HudConfig custom_fresh;
    apply_preset_minimal(custom_fresh);
    apply_preset_custom(custom_fresh);
    CHECK(custom_fresh.active_preset == "custom");
    CHECK(custom_fresh.metrics.size() == 9);
    CHECK(std::fabs(custom_fresh.font_size_px - 21.0f) < 0.01f);

    HudConfig custom_saved;
    apply_preset_minimal(custom_saved);
    custom_saved.font_size_px = 26.0f;
    CHECK(save_config_to_disk(custom_saved));

    HudConfig custom_loaded;
    apply_preset_advance(custom_loaded);
    apply_preset_custom(custom_loaded);
    CHECK(custom_loaded.active_preset == "custom");
    CHECK(custom_loaded.metrics.size() == 2);
    CHECK(std::fabs(custom_loaded.font_size_px - 26.0f) < 0.01f);

    if (prev_xdg) {
      ::setenv("XDG_CONFIG_HOME", saved_xdg_env.c_str(), 1);
    } else {
      ::unsetenv("XDG_CONFIG_HOME");
    }
  }

  SUBCASE("Custom Position Layout and Clamping") {
    ScopedTempDir tmp("fpshud_pos_");
    fs::path pos_ini = tmp / "test_custom_pos.ini";

    HudConfig custom_pos_cfg;
    custom_pos_cfg.layout = LayoutPosition::Custom;
    custom_pos_cfg.custom_pos_x = 0.75f;
    custom_pos_cfg.custom_pos_y = 0.40f;
    CHECK(save_ini_file(pos_ini, custom_pos_cfg, "Test Custom Pos"));

    HudConfig loaded_pos_cfg;
    CHECK(load_ini_file(pos_ini, loaded_pos_cfg));
    CHECK(loaded_pos_cfg.layout == LayoutPosition::Custom);
    CHECK(std::fabs(loaded_pos_cfg.custom_pos_x - 0.75f) < 0.005f);
    CHECK(std::fabs(loaded_pos_cfg.custom_pos_y - 0.40f) < 0.005f);
    CHECK(std::strcmp(layout_name(LayoutPosition::Custom), "Custom") == 0);

    float clamp_x1 = 0.0f, clamp_y1 = 0.0f;
    loaded_pos_cfg.custom_pos_x = 1.50f;
    loaded_pos_cfg.custom_pos_y = 1.50f;
    compute_clamped_hud_pos(loaded_pos_cfg, 1920.0f, 1080.0f, 300.0f, 200.0f, clamp_x1, clamp_y1);
    CHECK(std::fabs(clamp_x1 - 1620.0f) < 0.01f);
    CHECK(std::fabs(clamp_y1 - 880.0f) < 0.01f);

    float clamp_x2 = 0.0f, clamp_y2 = 0.0f;
    loaded_pos_cfg.custom_pos_x = -0.50f;
    loaded_pos_cfg.custom_pos_y = -0.50f;
    compute_clamped_hud_pos(loaded_pos_cfg, 1920.0f, 1080.0f, 300.0f, 200.0f, clamp_x2, clamp_y2);
    CHECK(std::fabs(clamp_x2 - 0.0f) < 0.01f);
    CHECK(std::fabs(clamp_y2 - 0.0f) < 0.01f);

    reset_layout_to_default(loaded_pos_cfg);
    CHECK(loaded_pos_cfg.layout == LayoutPosition::TopLeft);
    CHECK(loaded_pos_cfg.arrangement == ArrangementMode::Stack);
    CHECK(std::fabs(loaded_pos_cfg.custom_pos_x - 0.01f) < 0.005f);
    CHECK(std::fabs(loaded_pos_cfg.custom_pos_y - 0.02f) < 0.005f);
  }

  SUBCASE("Industry-Standard HEVC + CQP Config & Resolution Scaling") {
    ScopedTempDir tmp("fpshud_rec_");
    fs::path rec_ini = tmp / "test_recorder_cfg.ini";

    HudConfig rec_cfg;
    rec_cfg.record_codec = 1;         // HEVC
    rec_cfg.record_quality_mode = 0;  // Adaptive CQP
    rec_cfg.record_cq_level = 20;     // CQ 20
    rec_cfg.record_fps = 60;
    rec_cfg.record_container = 0;  // MP4
    CHECK(save_ini_file(rec_ini, rec_cfg, "Test Recorder Config"));

    HudConfig loaded_rec_cfg;
    CHECK(load_ini_file(rec_ini, loaded_rec_cfg));
    CHECK(loaded_rec_cfg.record_codec == 1);
    CHECK(loaded_rec_cfg.record_quality_mode == 0);
    CHECK(loaded_rec_cfg.record_cq_level == 20);
    CHECK(loaded_rec_cfg.record_fps == 60);
    CHECK(loaded_rec_cfg.record_container == 0);

    uint32_t rw = 0, rh = 0;
    resolve_recording_dimensions(0, 1920, 1080, rw, rh);
    CHECK(rw == 1920);
    CHECK(rh == 1080);

    resolve_recording_dimensions(1, 1920, 1080, rw, rh);
    CHECK(rw == 2560);
    CHECK(rh == 1440);

    resolve_recording_dimensions(2, 2560, 1440, rw, rh);
    CHECK(rw == 1920);
    CHECK(rh == 1080);

    resolve_recording_dimensions(3, 1920, 1080, rw, rh);
    CHECK(rw == 1280);
    CHECK(rh == 720);
  }

  SUBCASE("Interactive Bitrate Control & VBV Rate Ceiling") {
    ScopedTempDir tmp("fpshud_br_");
    fs::path br_ini = tmp / "test_bitrate_cfg.ini";

    HudConfig br_cfg;
    br_cfg.record_bitrate_kbps = 24000;
    CHECK(save_ini_file(br_ini, br_cfg, "Test Bitrate Config"));

    HudConfig loaded_br_cfg;
    CHECK(load_ini_file(br_ini, loaded_br_cfg));
    CHECK(loaded_br_cfg.record_bitrate_kbps == 24000);

    fs::path rroot = find_repo_root();
    if (!rroot.empty()) {
      std::string menu_src = read_text_file(rroot / "src" / "ui" / "gui_menu.cpp");
      std::string rec_src = read_text_file(rroot / "src" / "recorder" / "recorder_main.cpp");
      CHECK(menu_src.find("Bitrate") != std::string::npos);
      CHECK(menu_src.find("record_bitrate_kbps") != std::string::npos);
      CHECK(menu_src.find("SliderInt") != std::string::npos);
      CHECK((rec_src.find("rc_max_rate") != std::string::npos ||
             rec_src.find("maxrate") != std::string::npos));
    }
  }

  SUBCASE("Experimental Adaptive VFR Config & Controller Logic") {
    ScopedTempDir tmp("fpshud_vfr_");
    fs::path vfr_ini = tmp / "test_fpshud_exp.ini";

    HudConfig save_cfg{};
    save_cfg.record_adaptive_vfr = true;
    save_cfg.record_vfr_max_fps = 144;
    CHECK(save_ini_file(vfr_ini, save_cfg));

    HudConfig load_cfg{};
    CHECK(load_ini_file(vfr_ini, load_cfg));
    CHECK(load_cfg.record_adaptive_vfr == true);
    CHECK(load_cfg.record_vfr_max_fps == 144);

    HudConfig off_cfg{};
    off_cfg.record_adaptive_vfr = false;
    auto t0 = std::chrono::steady_clock::time_point{};
    auto t1 = t0 + std::chrono::milliseconds(100);
    auto t2 = t1 + std::chrono::milliseconds(2);  // 2ms elapsed (< 90% of 8.33ms @ 120 FPS)
    auto t3 = t1 + std::chrono::milliseconds(9);  // 9ms elapsed (>= 90% of 8.33ms @ 120 FPS)

    CHECK(!fpshud::experimental::AdaptiveVfrController::is_vfr_active(off_cfg));
    CHECK(fpshud::experimental::AdaptiveVfrController::is_vfr_active(save_cfg));
    CHECK(fpshud::experimental::AdaptiveVfrController::should_capture_frame(t1, t0, 120));
    CHECK(!fpshud::experimental::AdaptiveVfrController::should_capture_frame(t2, t1, 120));
    CHECK(fpshud::experimental::AdaptiveVfrController::should_capture_frame(t3, t1, 120));

    [&]<typename T = BenchmarkSummary>() {
      static_assert(
          !requires(T bs) { bs.saved_csv_path; },
          "saved_csv_path must be removed from BenchmarkSummary");
    }();

    fs::path rroot = find_repo_root();
    if (!rroot.empty()) {
      std::string vk_src = read_text_file(rroot / "src" / "layer" / "vulkan_layer.cpp");
      CHECK(vk_src.find("has_dma_buf_support") == std::string::npos);
      CHECK(vk_src.find("GetMemoryFdKHR") == std::string::npos);
    }
  }

  SUBCASE("Interactive 1-or-2-Key Shortcut Capture & Combo Matching") {
    InputManager im;
    HudConfig kcfg{};
    kcfg.hud_visible = true;
    kcfg.gui_open = false;

    // 1. Single-key capture: capture KEY_F8 (66) for ToggleHud
    im.start_keybind_capture(KeybindTarget::ToggleHud);
    CHECK(im.is_capturing_keybind());
    CHECK(im.capturing_target() == KeybindTarget::ToggleHud);
    im.handle_key_event(66, true, kcfg, 1920.0f, 1080.0f, nullptr);
    im.handle_key_event(66, false, kcfg, 1920.0f, 1080.0f, nullptr);
    CHECK(!im.is_capturing_keybind());
    CHECK(kcfg.bind_toggle_hud.mod_key == 0);
    CHECK(kcfg.bind_toggle_hud.main_key == 66);

    // 2. Two-key combo capture: capture LeftAlt (56) + F (33) for ToggleHud
    im.start_keybind_capture(KeybindTarget::ToggleHud);
    im.handle_key_event(56, true, kcfg, 1920.0f, 1080.0f, nullptr);  // Press LeftAlt
    CHECK(im.is_capturing_keybind());
    im.handle_key_event(33, true, kcfg, 1920.0f, 1080.0f, nullptr);  // Press F
    im.handle_key_event(33, false, kcfg, 1920.0f, 1080.0f, nullptr);
    im.handle_key_event(56, false, kcfg, 1920.0f, 1080.0f, nullptr);
    CHECK(!im.is_capturing_keybind());
    CHECK(kcfg.bind_toggle_hud.mod_key == 56);
    CHECK(kcfg.bind_toggle_hud.main_key == 33);

    // 3. Triggering custom 2-key combo: pressing F alone does NOT toggle hud_visible, pressing
    // LeftAlt + F DOES toggle hud_visible
    kcfg.hud_visible = true;
    im.handle_key_event(33, true, kcfg, 1920.0f, 1080.0f, nullptr);
    im.handle_key_event(33, false, kcfg, 1920.0f, 1080.0f, nullptr);
    CHECK(kcfg.hud_visible == true);

    im.handle_key_event(56, true, kcfg, 1920.0f, 1080.0f, nullptr);
    im.handle_key_event(33, true, kcfg, 1920.0f, 1080.0f, nullptr);
    im.handle_key_event(33, false, kcfg, 1920.0f, 1080.0f, nullptr);
    im.handle_key_event(56, false, kcfg, 1920.0f, 1080.0f, nullptr);
    CHECK(kcfg.hud_visible == false);

    // 4. Cancel capture via KEY_ESC (1)
    im.start_keybind_capture(KeybindTarget::ToggleHud);
    im.handle_key_event(1, true, kcfg, 1920.0f, 1080.0f, nullptr);
    im.handle_key_event(1, false, kcfg, 1920.0f, 1080.0f, nullptr);
    CHECK(!im.is_capturing_keybind());
    CHECK(kcfg.bind_toggle_hud.mod_key == 56);
    CHECK(kcfg.bind_toggle_hud.main_key == 33);

    // 5. Default F10 (68) must trigger Screenshot without toggling GUI open
    HudConfig f10_cfg{};
    f10_cfg.enabled = true;
    f10_cfg.gui_open = false;
    RecorderController::instance().consume_screenshot_request();
    im.handle_key_event(68, true, f10_cfg, 1920.0f, 1080.0f, nullptr);
    im.handle_key_event(68, false, f10_cfg, 1920.0f, 1080.0f, nullptr);
    CHECK(!f10_cfg.gui_open);
    CHECK(RecorderController::instance().consume_screenshot_request());
  }

  SUBCASE("INI Newline and Carriage Return Sanitization") {
    std::string tainted_val = "custom\ninjected_key=1\r\n[InjectedSection]\nmalicious=true";
    std::string sanitized_val = fpshud::sanitize_ini_value(tainted_val);
    CHECK(sanitized_val.find('\r') == std::string::npos);
    CHECK(sanitized_val.find('\n') == std::string::npos);

    fpshud::HudConfig test_inj_cfg;
    test_inj_cfg.active_preset = "custom\ninjected_key=1";
    test_inj_cfg.record_directory = "/tmp/rec\nrogue_key=pwned";
    ScopedTempDir tmp("fpshud_inj_");
    fs::path test_ini_path = tmp / "test_inj.ini";
    CHECK(fpshud::save_ini_file(test_ini_path, test_inj_cfg, "Test\nInjection"));

    std::string ini_content = read_text_file(test_ini_path);
    CHECK(ini_content.find("\ninjected_key=1\n") == std::string::npos);
    CHECK(ini_content.find("\nrogue_key=pwned\n") == std::string::npos);
  }
}

TEST_CASE("Core: Zero-Cost C++26 Logging Engine") {
  SUBCASE("Logger initialization and default stderr fallback") {
    ::unsetenv("FPS_HUD_LOG");
    fpshud::log::init();
    CHECK(fpshud::log::is_enabled(fpshud::log::Level::Warn));
    CHECK(fpshud::log::is_enabled(fpshud::log::Level::Error));
    CHECK(!fpshud::log::is_enabled(fpshud::log::Level::Debug));

    // Verify info macro compiles and executes safely without crash
    FPSHUD_LOG_INFO(fpshud::log::Subsystem::Core, "Test info %d", 42);
  }

  SUBCASE("Log file destination, formatting, and timestamp precision") {
    ScopedTempDir tmp("fpshud_log_");
    fs::path log_path = tmp / "logger_output.log";

    ::setenv("FPS_HUD_LOG", log_path.c_str(), 1);
    fpshud::log::init();

    CHECK(fpshud::log::is_enabled(fpshud::log::Level::Debug));
    CHECK(fpshud::log::is_enabled(fpshud::log::Level::Info));

    FPSHUD_LOG_WARN(fpshud::log::Subsystem::Core, "Test warn %s", "captured");
    fpshud::log::write(fpshud::log::Level::Info, fpshud::log::Subsystem::Core, "test_core.cpp", 132,
                       "Logger test payload %d", 999);

    // Reset logger back to stderr so the file descriptor is flushed and closed
    ::unsetenv("FPS_HUD_LOG");
    fpshud::log::init();

    std::string logged_content = read_text_file(log_path);
    CHECK(logged_content.find("[CORE]") != std::string::npos);
    CHECK(logged_content.find("Logger test payload 999") != std::string::npos);
    CHECK(logged_content.find("[INFO]") != std::string::npos);
    CHECK(logged_content.find("test_core.cpp:132") != std::string::npos);
    CHECK(logged_content.find('[') != std::string::npos);
    CHECK(logged_content.find(':') != std::string::npos);
    CHECK(logged_content.find('.') != std::string::npos);
    CHECK(logged_content.find("[WARN]") != std::string::npos);
    CHECK(logged_content.find("Test warn captured") != std::string::npos);
  }

  SUBCASE("Log level string parsing and filtering") {
    ::setenv("FPS_HUD_LOG", "error", 1);
    fpshud::log::init();
    CHECK(fpshud::log::is_enabled(fpshud::log::Level::Error));
    CHECK(!fpshud::log::is_enabled(fpshud::log::Level::Warn));
    CHECK(!fpshud::log::is_enabled(fpshud::log::Level::Info));

    ::setenv("FPS_HUD_LOG", "off", 1);
    fpshud::log::init();
    CHECK(!fpshud::log::is_enabled(fpshud::log::Level::Error));
    CHECK(!fpshud::log::is_enabled(fpshud::log::Level::Warn));
    CHECK(!fpshud::log::is_enabled(fpshud::log::Level::None));

    ::unsetenv("FPS_HUD_LOG");
    fpshud::log::init();
  }

  SUBCASE("Release mode macro evaporation verification") {
    int side_effect_counter = 0;
    FPSHUD_LOG_TRACE(fpshud::log::Subsystem::Core, "Trace counter: %d", ++side_effect_counter);
    FPSHUD_LOG_DEBUG(fpshud::log::Subsystem::Core, "Debug counter: %d", ++side_effect_counter);
#if !defined(FPSHUD_ENABLE_DEBUG_LOGGING)
    CHECK(side_effect_counter == 0);
#endif
  }
}
