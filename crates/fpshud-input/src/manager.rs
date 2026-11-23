use crate::keycodes::{
    BTN_MISC, KEY_ESC, KEY_F9, KEY_F11, KEY_F12, active_modifier_mask, is_bench_toggle_key,
    is_gui_toggle_key, is_hud_toggle_key, is_modifier_key, matches_keybind, modifier_bit_for_key,
};
use crate::x11::{X11Backend, X11Event};
use fpshud_core::{HudConfig, Keybind};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum SurfaceBackendType {
    #[default]
    Unknown,
    Wayland,
    X11,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum KeybindTarget {
    None,
    ToggleGui,
    ToggleHud,
    ToggleBenchmark,
    TakeScreenshot,
    Record,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputAction {
    ToggleGui,
    ToggleHud,
    ToggleBenchmark,
    TakeScreenshot,
    ToggleRecord,
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub enum UiInputEvent {
    MouseMove { x: f32, y: f32 },
    MouseDown { button: u8 },
    MouseUp { button: u8 },
    ScrollWheel { delta_x: f32, delta_y: f32 },
    KeyDown { code: u32, mod_mask: u32 },
    KeyUp { code: u32, mod_mask: u32 },
}

/// Manages surface disambiguation, X11/Wayland input, 4 hotkeys, & interactive capture.
pub struct InputManager {
    x11: Option<X11Backend>,
    wayland_display_hint: bool,
    x11_swapchain_active: bool,
    wayland_swapchain_active: bool,
    x11_target_window: Option<u32>,
    keys_down: [bool; 512],
    mouse_buttons: [bool; 3],
    cursor_pos: Option<(f32, f32)>,
    last_emitted_cursor: Option<(f32, f32)>,
    pending_ungrab: bool,
    capture_target: KeybindTarget,
    capture_mod_mask: u32,
    capture_mod_evdev: u32,
    external_overlay_active: bool,
}

impl Default for InputManager {
    fn default() -> Self {
        Self::new()
    }
}

impl InputManager {
    #[must_use]
    pub fn new() -> Self {
        Self {
            x11: Some(X11Backend::connect()),
            wayland_display_hint: false,
            x11_swapchain_active: false,
            wayland_swapchain_active: false,
            x11_target_window: None,
            keys_down: [false; 512],
            mouse_buttons: [false; 3],
            cursor_pos: None,
            last_emitted_cursor: None,
            pending_ungrab: false,
            capture_target: KeybindTarget::None,
            capture_mod_mask: 0,
            capture_mod_evdev: 0,
            external_overlay_active: false,
        }
    }

    #[must_use]
    pub fn new_headless() -> Self {
        Self {
            x11: None,
            wayland_display_hint: false,
            x11_swapchain_active: false,
            wayland_swapchain_active: false,
            x11_target_window: None,
            keys_down: [false; 512],
            mouse_buttons: [false; 3],
            cursor_pos: None,
            last_emitted_cursor: None,
            pending_ungrab: false,
            capture_target: KeybindTarget::None,
            capture_mod_mask: 0,
            capture_mod_evdev: 0,
            external_overlay_active: false,
        }
    }

    pub fn attach_wayland_display_hint(&mut self, present: bool) {
        self.wayland_display_hint = present;
    }

    pub fn notify_x11_swapchain(&mut self, window_id: u32) {
        self.x11_swapchain_active = true;
        self.wayland_swapchain_active = false;
        self.x11_target_window = (window_id != 0).then_some(window_id);
        if let Some(x11) = &self.x11 {
            x11.set_target_window(window_id);
        }
    }

    pub fn notify_wayland_swapchain(&mut self) {
        self.wayland_swapchain_active = true;
        self.x11_swapchain_active = false;
    }

    pub fn clear_x11_target_window_if_matches(&mut self, window_id: u32) {
        if self.x11_target_window == Some(window_id) {
            self.x11_target_window = None;
            if let Some(x11) = &self.x11 {
                x11.set_target_window(0);
            }
        }
    }

    pub fn reset_surface_hints(&mut self) {
        self.wayland_display_hint = false;
        self.x11_swapchain_active = false;
        self.wayland_swapchain_active = false;
        self.x11_target_window = None;
    }

    #[must_use]
    pub fn active_backend(&self) -> SurfaceBackendType {
        if self.x11_swapchain_active {
            SurfaceBackendType::X11
        } else if self.wayland_swapchain_active || self.wayland_display_hint {
            SurfaceBackendType::Wayland
        } else if self.x11.as_ref().is_some_and(X11Backend::is_connected) {
            SurfaceBackendType::X11
        } else {
            SurfaceBackendType::Unknown
        }
    }

    #[must_use]
    pub fn x11_target_window(&self) -> Option<u32> {
        self.x11_target_window
    }

    #[must_use]
    pub fn should_use_x11_grab(&self, gui_open: bool) -> bool {
        if !gui_open || self.external_overlay_active {
            return false;
        }
        self.x11_swapchain_active || (!self.wayland_swapchain_active && !self.wayland_display_hint)
    }

    /// Marks whether `fpshud-ui` owns an isolated compositor overlay surface (e.g. `zwlr_layer_shell_v1`).
    /// While active, all in-process X11 grabs are released so compositor focus stays clean.
    pub fn set_external_overlay_active(&mut self, active: bool) {
        self.external_overlay_active = active;
        if active {
            self.pending_ungrab = false;
            self.last_emitted_cursor = None;
            self.mouse_buttons = [false; 3];
            if let Some(x11) = &self.x11 {
                x11.set_grab(false);
            }
        }
    }

    #[must_use]
    pub fn is_external_overlay_active(&self) -> bool {
        self.external_overlay_active
    }

    /// Marks already-held keys (such as `wl_keyboard::enter` keys) as pressed without firing hotkeys.
    pub fn seed_keys_down(&mut self, codes: &[u32]) {
        for &code in codes {
            let idx = code as usize;
            if idx < self.keys_down.len() {
                self.keys_down[idx] = true;
            }
        }
    }

    pub fn start_keybind_capture(&mut self, target: KeybindTarget) {
        self.capture_target = target;
        self.capture_mod_mask = 0;
        self.capture_mod_evdev = 0;
    }

    pub fn cancel_keybind_capture(&mut self) {
        self.capture_target = KeybindTarget::None;
        self.capture_mod_mask = 0;
        self.capture_mod_evdev = 0;
    }

    #[must_use]
    pub fn is_capturing_keybind(&self) -> bool {
        self.capture_target != KeybindTarget::None
    }

    pub fn set_cursor_pos(&mut self, x: f32, y: f32) {
        self.cursor_pos = Some((x, y));
    }

    #[must_use]
    pub fn is_pending_ungrab(&self) -> bool {
        self.pending_ungrab
    }

    pub fn handle_key_event(
        &mut self,
        code: u32,
        down: bool,
        cfg: &mut HudConfig,
    ) -> Vec<InputAction> {
        let idx = code as usize;
        if idx < self.keys_down.len() {
            if self.keys_down[idx] == down {
                return Vec::new();
            }
            self.keys_down[idx] = down;
        }

        if self.capture_target != KeybindTarget::None {
            if code >= BTN_MISC {
                return Vec::new();
            }
            if down && code == KEY_ESC {
                self.cancel_keybind_capture();
                return Vec::new();
            }

            if down {
                if is_modifier_key(code) {
                    self.capture_mod_mask |= modifier_bit_for_key(code);
                    if self.capture_mod_evdev == 0 {
                        self.capture_mod_evdev = code;
                    }
                } else {
                    self.commit_capture(Keybind::new(self.capture_mod_mask, code), cfg);
                }
            } else if is_modifier_key(code) && self.capture_mod_evdev == code {
                self.commit_capture(Keybind::new(0, code), cfg);
            }
            return Vec::new();
        }

        if !down {
            return Vec::new();
        }

        let mut actions = Vec::new();
        let has_mod = active_modifier_mask(&self.keys_down) != 0;

        if matches_keybind(code, cfg.bind_screenshot, &self.keys_down) {
            actions.push(InputAction::TakeScreenshot);
        } else if matches_keybind(code, cfg.bind_record, &self.keys_down) {
            actions.push(InputAction::ToggleRecord);
        } else if matches_keybind(code, cfg.bind_toggle_gui, &self.keys_down)
            || (!has_mod
                && cfg.bind_toggle_gui.mod_key == 0
                && cfg.bind_toggle_gui.main_key == KEY_F12
                && cfg.key_toggle_gui >= 0
                && is_gui_toggle_key(code, cfg.key_toggle_gui))
        {
            cfg.gui_open = !cfg.gui_open;
            actions.push(InputAction::ToggleGui);
        } else if matches_keybind(code, cfg.bind_toggle_hud, &self.keys_down)
            || (!has_mod
                && cfg.bind_toggle_hud.mod_key == 0
                && cfg.bind_toggle_hud.main_key == KEY_F11
                && cfg.key_toggle_hud >= 0
                && is_hud_toggle_key(code, cfg.key_toggle_hud))
        {
            cfg.hud_visible = !cfg.hud_visible;
            actions.push(InputAction::ToggleHud);
        } else if matches_keybind(code, cfg.bind_benchmark, &self.keys_down)
            || (!has_mod
                && cfg.bind_benchmark.mod_key == 0
                && cfg.bind_benchmark.main_key == KEY_F9
                && cfg.key_benchmark >= 0
                && is_bench_toggle_key(code, cfg.key_benchmark))
        {
            actions.push(InputAction::ToggleBenchmark);
        }

        actions
    }

    fn commit_capture(&mut self, kb: Keybind, cfg: &mut HudConfig) {
        match self.capture_target {
            KeybindTarget::ToggleGui => {
                cfg.bind_toggle_gui = kb;
                cfg.key_toggle_gui = -1;
            }
            KeybindTarget::ToggleHud => {
                cfg.bind_toggle_hud = kb;
                cfg.key_toggle_hud = -1;
            }
            KeybindTarget::ToggleBenchmark => {
                cfg.bind_benchmark = kb;
                cfg.key_benchmark = -1;
            }
            KeybindTarget::TakeScreenshot => {
                cfg.bind_screenshot = kb;
            }
            KeybindTarget::Record => {
                cfg.bind_record = kb;
            }
            KeybindTarget::None => {}
        }
        self.cancel_keybind_capture();
    }

    pub fn dispatch_x11_mouse_buttons(
        &mut self,
        buttons: [bool; 3],
        cfg: &HudConfig,
    ) -> Vec<UiInputEvent> {
        let mut events = Vec::new();
        for (idx, (&prev, &curr)) in self.mouse_buttons.iter().zip(buttons.iter()).enumerate() {
            if prev != curr && (cfg.gui_open || !curr) {
                events.push(if curr {
                    UiInputEvent::MouseDown { button: idx as u8 }
                } else {
                    UiInputEvent::MouseUp { button: idx as u8 }
                });
            }
        }
        self.mouse_buttons = buttons;
        events
    }

    /// Software cursor is drawn ONLY when X11 hid the OS cursor;
    /// on Wayland and standard X11 grabs, the native OS cursor stays visible without double cursor.
    #[must_use]
    pub fn should_draw_software_cursor(&self, cfg: &HudConfig) -> bool {
        if !cfg.gui_open || self.external_overlay_active {
            return false;
        }
        self.x11
            .as_ref()
            .is_some_and(X11Backend::is_pointer_cursor_hidden)
    }

    pub fn sync_grab_state(&mut self, cfg: &HudConfig) {
        if self.external_overlay_active {
            self.pending_ungrab = false;
            if let Some(x11) = &self.x11 {
                x11.set_grab(false);
            }
            return;
        }
        let any_mouse_down = self.mouse_buttons.iter().any(|&b| b);
        self.pending_ungrab = !cfg.gui_open && any_mouse_down;

        let want_x11_grab = (cfg.gui_open || self.pending_ungrab) && self.should_use_x11_grab(true);
        if let Some(x11) = &self.x11 {
            x11.set_grab(want_x11_grab);
        }
    }

    fn flush_cursor_move(&mut self, cfg: &HudConfig, max_w: f32, max_h: f32, ui_events: &mut Vec<UiInputEvent>) {
        if !cfg.gui_open {
            self.last_emitted_cursor = None;
            return;
        }
        let (cx, cy) = match self.cursor_pos {
            Some(pos) => pos,
            None => {
                let centered = (max_w * 0.5, max_h * 0.5);
                self.cursor_pos = Some(centered);
                centered
            }
        };
        let nx = cx.clamp(0.0, max_w);
        let ny = cy.clamp(0.0, max_h);
        let moved = self.last_emitted_cursor.is_none_or(|(px, py)| {
            (nx - px).abs() > 0.25 || (ny - py).abs() > 0.25
        });
        if moved {
            self.last_emitted_cursor = Some((nx, ny));
            ui_events.push(UiInputEvent::MouseMove { x: nx, y: ny });
        }
    }

    pub fn poll_frame(
        &mut self,
        cfg: &mut HudConfig,
        width: f32,
        height: f32,
    ) -> (Vec<InputAction>, Vec<UiInputEvent>, bool) {
        let mut actions = Vec::new();
        let mut ui_events = Vec::new();
        let max_w = width.max(1.0);
        let max_h = height.max(1.0);

        if self.external_overlay_active {
            // Drain backend queues while external Wayland overlay owns compositor focus so no stale
            // events replay into the game or HUD when the overlay closes.
            if let Some(x11) = &self.x11 {
                for ev in x11.drain_events() {
                    if let X11Event::Key { code, down: false } = ev
                        && (code as usize) < self.keys_down.len()
                    {
                        self.keys_down[code as usize] = false;
                    }
                }
            }
            self.mouse_buttons = [false; 3];
            self.last_emitted_cursor = None;
            self.sync_grab_state(cfg);
            return (actions, ui_events, false);
        }

        let x11_pointer_allowed = !self.wayland_swapchain_active || self.x11_swapchain_active;
        let x11_events = self.x11.as_ref().map(X11Backend::drain_events).unwrap_or_default();
        for ev in x11_events {
            match ev {
                X11Event::Key { code, down } => {
                    actions.extend(self.handle_key_event(code, down, cfg));
                    if cfg.gui_open {
                        let mod_mask = active_modifier_mask(&self.keys_down);
                        ui_events.push(if down {
                            UiInputEvent::KeyDown { code, mod_mask }
                        } else {
                            UiInputEvent::KeyUp { code, mod_mask }
                        });
                    }
                }
                X11Event::CursorPos { x, y } if x11_pointer_allowed => {
                    self.cursor_pos = Some((x, y));
                }
                X11Event::MouseButtons(btns) if x11_pointer_allowed => {
                    self.flush_cursor_move(cfg, max_w, max_h, &mut ui_events);
                    ui_events.extend(self.dispatch_x11_mouse_buttons(btns, cfg));
                }
                X11Event::CursorPos { .. } | X11Event::MouseButtons(_) => {}
            }
        }

        self.flush_cursor_move(cfg, max_w, max_h, &mut ui_events);

        self.sync_grab_state(cfg);
        let grabbed = self.x11.as_ref().is_some_and(X11Backend::is_grabbed);
        (actions, ui_events, grabbed)
    }
}
