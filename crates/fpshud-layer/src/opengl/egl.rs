use crate::opengl::blit;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::sync::OnceLock;

const EGL_HEIGHT: c_int = 0x3056;
const EGL_WIDTH: c_int = 0x3057;

struct SymPtr(*mut c_void);
unsafe impl Send for SymPtr {}
unsafe impl Sync for SymPtr {}

static REAL_EGL_SWAP_BUFFERS: OnceLock<Option<SymPtr>> = OnceLock::new();
static REAL_EGL_GET_PROC_ADDRESS: OnceLock<Option<SymPtr>> = OnceLock::new();
static REAL_EGL_QUERY_SURFACE: OnceLock<Option<SymPtr>> = OnceLock::new();

unsafe fn resolve_next(sym: &CStr) -> Option<SymPtr> {
    let p = unsafe { libc::dlsym(libc::RTLD_NEXT, sym.as_ptr()) };
    (!p.is_null()).then_some(SymPtr(p))
}

unsafe extern "C" fn proc_lookup_egl(name: *const c_char) -> *const c_void {
    let slot = REAL_EGL_GET_PROC_ADDRESS
        .get_or_init(|| unsafe { resolve_next(c"eglGetProcAddress") });
    if let Some(SymPtr(ptr)) = slot {
        let real: unsafe extern "C" fn(*const c_char) -> *const c_void =
            unsafe { std::mem::transmute(*ptr) };
        return unsafe { real(name) };
    }
    std::ptr::null()
}

unsafe extern "C" fn lookup_gl_sym_egl(name: *const c_char) -> *const c_void {
    let ptr = unsafe { proc_lookup_egl(name) };
    if !ptr.is_null() || name.is_null() {
        return ptr;
    }
    unsafe { resolve_next(CStr::from_ptr(name)) }
        .map_or(std::ptr::null(), |sym| sym.0.cast_const())
}

pub(crate) unsafe fn hook_egl_get_proc_address(proc_name: *const c_char) -> *const c_void {
    if !proc_name.is_null() {
        let name = unsafe { CStr::from_ptr(proc_name) }.to_bytes();
        match name {
            b"eglSwapBuffers" => return (crate::eglSwapBuffers as *const ()).cast::<c_void>(),
            b"eglGetProcAddress" => {
                return (crate::eglGetProcAddress as *const ()).cast::<c_void>();
            }
            _ => {}
        }
    }
    unsafe { proc_lookup_egl(proc_name) }
}

pub(crate) unsafe fn hook_egl_swap_buffers(dpy: *mut c_void, surface: *mut c_void) -> u32 {
    let mut hint_w = 0_u32;
    let mut hint_h = 0_u32;

    if !dpy.is_null() && !surface.is_null() {
        let slot_qs = REAL_EGL_QUERY_SURFACE
            .get_or_init(|| unsafe { resolve_next(c"eglQuerySurface") });
        if let Some(SymPtr(ptr)) = slot_qs {
            let qs: unsafe extern "C" fn(*mut c_void, *mut c_void, c_int, *mut c_int) -> u32 =
                unsafe { std::mem::transmute(*ptr) };
            let mut w: c_int = 0;
            let mut h: c_int = 0;
            if unsafe { qs(dpy, surface, EGL_WIDTH, &mut w) } != 0
                && unsafe { qs(dpy, surface, EGL_HEIGHT, &mut h) } != 0
            {
                hint_w = w.max(0) as u32;
                hint_h = h.max(0) as u32;
            }
        }
    }

    unsafe {
        blit::on_swap_buffers(lookup_gl_sym_egl, hint_w, hint_h, "OPENGL EGL");
    }

    let slot = REAL_EGL_SWAP_BUFFERS.get_or_init(|| unsafe { resolve_next(c"eglSwapBuffers") });
    if let Some(SymPtr(ptr)) = slot {
        let real: unsafe extern "C" fn(*mut c_void, *mut c_void) -> u32 =
            unsafe { std::mem::transmute(*ptr) };
        unsafe { real(dpy, surface) }
    } else {
        0
    }
}
