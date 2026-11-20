//! Pure-Rust GPUI-CE HUD overlay, launch toast, and interactive 3-column dashboard.

pub mod dashboard;
pub mod hud;
pub mod state;
pub mod toast;

pub use dashboard::FpshudRootView;
pub use state::{SensorFilter, UiStore};

use fpshud_core::HudConfig;
use fpshud_ipc::UiToLayerMsg;
use fpshud_telemetry::TelemetrySnapshot;
use gpui::{
    App, Context, Global, IntoElement, ParentElement, Render, Styled, Window, div, px, rgba,
};

struct UiSyncGlobal {
    seq: u64,
    config: HudConfig,
    telemetry: TelemetrySnapshot,
}

impl Global for UiSyncGlobal {}

#[derive(Default)]
struct UiOutgoingQueueGlobal {
    messages: Vec<UiToLayerMsg>,
}

impl Global for UiOutgoingQueueGlobal {}

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
        let next_seq = cx.try_global::<UiSyncGlobal>().map_or(1, |g| g.seq + 1);
        cx.set_global(UiSyncGlobal {
            seq: next_seq,
            config,
            telemetry,
        });
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
            self.store.config = synced.config.clone();
            self.store.telemetry = synced.telemetry.clone();
            self.last_applied_seq = synced.seq;
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
            .bg(rgba(state::rgba_u32(0, 0, 0, 0)));

        if let Some(hud_el) = hud::render_hud(&self.store, viewport_w, viewport_h) {
            root = root.child(
                div()
                    .absolute()
                    .left(px(0.0))
                    .top(px(0.0))
                    .child(hud_el),
            );
        }

        if let Some(toast_el) = toast::render_launch_toast(&self.store) {
            root = root.child(
                div()
                    .absolute()
                    .left(px(16.0))
                    .bottom(px(16.0))
                    .child(toast_el),
            );
        }

        if let Some(dash_el) = self.render_dashboard(cx) {
            root = root.child(
                div()
                    .w(px(viewport_w))
                    .h(px(viewport_h))
                    .absolute()
                    .left(px(0.0))
                    .top(px(0.0))
                    .flex()
                    .items_center()
                    .justify_center()
                    .child(dash_el),
            );
        }

        root
    }
}
