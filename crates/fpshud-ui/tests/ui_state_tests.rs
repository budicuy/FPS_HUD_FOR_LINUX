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

    for &hist_len in &[0_usize, 1, 120] {
        snap.frametime_history = (0..hist_len).map(|idx| 8.0 + (idx % 5) as f32).collect();
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

#[test]
fn dashboard_mouse_click_fires_action_and_emits_config() {
    use fpshud_input::UiInputEvent;
    use gpui::{Context, InteractiveElement, IntoElement, MouseButton, Render, Styled, Window, div, px};
    use std::sync::{Arc, atomic::{AtomicUsize, Ordering}};

    struct ClickProbe {
        hits: Arc<AtomicUsize>,
    }
    impl Render for ClickProbe {
        fn render(&mut self, _w: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
            let hits = Arc::clone(&self.hits);
            div().w(px(300.0)).h(px(200.0)).on_mouse_down(MouseButton::Left, move |_, _, _| {
                hits.fetch_add(1, Ordering::Relaxed);
            })
        }
    }

    let probe_hits = Arc::new(AtomicUsize::new(0));
    let probe_hits_clone = Arc::clone(&probe_hits);
    let mut probe_rt = OffscreenGpuiRuntime::new_with_view(400, 300, 1.0, move |_, _| ClickProbe {
        hits: probe_hits_clone,
    })
    .unwrap();
    let mut probe_writer = ShmRingWriter::new(400, 300).unwrap();
    let _ = probe_rt.render_scene_to_slot(&mut probe_writer).unwrap();
    probe_rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: 50.0, y: 50.0 },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    assert_eq!(probe_hits.load(Ordering::Relaxed), 1, "raw ClickProbe hit failed");

    let cfg = HudConfig {
        hud_visible: true,
        gui_open: true,
        ..HudConfig::default()
    };
    let snap = TelemetrySnapshot::default();
    let mut rt = OffscreenGpuiRuntime::new_with_view(1280, 720, 1.0, move |_, _| {
        FpshudRootView::new(UiStore::new(cfg, snap))
    })
    .unwrap();
    let mut writer = ShmRingWriter::new(1280, 720).unwrap();

    // Render initial dashboard frame so GPUI hitboxes are laid out cleanly
    let _ = rt.render_scene_to_slot(&mut writer).unwrap();

    // Scan top half of centered dashboard
    let mut got_minimal = false;
    'scan: for y in (20..360).step_by(12) {
        for x in (100..650).step_by(18) {
            rt.dispatch_input(&[
                UiInputEvent::MouseMove { x: x as f32, y: y as f32 },
                UiInputEvent::MouseDown { button: 0 },
                UiInputEvent::MouseUp { button: 0 },
            ]);
            let _ = rt.render_scene_to_slot(&mut writer).unwrap();
            let msgs = rt.update_app(FpshudRootView::take_outgoing);
            if msgs.iter().any(|m| matches!(m, UiToLayerMsg::ConfigUpdated(c) if c.preset == PresetType::Minimal)) {
                got_minimal = true;
                break 'scan;
            }
        }
    }
    assert!(got_minimal, "clicking Minimal button via OffscreenGpuiRuntime must emit ConfigUpdated(Minimal)");

    // Clicking with centered cursor + verifying software cursor doesn't block Col 2 "FPS (5)" / sensor click
    assert!(fpshud_ui::dashboard::compute_dashboard_width(1280.0) >= 1000.0);
    assert!(fpshud_ui::dashboard::compute_dashboard_col2_width(1280.0) >= 480.0);

    // Click "Close" or preset again at 1280x720 with cursor drawn at center first -> must still hit header/sensor
    let mut got_advance = false;
    'scan_adv: for y in (30..200).step_by(10) {
        for x in (340..620).step_by(14) {
            rt.dispatch_input(&[
                UiInputEvent::MouseMove { x: x as f32, y: y as f32 },
                UiInputEvent::MouseDown { button: 0 },
                UiInputEvent::MouseUp { button: 0 },
            ]);
            let _ = rt.render_scene_to_slot(&mut writer).unwrap();
            let msgs = rt.update_app(FpshudRootView::take_outgoing);
            if msgs.iter().any(|m| matches!(m, UiToLayerMsg::ConfigUpdated(c) if c.preset == PresetType::Advance)) {
                got_advance = true;
                break 'scan_adv;
            }
        }
    }
    assert!(got_advance, "software cursor overlay must not block clicking Advance preset");
}

#[test]
fn dashboard_drag_moves_panel_and_preview_drag_continues_outside_box() {
    use fpshud_input::UiInputEvent;
    use fpshud_ui::dashboard::compute_dashboard_position;

    // Default centered position + offset clamping inside viewport
    let (cx, cy) = compute_dashboard_position(1280.0, 720.0, 1080.0, 560.0, 0.0, 0.0);
    assert!((cx - 100.0).abs() < 0.1 && (cy - 80.0).abs() < 0.1);
    let (mx, my) = compute_dashboard_position(1280.0, 720.0, 1080.0, 560.0, 500.0, -500.0);
    assert!((mx - 200.0).abs() < 0.1 && (my - 0.0).abs() < 0.1);

    let cfg = HudConfig {
        hud_visible: true,
        gui_open: true,
        ..HudConfig::default()
    };
    let snap = TelemetrySnapshot::default();
    let mut rt = OffscreenGpuiRuntime::new_with_view(1280, 720, 1.0, move |_, _| {
        FpshudRootView::new(UiStore::new(cfg, snap))
    })
    .unwrap();
    let mut writer = ShmRingWriter::new(1280, 720).unwrap();
    let _ = rt.render_scene_to_slot(&mut writer).unwrap();

    // Locate rendered dashboard bounds & preview box bounds directly via root view
    let (dash_origin, dash_w, prev_center) = rt.update_app(|cx| {
        let win = cx.windows()[0];
        win.update(cx, |root_any, _, cx| {
            let root = root_any.downcast::<FpshudRootView>().unwrap();
            let view = root.read(cx);
            let db = view.dashboard_bounds_snapshot();
            let pb = view.preview_bounds_snapshot();
            (
                (f32::from(db.origin.x), f32::from(db.origin.y)),
                f32::from(db.size.width),
                (
                    f32::from(pb.origin.x) + f32::from(pb.size.width) * 0.5,
                    f32::from(pb.origin.y) + f32::from(pb.size.height) * 0.5,
                ),
            )
        })
        .unwrap()
    });

    // Drag dashboard by its top title bar (horizontal midpoint, 16px inside top edge)
    let grab_x = dash_origin.0 + dash_w * 0.5;
    let grab_y = dash_origin.1 + 16.0;
    rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: grab_x, y: grab_y },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseMove {
            x: grab_x + 70.0,
            y: grab_y + 15.0,
        },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_slot(&mut writer).unwrap();
    let new_dash_origin = rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |root_any, _, cx| {
                let db = root_any
                    .downcast::<FpshudRootView>()
                    .unwrap()
                    .read(cx)
                    .dashboard_bounds_snapshot();
                (f32::from(db.origin.x), f32::from(db.origin.y))
            })
            .unwrap()
    });
    assert!(
        (new_dash_origin.0 - dash_origin.0 - 70.0).abs() < 2.0
            && (new_dash_origin.1 - dash_origin.1 - 15.0).abs() < 2.0,
        "dragging dashboard header must move panel by (+70, +15), got old={dash_origin:?} new={new_dash_origin:?}"
    );

    // Drag preview dot from inside preview_box to far bottom-right outside preview_box -> clamps custom_pos to (1.0, 1.0)
    let shifted_px = prev_center.0 + 70.0;
    let shifted_py = prev_center.1 + 15.0;
    rt.dispatch_input(&[
        UiInputEvent::MouseMove {
            x: shifted_px,
            y: shifted_py,
        },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseMove {
            x: shifted_px + 250.0,
            y: shifted_py + 250.0,
        },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_slot(&mut writer).unwrap();
    let msgs = rt.update_app(FpshudRootView::take_outgoing);
    assert!(
        msgs.iter().any(|m| matches!(
            m,
            UiToLayerMsg::ConfigUpdated(c)
                if (c.custom_pos_x - 1.0).abs() < 1e-4 && (c.custom_pos_y - 1.0).abs() < 1e-4
        )),
        "dragging preview dot outside preview_box must continue updating custom_pos to (1.0, 1.0)"
    );
}
