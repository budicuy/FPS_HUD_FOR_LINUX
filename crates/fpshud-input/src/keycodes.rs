use fpshud_core::Keybind;
use fpshud_core::keybind::{MOD_ALT, MOD_CTRL, MOD_SHIFT, MOD_SUPER};

pub const KEY_ESC: u32 = 1;
pub const KEY_LEFTCTRL: u32 = 29;
pub const KEY_LEFTSHIFT: u32 = 42;
pub const KEY_RIGHTSHIFT: u32 = 54;
pub const KEY_LEFTALT: u32 = 56;
pub const KEY_F8: u32 = 66;
pub const KEY_F9: u32 = 67;
pub const KEY_F10: u32 = 68;
pub const KEY_F11: u32 = 87;
pub const KEY_F12: u32 = 88;
pub const KEY_RIGHTCTRL: u32 = 97;
pub const KEY_RIGHTALT: u32 = 100;
pub const KEY_HOME: u32 = 102;
pub const KEY_PAGEUP: u32 = 104;
pub const KEY_END: u32 = 107;
pub const KEY_INSERT: u32 = 110;
pub const KEY_LEFTMETA: u32 = 125;
pub const KEY_RIGHTMETA: u32 = 126;
pub const BTN_MISC: u32 = 0x100;

#[must_use]
pub const fn is_modifier_key(code: u32) -> bool {
    matches!(
        code,
        KEY_LEFTCTRL
            | KEY_RIGHTCTRL
            | KEY_LEFTALT
            | KEY_RIGHTALT
            | KEY_LEFTSHIFT
            | KEY_RIGHTSHIFT
            | KEY_LEFTMETA
            | KEY_RIGHTMETA
    )
}

#[must_use]
pub const fn modifier_bit_for_key(code: u32) -> u32 {
    match code {
        KEY_LEFTCTRL | KEY_RIGHTCTRL => MOD_CTRL,
        KEY_LEFTALT | KEY_RIGHTALT => MOD_ALT,
        KEY_LEFTSHIFT | KEY_RIGHTSHIFT => MOD_SHIFT,
        KEY_LEFTMETA | KEY_RIGHTMETA => MOD_SUPER,
        _ => 0,
    }
}

#[must_use]
pub fn active_modifier_mask(keys_down: &[bool; 512]) -> u32 {
    let mut mask = 0;
    if keys_down[KEY_LEFTCTRL as usize] || keys_down[KEY_RIGHTCTRL as usize] {
        mask |= MOD_CTRL;
    }
    if keys_down[KEY_LEFTALT as usize] || keys_down[KEY_RIGHTALT as usize] {
        mask |= MOD_ALT;
    }
    if keys_down[KEY_LEFTSHIFT as usize] || keys_down[KEY_RIGHTSHIFT as usize] {
        mask |= MOD_SHIFT;
    }
    if keys_down[KEY_LEFTMETA as usize] || keys_down[KEY_RIGHTMETA as usize] {
        mask |= MOD_SUPER;
    }
    mask
}

#[must_use]
pub fn is_gui_toggle_key(code: u32, mode: i32) -> bool {
    match mode {
        2 => code == KEY_F10,
        3 => code == KEY_HOME,
        _ => code == KEY_F12,
    }
}

#[must_use]
pub fn is_hud_toggle_key(code: u32, mode: i32) -> bool {
    match mode {
        1 => code == KEY_END,
        _ => code == KEY_F11,
    }
}

#[must_use]
pub fn is_bench_toggle_key(code: u32, mode: i32) -> bool {
    match mode {
        1 => code == KEY_PAGEUP,
        _ => code == KEY_F9,
    }
}

#[must_use]
pub fn matches_keybind(code: u32, kb: Keybind, keys_down: &[bool; 512]) -> bool {
    if kb.main_key == 0 || code != kb.main_key {
        return false;
    }
    let held_mask = active_modifier_mask(keys_down);
    if kb.mod_key == 0 {
        return held_mask == 0;
    }
    if kb.mod_key <= (MOD_CTRL | MOD_ALT | MOD_SHIFT | MOD_SUPER) {
        return (held_mask & kb.mod_key) == kb.mod_key;
    }
    let idx = kb.mod_key as usize;
    if idx < keys_down.len() && keys_down[idx] {
        return true;
    }
    let legacy_bit = modifier_bit_for_key(kb.mod_key);
    legacy_bit != 0 && (held_mask & legacy_bit) == legacy_bit
}
