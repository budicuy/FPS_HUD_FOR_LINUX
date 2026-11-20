//! `cdylib libfpshud.so`: Pure-Rust Vulkan & OpenGL implicit overlay layer with strict 8-symbol C-ABI.

pub mod launcher;
pub mod runtime;
pub mod vulkan;

use ash::vk::{self, Handle};
use std::ffi::{c_char, c_void};
use vulkan::hooks::{
    VkNegotiateLayerInterface, lookup_device_proc, lookup_instance_proc,
    negotiate_loader_layer_interface_version,
};

/// Safe-ergonomic test wrapper around `lookup_instance_proc`.
///
/// # Safety
/// `p_name` must be null or point to a valid NUL-terminated C string.
#[must_use]
pub unsafe fn vk_get_instance_proc_addr(
    instance: *mut c_void,
    p_name: *const c_char,
) -> vk::PFN_vkVoidFunction {
    unsafe { lookup_instance_proc(vk::Instance::from_raw(instance as u64), p_name) }
}

/// Safe-ergonomic test wrapper around `lookup_device_proc`.
///
/// # Safety
/// `p_name` must be null or point to a valid NUL-terminated C string.
#[must_use]
pub unsafe fn vk_get_device_proc_addr(
    device: *mut c_void,
    p_name: *const c_char,
) -> vk::PFN_vkVoidFunction {
    unsafe { lookup_device_proc(vk::Device::from_raw(device as u64), p_name) }
}

/// Vulkan loader negotiation entrypoint exported by `libfpshud.so`.
///
/// # Safety
/// `p_version_struct` must be null or point to an initialized `VkNegotiateLayerInterface`.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn vkNegotiateLoaderLayerInterfaceVersion(
    p_version_struct: *mut VkNegotiateLayerInterface,
) -> vk::Result {
    unsafe { negotiate_loader_layer_interface_version(p_version_struct) }
}

/// Vulkan instance proc lookup entrypoint exported by `libfpshud.so`.
///
/// # Safety
/// `instance` and `p_name` must satisfy Vulkan's `vkGetInstanceProcAddr` contract.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn vkGetInstanceProcAddr(
    instance: vk::Instance,
    p_name: *const c_char,
) -> vk::PFN_vkVoidFunction {
    unsafe { lookup_instance_proc(instance, p_name) }
}

/// Vulkan device proc lookup entrypoint exported by `libfpshud.so`.
///
/// # Safety
/// `device` and `p_name` must satisfy Vulkan's `vkGetDeviceProcAddr` contract.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn vkGetDeviceProcAddr(
    device: vk::Device,
    p_name: *const c_char,
) -> vk::PFN_vkVoidFunction {
    unsafe { lookup_device_proc(device, p_name) }
}

unsafe fn dlsym_next(sym: &std::ffi::CStr) -> *mut c_void {
    unsafe { libc::dlsym(libc::RTLD_NEXT, sym.as_ptr()) }
}

/// GLX swap buffers hook exported by `libfpshud.so`.
///
/// # Safety
/// Arguments must satisfy GLX `glXSwapBuffers(Display*, GLXDrawable)`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn glXSwapBuffers(dpy: *mut c_void, drawable: *mut c_void) {
    let p = unsafe { dlsym_next(c"glXSwapBuffers") };
    if !p.is_null() {
        let real: unsafe extern "C" fn(*mut c_void, *mut c_void) =
            unsafe { std::mem::transmute(p) };
        unsafe { real(dpy, drawable) };
    }
}

/// GLX proc address resolver hook exported by `libfpshud.so`.
///
/// # Safety
/// `proc_name` must point to a valid NUL-terminated GLX symbol name.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn glXGetProcAddress(proc_name: *const u8) -> *const c_void {
    let p = unsafe { dlsym_next(c"glXGetProcAddress") };
    if !p.is_null() {
        let real: unsafe extern "C" fn(*const u8) -> *const c_void =
            unsafe { std::mem::transmute(p) };
        unsafe { real(proc_name) }
    } else {
        std::ptr::null()
    }
}

/// GLX ARB proc address resolver hook exported by `libfpshud.so`.
///
/// # Safety
/// `proc_name` must point to a valid NUL-terminated GLX symbol name.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn glXGetProcAddressARB(proc_name: *const u8) -> *const c_void {
    let p = unsafe { dlsym_next(c"glXGetProcAddressARB") };
    if !p.is_null() {
        let real: unsafe extern "C" fn(*const u8) -> *const c_void =
            unsafe { std::mem::transmute(p) };
        unsafe { real(proc_name) }
    } else {
        unsafe { glXGetProcAddress(proc_name) }
    }
}

/// EGL swap buffers hook exported by `libfpshud.so`.
///
/// # Safety
/// Arguments must satisfy EGL `eglSwapBuffers(EGLDisplay, EGLSurface)`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglSwapBuffers(dpy: *mut c_void, surface: *mut c_void) -> u32 {
    let p = unsafe { dlsym_next(c"eglSwapBuffers") };
    if !p.is_null() {
        let real: unsafe extern "C" fn(*mut c_void, *mut c_void) -> u32 =
            unsafe { std::mem::transmute(p) };
        unsafe { real(dpy, surface) }
    } else {
        0
    }
}

/// EGL proc address resolver hook exported by `libfpshud.so`.
///
/// # Safety
/// `proc_name` must point to a valid NUL-terminated EGL symbol name.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglGetProcAddress(proc_name: *const c_char) -> *const c_void {
    let p = unsafe { dlsym_next(c"eglGetProcAddress") };
    if !p.is_null() {
        let real: unsafe extern "C" fn(*const c_char) -> *const c_void =
            unsafe { std::mem::transmute(p) };
        unsafe { real(proc_name) }
    } else {
        std::ptr::null()
    }
}
