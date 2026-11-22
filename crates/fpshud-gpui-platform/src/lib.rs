//! Custom headless GPUI-CE `Platform` & `PlatformWindow` rendering to offscreen `wgpu` + SHM.

pub mod fonts;
pub mod offscreen;
pub mod platform;
pub mod window;

pub use offscreen::OffscreenSurfaceTarget;
pub use platform::{OffscreenDisplay, OffscreenPlatform};
pub use window::{OffscreenWindow, logical_to_device_size};

use fonts::register_embedded_fonts;
use fpshud_input::UiInputEvent;
use fpshud_ipc::{ShmRingWriter, ShmSlotMeta};
use gpui::{
    AnyWindowHandle, App, AppContext, Application, ApplicationHandle, Bounds, Context, Empty,
    KeyDownEvent, KeyUpEvent, Keystroke, Modifiers, MouseButton, MouseDownEvent, MouseMoveEvent,
    MouseUpEvent, Pixels, Platform, PlatformInput, Point, Render, RequestFrameOptions, ScrollDelta,
    ScrollWheelEvent, Size, TouchPhase, Window, WindowBounds, WindowOptions, px,
};
use parking_lot::Mutex;
use std::rc::Rc;
use window::OffscreenWindowState;

#[must_use]
pub fn mod_mask_to_gpui(mod_mask: u32) -> Modifiers {
    Modifiers {
        control: (mod_mask & 1) != 0,
        alt: (mod_mask & 2) != 0,
        shift: (mod_mask & 4) != 0,
        platform: (mod_mask & 8) != 0,
        function: false,
    }
}

#[must_use]
pub fn linux_keycode_to_gpui_key_name(code: u32) -> &'static str {
    match code {
        1 => "escape",
        14 => "backspace",
        15 => "tab",
        28 => "enter",
        57 => "space",
        59 => "f1",
        60 => "f2",
        61 => "f3",
        62 => "f4",
        63 => "f5",
        64 => "f6",
        65 => "f7",
        66 => "f8",
        67 => "f9",
        68 => "f10",
        87 => "f11",
        88 => "f12",
        102 => "home",
        103 => "up",
        104 => "pageup",
        105 => "left",
        106 => "right",
        107 => "end",
        108 => "down",
        109 => "pagedown",
        110 => "insert",
        111 => "delete",
        _ => "unknown",
    }
}

/// Translates serialized `UiInputEvent` values from `libfpshud.so` into GPUI `PlatformInput`.
#[must_use]
pub fn translate_ui_input_event(
    ev: &UiInputEvent,
    cursor_pos: Point<Pixels>,
    modifiers: Modifiers,
    pressed_button: Option<MouseButton>,
) -> PlatformInput {
    match *ev {
        UiInputEvent::MouseMove { .. } => PlatformInput::MouseMove(MouseMoveEvent {
            position: cursor_pos,
            pressed_button,
            modifiers,
        }),
        UiInputEvent::MouseDown { button } => PlatformInput::MouseDown(MouseDownEvent {
            button: map_mouse_button(button),
            position: cursor_pos,
            modifiers,
            click_count: 1,
            first_mouse: false,
        }),
        UiInputEvent::MouseUp { button } => PlatformInput::MouseUp(MouseUpEvent {
            button: map_mouse_button(button),
            position: cursor_pos,
            modifiers,
            click_count: 1,
        }),
        UiInputEvent::ScrollWheel { delta_x, delta_y } => {
            PlatformInput::ScrollWheel(ScrollWheelEvent {
                position: cursor_pos,
                delta: ScrollDelta::Lines(Point::new(delta_x, delta_y)),
                modifiers,
                touch_phase: TouchPhase::Moved,
            })
        }
        UiInputEvent::KeyDown { code, mod_mask } => PlatformInput::KeyDown(KeyDownEvent {
            keystroke: Keystroke {
                modifiers: mod_mask_to_gpui(mod_mask),
                key: linux_keycode_to_gpui_key_name(code).to_string(),
                key_char: None,
            },
            is_held: false,
            prefer_character_input: false,
        }),
        UiInputEvent::KeyUp { code, mod_mask } => PlatformInput::KeyUp(KeyUpEvent {
            keystroke: Keystroke {
                modifiers: mod_mask_to_gpui(mod_mask),
                key: linux_keycode_to_gpui_key_name(code).to_string(),
                key_char: None,
            },
        }),
    }
}

const fn map_mouse_button(button: u8) -> MouseButton {
    match button {
        1 => MouseButton::Right,
        2 => MouseButton::Middle,
        _ => MouseButton::Left,
    }
}

struct EmptyRootView;

impl Render for EmptyRootView {
    fn render(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl gpui::IntoElement {
        Empty
    }
}

/// Manages an embedded GPUI-CE `Application`, offscreen window, embedded fonts, & `wgpu` rendering.
pub struct OffscreenGpuiRuntime {
    platform: Rc<OffscreenPlatform>,
    app: ApplicationHandle,
    _window_handle: AnyWindowHandle,
    window_state: Rc<Mutex<OffscreenWindowState>>,
    cursor_pos: Point<Pixels>,
    modifiers: Modifiers,
    pressed_button: Option<MouseButton>,
    width: u32,
    height: u32,
    scale: f32,
}

impl OffscreenGpuiRuntime {
    pub fn new(width: u32, height: u32, scale: f32) -> anyhow::Result<Self> {
        Self::new_with_view(width, height, scale, |_w, _cx| EmptyRootView)
    }

    pub fn new_with_view<V: Render + 'static>(
        width: u32,
        height: u32,
        scale: f32,
        build: impl FnOnce(&mut Window, &mut Context<V>) -> V + 'static,
    ) -> anyhow::Result<Self> {
        let w = width.max(1);
        let h = height.max(1);
        let s = scale.clamp(0.5, 3.0);
        let platform = OffscreenPlatform::new(w, h, s);
        let app = Application::with_platform(platform.clone()).run_embedded(|_cx| {});

        let bounds = Bounds::new(
            Point::default(),
            Size::new(px((w as f32) / s), px((h as f32) / s)),
        );
        let window_handle = app.update(|cx| {
            cx.open_window(
                WindowOptions {
                    window_bounds: Some(WindowBounds::Windowed(bounds)),
                    titlebar: None,
                    show: true,
                    focus: true,
                    ..WindowOptions::default()
                },
                |window, cx| cx.new(|cx| build(window, cx)),
            )
        })?;

        let window_state = platform
            .last_window_state()
            .ok_or_else(|| anyhow::anyhow!("offscreen window handle was not captured"))?;

        platform.drain_tasks();

        Ok(Self {
            platform,
            app,
            _window_handle: window_handle.into(),
            window_state,
            cursor_pos: Point::default(),
            modifiers: Modifiers::default(),
            pressed_button: None,
            width: w,
            height: h,
            scale: s,
        })
    }

    pub fn register_embedded_fonts(&mut self) -> anyhow::Result<()> {
        register_embedded_fonts(self.platform.text_system().as_ref())
    }

    pub fn update_app<R>(&mut self, f: impl FnOnce(&mut App) -> R) -> R {
        let out = self.app.update(f);
        self.platform.drain_tasks();
        self.mark_dirty();
        out
    }

    pub fn dispatch_input(&mut self, events: &[UiInputEvent]) {
        if events.is_empty() {
            return;
        }
        let scale = self.scale.max(0.25);
        for ev in events {
            match *ev {
                UiInputEvent::MouseMove { x, y } => {
                    self.cursor_pos = Point::new(px(x / scale), px(y / scale));
                    self.window_state.lock().mouse_pos = self.cursor_pos;
                }
                UiInputEvent::MouseDown { button } => {
                    self.pressed_button = Some(map_mouse_button(button));
                }
                UiInputEvent::MouseUp { button } => {
                    let mapped = map_mouse_button(button);
                    if self.pressed_button == Some(mapped) {
                        self.pressed_button = None;
                    }
                }
                UiInputEvent::KeyDown { mod_mask, .. } | UiInputEvent::KeyUp { mod_mask, .. } => {
                    self.modifiers = mod_mask_to_gpui(mod_mask);
                    self.window_state.lock().modifiers = self.modifiers;
                }
                _ => {}
            }
            let platform_ev =
                translate_ui_input_event(ev, self.cursor_pos, self.modifiers, self.pressed_button);
            let cb = self.window_state.lock().input_cb.take();
            if let Some(mut cb) = cb {
                let _ = cb(platform_ev);
                self.window_state.lock().input_cb = Some(cb);
            }
        }
        self.platform.drain_tasks();
        self.mark_dirty();
    }

    pub fn resize(&mut self, width: u32, height: u32) {
        self.resize_with_scale(width, height, self.scale);
    }

    pub fn resize_with_scale(&mut self, width: u32, height: u32, scale: f32) {
        let w = width.max(1);
        let h = height.max(1);
        let s = scale.clamp(0.5, 3.0);
        if (w, h) == (self.width, self.height) && (s - self.scale).abs() < 1e-4 {
            return;
        }
        self.width = w;
        self.height = h;
        self.scale = s;
        let logical_size = Size::new(px((w as f32) / self.scale), px((h as f32) / self.scale));
        self.platform
            .set_display_size_and_scale(w, h, self.scale);

        {
            let mut st = self.window_state.lock();
            st.bounds.size = logical_size;
            let _ = st.target.resize(w, h);
            st.dirty = true;
        }
        let cb = self.window_state.lock().resize_cb.take();
        if let Some(mut cb) = cb {
            cb(logical_size, self.scale);
            self.window_state.lock().resize_cb = Some(cb);
        }
        self.platform.drain_tasks();
    }

    #[must_use]
    pub fn is_dirty(&self) -> bool {
        self.window_state.lock().dirty
    }

    pub fn mark_dirty(&mut self) {
        self.window_state.lock().dirty = true;
    }

    pub fn render_scene_to_slot(&mut self, writer: &mut ShmRingWriter) -> anyhow::Result<ShmSlotMeta> {
        self.platform.drain_tasks();
        let cb = self.window_state.lock().request_frame_cb.take();
        if let Some(mut cb) = cb {
            cb(RequestFrameOptions::default());
            self.window_state.lock().request_frame_cb = Some(cb);
        }
        self.platform.drain_tasks();
        let st = self.window_state.lock();
        st.target.write_to_shm(writer)
    }

    /// Renders the current GPUI scene and copies premultiplied RGBA8888 pixels directly into `out`.
    pub fn render_scene_to_rgba(&mut self, out: &mut Vec<u8>) -> anyhow::Result<(u32, u32)> {
        self.platform.drain_tasks();
        let cb = self.window_state.lock().request_frame_cb.take();
        if let Some(mut cb) = cb {
            cb(RequestFrameOptions::default());
            self.window_state.lock().request_frame_cb = Some(cb);
        }
        self.platform.drain_tasks();
        let st = self.window_state.lock();
        let rgba = st.target.last_rgba();
        let expected = (self.width as usize) * (self.height as usize) * 4;
        out.clear();
        if rgba.len() == expected {
            out.extend_from_slice(rgba);
        } else {
            out.resize(expected, 0);
        }
        Ok((self.width, self.height))
    }
}
