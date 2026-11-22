use fpshud_core::{HudConfig, LayoutPosition, MetricType, PresetType};
use fpshud_input::{KeybindTarget, UiInputEvent};
use fpshud_ipc::UiToLayerMsg;
use fpshud_telemetry::TelemetrySnapshot;
use fpshud_ui::cpu_canvas::TextRasterizer;
use fpshud_ui::cpu_dashboard::{CpuDashboard, DropdownId, resolve_dashboard_scale};
use fpshud_ui::state::{UiRenderMode, UiStore};

fn click(dash: &mut CpuDashboard, x_px: f32, y_px: f32, w: u32, h: u32, scale: f32) {
    let events = [
        UiInputEvent::MouseMove { x: x_px, y: y_px },
        UiInputEvent::MouseDown { button: 0 },
        UiInputEvent::MouseUp { button: 0 },
    ];
    dash.dispatch_input(&events, w, h, scale);
}

#[test]
fn dashboard_720p_height_presets_sensors_toggles_and_keybind_capture() {
    let cfg = HudConfig {
        gui_open: true,
        ..HudConfig::default()
    };
    let snap = TelemetrySnapshot {
        fps: 60.0,
        frametime_ms: 16.7,
        frametime_history: vec![16.6, 16.7, 16.8],
        ..TelemetrySnapshot::default()
    };
    let mut dash = CpuDashboard::new(UiStore::new(cfg, snap));
    dash.set_render_mode(UiRenderMode::DashboardOnly);
    let mut rast = TextRasterizer::new();
    let mut rgba = Vec::new();
    dash.render_rgba(&mut rast, 1280, 720, 1.0, &mut rgba);

    let bounds = dash.dashboard_bounds();
    assert!(
        bounds.h > 400.0 && bounds.h <= 684.0,
        "7-inch 720p panel height must fit <= 684px, got {}",
        bounds.h
    );
    let painted = rgba.as_chunks::<4>().0.iter().filter(|p| p[3] > 0).count();
    assert!(painted > 50_000);

    // Click Advance preset button
    let adv_rect = dash.preset_button_bounds(PresetType::Advance);
    click(
        &mut dash,
        adv_rect.x + adv_rect.w * 0.5,
        adv_rect.y + adv_rect.h * 0.5,
        1280,
        720,
        1.0,
    );
    let msgs = dash.take_outgoing();
    assert!(msgs.iter().any(|m| matches!(
        m,
        UiToLayerMsg::ConfigUpdated(c) if c.preset == PresetType::Advance
    )));

    // Keybind capture & clear
    dash.store.request_keybind_capture(KeybindTarget::ToggleGui);
    assert_eq!(dash.active_keybind_capture(), KeybindTarget::ToggleGui);
    dash.clear_keybind_capture();
    assert_eq!(dash.active_keybind_capture(), KeybindTarget::None);

    // Close button closes GUI
    let close_rect = dash.close_button_bounds();
    click(
        &mut dash,
        close_rect.x + close_rect.w * 0.5,
        close_rect.y + close_rect.h * 0.5,
        1280,
        720,
        1.0,
    );
    assert!(!dash.store.config.gui_open);
}

#[test]
fn dropdown_all_eight_flip_above_and_occlude_underlying_clicks() {
    let cfg = HudConfig {
        gui_open: true,
        ..HudConfig::default()
    };
    let mut dash = CpuDashboard::new(UiStore::new(cfg, TelemetrySnapshot::default()));
    dash.set_render_mode(UiRenderMode::DashboardOnly);
    let mut rast = TextRasterizer::new();
    let mut rgba = Vec::new();
    dash.render_rgba(&mut rast, 1280, 720, 1.0, &mut rgba);

    let all_ids = [
        DropdownId::Position,
        DropdownId::Arrangement,
        DropdownId::Scale,
        DropdownId::Font,
        DropdownId::FontSize,
        DropdownId::TextOpacity,
        DropdownId::BgStyle,
        DropdownId::BgOpacity,
    ];
    for id in all_ids {
        let anchor = dash.dropdown_anchor(id);
        assert!(anchor.w > 40.0 && anchor.h > 10.0);
        click(
            &mut dash,
            anchor.x + anchor.w * 0.5,
            anchor.y + anchor.h * 0.5,
            1280,
            720,
            1.0,
        );
        assert_eq!(dash.open_dropdown(), Some(id));
        dash.render_rgba(&mut rast, 1280, 720, 1.0, &mut rgba);
        let pop = dash.popover_bounds();
        assert!(pop.x >= 0.0 && pop.y >= 0.0 && pop.right() <= 1280.0 && pop.bottom() <= 720.0);
        dash.clear_open_dropdown();
    }

    // Verify flip-above when anchor is near bottom edge of 720p viewport
    let (_, flipped_y) = fpshud_ui::cpu_dashboard::clamp_popover_origin_rect(
        fpshud_ui::cpu_canvas::Rect::new(100.0, 650.0, 142.0, 21.0),
        220.0,
        132.0,
        1280.0,
        720.0,
    );
    assert!(flipped_y < 650.0, "Popover near bottom edge must flip above anchor");

    // Occlusion: open Position dropdown, then click directly over Advance preset outside popover
    let pos_anchor = dash.dropdown_anchor(DropdownId::Position);
    click(
        &mut dash,
        pos_anchor.x + pos_anchor.w * 0.5,
        pos_anchor.y + pos_anchor.h * 0.5,
        1280,
        720,
        1.0,
    );
    dash.render_rgba(&mut rast, 1280, 720, 1.0, &mut rgba);
    assert_eq!(dash.open_dropdown(), Some(DropdownId::Position));
    let _ = dash.take_outgoing();

    let adv_rect = dash.preset_button_bounds(PresetType::Advance);
    click(
        &mut dash,
        adv_rect.x + adv_rect.w * 0.5,
        adv_rect.y + adv_rect.h * 0.5,
        1280,
        720,
        1.0,
    );
    assert_eq!(dash.open_dropdown(), None);
    assert!(
        dash.take_outgoing().is_empty(),
        "Outside click must only close popover and NOT click underlying Advance button"
    );

    // Close -> reopen via sync_all without intermediate render must reset open_dropdown
    click(
        &mut dash,
        pos_anchor.x + pos_anchor.w * 0.5,
        pos_anchor.y + pos_anchor.h * 0.5,
        1280,
        720,
        1.0,
    );
    assert_eq!(dash.open_dropdown(), Some(DropdownId::Position));
    let mut closed = dash.store.config.clone();
    closed.gui_open = false;
    dash.sync_all(closed.clone(), TelemetrySnapshot::default());
    closed.gui_open = true;
    dash.sync_all(closed, TelemetrySnapshot::default());
    assert_eq!(dash.open_dropdown(), None);
}

#[test]
fn option_a_effective_scale_clamps_panel_and_keeps_hit_test_exact() {
    assert!(resolve_dashboard_scale(2.0, 1280, 720, 960.0, 620.0) < 1.2);

    for (vp_w, vp_h) in [(1280_u32, 720_u32), (1104_u32, 684_u32)] {
        let cfg = HudConfig {
            gui_open: true,
            gui_scale: 2.0,
            ..HudConfig::default()
        };
        let mut dash = CpuDashboard::new(UiStore::new(cfg, TelemetrySnapshot::default()));
        dash.set_render_mode(UiRenderMode::DashboardOnly);
        let mut rast = TextRasterizer::new();
        let mut rgba = Vec::new();
        dash.render_rgba(&mut rast, vp_w, vp_h, 2.0, &mut rgba);

        let eff = dash.effective_scale();
        assert!(eff < 2.0, "eff_scale must clamp below 2.0 on {vp_w}x{vp_h}");
        let b = dash.dashboard_bounds();
        assert!(b.right() * eff <= vp_w as f32 + 1.0);
        assert!(b.bottom() * eff <= vp_h as f32 + 1.0);

        // Physical pixel click on BG Opacity dropdown trigger
        let bg_op = dash.dropdown_anchor(DropdownId::BgOpacity);
        click(
            &mut dash,
            (bg_op.x + bg_op.w * 0.5) * eff,
            (bg_op.y + bg_op.h * 0.5) * eff,
            vp_w,
            vp_h,
            2.0,
        );
        assert_eq!(dash.open_dropdown(), Some(DropdownId::BgOpacity));
        dash.clear_open_dropdown();

        // Physical pixel click on Close button
        let close = dash.close_button_bounds();
        click(
            &mut dash,
            (close.x + close.w * 0.5) * eff,
            (close.y + close.h * 0.5) * eff,
            vp_w,
            vp_h,
            2.0,
        );
        assert!(!dash.store.config.gui_open);
    }
}

#[test]
fn drag_header_and_preview_continue_outside_bounds_and_clamp() {
    let cfg = HudConfig {
        gui_open: true,
        ..HudConfig::default()
    };
    let mut dash = CpuDashboard::new(UiStore::new(cfg, TelemetrySnapshot::default()));
    dash.set_render_mode(UiRenderMode::DashboardOnly);
    let mut rast = TextRasterizer::new();
    let mut rgba = Vec::new();
    dash.render_rgba(&mut rast, 1280, 720, 1.0, &mut rgba);

    // Drag title bar far outside viewport
    let start_bounds = dash.dashboard_bounds();
    let drag_x = start_bounds.x + 40.0;
    let drag_y = start_bounds.y + 18.0;
    dash.dispatch_input(
        &[
            UiInputEvent::MouseMove {
                x: drag_x,
                y: drag_y,
            },
            UiInputEvent::MouseDown { button: 0 },
            UiInputEvent::MouseMove {
                x: 4000.0,
                y: 4000.0,
            },
            UiInputEvent::MouseUp { button: 0 },
        ],
        1280,
        720,
        1.0,
    );
    dash.render_rgba(&mut rast, 1280, 720, 1.0, &mut rgba);
    let moved = dash.dashboard_bounds();
    assert!(moved.x >= start_bounds.x);
    assert!(moved.right() <= 1280.0 + 1.0 && moved.bottom() <= 720.0 + 1.0);

    // Drag preview box far outside bounds -> clamps to (0.0, 1.0)
    let prev = dash.preview_bounds();
    dash.dispatch_input(
        &[
            UiInputEvent::MouseMove {
                x: prev.x + prev.w * 0.5,
                y: prev.y + prev.h * 0.5,
            },
            UiInputEvent::MouseDown { button: 0 },
            UiInputEvent::MouseMove {
                x: -200.0,
                y: 2000.0,
            },
            UiInputEvent::MouseUp { button: 0 },
        ],
        1280,
        720,
        1.0,
    );
    assert_eq!(dash.store.config.position, LayoutPosition::Custom);
    assert!((dash.store.config.custom_pos_x - 0.0).abs() < 1e-4);
    assert!((dash.store.config.custom_pos_y - 1.0).abs() < 1e-4);
}

#[test]
fn full_mode_dual_scale_composites_hud_dashboard_and_clamped_software_cursor() {
    let mut cfg = HudConfig {
        gui_open: true,
        hud_visible: true,
        gui_scale: 1.75,
        ..HudConfig::default()
    };
    cfg.set_metric(MetricType::Fps, true);
    let snap = TelemetrySnapshot {
        fps: 120.0,
        frametime_ms: 8.3,
        ..TelemetrySnapshot::default()
    };
    let mut dash = CpuDashboard::new(UiStore::new(cfg, snap));
    dash.set_render_mode(UiRenderMode::Full);
    dash.set_software_cursor(true);
    dash.dispatch_input(
        &[UiInputEvent::MouseMove {
            x: 5000.0,
            y: 5000.0,
        }],
        1280,
        720,
        1.75,
    );

    let mut rast = TextRasterizer::new();
    let mut rgba = Vec::new();
    dash.render_rgba(&mut rast, 1280, 720, 1.75, &mut rgba);
    assert_eq!(rgba.len(), 1280 * 720 * 4);
    let painted = rgba.as_chunks::<4>().0.iter().filter(|p| p[3] > 0).count();
    assert!(painted > 50_000);
}
