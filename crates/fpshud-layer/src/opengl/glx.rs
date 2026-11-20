use crate::opengl::blit;
use crate::runtime::LayerRuntime;
use fpshud_input::SurfaceBackendType;
use std::ffi::{CStr, c_char, c_void};
use std::sync::OnceLock;

struct SymPtr(*mut c_void);
unsafe impl Send for SymPtr {}
unsafe impl Sync for SymPtr {}

static REAL_GLX_SWAP_BUFFERS: OnceLock<Option<SymPtr>> = OnceLock::new();
static REAL_GLX_GET_PROC_ADDRESS: OnceLock<Option<SymPtr>> = OnceLock::new();
static REAL_GLX_GET_PROC_ADDRESS_ARB: OnceLock<Option<SymPtr>> = OnceLock::new();

unsafe fn resolve_next(sym: &CStr) -> Option<SymPtr> {
    let p = unsafe { libc::dlsym(libc::RTLD_NEXT, sym.as_ptr()) };
    (!p.is_null()).then_some(SymPtr(p))
}

unsafe extern "C" fn proc_lookup_glx(name: *const c_char) -> *const c_void {
    unsafe { glx_get_proc_address_arb_forward(name.cast::<u8>()) }
}

unsafe fn glx_get_proc_address_arb_forward(proc_name: *const u8) -> *const c_void {
    let slot = REAL_GLX_GET_PROC_ADDRESS_ARB
        .get_or_init(|| unsafe { resolve_next(c"glXGetProcAddressARB") });
    if let Some(SymPtr(ptr)) = slot {
        let real: unsafe extern "C" fn(*const u8) -> *const c_void =
            unsafe { std::mem::transmute(*ptr) };
        return unsafe { real(proc_name) };
    }
    let slot_base =
        REAL_GLX_GET_PROC_ADDRESS.get_or_init(|| unsafe { resolve_next(c"glXGetProcAddress") });
    if let Some(SymPtr(ptr)) = slot_base {
        let real: unsafe extern "C" fn(*const u8) -> *const c_void =
            unsafe { std::mem::transmute(*ptr) };
        return unsafe { real(proc_name) };
    }
    std::ptr::null()
}

pub(crate) unsafe fn hook_glx_get_proc_address(proc_name: *const u8, arb: bool) -> *const c_void {
    if !proc_name.is_null() {
        let name = unsafe { CStr::from_ptr(proc_name.cast::<c_char>()) }.to_bytes();
        match name {
            b"glXSwapBuffers" => return (crate::glXSwapBuffers as *const ()).cast::<c_void>(),
            b"glXGetProcAddress" => {
                return (crate::glXGetProcAddress as *const ()).cast::<c_void>();
            }
            b"glXGetProcAddressARB" => {
                return (crate::glXGetProcAddressARB as *const ()).cast::<c_void>();
            }
            _ => {}
        }
    }
    if arb {
        unsafe { glx_get_proc_address_arb_forward(proc_name) }
    } else {
        let slot = REAL_GLX_GET_PROC_ADDRESS
            .get_or_init(|| unsafe { resolve_next(c"glXGetProcAddress") });
        if let Some(SymPtr(ptr)) = slot {
            let real: unsafe extern "C" fn(*const u8) -> *const c_void =
                unsafe { std::mem::transmute(*ptr) };
            return unsafe { real(proc_name) };
        }
        unsafe { glx_get_proc_address_arb_forward(proc_name) }
    }
}

pub(crate) unsafe fn hook_glx_swap_buffers(dpy: *mut c_void, drawable: *mut c_void) {
    if !dpy.is_null()
        && let Ok(mut rt) = LayerRuntime::global().lock()
    {
        let win_id = drawable as usize as u32;
        rt.notify_surface_swapchain(SurfaceBackendType::X11, win_id);
    }

    unsafe {
        blit::on_swap_buffers(Some(proc_lookup_glx), 0, 0, "OPENGL GLX");
    }

    let slot = REAL_GLX_SWAP_BUFFERS.get_or_init(|| unsafe { resolve_next(c"glXSwapBuffers") });
    if let Some(SymPtr(ptr)) = slot {
        let real: unsafe extern "C" fn(*mut c_void, *mut c_void) =
            unsafe { std::mem::transmute(*ptr) };
        unsafe { real(dpy, drawable) };
    }
}
