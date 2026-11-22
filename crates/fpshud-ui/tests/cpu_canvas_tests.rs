use fpshud_telemetry::TelemetrySnapshot;
use fpshud_ui::cpu_canvas::{CpuCanvas, Rect, TextRasterizer, validate_embedded_font_headers};
use fpshud_ui::state::rgba_u32;

#[test]
fn embedded_fonts_valid_and_text_measures_nonzero() {
    assert!(validate_embedded_font_headers());
    let mut rast = TextRasterizer::new();
    let w_sans = rast.measure_text(false, 14.0, "FPSHUD Settings");
    let w_mono = rast.measure_text(true, 14.0, "60 FPS");
    assert!(w_sans > 40.0 && w_mono > 20.0);
}

#[test]
fn canvas_rounded_rect_clip_toggle_sparkline_and_extreme_cursor_no_panic() {
    let mut buf = vec![0_u8; 320 * 180 * 4];
    let mut rast = TextRasterizer::new();
    let mut canvas = CpuCanvas::new(&mut buf, 320, 180, 1.0);
    canvas.fill_rounded_rect(
        Rect::new(10.0, 10.0, 120.0, 40.0),
        6.0,
        rgba_u32(20, 30, 50, 240),
    );
    canvas.stroke_rounded_rect(
        Rect::new(10.0, 10.0, 120.0, 40.0),
        6.0,
        1.0,
        rgba_u32(90, 160, 240, 255),
    );
    canvas.draw_toggle_switch(Rect::new(150.0, 12.0, 34.0, 18.0), true);
    canvas.draw_chevron(200.0, 20.0, false, rgba_u32(180, 210, 255, 255));

    let snap = TelemetrySnapshot {
        frametime_history: vec![16.6, 16.7, 25.0, 40.0],
        ..TelemetrySnapshot::default()
    };
    canvas.draw_frametime_sparkline(
        Rect::new(10.0, 70.0, 160.0, 24.0),
        &snap,
        rgba_u32(72, 140, 220, 140),
    );

    // Clipped text must not paint pixels outside clip rect x >= 80
    canvas.with_clip(Rect::new(10.0, 110.0, 60.0, 24.0), |c| {
        c.draw_text_in_rect(
            &mut rast,
            Rect::new(10.0, 110.0, 200.0, 24.0),
            false,
            13.0,
            rgba_u32(255, 255, 255, 255),
            "VERY LONG TEXT THAT MUST BE CLIPPED",
            false,
        );
    });
    // Review Focus #4: extreme cursor coords clamp safely without OOB panic
    canvas.draw_software_cursor(5000.0, 5000.0);
    canvas.draw_software_cursor(-50.0, -50.0);

    // Pixel at (90, 120) must remain transparent (0)
    assert_eq!(buf[(120 * 320 + 90) * 4 + 3], 0);

    let non_zero_alpha = buf.as_chunks::<4>().0.iter().filter(|p| p[3] > 0).count();
    assert!(non_zero_alpha > 4000);
}

#[test]
fn cpu_hud_renders_into_shared_canvas_with_hud_and_toast() {
    use fpshud_core::HudConfig;
    use fpshud_ui::cpu_hud::CpuHudRenderer;
    use fpshud_ui::state::UiStore;
    use fpshud_ui::toast::ToastState;

    let cfg = HudConfig {
        hud_visible: true,
        gui_open: false,
        ..HudConfig::default()
    };
    assert!(ToastState::format_hint_text(&cfg).contains("FPSHUD active"));

    let snap = TelemetrySnapshot {
        fps: 144.0,
        frametime_ms: 6.9,
        ..TelemetrySnapshot::default()
    };
    let mut store = UiStore::new(cfg, snap);
    store.elapsed_sec = 1.0;

    let hud = CpuHudRenderer::new();
    let mut rast = TextRasterizer::new();
    let frame = hud
        .render_with_rasterizer(&mut rast, &store, 1280, 720, 1.0)
        .expect("HUD frame");
    assert!(frame.width > 40 && frame.height > 20);

    let mut full_buf = vec![0_u8; 1280 * 720 * 4];
    let mut canvas = CpuCanvas::new(&mut full_buf, 1280, 720, 1.0);
    hud.render_into_canvas(&mut canvas, &mut rast, &store, 1280.0, 720.0);
    let top_alpha = full_buf[..1280 * 120 * 4]
        .as_chunks::<4>()
        .0
        .iter()
        .filter(|p| p[3] > 0)
        .count();
    let bot_alpha = full_buf[1280 * 640 * 4..]
        .as_chunks::<4>()
        .0
        .iter()
        .filter(|p| p[3] > 0)
        .count();
    assert!(top_alpha > 200);
    assert!(bot_alpha > 200);
}
