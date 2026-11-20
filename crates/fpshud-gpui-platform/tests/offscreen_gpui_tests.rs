use fpshud_gpui_platform::{
    OffscreenDisplay, OffscreenGpuiRuntime,
    fonts::{
        OPEN_SANS_BOLD_TTF, VERA_MONO_BOLD_TTF, create_embedded_text_system,
        validate_embedded_font_headers,
    },
    translate_ui_input_event,
    window::logical_to_device_size,
};
use fpshud_input::UiInputEvent;
use fpshud_ipc::{ShmRingReader, ShmRingWriter};
use gpui::{
    Context, DevicePixels, IntoElement, Modifiers, ParentElement, PlatformDisplay, PlatformInput,
    Point, Render, Size, Styled, Window, div, px, rgba,
};

struct ProbeOverlayView;

impl Render for ProbeOverlayView {
    fn render(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        // Leave outer margin transparent (alpha=0) and place a semi-transparent red box + white embedded text in center
        div().size_full().p(px(40.0)).child(
            div()
                .w(px(300.0))
                .h(px(120.0))
                .bg(rgba(0xff000080))
                .p(px(16.0))
                .text_color(rgba(0xffffffff))
                .text_size(px(24.0))
                .child("FPS 144"),
        )
    }
}

#[test]
fn embedded_fonts_register_without_fontconfig() {
    assert!(!OPEN_SANS_BOLD_TTF.is_empty());
    assert!(!VERA_MONO_BOLD_TTF.is_empty());
    assert!(validate_embedded_font_headers());

    let ts = create_embedded_text_system();
    let families = ts.all_font_names();
    assert!(!families.is_empty());
    let font_id = ts.font_id(&gpui::font("Open Sans")).expect("Open Sans font resolves");
    let metrics = ts.font_metrics(font_id);
    assert!(metrics.units_per_em > 0);
}

#[test]
fn input_translation_and_offscreen_window_properties() {
    let origin = Point::new(px(120.0), px(80.0));
    let mods = Modifiers {
        control: true,
        ..Modifiers::default()
    };

    assert!(matches!(
        translate_ui_input_event(&UiInputEvent::MouseMove { x: 240.0, y: 160.0 }, origin, mods, Some(gpui::MouseButton::Left)),
        PlatformInput::MouseMove(ev)
            if f32::from(ev.position.x) == 120.0
                && f32::from(ev.position.y) == 80.0
                && ev.pressed_button == Some(gpui::MouseButton::Left)
    ));
    assert!(matches!(
        translate_ui_input_event(&UiInputEvent::MouseDown { button: 0 }, origin, mods, None),
        PlatformInput::MouseDown(ev) if ev.button == gpui::MouseButton::Left
    ));
    assert!(matches!(
        translate_ui_input_event(&UiInputEvent::MouseUp { button: 1 }, origin, mods, None),
        PlatformInput::MouseUp(ev) if ev.button == gpui::MouseButton::Right
    ));
    assert!(matches!(
        translate_ui_input_event(&UiInputEvent::ScrollWheel { delta_x: 0.0, delta_y: -3.0 }, origin, mods, None),
        PlatformInput::ScrollWheel(_)
    ));
    assert!(matches!(
        translate_ui_input_event(&UiInputEvent::KeyDown { code: 88, mod_mask: 1 }, origin, mods, None),
        PlatformInput::KeyDown(ev) if ev.keystroke.modifiers.control && ev.keystroke.key == "f12"
    ));
    assert!(matches!(
        translate_ui_input_event(&UiInputEvent::KeyUp { code: 88, mod_mask: 0 }, origin, mods, None),
        PlatformInput::KeyUp(ev) if ev.keystroke.key == "f12"
    ));

    let disp = OffscreenDisplay::new(1920, 1080);
    assert_eq!(f32::from(disp.bounds().size.width), 1920.0);
    assert!(disp.uuid().is_ok());

    let dev = logical_to_device_size(Size::new(px(640.0), px(360.0)), 1.5);
    assert_eq!(dev, Size::new(DevicePixels(960), DevicePixels(540)));
}

#[test]
fn offscreen_runtime_renders_transparent_quad_and_text_then_resizes() {
    let mut rt = OffscreenGpuiRuntime::new_with_view(640, 360, 1.0, |_w, _cx| ProbeOverlayView)
        .expect("offscreen gpui runtime init");

    let mut writer = ShmRingWriter::new(640, 360).unwrap();
    let meta = rt.render_scene_to_slot(&mut writer).unwrap();
    assert_eq!(meta.width, 640);
    assert_eq!(meta.height, 360);

    let reader = ShmRingReader::from_borrowed_fd(writer.fd(), writer.mapped_byte_len()).unwrap();
    let (_, _, rgba_bytes) = reader.read_latest().unwrap();
    assert_eq!(rgba_bytes.len(), 640 * 360 * 4);

    // Outer corner (0,0) outside padding(40) must remain transparent alpha=0
    assert_eq!(rgba_bytes[3], 0);

    // Interior box pixel (x=100, y=100) must have nonzero alpha and red channel > 0
    let box_idx = (100 * 640 + 100) * 4;
    assert!(rgba_bytes[box_idx + 3] > 0);
    assert!(rgba_bytes[box_idx] > 0);

    // Check at least some bright text pixels rendered inside box bounds (40..340, 40..160)
    let mut bright_text_px = 0_usize;
    for y in 48..140 {
        for x in 48..220 {
            let idx = (y * 640 + x) * 4;
            if rgba_bytes[idx + 1] > 80 && rgba_bytes[idx + 2] > 80 {
                bright_text_px += 1;
            }
        }
    }
    assert!(bright_text_px > 10, "expected rasterized white glyphs, got {bright_text_px}");

    rt.dispatch_input(&[UiInputEvent::MouseMove { x: 100.0, y: 100.0 }]);
    rt.resize(800, 450);
    writer.resize(800, 450).unwrap();
    let meta2 = rt.render_scene_to_slot(&mut writer).unwrap();
    assert_eq!((meta2.width, meta2.height), (800, 450));
}
