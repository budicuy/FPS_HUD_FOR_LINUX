use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, AtomicU32, Ordering},
};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};
use x11rb::connection::Connection;
use x11rb::protocol::Event;
use x11rb::protocol::xproto::{
    ConnectionExt, CreateGCAux, EventMask, GrabMode, GrabStatus, Rectangle,
};
use x11rb::rust_connection::RustConnection;

const GRAB_RETRY_INTERVAL: Duration = Duration::from_millis(50);

/// Decodes X11 `KeyButMask` pointer bits into `[Left, Right, Middle]`.
#[must_use]
pub const fn decode_x11_button_mask(mask_bits: u16) -> [bool; 3] {
    [
        (mask_bits & (1 << 8)) != 0,  // Button1 = Left
        (mask_bits & (1 << 10)) != 0, // Button3 = Right
        (mask_bits & (1 << 9)) != 0,  // Button2 = Middle
    ]
}

/// Maps X11 `ButtonPress`/`ButtonRelease` detail code (`1=Left, 3=Right, 2=Middle`) into button index.
#[must_use]
pub const fn x11_detail_to_button_idx(detail: u8) -> Option<usize> {
    match detail {
        1 => Some(0),
        3 => Some(1),
        2 => Some(2),
        _ => None,
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum X11Event {
    Key { code: u32, down: bool },
    CursorPos { x: f32, y: f32 },
    MouseButtons([bool; 3]),
}

/// Computes Linux keycode state transitions (`x11_keycode - 8`) between consecutive 32-byte keymaps.
#[must_use]
pub fn diff_x11_keymap(prev: &[u8; 32], next: &[u8; 32]) -> Vec<(u32, bool)> {
    let mut events = Vec::new();
    for byte_idx in 0..32 {
        let changed = prev[byte_idx] ^ next[byte_idx];
        if changed == 0 {
            continue;
        }
        for bit in 0..8 {
            if (changed & (1 << bit)) != 0 {
                let x11_kc = (byte_idx * 8 + bit) as u32;
                if let Some(linux_kc) = x11_kc.checked_sub(8)
                    && linux_kc > 0
                {
                    let down = (next[byte_idx] & (1 << bit)) != 0;
                    events.push((linux_kc, down));
                }
            }
        }
    }
    events
}

#[must_use]
pub fn should_retry_grab(elapsed: Duration) -> bool {
    elapsed >= GRAB_RETRY_INTERVAL
}

struct SharedState {
    desired_grab: AtomicBool,
    is_grabbed: AtomicBool,
    target_window: AtomicU32,
    stop: AtomicBool,
    events: Mutex<Vec<X11Event>>,
}

/// Pure-Rust `x11rb` wire backend keeping zero dependency on `libX11.so`.
pub struct X11Backend {
    connected: bool,
    shared: Arc<SharedState>,
    worker: Option<JoinHandle<()>>,
}

impl Default for X11Backend {
    fn default() -> Self {
        Self::connect()
    }
}

impl X11Backend {
    #[must_use]
    pub fn connect() -> Self {
        let display_env = std::env::var("DISPLAY").ok();
        Self::connect_with_display(display_env.as_deref())
    }

    #[must_use]
    pub fn connect_with_display(display: Option<&str>) -> Self {
        let shared = Arc::new(SharedState {
            desired_grab: AtomicBool::new(false),
            is_grabbed: AtomicBool::new(false),
            target_window: AtomicU32::new(0),
            stop: AtomicBool::new(false),
            events: Mutex::new(Vec::new()),
        });

        let Some(dpy_str) = display else {
            return Self {
                connected: false,
                shared,
                worker: None,
            };
        };
        if dpy_str.trim().is_empty() {
            return Self {
                connected: false,
                shared,
                worker: None,
            };
        }

        let Ok((conn, screen_num)) = RustConnection::connect(Some(dpy_str)) else {
            return Self {
                connected: false,
                shared,
                worker: None,
            };
        };

        let root_win = conn.setup().roots[screen_num].root;
        let shared_bg = Arc::clone(&shared);
        let worker = thread::Builder::new()
            .name("fpshud_x11".to_string())
            .spawn(move || run_x11_loop(conn, root_win, shared_bg))
            .ok();

        Self {
            connected: worker.is_some(),
            shared,
            worker,
        }
    }

    #[must_use]
    pub fn is_connected(&self) -> bool {
        self.connected
    }

    pub fn set_target_window(&self, window_id: u32) {
        self.shared.target_window.store(window_id, Ordering::Relaxed);
    }

    pub fn set_grab(&self, grab: bool) {
        self.shared.desired_grab.store(grab, Ordering::Relaxed);
    }

    #[must_use]
    pub fn is_grabbed(&self) -> bool {
        self.shared.is_grabbed.load(Ordering::Relaxed)
    }

    pub fn drain_events(&self) -> Vec<X11Event> {
        self.shared
            .events
            .lock()
            .map(|mut g| std::mem::take(&mut *g))
            .unwrap_or_default()
    }
}

impl Drop for X11Backend {
    fn drop(&mut self) {
        self.shared.stop.store(true, Ordering::Relaxed);
        if let Some(handle) = self.worker.take() {
            let _ = handle.join();
        }
    }
}

fn create_blank_cursor(conn: &RustConnection, root: u32) -> Option<u32> {
    let pixmap = conn.generate_id().ok()?;
    let gc = conn.generate_id().ok()?;
    let cursor = conn.generate_id().ok()?;

    conn.create_pixmap(1, pixmap, root, 1, 1).ok()?;
    let gc_aux = CreateGCAux::new().foreground(0).background(0);
    conn.create_gc(gc, pixmap, &gc_aux).ok()?;
    conn.poly_fill_rectangle(
        pixmap,
        gc,
        &[Rectangle {
            x: 0,
            y: 0,
            width: 1,
            height: 1,
        }],
    )
    .ok()?;
    conn.create_cursor(cursor, pixmap, pixmap, 0, 0, 0, 0, 0, 0, 0, 0)
        .ok()?;
    let _ = conn.free_pixmap(pixmap);
    let _ = conn.free_gc(gc);
    let _ = conn.flush();
    Some(cursor)
}

fn translate_root_to_target(
    conn: &RustConnection,
    root: u32,
    target_window: u32,
    root_x: i16,
    root_y: i16,
) -> (f32, f32) {
    let win = if target_window > 1 {
        target_window
    } else {
        conn.get_input_focus()
            .ok()
            .and_then(|c| c.reply().ok())
            .map_or(root, |f| if f.focus > 1 { f.focus } else { root })
    };
    if win != root
        && let Ok(cookie) = conn.translate_coordinates(root, win, root_x, root_y)
        && let Ok(t) = cookie.reply()
    {
        return (f32::from(t.dst_x), f32::from(t.dst_y));
    }
    (f32::from(root_x), f32::from(root_y))
}

fn try_grab_pointer_on(
    conn: &RustConnection,
    win: u32,
    mask: EventMask,
    blank_cursor: u32,
) -> bool {
    conn.grab_pointer(
        false,
        win,
        mask,
        GrabMode::ASYNC,
        GrabMode::ASYNC,
        x11rb::NONE,
        blank_cursor,
        x11rb::CURRENT_TIME,
    )
    .ok()
    .and_then(|c| c.reply().ok())
    .is_some_and(|r| r.status == GrabStatus::SUCCESS)
}

fn run_x11_loop(conn: RustConnection, root: u32, shared: Arc<SharedState>) {
    let blank_cursor = create_blank_cursor(&conn, root).unwrap_or(x11rb::NONE);
    let mut prev_keymap = [0_u8; 32];
    let mut prev_buttons = [false; 3];
    let mut grab_active = false;
    let mut kbd_grab_active = false;
    let mut last_grab_attempt = Instant::now() - GRAB_RETRY_INTERVAL;

    while !shared.stop.load(Ordering::Relaxed) {
        let want_grab = shared.desired_grab.load(Ordering::Relaxed);
        if want_grab != grab_active {
            if want_grab {
                if should_retry_grab(last_grab_attempt.elapsed()) {
                    last_grab_attempt = Instant::now();
                    let target = shared.target_window.load(Ordering::Relaxed);
                    let grab_win = if target > 1 { target } else { root };
                    let mask = EventMask::BUTTON_PRESS
                        | EventMask::BUTTON_RELEASE
                        | EventMask::POINTER_MOTION;
                    let mut p_ok = try_grab_pointer_on(&conn, grab_win, mask, blank_cursor);
                    let mut active_win = grab_win;
                    if !p_ok && grab_win != root {
                        p_ok = try_grab_pointer_on(&conn, root, mask, blank_cursor);
                        active_win = root;
                    }
                    let k_ok = if p_ok {
                        conn.grab_keyboard(
                            false,
                            active_win,
                            x11rb::CURRENT_TIME,
                            GrabMode::ASYNC,
                            GrabMode::ASYNC,
                        )
                        .ok()
                        .and_then(|c| c.reply().ok())
                        .is_some_and(|r| r.status == GrabStatus::SUCCESS)
                    } else {
                        false
                    };
                    grab_active = p_ok;
                    kbd_grab_active = k_ok;
                    if !grab_active {
                        let _ = conn.ungrab_pointer(x11rb::CURRENT_TIME);
                        let _ = conn.ungrab_keyboard(x11rb::CURRENT_TIME);
                    }
                    shared.is_grabbed.store(grab_active, Ordering::Relaxed);
                }
            } else {
                let _ = conn.ungrab_pointer(x11rb::CURRENT_TIME);
                if kbd_grab_active {
                    let _ = conn.ungrab_keyboard(x11rb::CURRENT_TIME);
                }
                let _ = conn.flush();
                grab_active = false;
                kbd_grab_active = false;
                shared.is_grabbed.store(false, Ordering::Relaxed);
            }
        }

        let mut batch = Vec::new();
        let target = shared.target_window.load(Ordering::Relaxed);

        // Drain queued X11 events so fast clicks and motion while grabbed are never dropped
        while let Ok(Some(ev)) = conn.poll_for_event() {
            match ev {
                Event::MotionNotify(m) => {
                    let (wx, wy) = translate_root_to_target(&conn, root, target, m.root_x, m.root_y);
                    batch.push(X11Event::CursorPos { x: wx, y: wy });
                }
                Event::ButtonPress(b) => {
                    if let Some(idx) = x11_detail_to_button_idx(b.detail)
                        && !prev_buttons[idx]
                    {
                        prev_buttons[idx] = true;
                        batch.push(X11Event::MouseButtons(prev_buttons));
                    }
                }
                Event::ButtonRelease(b) => {
                    if let Some(idx) = x11_detail_to_button_idx(b.detail)
                        && prev_buttons[idx]
                    {
                        prev_buttons[idx] = false;
                        batch.push(X11Event::MouseButtons(prev_buttons));
                    }
                }
                _ => {}
            }
        }

        if let Ok(cookie) = conn.query_keymap()
            && let Ok(reply) = cookie.reply()
        {
            for (code, down) in diff_x11_keymap(&prev_keymap, &reply.keys) {
                batch.push(X11Event::Key { code, down });
            }
            prev_keymap = reply.keys;
        }

        if let Ok(ptr_cookie) = conn.query_pointer(root)
            && let Ok(ptr) = ptr_cookie.reply()
        {
            let (wx, wy) = translate_root_to_target(&conn, root, target, ptr.root_x, ptr.root_y);
            batch.push(X11Event::CursorPos { x: wx, y: wy });

            let btns = decode_x11_button_mask(ptr.mask.bits());
            if btns != prev_buttons {
                prev_buttons = btns;
                batch.push(X11Event::MouseButtons(btns));
            }
        }

        if !batch.is_empty()
            && let Ok(mut queue) = shared.events.lock()
        {
            queue.extend(batch);
        }

        thread::sleep(Duration::from_millis(8));
    }

    if grab_active {
        let _ = conn.ungrab_pointer(x11rb::CURRENT_TIME);
    }
    if kbd_grab_active {
        let _ = conn.ungrab_keyboard(x11rb::CURRENT_TIME);
    }
    if blank_cursor != x11rb::NONE {
        let _ = conn.free_cursor(blank_cursor);
    }
    let _ = conn.flush();
}
