//! Pure-Rust isolated Wayland overlay window (`zwlr_layer_shell_v1` primary, `xdg_toplevel` fallback).
//!
//! Runs inside the `fpshud-ui` daemon process using `wayland-client` pure-Rust socket transport
//! (zero `libwayland-client` `DT_NEEDED`, zero `libc`, zero `#[repr(C)]`). While open, the Wayland
//! compositor exclusively routes all pointer, relative-pointer, XWayland raw input, and keyboard
//! events to this overlay surface with the real native OS cursor (`wp_cursor_shape_v1`),
//! completely preventing double-input into any native Wayland or XWayland/Proton/Wine game.

use anyhow::{Context, Result, anyhow};
use fpshud_input::UiInputEvent;
use rustix::fs::{MemfdFlags, ftruncate, memfd_create};
use std::fs::File;
use std::io;
use std::os::fd::AsFd;
use std::os::unix::fs::{FileExt, FileTypeExt};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use wayland_client::backend::WaylandError;
use wayland_client::protocol::{
    wl_buffer, wl_compositor, wl_keyboard, wl_pointer, wl_registry, wl_seat, wl_shm, wl_shm_pool,
    wl_surface,
};
use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle, WEnum};
use wayland_protocols::wp::cursor_shape::v1::client::{
    wp_cursor_shape_device_v1, wp_cursor_shape_manager_v1,
};
use wayland_protocols::xdg::shell::client::{xdg_surface, xdg_toplevel, xdg_wm_base};
use wayland_protocols_wlr::layer_shell::v1::client::{zwlr_layer_shell_v1, zwlr_layer_surface_v1};

const DEFAULT_FALLBACK_W: u32 = 1280;
const DEFAULT_FALLBACK_H: u32 = 720;
const XDG_FALLBACK_W: u32 = 1104;
const XDG_FALLBACK_H: u32 = 684;

/// Converts premultiplied RGBA8888 pixels from GPUI/wgpu into little-endian Wayland `ARGB8888` (`[B, G, R, A]`).
pub fn rgba_to_wl_argb8888(rgba: &[u8], out_argb: &mut [u8]) {
    let (src_chunks, _) = rgba.as_chunks::<4>();
    let (dst_chunks, _) = out_argb.as_chunks_mut::<4>();
    for (src, dst) in src_chunks.iter().zip(dst_chunks.iter_mut()) {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = src[3];
    }
}

/// Decodes little-endian `u32` Linux evdev keycodes carried in `wl_keyboard::Event::Enter`.
#[must_use]
pub fn decode_wl_enter_keys(raw: &[u8]) -> Vec<u32> {
    let (chunks, _) = raw.as_chunks::<4>();
    chunks.iter().copied().map(u32::from_le_bytes).collect()
}

/// Maps Wayland/Linux evdev pointer button codes to HUD button indices (`0=left, 1=right, 2=middle`).
#[must_use]
pub const fn map_wl_pointer_button(button: u32) -> Option<u8> {
    match button {
        0x110 => Some(0),
        0x111 => Some(1),
        0x112 => Some(2),
        _ => None,
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum OverlayKeyEvent {
    InitialHeld(Vec<u32>),
    Key { code: u32, down: bool },
}

#[derive(Debug, Default)]
pub struct OverlayPollResult {
    pub ui_events: Vec<UiInputEvent>,
    pub key_events: Vec<OverlayKeyEvent>,
    pub resized: Option<(u32, u32)>,
    pub closed: bool,
}

enum OverlayRole {
    LayerSurface(zwlr_layer_surface_v1::ZwlrLayerSurfaceV1),
    XdgToplevel {
        xdg_surface: xdg_surface::XdgSurface,
        toplevel: xdg_toplevel::XdgToplevel,
    },
}

/// Picks the first non-busy `wl_shm` buffer slot, returning `None` if all slots are still held by the compositor.
#[must_use]
pub fn pick_free_buffer_slot(busy: &[bool]) -> Option<usize> {
    busy.iter().position(|&b| !b)
}

struct ShmBufferSlot {
    file: File,
    pool: wl_shm_pool::WlShmPool,
    buffer: wl_buffer::WlBuffer,
}

#[derive(Debug, Clone, Copy)]
struct BufferTag {
    generation: u64,
    slot_idx: usize,
}

struct OverlayState {
    compositor: Option<wl_compositor::WlCompositor>,
    shm: Option<wl_shm::WlShm>,
    seat: Option<wl_seat::WlSeat>,
    layer_shell: Option<zwlr_layer_shell_v1::ZwlrLayerShellV1>,
    xdg_wm_base: Option<xdg_wm_base::XdgWmBase>,
    cursor_mgr: Option<wp_cursor_shape_manager_v1::WpCursorShapeManagerV1>,
    pointer: Option<wl_pointer::WlPointer>,
    keyboard: Option<wl_keyboard::WlKeyboard>,
    cursor_shape_device: Option<wp_cursor_shape_device_v1::WpCursorShapeDeviceV1>,

    configured: bool,
    size: (u32, u32),
    pending_xdg_size: (u32, u32),
    resized: Option<(u32, u32)>,
    closed: bool,

    buffer_generation: u64,
    buffer_busy: [bool; 3],

    ui_events: Vec<UiInputEvent>,
    key_events: Vec<OverlayKeyEvent>,
}

impl OverlayState {
    fn new() -> Self {
        Self {
            compositor: None,
            shm: None,
            seat: None,
            layer_shell: None,
            xdg_wm_base: None,
            cursor_mgr: None,
            pointer: None,
            keyboard: None,
            cursor_shape_device: None,
            configured: false,
            size: (DEFAULT_FALLBACK_W, DEFAULT_FALLBACK_H),
            pending_xdg_size: (0, 0),
            resized: None,
            closed: false,
            buffer_generation: 0,
            buffer_busy: [false; 3],
            ui_events: Vec::new(),
            key_events: Vec::new(),
        }
    }

    fn ensure_cursor_shape_device(&mut self, qh: &QueueHandle<Self>) {
        if self.cursor_shape_device.is_none()
            && let (Some(mgr), Some(ptr)) = (&self.cursor_mgr, &self.pointer)
        {
            self.cursor_shape_device = Some(mgr.get_pointer(ptr, qh, ()));
        }
    }

    fn push_mouse_move(&mut self, x: f32, y: f32) {
        if let Some(UiInputEvent::MouseMove { x: lx, y: ly }) = self.ui_events.last_mut() {
            *lx = x;
            *ly = y;
        } else {
            self.ui_events.push(UiInputEvent::MouseMove { x, y });
        }
    }

    fn update_configured_size(&mut self, width: u32, height: u32) {
        let w = width.max(1);
        let h = height.max(1);
        if self.configured && (w, h) != self.size {
            self.resized = Some((w, h));
        }
        self.size = (w, h);
        self.configured = true;
    }
}

impl Dispatch<wl_registry::WlRegistry, ()> for OverlayState {
    fn event(
        state: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global {
            name,
            interface,
            version,
        } = event
        {
            match interface.as_str() {
                "wl_compositor" if state.compositor.is_none() => {
                    state.compositor = Some(registry.bind(name, version.min(4), qh, ()));
                }
                "wl_shm" if state.shm.is_none() => {
                    state.shm = Some(registry.bind(name, 1, qh, ()));
                }
                "wl_seat" if state.seat.is_none() => {
                    state.seat = Some(registry.bind(name, version.min(7), qh, ()));
                }
                "zwlr_layer_shell_v1" if state.layer_shell.is_none() => {
                    state.layer_shell = Some(registry.bind(name, version.min(4), qh, ()));
                }
                "xdg_wm_base" if state.xdg_wm_base.is_none() => {
                    state.xdg_wm_base = Some(registry.bind(name, version.min(5), qh, ()));
                }
                "wp_cursor_shape_manager_v1" if state.cursor_mgr.is_none() => {
                    state.cursor_mgr = Some(registry.bind(name, 1, qh, ()));
                }
                _ => {}
            }
        }
    }
}

impl Dispatch<wl_compositor::WlCompositor, ()> for OverlayState {
    fn event(
        _: &mut Self,
        _: &wl_compositor::WlCompositor,
        _: wl_compositor::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_shm::WlShm, ()> for OverlayState {
    fn event(
        _: &mut Self,
        _: &wl_shm::WlShm,
        _: wl_shm::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_shm_pool::WlShmPool, ()> for OverlayState {
    fn event(
        _: &mut Self,
        _: &wl_shm_pool::WlShmPool,
        _: wl_shm_pool::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_buffer::WlBuffer, BufferTag> for OverlayState {
    fn event(
        state: &mut Self,
        _: &wl_buffer::WlBuffer,
        event: wl_buffer::Event,
        tag: &BufferTag,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if matches!(event, wl_buffer::Event::Release)
            && tag.generation == state.buffer_generation
            && tag.slot_idx < state.buffer_busy.len()
        {
            state.buffer_busy[tag.slot_idx] = false;
        }
    }
}

impl Dispatch<wl_surface::WlSurface, ()> for OverlayState {
    fn event(
        _: &mut Self,
        _: &wl_surface::WlSurface,
        _: wl_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_seat::WlSeat, ()> for OverlayState {
    fn event(
        state: &mut Self,
        seat: &wl_seat::WlSeat,
        event: wl_seat::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_seat::Event::Capabilities {
            capabilities: WEnum::Value(caps),
        } = event
        {
            if caps.contains(wl_seat::Capability::Pointer) && state.pointer.is_none() {
                state.pointer = Some(seat.get_pointer(qh, ()));
                state.ensure_cursor_shape_device(qh);
            }
            if caps.contains(wl_seat::Capability::Keyboard) && state.keyboard.is_none() {
                state.keyboard = Some(seat.get_keyboard(qh, ()));
            }
        }
    }
}

impl Dispatch<wl_pointer::WlPointer, ()> for OverlayState {
    fn event(
        state: &mut Self,
        _: &wl_pointer::WlPointer,
        event: wl_pointer::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        match event {
            wl_pointer::Event::Enter {
                serial,
                surface_x,
                surface_y,
                ..
            } => {
                state.ensure_cursor_shape_device(qh);
                if let Some(shape_dev) = &state.cursor_shape_device {
                    shape_dev.set_shape(serial, wp_cursor_shape_device_v1::Shape::Default);
                }
                state.push_mouse_move(surface_x as f32, surface_y as f32);
            }
            wl_pointer::Event::Motion {
                surface_x,
                surface_y,
                ..
            } => {
                state.push_mouse_move(surface_x as f32, surface_y as f32);
            }
            wl_pointer::Event::Button {
                button,
                state: WEnum::Value(btn_state),
                ..
            } => {
                if let Some(idx) = map_wl_pointer_button(button) {
                    let ev = match btn_state {
                        wl_pointer::ButtonState::Pressed => UiInputEvent::MouseDown { button: idx },
                        wl_pointer::ButtonState::Released => UiInputEvent::MouseUp { button: idx },
                        _ => return,
                    };
                    state.ui_events.push(ev);
                }
            }
            wl_pointer::Event::Axis {
                axis: WEnum::Value(axis),
                value,
                ..
            } => {
                let delta = -(value as f32) / 15.0;
                match axis {
                    wl_pointer::Axis::VerticalScroll => {
                        state.ui_events.push(UiInputEvent::ScrollWheel {
                            delta_x: 0.0,
                            delta_y: delta,
                        });
                    }
                    wl_pointer::Axis::HorizontalScroll => {
                        state.ui_events.push(UiInputEvent::ScrollWheel {
                            delta_x: delta,
                            delta_y: 0.0,
                        });
                    }
                    _ => {}
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_keyboard::WlKeyboard, ()> for OverlayState {
    fn event(
        state: &mut Self,
        _: &wl_keyboard::WlKeyboard,
        event: wl_keyboard::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_keyboard::Event::Enter { keys, .. } => {
                let held = decode_wl_enter_keys(&keys);
                if !held.is_empty() {
                    state.key_events.push(OverlayKeyEvent::InitialHeld(held));
                }
            }
            wl_keyboard::Event::Key {
                key,
                state: WEnum::Value(key_state),
                ..
            } => {
                let down = match key_state {
                    wl_keyboard::KeyState::Pressed => true,
                    wl_keyboard::KeyState::Released => false,
                    _ => return,
                };
                state.key_events.push(OverlayKeyEvent::Key { code: key, down });
            }
            wl_keyboard::Event::Leave { .. } => {
                state.closed = true;
            }
            _ => {}
        }
    }
}

impl Dispatch<zwlr_layer_shell_v1::ZwlrLayerShellV1, ()> for OverlayState {
    fn event(
        _: &mut Self,
        _: &zwlr_layer_shell_v1::ZwlrLayerShellV1,
        _: zwlr_layer_shell_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<zwlr_layer_surface_v1::ZwlrLayerSurfaceV1, ()> for OverlayState {
    fn event(
        state: &mut Self,
        layer_surface: &zwlr_layer_surface_v1::ZwlrLayerSurfaceV1,
        event: zwlr_layer_surface_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwlr_layer_surface_v1::Event::Configure {
                serial,
                width,
                height,
            } => {
                layer_surface.ack_configure(serial);
                let w = if width > 0 { width } else { DEFAULT_FALLBACK_W };
                let h = if height > 0 { height } else { DEFAULT_FALLBACK_H };
                if width == 0 || height == 0 {
                    layer_surface.set_size(w, h);
                }
                state.update_configured_size(w, h);
            }
            zwlr_layer_surface_v1::Event::Closed => {
                state.closed = true;
            }
            _ => {}
        }
    }
}

impl Dispatch<xdg_wm_base::XdgWmBase, ()> for OverlayState {
    fn event(
        _: &mut Self,
        wm_base: &xdg_wm_base::XdgWmBase,
        event: xdg_wm_base::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_wm_base::Event::Ping { serial } = event {
            wm_base.pong(serial);
        }
    }
}

impl Dispatch<xdg_surface::XdgSurface, ()> for OverlayState {
    fn event(
        state: &mut Self,
        xdg_surf: &xdg_surface::XdgSurface,
        event: xdg_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_surface::Event::Configure { serial } = event {
            xdg_surf.ack_configure(serial);
            let w = if state.pending_xdg_size.0 > 0 {
                state.pending_xdg_size.0
            } else {
                XDG_FALLBACK_W
            };
            let h = if state.pending_xdg_size.1 > 0 {
                state.pending_xdg_size.1
            } else {
                XDG_FALLBACK_H
            };
            state.update_configured_size(w, h);
        }
    }
}

impl Dispatch<xdg_toplevel::XdgToplevel, ()> for OverlayState {
    fn event(
        state: &mut Self,
        _: &xdg_toplevel::XdgToplevel,
        event: xdg_toplevel::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            xdg_toplevel::Event::Configure { width, height, .. } => {
                if width > 0 && height > 0 {
                    state.pending_xdg_size = (width as u32, height as u32);
                }
            }
            xdg_toplevel::Event::Close => {
                state.closed = true;
            }
            _ => {}
        }
    }
}

impl Dispatch<wp_cursor_shape_manager_v1::WpCursorShapeManagerV1, ()> for OverlayState {
    fn event(
        _: &mut Self,
        _: &wp_cursor_shape_manager_v1::WpCursorShapeManagerV1,
        _: wp_cursor_shape_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wp_cursor_shape_device_v1::WpCursorShapeDeviceV1, ()> for OverlayState {
    fn event(
        _: &mut Self,
        _: &wp_cursor_shape_device_v1::WpCursorShapeDeviceV1,
        _: wp_cursor_shape_device_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

/// Isolated Wayland overlay surface owned by `fpshud-ui` whenever the Settings GUI is open.
pub struct WaylandOverlayPortal {
    conn: Connection,
    event_queue: EventQueue<OverlayState>,
    state: OverlayState,
    surface: wl_surface::WlSurface,
    role: OverlayRole,
    slots: Vec<ShmBufferSlot>,
    slots_size: (u32, u32),
    scratch_argb: Vec<u8>,
}

/// Discovers live `wayland-*` Unix socket paths in `dir`, excluding `.lock` and non-socket entries.
#[must_use]
pub fn discover_wayland_socket_candidates(dir: &Path) -> Vec<PathBuf> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut found = Vec::new();
    for entry in entries.flatten() {
        let name = entry.file_name();
        let Some(name_str) = name.to_str() else {
            continue;
        };
        if !name_str.starts_with("wayland-") || name_str.ends_with(".lock") {
            continue;
        }
        if entry
            .file_type()
            .is_ok_and(|ft| ft.is_socket())
        {
            found.push(entry.path());
        }
    }
    found.sort();
    found
}

fn connect_wayland_compositor() -> Result<Connection> {
    if std::env::var_os("WAYLAND_DISPLAY").is_some()
        && let Ok(conn) = Connection::connect_to_env()
    {
        return Ok(conn);
    }

    // Proton / Wine / SDL-X11 games often unset WAYLAND_DISPLAY while running under XWayland
    // (`DISPLAY` still set). Probe `$XDG_RUNTIME_DIR/wayland-*` so we still attach to the active
    // Wayland compositor for 100% isolated input, while staying strictly headless when both
    // `WAYLAND_DISPLAY` and `DISPLAY` are unset.
    if std::env::var_os("DISPLAY").is_some() {
        let runtime_dir = std::env::var_os("XDG_RUNTIME_DIR")
            .filter(|v| !v.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from(format!("/run/user/{}", rustix::process::getuid().as_raw())));
        for sock in discover_wayland_socket_candidates(&runtime_dir) {
            if let Ok(stream) = UnixStream::connect(&sock)
                && let Ok(conn) = Connection::from_socket(stream)
            {
                return Ok(conn);
            }
        }
    }

    Err(anyhow!("no reachable Wayland compositor socket"))
}

impl WaylandOverlayPortal {
    /// Attempts to open an isolated Wayland overlay (`Layer::Overlay` preferred, `xdg_toplevel` fallback).
    /// Returns `None` on pure X11, headless CI, or when `FPSHUD_DISABLE_WAYLAND_OVERLAY=1`.
    pub fn try_open() -> Option<Self> {
        if std::env::var("FPSHUD_DISABLE_WAYLAND_OVERLAY").as_deref() == Ok("1") {
            return None;
        }
        if std::env::var_os("WAYLAND_DISPLAY").is_none() && std::env::var_os("DISPLAY").is_none() {
            return None;
        }
        Self::open_inner().ok()
    }

    fn open_inner() -> Result<Self> {
        let conn = connect_wayland_compositor().context("connect wayland compositor")?;
        let mut event_queue = conn.new_event_queue::<OverlayState>();
        let qh = event_queue.handle();
        let _registry = conn.display().get_registry(&qh, ());

        let mut state = OverlayState::new();
        event_queue
            .roundtrip(&mut state)
            .context("wayland registry roundtrip")?;
        // Second roundtrip processes wl_seat capabilities (pointer + keyboard)
        event_queue
            .roundtrip(&mut state)
            .context("wayland seat capabilities roundtrip")?;
        state.ensure_cursor_shape_device(&qh);

        let compositor = state
            .compositor
            .clone()
            .ok_or_else(|| anyhow!("wl_compositor unavailable"))?;
        if state.shm.is_none() {
            return Err(anyhow!("wl_shm unavailable"));
        }

        let surface = compositor.create_surface(&qh, ());
        let role = if let Some(layer_shell) = state.layer_shell.clone() {
            let ls = layer_shell.get_layer_surface(
                &surface,
                None,
                zwlr_layer_shell_v1::Layer::Overlay,
                "fpshud-gui".to_string(),
                &qh,
                (),
            );
            ls.set_anchor(zwlr_layer_surface_v1::Anchor::all());
            ls.set_exclusive_zone(-1);
            ls.set_keyboard_interactivity(
                zwlr_layer_surface_v1::KeyboardInteractivity::Exclusive,
            );
            surface.commit();
            OverlayRole::LayerSurface(ls)
        } else if let Some(wm_base) = state.xdg_wm_base.clone() {
            let xdg_surface = wm_base.get_xdg_surface(&surface, &qh, ());
            let toplevel = xdg_surface.get_toplevel(&qh, ());
            toplevel.set_title("FPSHUD Settings".to_string());
            toplevel.set_app_id("fpshud-settings".to_string());
            surface.commit();
            OverlayRole::XdgToplevel {
                xdg_surface,
                toplevel,
            }
        } else {
            surface.destroy();
            return Err(anyhow!("neither zwlr_layer_shell_v1 nor xdg_wm_base available"));
        };

        for _ in 0..4 {
            event_queue
                .roundtrip(&mut state)
                .context("wayland initial configure roundtrip")?;
            if state.configured || state.closed {
                break;
            }
        }
        if !state.configured || state.closed {
            match role {
                OverlayRole::LayerSurface(ls) => ls.destroy(),
                OverlayRole::XdgToplevel {
                    xdg_surface,
                    toplevel,
                } => {
                    toplevel.destroy();
                    xdg_surface.destroy();
                }
            }
            surface.destroy();
            return Err(anyhow!("wayland overlay surface was not configured"));
        }

        Ok(Self {
            conn,
            event_queue,
            state,
            surface,
            role,
            slots: Vec::new(),
            slots_size: (0, 0),
            scratch_argb: Vec::new(),
        })
    }

    #[must_use]
    pub fn size(&self) -> (u32, u32) {
        self.state.size
    }

    #[cfg(test)]
    #[must_use]
    pub fn is_layer_shell(&self) -> bool {
        matches!(self.role, OverlayRole::LayerSurface(_))
    }

    /// Non-blocking Wayland event drain returning pointer/scroll/keyboard/resize/close updates.
    pub fn poll_events(&mut self) -> OverlayPollResult {
        let _ = self.conn.flush();
        if self.event_queue.dispatch_pending(&mut self.state).is_err() {
            self.state.closed = true;
        }
        if !self.state.closed
            && let Some(guard) = self.event_queue.prepare_read()
        {
            match guard.read() {
                Ok(_) => {
                    if self.event_queue.dispatch_pending(&mut self.state).is_err() {
                        self.state.closed = true;
                    }
                }
                Err(WaylandError::Io(ref io_err))
                    if io_err.kind() == io::ErrorKind::WouldBlock => {}
                Err(_) => {
                    self.state.closed = true;
                }
            }
        }
        let _ = self.conn.flush();

        OverlayPollResult {
            ui_events: std::mem::take(&mut self.state.ui_events),
            key_events: std::mem::take(&mut self.state.key_events),
            resized: self.state.resized.take(),
            closed: self.state.closed,
        }
    }

    fn destroy_slots(&mut self) {
        for slot in self.slots.drain(..) {
            slot.buffer.destroy();
            slot.pool.destroy();
        }
        self.slots_size = (0, 0);
    }

    fn ensure_slots(&mut self, width: u32, height: u32) -> Result<()> {
        if self.slots_size == (width, height) && self.slots.len() == 3 {
            return Ok(());
        }
        self.destroy_slots();

        let shm = self
            .state
            .shm
            .clone()
            .ok_or_else(|| anyhow!("wl_shm unavailable"))?;
        let qh = self.event_queue.handle();
        let stride = width
            .checked_mul(4)
            .ok_or_else(|| anyhow!("overlay stride overflow"))?;
        let byte_len = stride
            .checked_mul(height)
            .ok_or_else(|| anyhow!("overlay buffer byte_len overflow"))?;
        let byte_len_i32 =
            i32::try_from(byte_len).map_err(|_| anyhow!("overlay buffer exceeds i32"))?;

        self.state.buffer_generation = self.state.buffer_generation.wrapping_add(1);
        self.state.buffer_busy = [false; 3];
        let generation = self.state.buffer_generation;

        for slot_idx in 0..3 {
            let fd = memfd_create(c"fpshud-wl-overlay", MemfdFlags::CLOEXEC)
                .context("memfd_create wayland overlay buffer")?;
            ftruncate(&fd, u64::from(byte_len)).context("ftruncate wayland overlay buffer")?;
            let file = File::from(fd);
            let pool = shm.create_pool(file.as_fd(), byte_len_i32, &qh, ());
            let buffer = pool.create_buffer(
                0,
                width as i32,
                height as i32,
                stride as i32,
                wl_shm::Format::Argb8888,
                &qh,
                BufferTag {
                    generation,
                    slot_idx,
                },
            );
            self.slots.push(ShmBufferSlot { file, pool, buffer });
        }
        self.slots_size = (width, height);
        Ok(())
    }

    /// Presents premultiplied RGBA8888 pixels to the isolated Wayland overlay surface.
    /// Returns `Ok(false)` when all 3 buffer slots are still held by the compositor (caller should retry).
    pub fn present_rgba(&mut self, width: u32, height: u32, rgba: &[u8]) -> Result<bool> {
        if self.state.closed {
            return Ok(false);
        }
        let w = width.max(1);
        let h = height.max(1);
        self.ensure_slots(w, h)?;

        let Some(slot_idx) = pick_free_buffer_slot(&self.state.buffer_busy) else {
            return Ok(false);
        };
        let byte_len = (w as usize)
            .checked_mul(h as usize)
            .and_then(|px| px.checked_mul(4))
            .ok_or_else(|| anyhow!("overlay byte_len overflow"))?;
        self.scratch_argb.resize(byte_len, 0);
        rgba_to_wl_argb8888(rgba, &mut self.scratch_argb);

        let slot = &self.slots[slot_idx];
        slot.file
            .write_all_at(&self.scratch_argb, 0)
            .context("write wayland overlay memfd")?;
        self.state.buffer_busy[slot_idx] = true;

        self.surface.attach(Some(&slot.buffer), 0, 0);
        self.surface.damage_buffer(0, 0, w as i32, h as i32);
        self.surface.commit();
        self.conn.flush().context("flush wayland overlay commit")?;
        Ok(true)
    }
}

impl Drop for WaylandOverlayPortal {
    fn drop(&mut self) {
        self.destroy_slots();
        match &self.role {
            OverlayRole::LayerSurface(ls) => ls.destroy(),
            OverlayRole::XdgToplevel {
                xdg_surface,
                toplevel,
            } => {
                toplevel.destroy();
                xdg_surface.destroy();
            }
        }
        self.surface.destroy();
        if let Some(dev) = self.state.cursor_shape_device.take() {
            dev.destroy();
        }
        if let Some(ptr) = self.state.pointer.take()
            && ptr.version() >= 3
        {
            ptr.release();
        }
        if let Some(kb) = self.state.keyboard.take()
            && kb.version() >= 3
        {
            kb.release();
        }
        if let Some(seat) = self.state.seat.take()
            && seat.version() >= 5
        {
            seat.release();
        }
        let _ = self.conn.flush();
    }
}
