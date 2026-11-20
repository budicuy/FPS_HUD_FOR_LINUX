use std::fs::{self, File, OpenOptions};
use std::io::Read;
use std::os::fd::AsRawFd;
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;

const EVIOCGRAB: libc::Ioctl = 0x4004_4590_u32 as libc::Ioctl;
const EVIOCGBIT_EV: libc::Ioctl = 0x8008_4520_u32 as libc::Ioctl;
const EVIOCGBIT_KEY: libc::Ioctl = 0x8008_4521_u32 as libc::Ioctl;
const EVIOCGBIT_REL: libc::Ioctl = 0x8008_4522_u32 as libc::Ioctl;

const EV_KEY: u16 = 0x01;
const EV_REL: u16 = 0x02;
const EV_ABS: u16 = 0x03;

const REL_X: u16 = 0x00;
const REL_Y: u16 = 0x01;
const REL_WHEEL: u16 = 0x08;

const BTN_LEFT: u16 = 0x110;
const BTN_RIGHT: u16 = 0x111;
const BTN_MIDDLE: u16 = 0x112;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct LinuxInputEvent {
    tv_sec: libc::time_t,
    tv_usec: libc::suseconds_t,
    type_: u16,
    code: u16,
    value: i32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EvdevDeviceRole {
    Ignore,
    Keyboard,
    Mouse,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EvdevEvent {
    Key { code: u32, down: bool },
    RelMotion { dx: i32, dy: i32 },
    Scroll { delta: i32 },
    MouseBtn { button: u8, down: bool },
}

/// Classifies an evdev node by capability bitmasks so gamepads/power buttons are never grabbed.
#[must_use]
pub const fn classify_evdev_caps(ev_bits: u64, key_bits: u64, rel_bits: u64) -> EvdevDeviceRole {
    let has_key = (ev_bits & (1_u64 << EV_KEY)) != 0;
    let has_rel = (ev_bits & (1_u64 << EV_REL)) != 0;
    let has_abs = (ev_bits & (1_u64 << EV_ABS)) != 0;

    if has_rel && (rel_bits & 0b11) == 0b11 {
        return EvdevDeviceRole::Mouse;
    }
    // Standard keyboards expose EV_KEY with alphabetic/ESC keys (bits 1..60) and lack EV_REL/EV_ABS
    if has_key && !has_rel && !has_abs && (key_bits & 0x0FFF_FFFF_FFFF_FFFE) != 0 {
        return EvdevDeviceRole::Keyboard;
    }
    EvdevDeviceRole::Ignore
}

#[must_use]
pub fn translate_raw_evdev(type_: u16, code: u16, value: i32) -> Option<EvdevEvent> {
    match type_ {
        EV_KEY if value == 0 || value == 1 => {
            let down = value == 1;
            match code {
                BTN_LEFT => Some(EvdevEvent::MouseBtn { button: 0, down }),
                BTN_RIGHT => Some(EvdevEvent::MouseBtn { button: 1, down }),
                BTN_MIDDLE => Some(EvdevEvent::MouseBtn { button: 2, down }),
                0x100..=0x1FF => None, // Non-mouse gamepad/stylus buttons
                _ => Some(EvdevEvent::Key {
                    code: u32::from(code),
                    down,
                }),
            }
        }
        EV_REL => match code {
            REL_X if value != 0 => Some(EvdevEvent::RelMotion { dx: value, dy: 0 }),
            REL_Y if value != 0 => Some(EvdevEvent::RelMotion { dx: 0, dy: value }),
            REL_WHEEL if value != 0 => Some(EvdevEvent::Scroll { delta: value }),
            _ => None,
        },
        _ => None,
    }
}

fn read_cap_u64(file: &File, req: libc::Ioctl) -> u64 {
    let mut bits = [0_u8; 8];
    let rc = unsafe { libc::ioctl(file.as_raw_fd(), req, bits.as_mut_ptr()) };
    if rc >= 0 {
        u64::from_le_bytes(bits)
    } else {
        0
    }
}

/// Optional sudo-free `/dev/input/event*` reader (filtered strictly to keyboards and mice).
pub struct EvdevBackend {
    files: Vec<File>,
    grabbed: bool,
}

impl Default for EvdevBackend {
    fn default() -> Self {
        Self::probe_directory(Path::new("/dev/input"))
    }
}

impl EvdevBackend {
    #[must_use]
    pub fn probe_directory(dir: &Path) -> Self {
        let Ok(entries) = fs::read_dir(dir) else {
            return Self {
                files: Vec::new(),
                grabbed: false,
            };
        };
        let mut files = Vec::new();
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            if !name.starts_with("event") {
                continue;
            }
            if let Ok(file) = OpenOptions::new()
                .read(true)
                .custom_flags(libc::O_NONBLOCK | libc::O_CLOEXEC)
                .open(entry.path())
            {
                let ev_bits = read_cap_u64(&file, EVIOCGBIT_EV);
                let key_bits = read_cap_u64(&file, EVIOCGBIT_KEY);
                let rel_bits = read_cap_u64(&file, EVIOCGBIT_REL);
                if classify_evdev_caps(ev_bits, key_bits, rel_bits) != EvdevDeviceRole::Ignore {
                    files.push(file);
                }
            }
        }
        Self {
            files,
            grabbed: false,
        }
    }

    #[must_use]
    pub fn device_count(&self) -> usize {
        self.files.len()
    }

    pub fn set_grab(&mut self, grab: bool) {
        if self.files.is_empty() || self.grabbed == grab {
            return;
        }
        let arg: libc::c_int = i32::from(grab);
        for file in &self.files {
            unsafe {
                let _ = libc::ioctl(file.as_raw_fd(), EVIOCGRAB, arg);
            }
        }
        self.grabbed = grab;
    }

    pub fn poll_events(&mut self) -> Vec<EvdevEvent> {
        let mut out = Vec::new();
        let ev_size = size_of::<LinuxInputEvent>();
        let mut raw = [0_u8; 24 * 16];

        for file in &mut self.files {
            loop {
                match file.read(&mut raw) {
                    Ok(n) if n >= ev_size => {
                        for chunk in raw[..n].chunks_exact(ev_size) {
                            let ev = unsafe {
                                std::ptr::read_unaligned(chunk.as_ptr().cast::<LinuxInputEvent>())
                            };
                            if let Some(mapped) = translate_raw_evdev(ev.type_, ev.code, ev.value) {
                                out.push(mapped);
                            }
                        }
                    }
                    _ => break,
                }
            }
        }
        out
    }
}

impl Drop for EvdevBackend {
    fn drop(&mut self) {
        if self.grabbed {
            self.set_grab(false);
        }
    }
}
