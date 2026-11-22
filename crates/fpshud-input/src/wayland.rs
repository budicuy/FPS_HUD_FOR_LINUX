//! Pure-Rust dynamic Wayland client input listener (`wl_seat` -> `wl_pointer` & `wl_keyboard`),
//! zero `DT_NEEDED`, zero `#[repr(C)]`, zero `libc`.

use libloading::Library;
use rustix::fs::{MemfdFlags, ftruncate, memfd_create};
use std::ffi::{CStr, c_char, c_int, c_void};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
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
    compositor: *const wl_interface,
    subcompositor: *const wl_interface,
    shm: *const wl_interface,
    shm_pool: *const wl_interface,
    buffer: *const wl_interface,
    surface: *const wl_interface,
    subsurface: *const wl_interface,
    region: *const wl_interface,
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
        let compositor = get_iface(b"wl_compositor_interface\0")?;
        let subcompositor = get_iface(b"wl_subcompositor_interface\0")?;
        let shm = get_iface(b"wl_shm_interface\0")?;
        let shm_pool = get_iface(b"wl_shm_pool_interface\0")?;
        let buffer = get_iface(b"wl_buffer_interface\0")?;
        let surface = get_iface(b"wl_surface_interface\0")?;
        let subsurface = get_iface(b"wl_subsurface_interface\0")?;
        let region = get_iface(b"wl_region_interface\0")?;
        Some(WaylandRuntime {
            client,
            ifaces: WaylandIfaces {
                _lib: lib,
                registry,
                seat,
                keyboard,
                pointer,
                compositor,
                subcompositor,
                shm,
                shm_pool,
                buffer,
                surface,
                subsurface,
                region,
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
    compositor: *mut wl_proxy,
    subcompositor: *mut wl_proxy,
    shm: *mut wl_proxy,
    parent_surface: *mut wl_proxy,
    shield_surface: *mut wl_proxy,
    shield_subsurface: *mut wl_proxy,
    shield_pool: *mut wl_proxy,
    shield_buffer: *mut wl_proxy,
    shield_fd: Option<OwnedFd>,
    shield_size: (u32, u32),
    shield_active: bool,
}

unsafe impl Send for SharedWaylandState {}
unsafe impl Sync for SharedWaylandState {}

unsafe fn destroy_proxy(
    client: &WaylandClient,
    slot: &mut *mut wl_proxy,
    destroy_opcode: Option<u32>,
) {
    if !slot.is_null() {
        if let Some(op) = destroy_opcode {
            unsafe { (client.wl_proxy_marshal_array)(*slot, op, std::ptr::null_mut()) };
        }
        unsafe { (client.wl_proxy_destroy)(*slot) };
        *slot = std::ptr::null_mut();
    }
}

unsafe fn bind_wl_global(
    rt: &WaylandRuntime,
    registry: *mut wl_proxy,
    name: u32,
    bind_ver: u32,
    iface: *const wl_interface,
    queue: *mut wl_event_queue,
) -> *mut wl_proxy {
    let mut args = [
        wl_argument { u: name },
        wl_argument {
            s: unsafe { (*iface).name },
        },
        wl_argument { u: bind_ver },
        wl_argument { n: 0 },
    ];
    let proxy = unsafe {
        (rt.client.wl_proxy_marshal_array_constructor_versioned)(
            registry,
            0,
            args.as_mut_ptr(),
            iface,
            bind_ver,
        )
    };
    if !proxy.is_null() {
        unsafe { (rt.client.wl_proxy_set_queue)(proxy, queue) };
    }
    proxy
}

impl SharedWaylandState {
    fn new() -> Box<Self> {
        Box::new(Self {
            events: Mutex::new(Vec::new()),
            queue: std::ptr::null_mut(),
            registry: std::ptr::null_mut(),
            seat: std::ptr::null_mut(),
            keyboard: std::ptr::null_mut(),
            pointer: std::ptr::null_mut(),
            compositor: std::ptr::null_mut(),
            subcompositor: std::ptr::null_mut(),
            shm: std::ptr::null_mut(),
            parent_surface: std::ptr::null_mut(),
            shield_surface: std::ptr::null_mut(),
            shield_subsurface: std::ptr::null_mut(),
            shield_pool: std::ptr::null_mut(),
            shield_buffer: std::ptr::null_mut(),
            shield_fd: None,
            shield_size: (0, 0),
            shield_active: false,
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

    unsafe fn teardown_shield_buffer(&mut self, client: &WaylandClient) {
        unsafe {
            destroy_proxy(client, &mut self.shield_buffer, Some(0));
            destroy_proxy(client, &mut self.shield_pool, Some(1));
        }
        self.shield_fd = None;
        self.shield_size = (0, 0);
    }

    unsafe fn teardown_shield_proxies(&mut self, client: &WaylandClient) {
        unsafe {
            self.teardown_shield_buffer(client);
            destroy_proxy(client, &mut self.shield_subsurface, Some(0));
            destroy_proxy(client, &mut self.shield_surface, Some(0));
        }
        self.shield_active = false;
    }

    unsafe fn teardown_proxies(&mut self, client: &WaylandClient) {
        unsafe {
            self.teardown_shield_proxies(client);
            destroy_proxy(client, &mut self.pointer, None);
            destroy_proxy(client, &mut self.keyboard, None);
            destroy_proxy(client, &mut self.seat, None);
            destroy_proxy(client, &mut self.shm, None);
            destroy_proxy(client, &mut self.subcompositor, Some(0));
            destroy_proxy(client, &mut self.compositor, None);
            destroy_proxy(client, &mut self.registry, None);
        }
        if !self.queue.is_null() {
            unsafe { (client.wl_event_queue_destroy)(self.queue) };
            self.queue = std::ptr::null_mut();
        }
        self.parent_surface = std::ptr::null_mut();
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
    let Ok(iface_name) = unsafe { CStr::from_ptr(interface) }.to_str() else {
        return;
    };
    let Some(rt) = load_wayland_runtime() else {
        return;
    };
    match iface_name {
        "wl_seat" if state.seat.is_null() => {
            let seat = unsafe {
                bind_wl_global(rt, registry, name, version.min(7), rt.ifaces.seat, state.queue)
            };
            if !seat.is_null() {
                state.seat = seat;
                unsafe {
                    let _ = (rt.client.wl_proxy_add_listener)(
                        seat,
                        SEAT_LISTENER.as_ptr().cast_mut(),
                        data,
                    );
                }
            }
        }
        "wl_compositor" if state.compositor.is_null() => {
            state.compositor = unsafe {
                bind_wl_global(
                    rt,
                    registry,
                    name,
                    version.min(4),
                    rt.ifaces.compositor,
                    state.queue,
                )
            };
        }
        "wl_subcompositor" if state.subcompositor.is_null() => {
            state.subcompositor = unsafe {
                bind_wl_global(
                    rt,
                    registry,
                    name,
                    version.min(1),
                    rt.ifaces.subcompositor,
                    state.queue,
                )
            };
        }
        "wl_shm" if state.shm.is_null() => {
            state.shm = unsafe {
                bind_wl_global(rt, registry, name, version.min(1), rt.ifaces.shm, state.queue)
            };
        }
        _ => {}
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

    #[must_use]
    pub fn is_shield_active(&self) -> bool {
        self.shared.shield_active
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

    pub fn set_parent_surface(&mut self, surface: *mut c_void) {
        if surface.is_null() {
            return;
        }
        let next = surface.cast::<wl_proxy>();
        if self.shared.parent_surface == next {
            return;
        }
        if let Some(rt) = load_wayland_runtime() {
            unsafe { self.shared.teardown_shield_proxies(rt.client) };
        }
        self.shared.parent_surface = next;
    }

    pub fn clear_parent_surface_if_matches(&mut self, surface: *mut c_void) {
        if surface.is_null() {
            return;
        }
        if self.shared.parent_surface == surface.cast::<wl_proxy>() {
            if let Some(rt) = load_wayland_runtime() {
                unsafe { self.shared.teardown_shield_proxies(rt.client) };
            }
            self.shared.parent_surface = std::ptr::null_mut();
        }
    }

    pub fn sync_input_shield(&mut self, active: bool, width: u32, height: u32) {
        if self.display.is_null() {
            return;
        }
        if !active {
            if !self.shared.shield_active {
                return;
            }
            if !self.shared.shield_surface.is_null()
                && let Some(rt) = load_wayland_runtime()
            {
                unsafe {
                    let mut att = [
                        wl_argument { o: std::ptr::null() },
                        wl_argument { i: 0 },
                        wl_argument { i: 0 },
                    ];
                    (rt.client.wl_proxy_marshal_array)(
                        self.shared.shield_surface,
                        1, // wl_surface::attach(NULL, 0, 0)
                        att.as_mut_ptr(),
                    );
                    if !self.shared.compositor.is_null() {
                        let empty_reg = make_wl_region(rt, self.shared.compositor, None);
                        if !empty_reg.is_null() {
                            let mut reg_arg = [wl_argument { o: empty_reg.cast() }];
                            (rt.client.wl_proxy_marshal_array)(
                                self.shared.shield_surface,
                                5, // wl_surface::set_input_region
                                reg_arg.as_mut_ptr(),
                            );
                            let mut reg_slot = empty_reg;
                            destroy_proxy(rt.client, &mut reg_slot, Some(0));
                        }
                    }
                    (rt.client.wl_proxy_marshal_array)(
                        self.shared.shield_surface,
                        6, // wl_surface::commit
                        std::ptr::null_mut(),
                    );
                    let _ = (rt.client.wl_display_flush)(self.display);
                }
            }
            self.shared.shield_active = false;
            return;
        }

        self.ensure_queue();
        if self.shared.parent_surface.is_null()
            || self.shared.compositor.is_null()
            || self.shared.subcompositor.is_null()
            || self.shared.shm.is_null()
        {
            return;
        }
        let Some(rt) = load_wayland_runtime() else {
            return;
        };

        let w = width.clamp(1, 8192);
        let h = height.clamp(1, 8192);
        if self.shared.shield_active && self.shared.shield_size == (w, h) {
            return;
        }

        if !unsafe { ensure_shield_surface(rt, &mut self.shared) } {
            return;
        }
        if !unsafe { ensure_shield_shm_buffer(rt, &mut self.shared, w, h) } {
            return;
        }

        unsafe {
            let opaque_reg = make_wl_region(rt, self.shared.compositor, None);
            let input_reg = make_wl_region(
                rt,
                self.shared.compositor,
                Some((0, 0, w as i32, h as i32)),
            );

            let mut att = [
                wl_argument {
                    o: self.shared.shield_buffer.cast(),
                },
                wl_argument { i: 0 },
                wl_argument { i: 0 },
            ];
            (rt.client.wl_proxy_marshal_array)(self.shared.shield_surface, 1, att.as_mut_ptr());

            if !opaque_reg.is_null() {
                let mut o_arg = [wl_argument { o: opaque_reg.cast() }];
                (rt.client.wl_proxy_marshal_array)(
                    self.shared.shield_surface,
                    4, // wl_surface::set_opaque_region
                    o_arg.as_mut_ptr(),
                );
            }
            if !input_reg.is_null() {
                let mut i_arg = [wl_argument { o: input_reg.cast() }];
                (rt.client.wl_proxy_marshal_array)(
                    self.shared.shield_surface,
                    5, // wl_surface::set_input_region
                    i_arg.as_mut_ptr(),
                );
            }
            let mut dmg = [
                wl_argument { i: 0 },
                wl_argument { i: 0 },
                wl_argument { i: w as i32 },
                wl_argument { i: h as i32 },
            ];
            (rt.client.wl_proxy_marshal_array)(self.shared.shield_surface, 2, dmg.as_mut_ptr());
            (rt.client.wl_proxy_marshal_array)(
                self.shared.shield_surface,
                6, // wl_surface::commit
                std::ptr::null_mut(),
            );

            let mut o_slot = opaque_reg;
            destroy_proxy(rt.client, &mut o_slot, Some(0));
            let mut i_slot = input_reg;
            destroy_proxy(rt.client, &mut i_slot, Some(0));
            let _ = (rt.client.wl_display_flush)(self.display);
        }
        self.shared.shield_active = true;
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

unsafe fn make_wl_region(
    rt: &WaylandRuntime,
    compositor: *mut wl_proxy,
    rect: Option<(i32, i32, i32, i32)>,
) -> *mut wl_proxy {
    let mut args = [wl_argument { n: 0 }];
    let reg = unsafe {
        (rt.client.wl_proxy_marshal_array_constructor)(
            compositor,
            1, // wl_compositor::create_region
            args.as_mut_ptr(),
            rt.ifaces.region,
        )
    };
    if !reg.is_null()
        && let Some((x, y, w, h)) = rect
    {
        let mut radd = [
            wl_argument { i: x },
            wl_argument { i: y },
            wl_argument { i: w },
            wl_argument { i: h },
        ];
        unsafe { (rt.client.wl_proxy_marshal_array)(reg, 1, radd.as_mut_ptr()) };
    }
    reg
}

unsafe fn ensure_shield_surface(rt: &WaylandRuntime, state: &mut SharedWaylandState) -> bool {
    if !state.shield_surface.is_null() && !state.shield_subsurface.is_null() {
        return true;
    }
    let mut args = [wl_argument { n: 0 }];
    let surf = unsafe {
        (rt.client.wl_proxy_marshal_array_constructor)(
            state.compositor,
            0, // wl_compositor::create_surface
            args.as_mut_ptr(),
            rt.ifaces.surface,
        )
    };
    if surf.is_null() {
        return false;
    }
    unsafe { (rt.client.wl_proxy_set_queue)(surf, state.queue) };

    let mut sargs = [
        wl_argument { n: 0 },
        wl_argument { o: surf.cast() },
        wl_argument {
            o: state.parent_surface.cast(),
        },
    ];
    let sub = unsafe {
        (rt.client.wl_proxy_marshal_array_constructor)(
            state.subcompositor,
            1, // wl_subcompositor::get_subsurface
            sargs.as_mut_ptr(),
            rt.ifaces.subsurface,
        )
    };
    if sub.is_null() {
        let mut s_slot = surf;
        unsafe { destroy_proxy(rt.client, &mut s_slot, Some(0)) };
        return false;
    }
    unsafe {
        (rt.client.wl_proxy_set_queue)(sub, state.queue);
        (rt.client.wl_proxy_marshal_array)(sub, 5 /* set_desync */, std::ptr::null_mut());
        let mut pos = [wl_argument { i: 0 }, wl_argument { i: 0 }];
        (rt.client.wl_proxy_marshal_array)(sub, 1 /* set_position */, pos.as_mut_ptr());
        let mut above = [wl_argument {
            o: state.parent_surface.cast(),
        }];
        (rt.client.wl_proxy_marshal_array)(sub, 2 /* place_above */, above.as_mut_ptr());
    }
    state.shield_surface = surf;
    state.shield_subsurface = sub;
    true
}

unsafe fn ensure_shield_shm_buffer(
    rt: &WaylandRuntime,
    state: &mut SharedWaylandState,
    w: u32,
    h: u32,
) -> bool {
    if !state.shield_buffer.is_null() && state.shield_size == (w, h) {
        return true;
    }
    unsafe { state.teardown_shield_buffer(rt.client) };

    let stride = match w.checked_mul(4) {
        Some(s) if s <= i32::MAX as u32 => s,
        _ => return false,
    };
    let byte_len = match stride.checked_mul(h) {
        Some(len) if len > 0 && len <= i32::MAX as u32 => len,
        _ => return false,
    };
    let Ok(fd) = memfd_create(c"fpshud-wl-shield", MemfdFlags::CLOEXEC) else {
        return false;
    };
    if ftruncate(&fd, u64::from(byte_len)).is_err() {
        return false;
    }

    let mut pargs = [
        wl_argument { n: 0 },
        wl_argument { h: fd.as_raw_fd() },
        wl_argument { i: byte_len as i32 },
    ];
    let pool = unsafe {
        (rt.client.wl_proxy_marshal_array_constructor)(
            state.shm,
            0, // wl_shm::create_pool
            pargs.as_mut_ptr(),
            rt.ifaces.shm_pool,
        )
    };
    if pool.is_null() {
        return false;
    }
    unsafe { (rt.client.wl_proxy_set_queue)(pool, state.queue) };

    let mut bargs = [
        wl_argument { n: 0 },
        wl_argument { i: 0 },
        wl_argument { i: w as i32 },
        wl_argument { i: h as i32 },
        wl_argument { i: stride as i32 },
        wl_argument { u: 0 }, // WL_SHM_FORMAT_ARGB8888 (zeroed memfd = 100% transparent)
    ];
    let buf = unsafe {
        (rt.client.wl_proxy_marshal_array_constructor)(
            pool,
            0, // wl_shm_pool::create_buffer
            bargs.as_mut_ptr(),
            rt.ifaces.buffer,
        )
    };
    if buf.is_null() {
        let mut p_slot = pool;
        unsafe { destroy_proxy(rt.client, &mut p_slot, Some(1)) };
        return false;
    }
    unsafe { (rt.client.wl_proxy_set_queue)(buf, state.queue) };

    state.shield_fd = Some(fd);
    state.shield_pool = pool;
    state.shield_buffer = buf;
    state.shield_size = (w, h);
    true
}

/// Live Wayland compositor test helper: attaches a dummy parent `wl_surface`, activates &
/// deactivates the subsurface input shield, and checks `wl_display_get_error == 0`.
#[must_use]
pub fn test_roundtrip_subsurface_shield(width: u32, height: u32) -> Option<(bool, bool, i32)> {
    let rt = load_wayland_runtime()?;
    let dpy = unsafe { (rt.client.wl_display_connect)(std::ptr::null()) };
    if dpy.is_null() {
        return None;
    }

    let mut wb = WaylandBackend::new();
    wb.attach_display(dpy.cast());
    wb.ensure_queue();
    if wb.shared.compositor.is_null() {
        drop(wb);
        unsafe { (rt.client.wl_display_disconnect)(dpy) };
        return None;
    }

    let mut a = [wl_argument { n: 0 }];
    let mut parent = unsafe {
        (rt.client.wl_proxy_marshal_array_constructor)(
            wb.shared.compositor,
            0,
            a.as_mut_ptr(),
            rt.ifaces.surface,
        )
    };
    if parent.is_null() {
        drop(wb);
        unsafe { (rt.client.wl_display_disconnect)(dpy) };
        return None;
    }

    wb.set_parent_surface(parent.cast());
    wb.sync_input_shield(true, width, height);
    let _ = unsafe { (rt.client.wl_display_roundtrip_queue)(dpy, wb.shared.queue) };
    let was_active = wb.is_shield_active();

    wb.sync_input_shield(false, width, height);
    let _ = unsafe { (rt.client.wl_display_roundtrip_queue)(dpy, wb.shared.queue) };
    let deactivated = !wb.is_shield_active();

    let err = unsafe { (rt.client.wl_display_get_error)(dpy) };
    wb.clear_parent_surface_if_matches(parent.cast());
    unsafe { destroy_proxy(rt.client, &mut parent, Some(0)) };
    wb.detach_display(std::ptr::null_mut());
    unsafe {
        (rt.client.wl_display_disconnect)(dpy);
    }
    // Dropping wb after wl_display_disconnect must be a safe no-op once detached.
    drop(wb);
    Some((was_active, deactivated, err))
}

impl Drop for WaylandBackend {
    fn drop(&mut self) {
        self.detach_display(std::ptr::null_mut());
    }
}
