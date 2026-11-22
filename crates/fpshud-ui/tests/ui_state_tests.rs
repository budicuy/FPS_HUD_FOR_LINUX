use fpshud_analytics::BenchmarkSummary;
use fpshud_core::{ColorRgba, HudConfig, LayoutPosition, MetricType, PresetType};
use fpshud_input::KeybindTarget;
use fpshud_ipc::UiToLayerMsg;
use fpshud_telemetry::TelemetrySnapshot;
use fpshud_ui::cpu_dashboard::{
    ARRANGEMENT_OPTIONS, BG_OPACITY_PCTS, BG_STYLE_OPTIONS, FONT_OPTIONS, FONT_SIZE_OPTIONS,
    POSITION_OPTIONS, SCALE_OPTIONS, TEXT_COLOR_SWATCHES, TEXT_OPACITY_PCTS,
    clamp_popover_origin_rect, compute_dashboard_col2_width, compute_dashboard_position,
    compute_dashboard_width, map_preview_point_to_norm,
};
use fpshud_ui::cpu_hud::CpuHudRenderer;
use fpshud_ui::state::{
    DEFAULT_ACCENT_TEXT_COLOR, MetricCategory, SensorFilter, UiStore, effective_text_color,
    format_benchmark_summary, group_metrics_ordered, hud_metric_short_label, metric_category,
    resolve_metric_value_color, rgba_u32,
};
use fpshud_ui::toast::ToastState;
use fpshud_ui::Rect;

#[test]
fn ui_store_all_metrics_filters_thermal_drag_and_actions() {
    let cfg = HudConfig {
        metrics: MetricType::ALL.to_vec(),
        ..HudConfig::default()
    };
    let snap = TelemetrySnapshot {
        fps: 24.0,
        fps_avg: 60.0,
        frametime_ms: 41.0,
        gpu_temp_c: 86.0,
        cpu_temp_c: 75.0,
        gpu_load: 98.0,
        cpu_load: 85.0,
        ram_used_gb: 15.0,
        ram_total_gb: 16.0,
        ..TelemetrySnapshot::default()
    };
    let mut store = UiStore::new(cfg, snap);

    assert_eq!(store.filtered_metrics(SensorFilter::All).len(), 31);
    assert!(!store.filtered_metrics(SensorFilter::Standard).is_empty());
    assert_eq!(
        store
            .filtered_metrics(SensorFilter::Group(MetricCategory::Gpu))
            .len(),
        8
    );

    for &m in MetricType::ALL {
        let (lbl, val, _, _) = store.format_metric_row(m);
        assert!(!lbl.is_empty());
        assert!(!val.is_empty());
    }

    let (_, warn_gpu_temp) =
        resolve_metric_value_color(MetricType::GpuTemp, &store.telemetry, &store.config, 255);
    assert!(warn_gpu_temp);

    store.apply_preset(PresetType::Advance);
    assert_eq!(store.config.preset, PresetType::Advance);
    store.set_custom_pos(0.25, 0.75);
    assert_eq!(store.config.position, LayoutPosition::Custom);
    store.trigger_screenshot();
    store.trigger_benchmark();
    store.trigger_open_save_folder();
    store.request_keybind_capture(KeybindTarget::ToggleHud);
    assert_eq!(store.keybind_label(KeybindTarget::ToggleHud), "Press keys...");

    let msgs = store.take_outgoing();
    assert!(msgs.iter().any(|m| matches!(m, UiToLayerMsg::ScreenshotRequest { .. })));
    assert!(msgs.iter().any(|m| matches!(m, UiToLayerMsg::BenchmarkToggle)));
    assert!(msgs.iter().any(|m| matches!(m, UiToLayerMsg::OpenSaveFolder)));
    assert!(msgs.iter().any(|m| matches!(
        m,
        UiToLayerMsg::KeybindCaptureState {
            capturing: true,
            target: KeybindTarget::ToggleHud
        }
    )));

    let bench = BenchmarkSummary {
        avg_fps: 120.0,
        fps_1pct_low: 95.0,
        fps_01pct_low: 80.0,
        min_fps: 75.0,
        max_fps: 144.0,
        total_frames: 3600,
        duration_sec: 30.0,
        csv_path: std::path::PathBuf::from("/tmp/bench.csv"),
    };
    assert!(format_benchmark_summary(&bench).contains("120 avg"));
}

#[test]
fn hud_grouping_middle_dot_order_and_effective_text_color_opacity_100() {
    let grouped = group_metrics_ordered(&[
        MetricType::Ram,
        MetricType::GpuTemp,
        MetricType::Fps,
        MetricType::CpuLoad,
        MetricType::EngineApi,
    ]);
    assert_eq!(grouped.len(), 5);
    assert_eq!(grouped[0].0, MetricCategory::Fps);
    assert_eq!(grouped[1].0, MetricCategory::Gpu);
    assert_eq!(grouped[2].0, MetricCategory::Cpu);
    assert_eq!(grouped[3].0, MetricCategory::Ram);
    assert_eq!(grouped[4].0, MetricCategory::Sys);
    assert_eq!(metric_category(MetricType::Fps), MetricCategory::Fps);
    assert_eq!(hud_metric_short_label(MetricType::FrameTimes), "FT");

    let mut cfg = HudConfig {
        custom_text_color_enabled: false,
        ..HudConfig::default()
    };
    assert_eq!(effective_text_color(&cfg), ColorRgba::new(255, 255, 255, 255));
    cfg.custom_text_color_enabled = true;
    cfg.text_color = DEFAULT_ACCENT_TEXT_COLOR;
    assert_eq!(effective_text_color(&cfg), DEFAULT_ACCENT_TEXT_COLOR);
    assert_eq!(rgba_u32(1, 2, 3, 4), 0x0102_0304);
}

#[test]
fn dashboard_layout_and_popover_geometry_helpers() {
    assert!((compute_dashboard_width(1280.0) - 1140.0).abs() < 1e-4);
    assert!(compute_dashboard_col2_width(1280.0) >= 380.0);
    let (x, y) = compute_dashboard_position(1280.0, 720.0, 960.0, 576.0, 0.0, 0.0);
    assert!((x - 160.0).abs() < 1e-4);
    assert!((y - 72.0).abs() < 1e-4);

    let (nx, ny) = map_preview_point_to_norm(150.0, 100.0, 100.0, 50.0, 200.0, 100.0);
    assert!((nx - 0.25).abs() < 1e-4);
    assert!((ny - 0.5).abs() < 1e-4);

    let (px, py) = clamp_popover_origin_rect(Rect::new(100.0, 680.0, 142.0, 21.0), 156.0, 140.0, 1280.0, 720.0);
    assert!(px >= 8.0 && py < 680.0);

    assert_eq!(POSITION_OPTIONS.len(), 7);
    assert_eq!(ARRANGEMENT_OPTIONS.len(), 3);
    assert_eq!(SCALE_OPTIONS.len(), 7);
    assert_eq!(FONT_OPTIONS.len(), 2);
    assert_eq!(FONT_SIZE_OPTIONS.len(), 10);
    assert_eq!(TEXT_OPACITY_PCTS.len(), 5);
    assert_eq!(BG_STYLE_OPTIONS.len(), 3);
    assert_eq!(BG_OPACITY_PCTS.len(), 20);
    assert_eq!(TEXT_COLOR_SWATCHES.len(), 4);
}

#[test]
fn toast_visibility_countdown_and_cpu_hud_bounding_box() {
    assert!(ToastState::should_show(0.0));
    assert!(ToastState::should_show(5.4));
    assert!(!ToastState::should_show(5.6));
    assert!(!ToastState::should_show(f32::NAN));

    let cfg = HudConfig {
        hud_visible: true,
        gui_open: false,
        ..HudConfig::default()
    };
    let snap = TelemetrySnapshot {
        fps: 90.0,
        frametime_ms: 11.1,
        ..TelemetrySnapshot::default()
    };
    let store = UiStore::new(cfg, snap);
    let mut hud = CpuHudRenderer::new();
    let frame = hud.render(&store, 1280, 720, 1.0).expect("HUD frame");
    assert!(frame.width < 1280 && frame.height < 720);
    let non_zero = frame.rgba.as_chunks::<4>().0.iter().filter(|p| p[3] > 0).count();
    assert!(non_zero > 100);
}
