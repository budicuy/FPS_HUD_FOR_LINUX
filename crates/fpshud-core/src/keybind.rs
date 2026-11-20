use crate::config::Keybind;

pub const MOD_CTRL: u32 = 1 << 0;
pub const MOD_ALT: u32 = 1 << 1;
pub const MOD_SHIFT: u32 = 1 << 2;
pub const MOD_SUPER: u32 = 1 << 3;

/// Maps Linux evdev keycodes (`linux/input-event-codes.h`) to concise UI key names.
#[must_use]
pub const fn linux_keycode_name(code: u32) -> &'static str {
    match code {
        0 => "None",
        1 => "ESC",
        2 => "1",
        3 => "2",
        4 => "3",
        5 => "4",
        6 => "5",
        7 => "6",
        8 => "7",
        9 => "8",
        10 => "9",
        11 => "0",
        12 => "-",
        13 => "=",
        14 => "Backspace",
        15 => "Tab",
        16 => "Q",
        17 => "W",
        18 => "E",
        19 => "R",
        20 => "T",
        21 => "Y",
        22 => "U",
        23 => "I",
        24 => "O",
        25 => "P",
        26 => "[",
        27 => "]",
        28 => "Enter",
        29 | 97 => "Ctrl",
        30 => "A",
        31 => "S",
        32 => "D",
        33 => "F",
        34 => "G",
        35 => "H",
        36 => "J",
        37 => "K",
        38 => "L",
        39 => ";",
        40 => "'",
        41 => "`",
        42 | 54 => "Shift",
        43 => "\\",
        44 => "Z",
        45 => "X",
        46 => "C",
        47 => "V",
        48 => "B",
        49 => "N",
        50 => "M",
        51 => ",",
        52 => ".",
        53 => "/",
        55 => "KP*",
        56 | 100 => "Alt",
        57 => "Space",
        58 => "CapsLock",
        59 => "F1",
        60 => "F2",
        61 => "F3",
        62 => "F4",
        63 => "F5",
        64 => "F6",
        65 => "F7",
        66 => "F8",
        67 => "F9",
        68 => "F10",
        69 => "NumLock",
        70 => "ScrollLock",
        87 => "F11",
        88 => "F12",
        99 => "PrintScr",
        102 => "Home",
        103 => "Up",
        104 => "PageUp",
        105 => "Left",
        106 => "Right",
        107 => "End",
        108 => "Down",
        109 => "PageDown",
        110 => "Insert",
        111 => "Delete",
        119 => "Pause",
        125 | 126 => "Super",
        _ => "Key",
    }
}

fn format_single_keycode(code: u32) -> String {
    let name = linux_keycode_name(code);
    if name == "Key" {
        format!("Key#{code}")
    } else {
        name.to_string()
    }
}

/// Formats a `Keybind` into human-readable shortcut notation (e.g. `"Ctrl + F10"`, `"F12"`, `"None"`).
#[must_use]
pub fn format_keybind(kb: Keybind) -> String {
    if kb.main_key == 0 {
        return "None".to_string();
    }

    if kb.mod_key == 0 {
        return format_single_keycode(kb.main_key);
    }

    // Support both bitmask modifiers (1..=15) and legacy evdev modifier keycodes
    if kb.mod_key <= (MOD_CTRL | MOD_ALT | MOD_SHIFT | MOD_SUPER) {
        let mut parts: Vec<&'static str> = Vec::with_capacity(4);
        if (kb.mod_key & MOD_CTRL) != 0 {
            parts.push("Ctrl");
        }
        if (kb.mod_key & MOD_ALT) != 0 {
            parts.push("Alt");
        }
        if (kb.mod_key & MOD_SHIFT) != 0 {
            parts.push("Shift");
        }
        if (kb.mod_key & MOD_SUPER) != 0 {
            parts.push("Super");
        }
        if parts.is_empty() {
            return format_single_keycode(kb.main_key);
        }
        return format!("{} + {}", parts.join(" + "), format_single_keycode(kb.main_key));
    }

    if kb.mod_key != kb.main_key {
        format!(
            "{} + {}",
            format_single_keycode(kb.mod_key),
            format_single_keycode(kb.main_key)
        )
    } else {
        format_single_keycode(kb.main_key)
    }
}
