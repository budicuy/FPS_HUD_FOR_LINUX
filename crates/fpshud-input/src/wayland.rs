//! Pure-Rust dynamic Wayland client input listener (`wl_seat` -> `wl_pointer` & `wl_keyboard`),
//! zero `DT_NEEDED`, zero `#[repr(C)]`, zero `libc`.

use libloading::Library;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::os::fd::{FromRawFd, OwnedFd};
use std::sync::{Mutex, OnceLock};
use wayland_sys::client::{WaylandClient, wayland_client_option, wl_display, wl_event_queue, wl_proxy};
use wayland_sys::common::{wl_argument, wl_interface};

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum WaylandEvent {
    Key { code: u32, down: bool },
    CursorPos { x: f32, y: f32 },
    MouseBtn { button: u8, down: bool },
    Scroll { delta_x: f32, delta_y: f32 },
}

/// Translates Linux evdev button codes delivered over `wl_pointer::button` into HUD button indices.
#[must_use]
pub const fn map_wl_button(button: u32) -> Option<u8> {
    match button {
        0x110 => Some(0), // BTN_LEFT
        0x111 => Some(1), // BTN_RIGHT
        0x112 => Some(2), // BTN_MIDDLE
        _ => None,
    }
}

struct WaylandIfaces {
    _lib: Library,
    registry: *const wl_interface,
    seat: *const wl_interface,
    keyboard: *const wl_interface,
    pointer: *const wl_interface,
}

unsafe impl Send for WaylandIfaces {}
unsafe impl Sync for WaylandIfaces {}

struct WaylandRuntime {
    client: &'static WaylandClient,
    ifaces: WaylandIfaces,
}

fn load_wayland_runtime() -> Option<&'static WaylandRuntime> {
    static RT: OnceLock<Option<WaylandRuntime>> = OnceLock::new();
    RT.get_or_init(|| {
        let client = wayland_client_option()?;
        let lib = unsafe {
            Library::new("libwayland-client.so.0")
                .or_else(|_| Library::new("libwayland-client.so"))
                .ok()?
        };
        let get_iface = |sym: &[u8]| -> Option<*const wl_interface> {
            // `libloading::Library::get<T>` checks `size_of::<T>() == size_of::<*mut c_void>()`;
            // reading `*const wl_interface` retrieves the `dlsym` address of the static struct directly.
            let s = unsafe { lib.get::<*const wl_interface>(sym).ok()? };
            let ptr = *s;
            (!ptr.is_null()).then_some(ptr)
        };
        let registry = get_iface(b"wl_registry_interface\0")?;
        let seat = get_iface(b"wl_seat_interface\0")?;
        let keyboard = get_iface(b"wl_keyboard_interface\0")?;
        let pointer = get_iface(b"wl_pointer_interface\0")?;
        Some(WaylandRuntime {
            client,
            ifaces: WaylandIfaces {
                _lib: lib,
                registry,
                seat,
                keyboard,
                pointer,
            },
        })
    })
    .as_ref()
}

/// Returns `true` when `libwayland-client` is loadable in the current process environment.
#[must_use]
pub fn wayland_client_library_present() -> bool {
    unsafe {
        Library::new("libwayland-client.so.0")
            .or_else(|_| Library::new("libwayland-client.so"))
            .is_ok()
    }
}

/// Verifies that dynamic Wayland client symbols and `wl_interface` descriptors resolve cleanly.
#[must_use]
pub fn probe_wayland_interfaces() -> bool {
    load_wayland_runtime().is_some()
}

struct SharedWaylandState {
    events: Mutex<Vec<WaylandEvent>>,
    queue: *mut wl_event_queue,
    registry: *mut wl_proxy,
    seat: *mut wl_proxy,
    keyboard: *mut wl_proxy,
    pointer: *mut wl_proxy,
}

unsafe impl Send for SharedWaylandState {}
unsafe impl Sync for SharedWaylandState {}

impl SharedWaylandState {
    fn new() -> Box<Self> {
        Box::new(Self {
            events: Mutex::new(Vec::new()),
            queue: std::ptr::null_mut(),
            registry: std::ptr::null_mut(),
            seat: std::ptr::null_mut(),
            keyboard: std::ptr::null_mut(),
            pointer: std::ptr::null_mut(),
        })
    }

    fn push_event(&self, ev: WaylandEvent) {
        if let Ok(mut q) = self.events.lock() {
            if q.len() >= 1024 {
                q.drain(..512);
            }
            q.push(ev);
        }
    }

    unsafe fn teardown_proxies(&mut self, client: &WaylandClient) {
        if !self.pointer.is_null() {
            unsafe { (client.wl_proxy_destroy)(self.pointer) };
            self.pointer = std::ptr::null_mut();
        }
        if !self.keyboard.is_null() {
            unsafe { (client.wl_proxy_destroy)(self.keyboard) };
            self.keyboard = std::ptr::null_mut();
        }
        if !self.seat.is_null() {
            unsafe { (client.wl_proxy_destroy)(self.seat) };
            self.seat = std::ptr::null_mut();
        }
        if !self.registry.is_null() {
            unsafe { (client.wl_proxy_destroy)(self.registry) };
            self.registry = std::ptr::null_mut();
        }
        if !self.queue.is_null() {
            unsafe { (client.wl_event_queue_destroy)(self.queue) };
            self.queue = std::ptr::null_mut();
        }
    }
}

unsafe extern "C" fn wl_reg_global(
    data: *mut c_void,
    registry: *mut wl_proxy,
    name: u32,
    interface: *const c_char,
    version: u32,
) {
    if data.is_null() || registry.is_null() || interface.is_null() {
        return;
    }
    let state = unsafe { &mut *(data.cast::<SharedWaylandState>()) };
    if !state.seat.is_null() {
        return;
    }
    let Ok(iface_name) = unsafe { CStr::from_ptr(interface) }.to_str() else {
        return;
    };
    if iface_name != "wl_seat" {
        return;
    }
    let Some(rt) = load_wayland_runtime() else {
        return;
    };
    let bind_ver = version.min(7);
    let seat_iface = rt.ifaces.seat;
    let mut args = [
        wl_argument { u: name },
        wl_argument {
            s: unsafe { (*seat_iface).name },
        },
        wl_argument { u: bind_ver },
        wl_argument { n: 0 },
    ];
    let seat = unsafe {
        (rt.client.wl_proxy_marshal_array_constructor_versioned)(
            registry,
            0,
            args.as_mut_ptr(),
            seat_iface,
            bind_ver,
        )
    };
    if seat.is_null() {
        return;
    }
    state.seat = seat;
    unsafe {
        (rt.client.wl_proxy_set_queue)(seat, state.queue);
        let _ = (rt.client.wl_proxy_add_listener)(seat, SEAT_LISTENER.as_ptr().cast_mut(), data);
    }
}

unsafe extern "C" fn wl_reg_global_remove(_data: *mut c_void, _registry: *mut wl_proxy, _name: u32) {}

unsafe extern "C" fn wl_seat_caps(data: *mut c_void, seat: *mut wl_proxy, caps: u32) {
    if data.is_null() || seat.is_null() {
        return;
    }
    let state = unsafe { &mut *(data.cast::<SharedWaylandState>()) };
    let Some(rt) = load_wayland_runtime() else {
        return;
    };

    if (caps & 1) != 0 && state.keyboard.is_null() {
        let mut args = [wl_argument { n: 0 }];
        let kbd = unsafe {
            (rt.client.wl_proxy_marshal_array_constructor)(
                seat,
                1, // wl_seat::get_keyboard
                args.as_mut_ptr(),
                rt.ifaces.keyboard,
            )
        };
        if !kbd.is_null() {
            state.keyboard = kbd;
            unsafe {
                (rt.client.wl_proxy_set_queue)(kbd, state.queue);
                let _ = (rt.client.wl_proxy_add_listener)(
                    kbd,
                    KEYBOARD_LISTENER.as_ptr().cast_mut(),
                    data,
                );
            }
        }
    }

    if (caps & 2) != 0 && state.pointer.is_null() {
        let mut args = [wl_argument { n: 0 }];
        let ptr = unsafe {
            (rt.client.wl_proxy_marshal_array_constructor)(
                seat,
                0, // wl_seat::get_pointer
                args.as_mut_ptr(),
                rt.ifaces.pointer,
            )
        };
        if !ptr.is_null() {
            state.pointer = ptr;
            unsafe {
                (rt.client.wl_proxy_set_queue)(ptr, state.queue);
                let _ = (rt.client.wl_proxy_add_listener)(
                    ptr,
                    POINTER_LISTENER.as_ptr().cast_mut(),
                    data,
                );
            }
        }
    }
}

unsafe extern "C" fn wl_seat_name(_data: *mut c_void, _seat: *mut wl_proxy, _name: *const c_char) {}

unsafe extern "C" fn wl_kbd_keymap(
    _data: *mut c_void,
    _kbd: *mut wl_proxy,
    _format: u32,
    fd: c_int,
    _size: u32,
) {
    if fd >= 0 {
        // RAII close without needing `libc::close`
        let _closed = unsafe { OwnedFd::from_raw_fd(fd) };
    }
}

unsafe extern "C" fn wl_kbd_enter(
    _data: *mut c_void,
    _kbd: *mut wl_proxy,
    _serial: u32,
    _surface: *mut c_void,
    _keys: *mut c_void,
) {
}

unsafe extern "C" fn wl_kbd_leave(
    _data: *mut c_void,
    _kbd: *mut wl_proxy,
    _serial: u32,
    _surface: *mut c_void,
) {
}

unsafe extern "C" fn wl_kbd_key(
    data: *mut c_void,
    _kbd: *mut wl_proxy,
    _serial: u32,
    _time: u32,
    key: u32,
    state_val: u32,
) {
    if data.is_null() {
        return;
    }
    let state = unsafe { &*(data.cast::<SharedWaylandState>()) };
    state.push_event(WaylandEvent::Key {
        code: key,
        down: state_val == 1,
    });
}

unsafe extern "C" fn wl_kbd_modifiers(
    _data: *mut c_void,
    _kbd: *mut wl_proxy,
    _serial: u32,
    _depressed: u32,
    _latched: u32,
    _locked: u32,
    _group: u32,
) {
}

unsafe extern "C" fn wl_kbd_repeat_info(_data: *mut c_void, _kbd: *mut wl_proxy, _r: i32, _d: i32) {}

unsafe extern "C" fn wl_ptr_enter(
    data: *mut c_void,
    _ptr: *mut wl_proxy,
    _serial: u32,
    _surface: *mut c_void,
    sx: i32,
    sy: i32,
) {
    if data.is_null() {
        return;
    }
    let state = unsafe { &*(data.cast::<SharedWaylandState>()) };
    state.push_event(WaylandEvent::CursorPos {
        x: (sx as f32) / 256.0,
        y: (sy as f32) / 256.0,
    });
}

unsafe extern "C" fn wl_ptr_leave(
    _data: *mut c_void,
    _ptr: *mut wl_proxy,
    _serial: u32,
    _surface: *mut c_void,
) {
}

unsafe extern "C" fn wl_ptr_motion(
    data: *mut c_void,
    _ptr: *mut wl_proxy,
    _time: u32,
    sx: i32,
    sy: i32,
) {
    if data.is_null() {
        return;
    }
    let state = unsafe { &*(data.cast::<SharedWaylandState>()) };
    state.push_event(WaylandEvent::CursorPos {
        x: (sx as f32) / 256.0,
        y: (sy as f32) / 256.0,
    });
}

unsafe extern "C" fn wl_ptr_button(
    data: *mut c_void,
    _ptr: *mut wl_proxy,
    _serial: u32,
    _time: u32,
    button: u32,
    state_val: u32,
) {
    if data.is_null() {
        return;
    }
    if let Some(btn_idx) = map_wl_button(button) {
        let state = unsafe { &*(data.cast::<SharedWaylandState>()) };
        state.push_event(WaylandEvent::MouseBtn {
            button: btn_idx,
            down: state_val == 1,
        });
    }
}

unsafe extern "C" fn wl_ptr_axis(
    data: *mut c_void,
    _ptr: *mut wl_proxy,
    _time: u32,
    axis: u32,
    value: i32,
) {
    if data.is_null() {
        return;
    }
    let delta = -(value as f32) / 256.0 / 15.0;
    let state = unsafe { &*(data.cast::<SharedWaylandState>()) };
    if axis == 0 {
        state.push_event(WaylandEvent::Scroll {
            delta_x: 0.0,
            delta_y: delta,
        });
    } else if axis == 1 {
        state.push_event(WaylandEvent::Scroll {
            delta_x: delta,
            delta_y: 0.0,
        });
    }
}

unsafe extern "C" fn wl_ptr_frame(_data: *mut c_void, _ptr: *mut wl_proxy) {}
unsafe extern "C" fn wl_ptr_axis_source(_data: *mut c_void, _ptr: *mut wl_proxy, _src: u32) {}
unsafe extern "C" fn wl_ptr_axis_stop(_data: *mut c_void, _ptr: *mut wl_proxy, _t: u32, _a: u32) {}
unsafe extern "C" fn wl_ptr_axis_discrete(
    _data: *mut c_void,
    _ptr: *mut wl_proxy,
    _a: u32,
    _d: i32,
) {
}
unsafe extern "C" fn wl_ptr_axis_value120(
    _data: *mut c_void,
    _ptr: *mut wl_proxy,
    _a: u32,
    _v: i32,
) {
}
unsafe extern "C" fn wl_ptr_axis_relative_direction(
    _data: *mut c_void,
    _ptr: *mut wl_proxy,
    _a: u32,
    _d: u32,
) {
}

static REGISTRY_LISTENER: [extern "C" fn(); 2] = unsafe {
    [
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, *const c_char, u32),
            extern "C" fn(),
        >(wl_reg_global),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32),
            extern "C" fn(),
        >(wl_reg_global_remove),
    ]
};

static SEAT_LISTENER: [extern "C" fn(); 2] = unsafe {
    [
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32),
            extern "C" fn(),
        >(wl_seat_caps),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, *const c_char),
            extern "C" fn(),
        >(wl_seat_name),
    ]
};

static KEYBOARD_LISTENER: [extern "C" fn(); 6] = unsafe {
    [
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, c_int, u32),
            extern "C" fn(),
        >(wl_kbd_keymap),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, *mut c_void, *mut c_void),
            extern "C" fn(),
        >(wl_kbd_enter),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, *mut c_void),
            extern "C" fn(),
        >(wl_kbd_leave),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, u32, u32, u32),
            extern "C" fn(),
        >(wl_kbd_key),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, u32, u32, u32, u32),
            extern "C" fn(),
        >(wl_kbd_modifiers),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, i32, i32),
            extern "C" fn(),
        >(wl_kbd_repeat_info),
    ]
};

static POINTER_LISTENER: [extern "C" fn(); 11] = unsafe {
    [
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, *mut c_void, i32, i32),
            extern "C" fn(),
        >(wl_ptr_enter),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, *mut c_void),
            extern "C" fn(),
        >(wl_ptr_leave),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, i32, i32),
            extern "C" fn(),
        >(wl_ptr_motion),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, u32, u32, u32),
            extern "C" fn(),
        >(wl_ptr_button),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, u32, i32),
            extern "C" fn(),
        >(wl_ptr_axis),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy),
            extern "C" fn(),
        >(wl_ptr_frame),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32),
            extern "C" fn(),
        >(wl_ptr_axis_source),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, u32),
            extern "C" fn(),
        >(wl_ptr_axis_stop),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, i32),
            extern "C" fn(),
        >(wl_ptr_axis_discrete),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, i32),
            extern "C" fn(),
        >(wl_ptr_axis_value120),
        std::mem::transmute::<
            unsafe extern "C" fn(*mut c_void, *mut wl_proxy, u32, u32),
            extern "C" fn(),
        >(wl_ptr_axis_relative_direction),
    ]
};

/// Non-intrusive Wayland input listener attached to the application's own `wl_display` queue.
pub struct WaylandBackend {
    display: *mut wl_display,
    queue_initialized: bool,
    shared: Box<SharedWaylandState>,
}

unsafe impl Send for WaylandBackend {}

impl Default for WaylandBackend {
    fn default() -> Self {
        Self::new()
    }
}

impl WaylandBackend {
    #[must_use]
    pub fn new() -> Self {
        Self {
            display: std::ptr::null_mut(),
            queue_initialized: false,
            shared: SharedWaylandState::new(),
        }
    }

    #[must_use]
    pub fn new_for_test() -> Self {
        Self::new()
    }

    pub fn push_test_event(&self, ev: WaylandEvent) {
        self.shared.push_event(ev);
    }

    #[must_use]
    pub fn has_display(&self) -> bool {
        !self.display.is_null()
    }

    pub fn attach_display(&mut self, display: *mut c_void) {
        if display.is_null() {
            return;
        }
        let next = display.cast::<wl_display>();
        if self.display == next {
            return;
        }
        self.detach_display(std::ptr::null_mut());
        self.display = next;
        self.queue_initialized = false;
    }

    pub fn detach_display(&mut self, display: *mut c_void) {
        if !display.is_null() && self.display != display.cast::<wl_display>() {
            return;
        }
        if let Some(rt) = load_wayland_runtime() {
            unsafe { self.shared.teardown_proxies(rt.client) };
        }
        self.display = std::ptr::null_mut();
        self.queue_initialized = false;
        if let Ok(mut q) = self.shared.events.lock() {
            q.clear();
        }
    }

    fn ensure_queue(&mut self) {
        if self.display.is_null() || self.queue_initialized {
            return;
        }
        self.queue_initialized = true;
        let Some(rt) = load_wayland_runtime() else {
            return;
        };
        let queue = unsafe { (rt.client.wl_display_create_queue)(self.display) };
        if queue.is_null() {
            return;
        }
        self.shared.queue = queue;

        let dpy_proxy = self.display.cast::<wl_proxy>();
        let wrapper = unsafe { (rt.client.wl_proxy_create_wrapper)(dpy_proxy) };
        if wrapper.is_null() {
            return;
        }
        unsafe { (rt.client.wl_proxy_set_queue)(wrapper, queue) };
        let mut args = [wl_argument { n: 0 }];
        let reg = unsafe {
            (rt.client.wl_proxy_marshal_array_constructor)(
                wrapper,
                1, // wl_display::get_registry
                args.as_mut_ptr(),
                rt.ifaces.registry,
            )
        };
        unsafe { (rt.client.wl_proxy_wrapper_destroy)(wrapper) };
        if reg.is_null() {
            return;
        }
        self.shared.registry = reg;
        let data_ptr = std::ptr::from_mut(&mut *self.shared).cast::<c_void>();
        unsafe {
            let _ = (rt.client.wl_proxy_add_listener)(
                reg,
                REGISTRY_LISTENER.as_ptr().cast_mut(),
                data_ptr,
            );
            let _ = (rt.client.wl_display_roundtrip_queue)(self.display, queue);
            if !self.shared.seat.is_null() {
                let _ = (rt.client.wl_display_roundtrip_queue)(self.display, queue);
            }
        }
    }

    pub fn poll_events(&mut self) -> Vec<WaylandEvent> {
        if !self.display.is_null() {
            self.ensure_queue();
            if !self.shared.queue.is_null()
                && let Some(rt) = load_wayland_runtime()
            {
                unsafe {
                    let _ = (rt.client.wl_display_dispatch_queue_pending)(
                        self.display,
                        self.shared.queue,
                    );
                }
            }
        }
        self.shared
            .events
            .lock()
            .map(|mut g| std::mem::take(&mut *g))
            .unwrap_or_default()
    }
}

impl Drop for WaylandBackend {
    fn drop(&mut self) {
        self.detach_display(std::ptr::null_mut());
    }
}
