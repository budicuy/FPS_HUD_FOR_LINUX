use fpshud_core::HudConfig;
use fpshud_core::keybind::format_keybind;

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
