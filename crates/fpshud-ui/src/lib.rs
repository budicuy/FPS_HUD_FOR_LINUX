#![forbid(unsafe_code)]
//! Pure-Rust GPUI-CE HUD overlay, launch toast, and interactive 3-column dashboard.

pub mod dashboard;
pub mod hud;
pub mod state;
pub mod toast;
pub mod widgets;

pub use dashboard::{FpshudRootView, UiRenderMode};
pub use state::{MetricCategory, SensorFilter, UiStore};
pub use widgets::DropdownId;

use fpshud_core::HudConfig;
use fpshud_ipc::UiToLayerMsg;
use fpshud_telemetry::TelemetrySnapshot;
use gpui::{
    App, Context, Global, InteractiveElement, IntoElement, MouseButton, MouseMoveEvent,
    MouseUpEvent, ParentElement, Render, Styled, Window, div, px, rgba,
};

struct UiSyncGlobal {
    seq: u64,
    gui_closed_seq: u64,
    config: HudConfig,
    telemetry: TelemetrySnapshot,
}

impl Global for UiSyncGlobal {}

#[derive(Default)]
struct UiOutgoingQueueGlobal {
    messages: Vec<UiToLayerMsg>,
}

impl Global for UiOutgoingQueueGlobal {}

struct UiSoftwareCursorGlobal {
    seq: u64,
    enabled: bool,
}

impl Global for UiSoftwareCursorGlobal {}

struct UiRenderModeGlobal {
    seq: u64,
    mode: UiRenderMode,
}

impl Global for UiRenderModeGlobal {}

struct UiClearCaptureGlobal {
    seq: u64,
}

impl Global for UiClearCaptureGlobal {}

struct UiClearDropdownGlobal {
    seq: u64,
}

impl Global for UiClearDropdownGlobal {}

pub(crate) fn push_outgoing_global(cx: &mut App, mut msgs: Vec<UiToLayerMsg>) {
    let mut existing = cx
        .try_global::<UiOutgoingQueueGlobal>()
        .map_or_else(Vec::new, |g| g.messages.clone());
    existing.append(&mut msgs);
    cx.set_global(UiOutgoingQueueGlobal { messages: existing });
}

impl FpshudRootView {
    /// Pushes updated configuration and telemetry snapshot into the GPUI app state & requests redraw.
    pub fn sync_all(cx: &mut App, config: HudConfig, telemetry: TelemetrySnapshot) {
        let (next_seq, prev_closed_seq) = cx
            .try_global::<UiSyncGlobal>()
            .map_or((1, 0), |g| (g.seq + 1, g.gui_closed_seq));
        let gui_closed_seq = if !config.gui_open {
            prev_closed_seq + 1
        } else {
            prev_closed_seq
        };
        cx.set_global(UiSyncGlobal {
            seq: next_seq,
            gui_closed_seq,
            config,
            telemetry,
        });
        cx.refresh_windows();
    }

    /// Enables or disables the software fallback cursor when evdev/X11 hides the native OS cursor.
    pub fn set_software_cursor(cx: &mut App, enabled: bool) {
        let next_seq = cx
            .try_global::<UiSoftwareCursorGlobal>()
            .map_or(1, |g| g.seq + 1);
        cx.set_global(UiSoftwareCursorGlobal {
            seq: next_seq,
            enabled,
        });
        cx.refresh_windows();
    }

    /// Selects whether this runtime renders `Full`, `HudOnly`, or `DashboardOnly`.
    pub fn set_render_mode(cx: &mut App, mode: UiRenderMode) {
        let next_seq = cx
            .try_global::<UiRenderModeGlobal>()
            .map_or(1, |g| g.seq + 1);
        cx.set_global(UiRenderModeGlobal {
            seq: next_seq,
            mode,
        });
        cx.refresh_windows();
    }

    /// Clears active keybind capture prompt (e.g. when cancelled via ESC in the external overlay).
    pub fn clear_keybind_capture(cx: &mut App) {
        let next_seq = cx
            .try_global::<UiClearCaptureGlobal>()
            .map_or(1, |g| g.seq + 1);
        cx.set_global(UiClearCaptureGlobal { seq: next_seq });
        cx.refresh_windows();
    }

    /// Closes any open dropdown popover (e.g. when reopening the cached external Wayland overlay).
    pub fn clear_open_dropdown(cx: &mut App) {
        let next_seq = cx
            .try_global::<UiClearDropdownGlobal>()
            .map_or(1, |g| g.seq + 1);
        cx.set_global(UiClearDropdownGlobal { seq: next_seq });
        cx.refresh_windows();
    }

    /// Drains any outgoing IPC actions produced by interactive GPUI dashboard handlers.
    pub fn take_outgoing(cx: &mut App) -> Vec<UiToLayerMsg> {
        let msgs = cx
            .try_global::<UiOutgoingQueueGlobal>()
            .map_or_else(Vec::new, |g| g.messages.clone());
        if !msgs.is_empty() {
            cx.set_global(UiOutgoingQueueGlobal::default());
        }
        msgs
    }
}

impl Render for FpshudRootView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        if let Some(synced) = cx.try_global::<UiSyncGlobal>()
            && synced.seq > self.last_applied_seq
        {
            let keys_changed = synced.config.bind_toggle_gui != self.store.config.bind_toggle_gui
                || synced.config.bind_toggle_hud != self.store.config.bind_toggle_hud
                || synced.config.bind_benchmark != self.store.config.bind_benchmark
                || synced.config.bind_screenshot != self.store.config.bind_screenshot;
            if keys_changed {
                self.store.active_keybind_capture = fpshud_input::KeybindTarget::None;
            }
            if synced.gui_closed_seq > self.last_gui_closed_seq {
                self.open_dropdown = None;
                self.last_gui_closed_seq = synced.gui_closed_seq;
            }
            self.store.config = synced.config.clone();
            self.store.telemetry = synced.telemetry.clone();
            self.last_applied_seq = synced.seq;
        }
        if let Some(sw) = cx.try_global::<UiSoftwareCursorGlobal>()
            && sw.seq > self.last_cursor_seq
        {
            self.draw_software_cursor = sw.enabled;
            self.last_cursor_seq = sw.seq;
        }
        if let Some(rm) = cx.try_global::<UiRenderModeGlobal>()
            && rm.seq > self.last_render_mode_seq
        {
            self.render_mode = rm.mode;
            self.last_render_mode_seq = rm.seq;
        }
        if let Some(cc) = cx.try_global::<UiClearCaptureGlobal>()
            && cc.seq > self.last_clear_capture_seq
        {
            self.store.active_keybind_capture = fpshud_input::KeybindTarget::None;
            self.last_clear_capture_seq = cc.seq;
        }
        if let Some(cd) = cx.try_global::<UiClearDropdownGlobal>()
            && cd.seq > self.last_clear_dropdown_seq
        {
            self.open_dropdown = None;
            self.last_clear_dropdown_seq = cd.seq;
        }
        if !self.store.config.gui_open || self.render_mode == UiRenderMode::HudOnly {
            self.open_dropdown = None;
            self.dragging_gui = false;
            self.dragging_preview = false;
            if self.render_mode == UiRenderMode::HudOnly {
                self.dashboard_bounds.set(gpui::Bounds::default());
            }
        }
        self.store.elapsed_sec = self.started_at.elapsed().as_secs_f32();
        let pending = self.store.take_outgoing();
        if !pending.is_empty() {
            push_outgoing_global(cx, pending);
        }

        let vp = window.viewport_size();
        let viewport_w = f32::from(vp.width).max(1.0);
        let viewport_h = f32::from(vp.height).max(1.0);

        let mut root = div()
            .w(px(viewport_w))
            .h(px(viewport_h))
            .relative()
            .bg(rgba(state::rgba_u32(0, 0, 0, 0)))
            .on_mouse_move(cx.listener(move |view, ev: &MouseMoveEvent, _, cx| {
                if view.dragging_gui {
                    view.update_gui_drag_from_point(
                        f32::from(ev.position.x),
                        f32::from(ev.position.y),
                        viewport_w,
                        viewport_h,
                        cx,
                    );
                }
                if view.dragging_preview {
                    view.update_preview_pos_from_point(
                        f32::from(ev.position.x),
                        f32::from(ev.position.y),
                        cx,
                    );
                }
            }))
            .on_mouse_up(
                MouseButton::Left,
                cx.listener(|view, _ev: &MouseUpEvent, _, _| {
                    view.dragging_gui = false;
                    view.dragging_preview = false;
                }),
            )
            .on_mouse_up_out(
                MouseButton::Left,
                cx.listener(|view, _ev: &MouseUpEvent, _, _| {
                    view.dragging_gui = false;
                    view.dragging_preview = false;
                }),
            );

        let show_hud = self.render_mode != UiRenderMode::DashboardOnly;
        let show_dashboard = self.render_mode != UiRenderMode::HudOnly;

        if show_hud && let Some(hud_el) = hud::render_hud(&self.store, viewport_w, viewport_h) {
            root = root.child(
                div()
                    .absolute()
                    .left(px(0.0))
                    .top(px(0.0))
                    .child(hud_el),
            );
        }

        if show_hud && let Some(toast_el) = toast::render_launch_toast(&self.store) {
            root = root.child(
                div()
                    .absolute()
                    .left(px(16.0))
                    .bottom(px(16.0))
                    .child(toast_el),
            );
        }

        if show_dashboard && let Some(dash_el) = self.render_dashboard(viewport_w, cx) {
            let (dash_x, dash_y) = self.resolved_dashboard_position(viewport_w, viewport_h);
            root = root.child(
                div()
                    .absolute()
                    .left(px(dash_x))
                    .top(px(dash_y))
                    .child(dash_el),
            );
        }

        if show_dashboard
            && self.store.config.gui_open
            && let Some(dropdown_overlay) =
                self.render_active_dropdown_overlay(viewport_w, viewport_h, cx)
        {
            root = root.child(gpui::deferred(dropdown_overlay).priority(10));
        }

        if self.render_mode == UiRenderMode::Full
            && self.store.config.gui_open
            && self.draw_software_cursor
        {
            let cur = window.mouse_position();
            let cursor_x = f32::from(cur.x).clamp(0.0, (viewport_w - 6.0).max(0.0));
            let cursor_y = f32::from(cur.y).clamp(0.0, (viewport_h - 6.0).max(0.0));
            root = root.child(
                div()
                    .absolute()
                    .left(px(cursor_x))
                    .top(px(cursor_y))
                    .w(px(14.0))
                    .h(px(14.0))
                    .rounded(px(2.0))
                    .bg(rgba(state::rgba_u32(12, 16, 24, 245)))
                    .border_2()
                    .border_color(rgba(state::rgba_u32(255, 255, 255, 255)))
                    .child(
                        div()
                            .w(px(6.0))
                            .h(px(6.0))
                            .bg(rgba(state::rgba_u32(95, 205, 255, 255))),
                    ),
            );
        }

        root
    }
}
