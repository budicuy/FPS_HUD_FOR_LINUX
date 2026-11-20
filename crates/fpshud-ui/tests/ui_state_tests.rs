use fpshud_core::{ArrangementMode, BackgroundStyle, HudConfig, MetricType, PresetType};
use fpshud_gpui_platform::OffscreenGpuiRuntime;
use fpshud_input::KeybindTarget;
use fpshud_ipc::{ShmRingWriter, UiToLayerMsg};
use fpshud_telemetry::{CpuPowerSource, TelemetrySnapshot};
use fpshud_ui::{
    FpshudRootView, SensorFilter, UiStore,
    toast::ToastState,
};

#[test]
fn ui_store_all_metrics_filters_thermal_drag_and_actions() {
    let mut store = UiStore::new(HudConfig::default(), TelemetrySnapshot::default());

    // 1. Sensor filtering
    assert_eq!(store.filtered_metrics(SensorFilter::All).len(), 31);
    assert!(!store.filtered_metrics(SensorFilter::Standard).is_empty());
    assert_eq!(store.filtered_metrics(SensorFilter::Fps).len(), 5);

    // 2. Formatting all 31 metrics + '~' EstimatedDvfs vs DirectHwmon + Unavailable N/A
    store.telemetry.cpu_power_w = 48.0;
    store.telemetry.cpu_power_source = CpuPowerSource::EstimatedDvfs;
    let (_, val_est, _, _) = store.format_metric_row(MetricType::CpuPower);
    assert!(val_est.starts_with('~'));

    store.telemetry.cpu_power_source = CpuPowerSource::DirectHwmon;
    let (_, val_hw, _, _) = store.format_metric_row(MetricType::CpuPower);
    assert!(!val_hw.starts_with('~'));

    store.telemetry.cpu_power_source = CpuPowerSource::Unavailable;
    let (_, val_na, _, _) = store.format_metric_row(MetricType::CpuPower);
    assert_eq!(val_na, "N/A");

    // Thermal warning + category colors
    store.config.category_colors_enabled = true;
    store.config.thermal_warnings_enabled = true;
    store.telemetry.cpu_temp_c = 96.0;
    let (_, _, color_warn, is_warn) = store.format_metric_row(MetricType::CpuTemp);
    assert!(is_warn);
    assert_ne!(color_warn, 0xffff_ffff);

    for &m in MetricType::ALL {
        let (label, val, _, _) = store.format_metric_row(m);
        assert!(!label.is_empty() && !val.is_empty());
        assert!(!val.contains("NaN"));
    }

    // 3. Drag clamping + presets + Column 3 actions
    store.set_custom_pos(-0.5, 2.5);
    assert!((store.config.custom_pos_x - 0.0).abs() < 1e-5);
    assert!((store.config.custom_pos_y - 1.0).abs() < 1e-5);

    store.apply_preset(PresetType::Minimal);
    assert_eq!(store.config.preset, PresetType::Minimal);
    store.apply_preset(PresetType::Advance);
    assert_eq!(store.config.metrics.len(), 31);

    store.trigger_screenshot();
    store.trigger_benchmark();
    store.trigger_open_save_folder();
    store.set_screenshot_mode(true);
    store.request_keybind_capture(KeybindTarget::ToggleGui);
    let out = store.take_outgoing();
    assert!(out.iter().any(|m| matches!(m, UiToLayerMsg::ScreenshotRequest { with_hud: false })));
    assert!(out.iter().any(|m| matches!(m, UiToLayerMsg::BenchmarkToggle)));
    assert!(out.iter().any(|m| matches!(m, UiToLayerMsg::OpenSaveFolder)));
    assert!(out.iter().any(|m| matches!(m, UiToLayerMsg::ConfigUpdated(c) if c.screenshot_with_hud)));
    assert!(out.iter().any(|m| matches!(m, UiToLayerMsg::KeybindCaptureState { capturing: true, target: KeybindTarget::ToggleGui })));
}

#[test]
fn toast_visibility_countdown_and_hud_dashboard_smoke_render() {
    assert!(ToastState::should_show(0.0));
    assert!(ToastState::should_show(5.4));
    assert!(!ToastState::should_show(5.5));
    assert!(!ToastState::should_show(10.0));

    let mut cfg = HudConfig {
        hud_visible: true,
        hud_graph_enabled: true,
        ..HudConfig::default()
    };
    let mut snap = TelemetrySnapshot {
        fps: 120.0,
        frametime_ms: 8.3,
        ..TelemetrySnapshot::default()
    };

    let cfg_init = cfg.clone();
    let snap_init = snap.clone();
    let mut rt = OffscreenGpuiRuntime::new_with_view(960, 640, 1.0, move |_, _| {
        FpshudRootView::new(UiStore::new(cfg_init, snap_init))
    })
    .unwrap();
    let mut writer = ShmRingWriter::new(960, 640).unwrap();

    for &hist_len in &[0_u8, 1, 120] {
        snap.frametime_history_len = hist_len;
        for (idx, slot) in snap.frametime_history.iter_mut().enumerate().take(hist_len as usize) {
            *slot = 8.0 + (idx % 5) as f32;
        }
        for &arr in &[ArrangementMode::Stack, ArrangementMode::Double, ArrangementMode::Linear] {
            for &bg in &[BackgroundStyle::None, BackgroundStyle::Translucent, BackgroundStyle::Solid] {
                cfg.arrangement = arr;
                cfg.background_style = bg;
                rt.update_app(|cx| FpshudRootView::sync_all(cx, cfg.clone(), snap.clone()));
                let meta = rt.render_scene_to_slot(&mut writer).unwrap();
                assert_eq!((meta.width, meta.height), (960, 640));
            }
        }
    }

    cfg.gui_open = true;
    rt.update_app(|cx| FpshudRootView::sync_all(cx, cfg.clone(), snap.clone()));
    let meta_gui = rt.render_scene_to_slot(&mut writer).unwrap();
    assert_eq!((meta_gui.width, meta_gui.height), (960, 640));
}

#[test]
fn toggle_metric_preserves_unsaved_edits_and_preview_local_coord_mapping() {
    let cfg = HudConfig {
        gui_scale: 1.5,
        bg_alpha: 0.72,
        hud_graph_enabled: true,
        ..HudConfig::default()
    };
    let initial_temp = cfg.has_metric(MetricType::CpuTemp);
    let mut store = UiStore::new(cfg, TelemetrySnapshot::default());

    store.toggle_metric(MetricType::CpuTemp);
    assert_eq!(store.config.preset, PresetType::Custom);
    assert_ne!(store.config.has_metric(MetricType::CpuTemp), initial_temp);
    assert!((store.config.gui_scale - 1.5).abs() < 1e-5);
    assert!((store.config.bg_alpha - 0.72).abs() < 1e-5);
    assert!(store.config.hud_graph_enabled);

    store.step_scale(0.25);
    assert!((store.config.gui_scale - 1.75).abs() < 1e-5);
    store.step_font_size(2.0);
    assert!((store.config.font_size_px - 22.0).abs() < 1e-5);
    store.step_text_opacity(-0.2);
    assert!((store.config.text_opacity - 0.8).abs() < 1e-5);
    store.step_bg_alpha(-0.12);
    assert!((store.config.bg_alpha - 0.6).abs() < 1e-5);
    store.toggle_hud_graph();
    assert!(!store.config.hud_graph_enabled);
    let old_col = store.config.text_color;
    store.cycle_text_color();
    assert_ne!(store.config.text_color, old_col);

    // Local coordinate mapping inside a preview box positioned at (48, 120) with size 160x90
    let (nx0, ny0) = fpshud_ui::dashboard::map_preview_point_to_norm(48.0, 120.0, 48.0, 120.0, 160.0, 90.0);
    assert!((nx0 - 0.0).abs() < 1e-5 && (ny0 - 0.0).abs() < 1e-5);
    let (nx1, ny1) = fpshud_ui::dashboard::map_preview_point_to_norm(208.0, 210.0, 48.0, 120.0, 160.0, 90.0);
    assert!((nx1 - 1.0).abs() < 1e-5 && (ny1 - 1.0).abs() < 1e-5);
    let (nxc, nyc) = fpshud_ui::dashboard::map_preview_point_to_norm(128.0, 165.0, 48.0, 120.0, 160.0, 90.0);
    assert!((nxc - 0.5).abs() < 1e-5 && (nyc - 0.5).abs() < 1e-5);
}
