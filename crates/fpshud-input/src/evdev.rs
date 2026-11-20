use evdev::{
    AttributeSetRef, Device, EventSummary, EventType, InputEvent, KeyCode, RelativeAxisCode,
};
use std::fs;
use std::io::ErrorKind;
use std::path::Path;

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

/// Classifies an evdev node by capability sets so gamepads and power buttons are never grabbed.
#[must_use]
pub fn classify_device_caps(
    events: &AttributeSetRef<EventType>,
    keys: Option<&AttributeSetRef<KeyCode>>,
    rels: Option<&AttributeSetRef<RelativeAxisCode>>,
) -> EvdevDeviceRole {
    let has_key = events.contains(EventType::KEY);
    let has_rel = events.contains(EventType::RELATIVE);
    let has_abs = events.contains(EventType::ABSOLUTE);

    if has_rel
        && rels.is_some_and(|r| {
            r.contains(RelativeAxisCode::REL_X) && r.contains(RelativeAxisCode::REL_Y)
        })
    {
        return EvdevDeviceRole::Mouse;
    }

    // Standard keyboards expose EV_KEY with alphabetic/ESC keys (codes 1..=60) and lack EV_REL/EV_ABS.
    if has_key
        && !has_rel
        && !has_abs
        && keys.is_some_and(|k| k.iter().any(|code| (1..=60).contains(&code.0)))
    {
        return EvdevDeviceRole::Keyboard;
    }

    EvdevDeviceRole::Ignore
}

#[must_use]
pub fn translate_input_event(ev: InputEvent) -> Option<EvdevEvent> {
    match ev.destructure() {
        EventSummary::Key(_, key, value) if value == 0 || value == 1 => {
            let down = value == 1;
            match key {
                KeyCode::BTN_LEFT => Some(EvdevEvent::MouseBtn { button: 0, down }),
                KeyCode::BTN_RIGHT => Some(EvdevEvent::MouseBtn { button: 1, down }),
                KeyCode::BTN_MIDDLE => Some(EvdevEvent::MouseBtn { button: 2, down }),
                k if (0x100..=0x1FF).contains(&k.0) => None, // Non-mouse gamepad/stylus buttons
                k => Some(EvdevEvent::Key {
                    code: u32::from(k.0),
                    down,
                }),
            }
        }
        EventSummary::RelativeAxis(_, axis, value) if value != 0 => match axis {
            RelativeAxisCode::REL_X => Some(EvdevEvent::RelMotion { dx: value, dy: 0 }),
            RelativeAxisCode::REL_Y => Some(EvdevEvent::RelMotion { dx: 0, dy: value }),
            RelativeAxisCode::REL_WHEEL => Some(EvdevEvent::Scroll { delta: value }),
            _ => None,
        },
        _ => None,
    }
}

/// Optional sudo-free `/dev/input/event*` reader built on pure-Rust `evdev` (keyboards and mice only).
pub const MAX_FETCH_BATCHES_PER_POLL: usize = 16;

/// Abstraction over a single non-blocking batch read so `poll_events` avoids extra allocation and stays testable without `/dev/input`.
pub trait EventBatchSource {
    fn drain_one_batch(&mut self, out: &mut Vec<EvdevEvent>) -> std::io::Result<usize>;
}

impl EventBatchSource for Device {
    fn drain_one_batch(&mut self, out: &mut Vec<EvdevEvent>) -> std::io::Result<usize> {
        let mut raw_count = 0usize;
        for ev in self.fetch_events()? {
            raw_count += 1;
            if let Some(mapped) = translate_input_event(ev) {
                out.push(mapped);
            }
        }
        Ok(raw_count)
    }
}

impl<F, I> EventBatchSource for F
where
    F: FnMut() -> std::io::Result<I>,
    I: Iterator<Item = InputEvent>,
{
    fn drain_one_batch(&mut self, out: &mut Vec<EvdevEvent>) -> std::io::Result<usize> {
        let mut raw_count = 0usize;
        for ev in (self)()? {
            raw_count += 1;
            if let Some(mapped) = translate_input_event(ev) {
                out.push(mapped);
            }
        }
        Ok(raw_count)
    }
}

/// Drains up to [`MAX_FETCH_BATCHES_PER_POLL`] non-blocking event batches until `WouldBlock`.
pub fn drain_event_batches(source: &mut impl EventBatchSource) -> Vec<EvdevEvent> {
    let mut out = Vec::new();
    for _ in 0..MAX_FETCH_BATCHES_PER_POLL {
        match source.drain_one_batch(&mut out) {
            Ok(0) => break,
            Ok(_) => {}
            Err(err) if err.kind() == ErrorKind::WouldBlock => break,
            Err(_) => break,
        }
    }
    out
}

pub struct EvdevBackend {
    devices: Vec<Device>,
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
                devices: Vec::new(),
                grabbed: false,
            };
        };

        let mut devices = Vec::new();
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            if !name.starts_with("event") {
                continue;
            }
            if let Ok(device) = Device::open(entry.path())
                && device.set_nonblocking(true).is_ok()
                && classify_device_caps(
                    device.supported_events(),
                    device.supported_keys(),
                    device.supported_relative_axes(),
                ) != EvdevDeviceRole::Ignore
            {
                devices.push(device);
            }
        }

        Self {
            devices,
            grabbed: false,
        }
    }

    #[must_use]
    pub fn device_count(&self) -> usize {
        self.devices.len()
    }

    #[must_use]
    pub fn is_grabbed(&self) -> bool {
        self.grabbed
    }

    pub fn set_grab(&mut self, grab: bool) {
        if self.devices.is_empty() || self.grabbed == grab {
            return;
        }
        for device in &mut self.devices {
            let _ = if grab { device.grab() } else { device.ungrab() };
        }
        self.grabbed = grab;
    }

    pub fn poll_events(&mut self) -> Vec<EvdevEvent> {
        let mut out = Vec::new();
        for device in &mut self.devices {
            out.extend(drain_event_batches(device));
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
