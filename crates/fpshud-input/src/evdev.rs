use std::fs::{self, File, OpenOptions};
use std::io::Read;
use std::os::fd::AsRawFd;
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;

const EVIOCGRAB: libc::Ioctl = 0x4004_4590_u32 as libc::Ioctl;
const EV_KEY: u16 = 0x01;

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
pub enum EvdevEvent {
    Key { code: u32, down: bool },
}

/// Optional sudo-free `/dev/input/event*` reader (active only if user holds `input` group access).
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
                files.push(file);
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
                            if ev.type_ == EV_KEY && (ev.value == 0 || ev.value == 1) {
                                out.push(EvdevEvent::Key {
                                    code: u32::from(ev.code),
                                    down: ev.value == 1,
                                });
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
