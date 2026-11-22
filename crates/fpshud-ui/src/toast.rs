use crate::state::{UiStore, rgba_u32};
use fpshud_core::HudConfig;
use fpshud_core::keybind::format_keybind;
use gpui::{AnyElement, IntoElement, ParentElement, Styled, div, px, rgba};

pub const TOAST_DURATION_SEC: f32 = 5.5;

pub struct ToastState;

impl ToastState {
    /// Returns `true` during the initial `0.0s .. 5.5s` launch hint window.
    #[must_use]
    pub fn should_show(elapsed_sec: f32) -> bool {
        elapsed_sec.is_finite() && (0.0..TOAST_DURATION_SEC).contains(&elapsed_sec)
    }

    /// Formats the launch toast hint banner text for the active keybinds.
    #[must_use]
    pub fn format_hint_text(cfg: &HudConfig) -> String {
        let gui_key = format_keybind(cfg.bind_toggle_gui);
        let hud_key = format_keybind(cfg.bind_toggle_hud);
        format!("FPSHUD active | Dashboard: {gui_key} | HUD: {hud_key}")
    }
}

#[must_use]
pub fn render_launch_toast(store: &UiStore) -> Option<AnyElement> {
    if store.config.gui_open || !ToastState::should_show(store.elapsed_sec) {
        return None;
    }
    let gui_key = format_keybind(store.config.bind_toggle_gui);
    let hud_key = format_keybind(store.config.bind_toggle_hud);
    let text = format!("FPSHUD active | Dashboard: {gui_key} | HUD: {hud_key}");

    Some(
        div()
            .flex()
            .flex_row()
            .items_center()
            .px(px(12.0))
            .py(px(6.0))
            .rounded(px(6.0))
            .bg(rgba(rgba_u32(16, 22, 30, 215)))
            .border_1()
            .border_color(rgba(rgba_u32(82, 168, 255, 160)))
            .text_size(px(13.0))
            .text_color(rgba(rgba_u32(232, 240, 250, 245)))
            .child(text)
            .into_any_element(),
    )
}
