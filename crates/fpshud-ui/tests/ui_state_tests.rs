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
    assert_eq!(
        store
            .filtered_metrics(SensorFilter::Group(fpshud_ui::MetricCategory::Fps))
            .len(),
        5
    );

    // 2. Formatting all 31 metrics + non-negative CpuPower + Unavailable N/A
    store.telemetry.cpu_power_w = 48.0;
    store.telemetry.cpu_power_source = CpuPowerSource::EstimatedDvfs;
    let (_, val_est, _, _) = store.format_metric_row(MetricType::CpuPower);
    assert_eq!(val_est, "48.0 W");

    store.telemetry.cpu_power_source = CpuPowerSource::DirectHwmon;
    let (_, val_hw, _, _) = store.format_metric_row(MetricType::CpuPower);
    assert_eq!(val_hw, "48.0 W");

    store.telemetry.cpu_power_source = CpuPowerSource::Unavailable;
    let (_, val_na, _, _) = store.format_metric_row(MetricType::CpuPower);
    assert_eq!(val_na, "N/A");

    // Reset Settings restores defaults while keeping GUI open and auto-saving via ConfigUpdated
    store.config.gui_open = true;
    store.config.gui_scale = 1.5;
    store.config.bg_alpha = 0.9;
    store.reset_settings();
    assert!(store.config.gui_open);
    assert!((store.config.gui_scale - HudConfig::default().gui_scale).abs() < 1e-4);
    assert!(store.take_outgoing().iter().any(|m| matches!(m, UiToLayerMsg::ConfigUpdated(c) if c.gui_open)));

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
        for x in (200..620).step_by(14) {
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

#[test]
fn software_cursor_defaults_off_and_toggles_via_ipc_sync() {
    let cfg = HudConfig {
        hud_visible: true,
        gui_open: true,
        ..HudConfig::default()
    };
    let snap = TelemetrySnapshot::default();
    let mut rt = OffscreenGpuiRuntime::new_with_view(640, 480, 1.0, move |_, _| {
        FpshudRootView::new(UiStore::new(cfg, snap))
    })
    .unwrap();
    let mut writer = ShmRingWriter::new(640, 480).unwrap();
    let _ = rt.render_scene_to_slot(&mut writer).unwrap();

    let initial_sw = rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |root_any, _, cx| {
                root_any
                    .downcast::<FpshudRootView>()
                    .unwrap()
                    .read(cx)
                    .software_cursor_enabled()
            })
            .unwrap()
    });
    assert!(!initial_sw, "software cursor must default to off so native OS cursor is used without double cursor");

    rt.update_app(|cx| FpshudRootView::set_software_cursor(cx, true));
    let _ = rt.render_scene_to_slot(&mut writer).unwrap();
    let enabled_sw = rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |root_any, _, cx| {
                root_any
                    .downcast::<FpshudRootView>()
                    .unwrap()
                    .read(cx)
                    .software_cursor_enabled()
            })
            .unwrap()
    });
    assert!(enabled_sw, "software cursor should activate when evdev/hidden grab requests it");

    rt.update_app(|cx| FpshudRootView::set_software_cursor(cx, false));
    let _ = rt.render_scene_to_slot(&mut writer).unwrap();
    let disabled_sw = rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |root_any, _, cx| {
                root_any
                    .downcast::<FpshudRootView>()
                    .unwrap()
                    .read(cx)
                    .software_cursor_enabled()
            })
            .unwrap()
    });
    assert!(!disabled_sw);
}

#[test]
fn render_mode_splits_hud_only_and_dashboard_only_without_duplication() {
    use fpshud_input::UiInputEvent;
    use fpshud_ui::UiRenderMode;

    let cfg = HudConfig {
        hud_visible: true,
        gui_open: true,
        ..HudConfig::default()
    };
    let snap = TelemetrySnapshot {
        fps: 90.0,
        frametime_ms: 11.1,
        ..TelemetrySnapshot::default()
    };

    // 1. HudOnly mode must suppress dashboard hitboxes (even when gui_open == true)
    let mut hud_rt = OffscreenGpuiRuntime::new_with_view(1280, 720, 1.0, {
        let cfg = cfg.clone();
        let snap = snap.clone();
        move |_, _| FpshudRootView::new(UiStore::new(cfg, snap))
    })
    .unwrap();
    hud_rt.update_app(|cx| FpshudRootView::set_render_mode(cx, UiRenderMode::HudOnly));
    let mut rgba_hud = Vec::new();
    let (hw, hh) = hud_rt.render_scene_to_rgba(&mut rgba_hud).unwrap();
    assert_eq!((hw, hh), (1280, 720));
    assert_eq!(rgba_hud.len(), 1280 * 720 * 4);
    assert!(rgba_hud.as_chunks::<4>().0.iter().any(|px| px[3] > 0), "HudOnly must still paint the live HUD");
    // Dashboard bounds must remain zeroed because dashboard was skipped in HudOnly mode
    let hud_dash_w = hud_rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |root_any, _, cx| {
                f32::from(
                    root_any
                        .downcast::<FpshudRootView>()
                        .unwrap()
                        .read(cx)
                        .dashboard_bounds_snapshot()
                        .size
                        .width,
                )
            })
            .unwrap()
    });
    assert!(hud_dash_w < 1.0, "HudOnly must not lay out dashboard, got width={hud_dash_w}");

    // 2. DashboardOnly mode must paint & handle clicks on the Settings dashboard without HUD/cursor
    let mut dash_rt = OffscreenGpuiRuntime::new_with_view(1280, 720, 1.0, move |_, _| {
        FpshudRootView::new(UiStore::new(cfg, snap))
    })
    .unwrap();
    dash_rt.update_app(|cx| FpshudRootView::set_render_mode(cx, UiRenderMode::DashboardOnly));
    let mut rgba_dash = Vec::new();
    let (dw, dh) = dash_rt.render_scene_to_rgba(&mut rgba_dash).unwrap();
    assert_eq!((dw, dh), (1280, 720));
    // Top-left corner (where default TL HUD sits at (14, 14)) must be fully transparent in DashboardOnly mode
    let tl_idx = (18 * 1280 + 18) * 4;
    assert_eq!(rgba_dash[tl_idx + 3], 0, "DashboardOnly must not paint HUD at top-left");

    // Clicking Minimal preset button must still work in DashboardOnly mode
    let mut got_minimal = false;
    'scan: for y in (20..360).step_by(12) {
        for x in (100..650).step_by(18) {
            dash_rt.dispatch_input(&[
                UiInputEvent::MouseMove { x: x as f32, y: y as f32 },
                UiInputEvent::MouseDown { button: 0 },
                UiInputEvent::MouseUp { button: 0 },
            ]);
            let _ = dash_rt.render_scene_to_rgba(&mut rgba_dash).unwrap();
            let msgs = dash_rt.update_app(FpshudRootView::take_outgoing);
            if msgs.iter().any(|m| matches!(m, UiToLayerMsg::ConfigUpdated(c) if c.preset == PresetType::Minimal)) {
                got_minimal = true;
                break 'scan;
            }
        }
    }
    assert!(got_minimal, "DashboardOnly mode must remain fully interactive");

    // 3. Clearing keybind capture externally (e.g. ESC in external Wayland overlay) works cleanly
    dash_rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |root_any, _, cx| {
                let root = root_any.downcast::<FpshudRootView>().unwrap();
                root.update(cx, |v, _| {
                    v.request_keybind_capture_for_test(KeybindTarget::ToggleGui);
                });
            })
            .unwrap();
        FpshudRootView::clear_keybind_capture(cx);
    });
    let _ = dash_rt.render_scene_to_rgba(&mut rgba_dash).unwrap();
    let capture_after = dash_rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |root_any, _, cx| {
                root_any
                    .downcast::<FpshudRootView>()
                    .unwrap()
                    .read(cx)
                    .active_keybind_capture_snapshot()
            })
            .unwrap()
    });
    assert_eq!(capture_after, KeybindTarget::None);
}

#[test]
fn hud_grouping_middle_dot_order_and_effective_text_color_opacity_100() {
    use fpshud_core::{ColorRgba, MetricType};
    use fpshud_ui::{
        MetricCategory, SensorFilter,
        state::{effective_text_color, group_metrics_ordered, hud_metric_short_label},
    };

    // Urutan acak lintas kategori wajib terkelompok kanonik FPS -> GPU -> CPU -> RAM -> SYS
    let scrambled = vec![
        MetricType::NetSpeed,
        MetricType::CpuTemp,
        MetricType::Ram,
        MetricType::FrameTimes,
        MetricType::GpuPower,
        MetricType::Fps,
        MetricType::GpuLoad,
        MetricType::CpuLoad,
    ];
    let grouped = group_metrics_ordered(&scrambled);
    let cats: Vec<MetricCategory> = grouped.iter().map(|(c, _)| *c).collect();
    assert_eq!(
        cats,
        vec![
            MetricCategory::Fps,
            MetricCategory::Gpu,
            MetricCategory::Cpu,
            MetricCategory::Ram,
            MetricCategory::Sys,
        ]
    );
    assert_eq!(grouped[0].1, vec![MetricType::Fps, MetricType::FrameTimes]);
    assert_eq!(grouped[1].1, vec![MetricType::GpuLoad, MetricType::GpuPower]);
    assert_eq!(grouped[2].1, vec![MetricType::CpuLoad, MetricType::CpuTemp]);

    // Grup tanpa metrik tidak boleh muncul; seluruh short label tidak kosong
    assert!(group_metrics_ordered(&[]).is_empty());
    for &m in MetricType::ALL {
        assert!(!hud_metric_short_label(m).is_empty());
    }

    // SensorFilter::Group mencakup tepat 31 sensor tanpa overlap
    let mut store = UiStore::new(HudConfig::default(), TelemetrySnapshot::default());
    assert_eq!(store.filtered_metrics(SensorFilter::Group(MetricCategory::Fps)).len(), 5);
    assert_eq!(store.filtered_metrics(SensorFilter::Group(MetricCategory::Gpu)).len(), 8);
    assert_eq!(store.filtered_metrics(SensorFilter::Group(MetricCategory::Cpu)).len(), 6);
    assert_eq!(store.filtered_metrics(SensorFilter::Group(MetricCategory::Ram)).len(), 4);
    assert_eq!(store.filtered_metrics(SensorFilter::Group(MetricCategory::Sys)).len(), 8);

    // Toggle Text Color dari default putih otomatis memilih Ice Cyan; OFF kembali putih tapi swatch tetap tersimpan
    assert!(!store.config.custom_text_color_enabled);
    assert_eq!(effective_text_color(&store.config), ColorRgba::new(255, 255, 255, 255));
    store.toggle_custom_text_color();
    assert!(store.config.custom_text_color_enabled);
    assert_ne!(store.config.text_color, ColorRgba::new(255, 255, 255, 255));
    let saved_tint = store.config.text_color;
    store.toggle_custom_text_color();
    assert!(!store.config.custom_text_color_enabled);
    assert_eq!(store.config.text_color, saved_tint);
    assert_eq!(effective_text_color(&store.config), ColorRgba::new(255, 255, 255, 255));

    // set_bg_alpha_pct mendukung 5%..100% dan keluar dari BackgroundStyle::None otomatis
    store.config.background_style = BackgroundStyle::None;
    store.set_bg_alpha_pct(100);
    assert!((store.config.bg_alpha - 1.0).abs() < 1e-4);
    assert_eq!(store.config.background_style, BackgroundStyle::Translucent);
    store.set_bg_alpha_pct(5);
    assert!((store.config.bg_alpha - 0.05).abs() < 1e-4);

    // Render smoke test HUD Opsi A pada 0, 1, dan 31 metrik (Advance) lintas Stack/Double/Linear dengan bg_alpha=1.0
    let mut cfg = HudConfig {
        hud_visible: true,
        gui_open: false,
        bg_alpha: 1.0,
        background_style: BackgroundStyle::Translucent,
        ..HudConfig::default()
    };
    let snap = TelemetrySnapshot {
        fps: 59.0,
        frametime_ms: 16.8,
        gpu_load: 49.0,
        gpu_temp_c: 88.0,
        cpu_load: 22.0,
        ..TelemetrySnapshot::default()
    };
    let mut rt = OffscreenGpuiRuntime::new_with_view(1280, 720, 1.0, {
        let cfg = cfg.clone();
        let snap = snap.clone();
        move |_, _| FpshudRootView::new(UiStore::new(cfg, snap))
    })
    .unwrap();
    let mut rgba = Vec::new();
    for metrics in [vec![], vec![MetricType::Fps], MetricType::ALL.to_vec()] {
        cfg.metrics = metrics;
        for arr in [ArrangementMode::Stack, ArrangementMode::Double, ArrangementMode::Linear] {
            cfg.arrangement = arr;
            rt.update_app(|cx| FpshudRootView::sync_all(cx, cfg.clone(), snap.clone()));
            let (w, h) = rt.render_scene_to_rgba(&mut rgba).unwrap();
            assert_eq!((w, h), (1280, 720));
        }
    }
}

#[test]
fn widgets_option_tables_and_popover_viewport_clamping() {
    use fpshud_ui::widgets::{
        BG_OPACITY_PCTS, DropdownId, FONT_OPTIONS, FONT_SIZE_OPTIONS, POSITION_OPTIONS,
        SCALE_OPTIONS, TEXT_COLOR_SWATCHES, TEXT_OPACITY_PCTS, clamp_popover_origin,
    };
    use gpui::{Bounds, Point, Size, px};

    assert_eq!(BG_OPACITY_PCTS.len(), 20);
    assert_eq!(BG_OPACITY_PCTS[0], 5);
    assert_eq!(BG_OPACITY_PCTS[19], 100);
    for pair in BG_OPACITY_PCTS.windows(2) {
        assert_eq!(pair[1] - pair[0], 5);
    }

    assert_eq!(POSITION_OPTIONS.len(), 7);
    assert!(POSITION_OPTIONS.iter().any(|(lbl, _)| *lbl == "Top Center"));
    assert!(POSITION_OPTIONS.iter().any(|(lbl, _)| *lbl == "Bottom Center"));
    assert!(!POSITION_OPTIONS.iter().any(|(lbl, _)| *lbl == "TL" || *lbl == "BC"));

    assert_eq!(FONT_OPTIONS.len(), 2);
    assert!(SCALE_OPTIONS.contains(&1.0) && SCALE_OPTIONS.contains(&0.5) && SCALE_OPTIONS.contains(&2.0));
    assert!(FONT_SIZE_OPTIONS.contains(&12) && FONT_SIZE_OPTIONS.contains(&20) && FONT_SIZE_OPTIONS.contains(&36));
    assert_eq!(TEXT_OPACITY_PCTS, [20, 40, 60, 80, 100]);
    assert_eq!(TEXT_COLOR_SWATCHES.len(), 4);
    assert_eq!(DropdownId::BgOpacity.index(), 7);

    let bottom_anchor = Bounds::new(
        Point::new(px(180.0), px(660.0)),
        Size::new(px(150.0), px(24.0)),
    );
    let (px_x, px_y) = clamp_popover_origin(bottom_anchor, 220.0, 160.0, 1280.0, 720.0);
    assert!(px_y >= 8.0 && px_y + 160.0 <= 712.0, "popup must flip upward inside 720p, got y={px_y}");
    assert!(px_x >= 8.0 && px_x + 220.0 <= 1272.0);

    let right_anchor = Bounds::new(
        Point::new(px(860.0), px(100.0)),
        Size::new(px(140.0), px(24.0)),
    );
    let (rx, ry) = clamp_popover_origin(right_anchor, 220.0, 140.0, 960.0, 640.0);
    assert!(rx + 220.0 <= 952.0 && ry >= 100.0);
}

#[test]
fn settings_gaya_x_dropdowns_occlusion_toggles_groups_and_7inch_height() {
    use fpshud_core::{FontFamily, LayoutPosition};
    use fpshud_input::UiInputEvent;
    use fpshud_ui::DropdownId;

    let cfg = HudConfig {
        hud_visible: true,
        gui_open: true,
        ..HudConfig::default()
    };
    let snap = TelemetrySnapshot {
        fps: 59.0,
        frametime_ms: 16.8,
        gpu_load: 49.0,
        cpu_load: 22.0,
        ..TelemetrySnapshot::default()
    };
    let mut rt = OffscreenGpuiRuntime::new_with_view(1280, 720, 1.0, move |_, _| {
        FpshudRootView::new(UiStore::new(cfg, snap))
    })
    .unwrap();
    let mut rgba = Vec::new();
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();

    // 1. Verifikasi tinggi total <= 660 px di viewport handheld 1280x720 & preview_box besar >= 220x110
    let (dash_b, prev_b, pos_anchor, bg_anchor) = rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |root_any, _, cx| {
                let v = root_any.downcast::<FpshudRootView>().unwrap().read(cx);
                (
                    v.dashboard_bounds_snapshot(),
                    v.preview_bounds_snapshot(),
                    v.dropdown_anchor_snapshot(DropdownId::Position),
                    v.dropdown_anchor_snapshot(DropdownId::BgOpacity),
                )
            })
            .unwrap()
    });
    assert!(
        f32::from(dash_b.size.height) > 300.0 && f32::from(dash_b.size.height) <= 660.0,
        "Gaya X dashboard must fit 7-inch 720p cleanly (<=660px), got {:?}",
        dash_b.size
    );
    assert!(
        f32::from(prev_b.size.width) >= 220.0 && f32::from(prev_b.size.height) >= 110.0,
        "Handheld grabbing preview_box must be enlarged, got {:?}",
        prev_b.size
    );

    // 2. Klik trigger dropdown Position -> membuka popup Position
    let pos_cx = f32::from(pos_anchor.origin.x) + f32::from(pos_anchor.size.width) * 0.5;
    let pos_cy = f32::from(pos_anchor.origin.y) + f32::from(pos_anchor.size.height) * 0.5;
    rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: pos_cx, y: pos_cy },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    let (open_now, pop_b) = rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |r, _, cx| {
                let v = r.downcast::<FpshudRootView>().unwrap().read(cx);
                (v.open_dropdown_snapshot(), v.active_popover_bounds_snapshot())
            })
            .unwrap()
    });
    assert_eq!(open_now, Some(DropdownId::Position));
    assert!(f32::from(pop_b.size.height) > 100.0);

    // Klik item ke-2 ("Top Center") memakai bounds popover aktual -> memilih TopCenter & menutup dropdown tanpa tembus
    let item2_x = f32::from(pop_b.origin.x) + f32::from(pop_b.size.width) * 0.5;
    let item2_y = f32::from(pop_b.origin.y) + f32::from(pop_b.size.height) * (1.5 / 7.0);
    rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: item2_x, y: item2_y },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    let msgs_pos = rt.update_app(FpshudRootView::take_outgoing);
    assert!(
        msgs_pos.iter().any(|m| matches!(m, UiToLayerMsg::ConfigUpdated(c) if c.position == LayoutPosition::TopCenter && c.arrangement == ArrangementMode::Stack)),
        "selecting Top Center must update position without clicking through to Arrangement, got {msgs_pos:?}"
    );
    let closed_after = rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |r, _, cx| r.downcast::<FpshudRootView>().unwrap().read(cx).open_dropdown_snapshot())
            .unwrap()
    });
    assert_eq!(closed_after, None);

    // 3. Klik trigger BgOpacity -> membuka grid 4x5 (5%..100%), klik sel pojok kanan bawah ("100%") -> bg_alpha == 1.0
    let bg_cx = f32::from(bg_anchor.origin.x) + f32::from(bg_anchor.size.width) * 0.5;
    let bg_cy = f32::from(bg_anchor.origin.y) + f32::from(bg_anchor.size.height) * 0.5;
    rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: bg_cx, y: bg_cy },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    let bg_pop = rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |r, _, cx| {
                let v = r.downcast::<FpshudRootView>().unwrap().read(cx);
                assert_eq!(v.open_dropdown_snapshot(), Some(DropdownId::BgOpacity));
                v.active_popover_bounds_snapshot()
            })
            .unwrap()
    });
    assert!(
        f32::from(bg_pop.origin.y) >= 8.0 && f32::from(bg_pop.origin.y) + f32::from(bg_pop.size.height) <= 712.0,
        "BgOpacity 4x5 popover must stay fully clamped inside 720p viewport, got {bg_pop:?}"
    );
    let cell_100_x = f32::from(bg_pop.origin.x) + f32::from(bg_pop.size.width) * 0.88;
    let cell_100_y = f32::from(bg_pop.origin.y) + f32::from(bg_pop.size.height) * 0.90;
    rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: cell_100_x, y: cell_100_y },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    let msgs_bg = rt.update_app(FpshudRootView::take_outgoing);
    assert!(
        msgs_bg.iter().any(|m| matches!(m, UiToLayerMsg::ConfigUpdated(c) if (c.bg_alpha - 1.0).abs() < 1e-4 && c.thermal_warnings_enabled)),
        "selecting 100% from BgOpacity grid must set bg_alpha=1.0 without toggling controls behind it, got {msgs_bg:?}"
    );

    // 4. Buka dropdown lagi lalu klik backdrop luar (2, 2) -> menutup dropdown bersih
    rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: bg_cx, y: bg_cy },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: 2.0, y: 2.0 },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    let after_backdrop = rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |r, _, cx| r.downcast::<FpshudRootView>().unwrap().read(cx).open_dropdown_snapshot())
            .unwrap()
    });
    assert_eq!(after_backdrop, None);
    let _keep_import = FontFamily::VeraMono;
}

#[test]
fn open_dropdown_resets_across_close_reopen_without_intermediate_render_and_on_drag() {
    use fpshud_input::UiInputEvent;
    use fpshud_ui::DropdownId;

    let mut cfg = HudConfig {
        hud_visible: true,
        gui_open: true,
        ..HudConfig::default()
    };
    let snap = TelemetrySnapshot::default();
    let mut rt = OffscreenGpuiRuntime::new_with_view(1280, 720, 1.0, {
        let cfg = cfg.clone();
        let snap = snap.clone();
        move |_, _| FpshudRootView::new(UiStore::new(cfg, snap))
    })
    .unwrap();
    let mut rgba = Vec::new();
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();

    let (dash_b, pos_anchor) = rt.update_app(|cx| {
        cx.windows()[0]
            .update(cx, |r, _, cx| {
                let v = r.downcast::<FpshudRootView>().unwrap().read(cx);
                (
                    v.dashboard_bounds_snapshot(),
                    v.dropdown_anchor_snapshot(DropdownId::Position),
                )
            })
            .unwrap()
    });
    let pos_cx = f32::from(pos_anchor.origin.x) + f32::from(pos_anchor.size.width) * 0.5;
    let pos_cy = f32::from(pos_anchor.origin.y) + f32::from(pos_anchor.size.height) * 0.5;

    // 1. Open Position dropdown
    rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: pos_cx, y: pos_cy },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    assert_eq!(
        rt.update_app(|cx| cx.windows()[0]
            .update(cx, |r, _, cx| r.downcast::<FpshudRootView>().unwrap().read(cx).open_dropdown_snapshot())
            .unwrap()),
        Some(DropdownId::Position)
    );

    // 2. Close & reopen via sync_all WITHOUT an intermediate render -> stale dropdown MUST be cleared
    cfg.gui_open = false;
    rt.update_app(|cx| FpshudRootView::sync_all(cx, cfg.clone(), snap.clone()));
    cfg.gui_open = true;
    rt.update_app(|cx| FpshudRootView::sync_all(cx, cfg.clone(), snap.clone()));
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    assert_eq!(
        rt.update_app(|cx| cx.windows()[0]
            .update(cx, |r, _, cx| r.downcast::<FpshudRootView>().unwrap().read(cx).open_dropdown_snapshot())
            .unwrap()),
        None,
        "stale open_dropdown must reset across gui_open false->true without intermediate render"
    );

    // 3. Reopen dropdown, then call explicit clear_open_dropdown (used by Wayland overlay reopen)
    rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: pos_cx, y: pos_cy },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    rt.update_app(FpshudRootView::clear_open_dropdown);
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    assert_eq!(
        rt.update_app(|cx| cx.windows()[0]
            .update(cx, |r, _, cx| r.downcast::<FpshudRootView>().unwrap().read(cx).open_dropdown_snapshot())
            .unwrap()),
        None
    );

    // 4. Open dropdown again, then start dragging title bar -> closes dropdown cleanly
    rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: pos_cx, y: pos_cy },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    let drag_x = f32::from(dash_b.origin.x) + f32::from(dash_b.size.width) * 0.5;
    let drag_y = f32::from(dash_b.origin.y) + 16.0;
    rt.dispatch_input(&[
        UiInputEvent::MouseMove { x: drag_x, y: drag_y },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ]);
    let _ = rt.render_scene_to_rgba(&mut rgba).unwrap();
    assert_eq!(
        rt.update_app(|cx| cx.windows()[0]
            .update(cx, |r, _, cx| r.downcast::<FpshudRootView>().unwrap().read(cx).open_dropdown_snapshot())
            .unwrap()),
        None
    );
}
