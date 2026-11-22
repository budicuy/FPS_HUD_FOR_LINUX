use crate::runtime::LayerRuntime;
use crate::vulkan::swapchain::{
    DeviceData, HookRegistry, InstanceData, PfnVkSetDeviceLoaderData, SwapchainData,
    copy_dispatch_key_raw, dispatch_key_from_raw, find_device_by_key, find_instance_by_key,
    find_swapchain, registry,
};
use ash::vk::{self, Handle};
use ash_layer::{
    LayerDeviceCreateInfo, LayerFunction, LayerInstanceCreateInfo, NegotiateLayerInterface,
    NegotiateLayerStructType,
};
use fpshud_input::SurfaceBackendType;
use std::ffi::{CStr, c_char, c_void};
use std::sync::{Arc, Mutex};

const MIN_SUPPORTED_INTERFACE_VERSION: u32 = 2;

unsafe fn pfn_to_ptr(f: vk::PFN_vkVoidFunction) -> *const c_void {
    match f {
        Some(func) => (func as *const ()).cast::<c_void>(),
        None => std::ptr::null(),
    }
}

unsafe fn ptr_to_pfn(ptr: *const c_void) -> vk::PFN_vkVoidFunction {
    if ptr.is_null() {
        None
    } else {
        Some(unsafe { std::mem::transmute::<*const c_void, unsafe extern "system" fn()>(ptr) })
    }
}

pub(crate) unsafe fn negotiate_loader_layer_interface_version(
    p_version_struct: *mut NegotiateLayerInterface,
) -> vk::Result {
    if p_version_struct.is_null() {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    }
    let st = unsafe { &mut *p_version_struct };
    if st.s_type != NegotiateLayerStructType::INTERFACE_STRUCT
        || st.loader_layer_interface_version < MIN_SUPPORTED_INTERFACE_VERSION
    {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    }
    if st.loader_layer_interface_version > 2 {
        st.loader_layer_interface_version = 2;
    }
    st.pfn_get_instance_proc_addr = Some(crate::vkGetInstanceProcAddr);
    st.pfn_get_device_proc_addr = Some(crate::vkGetDeviceProcAddr);
    st.pfn_get_physical_device_proc_addr = None;
    vk::Result::SUCCESS
}

#[must_use]
pub fn verify_loader_negotiation_contract() -> bool {
    if unsafe { negotiate_loader_layer_interface_version(std::ptr::null_mut()) }
        != vk::Result::ERROR_INITIALIZATION_FAILED
    {
        return false;
    }

    let mut v1 = NegotiateLayerInterface {
        s_type: NegotiateLayerStructType::INTERFACE_STRUCT,
        p_next: std::ptr::null_mut(),
        loader_layer_interface_version: 1,
        pfn_get_instance_proc_addr: None,
        pfn_get_device_proc_addr: None,
        pfn_get_physical_device_proc_addr: None,
    };
    if unsafe { negotiate_loader_layer_interface_version(&raw mut v1) }
        != vk::Result::ERROR_INITIALIZATION_FAILED
    {
        return false;
    }

    let mut v2 = NegotiateLayerInterface {
        s_type: NegotiateLayerStructType::INTERFACE_STRUCT,
        p_next: std::ptr::null_mut(),
        loader_layer_interface_version: 2,
        pfn_get_instance_proc_addr: None,
        pfn_get_device_proc_addr: None,
        pfn_get_physical_device_proc_addr: None,
    };
    (unsafe { negotiate_loader_layer_interface_version(&raw mut v2) }) == vk::Result::SUCCESS
        && v2.pfn_get_instance_proc_addr.is_some()
        && v2.pfn_get_device_proc_addr.is_some()
}

pub(crate) unsafe extern "system" fn hook_create_instance(
    p_create_info: *const vk::InstanceCreateInfo<'_>,
    p_allocator: *const vk::AllocationCallbacks<'_>,
    p_instance: *mut vk::Instance,
) -> vk::Result {
    if p_create_info.is_null() || p_instance.is_null() {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    }

    let mut cur = unsafe { (*p_create_info).p_next }
        .cast::<LayerInstanceCreateInfo>()
        .cast_mut();
    let mut link_info: *mut LayerInstanceCreateInfo = std::ptr::null_mut();
    while !cur.is_null() {
        let info = unsafe { &*cur };
        if info.s_type == vk::StructureType::LOADER_INSTANCE_CREATE_INFO
            && info.function == LayerFunction::LAYER_LINK_INFO
        {
            link_info = cur;
            break;
        }
        cur = info.p_next.cast::<LayerInstanceCreateInfo>().cast_mut();
    }
    if link_info.is_null() || unsafe { (*link_info).u.p_layer_info }.is_null() {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    }

    let layer_info = unsafe { &*(*link_info).u.p_layer_info };
    let Some(next_gipa) = layer_info.pfn_next_get_instance_proc_addr else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    unsafe {
        (*link_info).u.p_layer_info = layer_info.p_next;
    }

    let Some(fp_create) = (unsafe { next_gipa(vk::Instance::null(), c"vkCreateInstance".as_ptr()) })
    else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let create_fn: vk::PFN_vkCreateInstance = unsafe { std::mem::transmute(fp_create) };
    let res = unsafe { create_fn(p_create_info, p_allocator, p_instance) };
    if res != vk::Result::SUCCESS {
        return res;
    }

    let instance_handle = unsafe { *p_instance };
    let instance = unsafe {
        ash::Instance::load_with(
            |name| pfn_to_ptr(next_gipa(instance_handle, name.as_ptr())),
            instance_handle,
        )
    };

    let load_inst_opt = |name: &CStr| -> Option<unsafe extern "system" fn()> {
        unsafe { next_gipa(instance_handle, name.as_ptr()) }
    };

    let data = Arc::new(InstanceData {
        handle: instance_handle,
        next_gipa,
        instance,
        create_xlib_surface_khr: load_inst_opt(c"vkCreateXlibSurfaceKHR")
            .map(|f| unsafe { std::mem::transmute(f) }),
        create_xcb_surface_khr: load_inst_opt(c"vkCreateXcbSurfaceKHR")
            .map(|f| unsafe { std::mem::transmute(f) }),
        create_wayland_surface_khr: load_inst_opt(c"vkCreateWaylandSurfaceKHR")
            .map(|f| unsafe { std::mem::transmute(f) }),
        destroy_surface_khr: load_inst_opt(c"vkDestroySurfaceKHR")
            .map(|f| unsafe { std::mem::transmute(f) }),
    });

    if let Ok(mut reg) = registry().lock() {
        reg.instances
            .insert(dispatch_key_from_raw(instance_handle.as_raw()), data);
    }
    vk::Result::SUCCESS
}

pub(crate) unsafe extern "system" fn hook_destroy_instance(
    instance: vk::Instance,
    p_allocator: *const vk::AllocationCallbacks<'_>,
) {
    if instance == vk::Instance::null() {
        return;
    }
    let key = dispatch_key_from_raw(instance.as_raw());
    let removed = registry().lock().ok().and_then(|mut reg| {
        reg.surfaces.clear();
        let hit = reg.instances.remove(&key);
        if let Some(ref target) = hit {
            reg.instances.retain(|_, v| !Arc::ptr_eq(v, target));
        }
        hit
    });
    if let Ok(mut rt) = LayerRuntime::global().lock() {
        rt.input.attach_wayland_display_hint(false);
        rt.input.reset_surface_hints();
    }
    if let Some(id) = removed {
        let cb = unsafe { p_allocator.as_ref() };
        unsafe { id.instance.destroy_instance(cb) };
    }
}

pub(crate) unsafe extern "system" fn hook_enumerate_physical_devices(
    instance: vk::Instance,
    p_count: *mut u32,
    p_devices: *mut vk::PhysicalDevice,
) -> vk::Result {
    let key = dispatch_key_from_raw(instance.as_raw());
    let Some(id) = find_instance_by_key(key) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let fp = id.instance.fp_v1_0().enumerate_physical_devices;
    let res = unsafe { fp(instance, p_count, p_devices) };
    if res == vk::Result::SUCCESS && !p_devices.is_null() && !p_count.is_null() {
        let count = unsafe { *p_count } as usize;
        if count > 0
            && let Ok(mut reg) = registry().lock()
        {
            let slice = unsafe { std::slice::from_raw_parts(p_devices, count) };
            for &pd in slice {
                if pd != vk::PhysicalDevice::null() {
                    reg.instances
                        .insert(dispatch_key_from_raw(pd.as_raw()), Arc::clone(&id));
                }
            }
        }
    }
    res
}

pub(crate) unsafe extern "system" fn hook_enumerate_physical_device_groups(
    instance: vk::Instance,
    p_count: *mut u32,
    p_props: *mut vk::PhysicalDeviceGroupProperties<'_>,
) -> vk::Result {
    let key = dispatch_key_from_raw(instance.as_raw());
    let Some(id) = find_instance_by_key(key) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let fn_ptr = unsafe {
        (id.next_gipa)(instance, c"vkEnumeratePhysicalDeviceGroups".as_ptr())
            .or_else(|| (id.next_gipa)(instance, c"vkEnumeratePhysicalDeviceGroupsKHR".as_ptr()))
    };
    let Some(fp) = fn_ptr else {
        return vk::Result::ERROR_EXTENSION_NOT_PRESENT;
    };
    let typed: vk::PFN_vkEnumeratePhysicalDeviceGroups = unsafe { std::mem::transmute(fp) };
    let res = unsafe { typed(instance, p_count, p_props) };
    if res == vk::Result::SUCCESS && !p_props.is_null() && !p_count.is_null() {
        let count = unsafe { *p_count } as usize;
        if count > 0
            && let Ok(mut reg) = registry().lock()
        {
            let slice = unsafe { std::slice::from_raw_parts(p_props, count) };
            for grp in slice {
                for &pd in &grp.physical_devices[..grp.physical_device_count as usize] {
                    if pd != vk::PhysicalDevice::null() {
                        reg.instances
                            .insert(dispatch_key_from_raw(pd.as_raw()), Arc::clone(&id));
                    }
                }
            }
        }
    }
    res
}

pub(crate) unsafe extern "system" fn hook_create_xlib_surface_khr(
    instance: vk::Instance,
    p_create_info: *const vk::XlibSurfaceCreateInfoKHR<'_>,
    p_allocator: *const vk::AllocationCallbacks<'_>,
    p_surface: *mut vk::SurfaceKHR,
) -> vk::Result {
    let Some(id) = find_instance_by_key(dispatch_key_from_raw(instance.as_raw())) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let Some(fp) = id.create_xlib_surface_khr else {
        return vk::Result::ERROR_EXTENSION_NOT_PRESENT;
    };
    let res = unsafe { fp(instance, p_create_info, p_allocator, p_surface) };
    if res == vk::Result::SUCCESS && !p_surface.is_null() {
        let surf = unsafe { *p_surface };
        let win = if p_create_info.is_null() {
            0
        } else {
            unsafe { (*p_create_info).window as u32 }
        };
        if surf != vk::SurfaceKHR::null()
            && let Ok(mut reg) = registry().lock()
        {
            reg.surfaces
                .insert(surf.as_raw(), (SurfaceBackendType::X11, win as u64));
        }
    }
    res
}

pub(crate) unsafe extern "system" fn hook_create_xcb_surface_khr(
    instance: vk::Instance,
    p_create_info: *const vk::XcbSurfaceCreateInfoKHR<'_>,
    p_allocator: *const vk::AllocationCallbacks<'_>,
    p_surface: *mut vk::SurfaceKHR,
) -> vk::Result {
    let Some(id) = find_instance_by_key(dispatch_key_from_raw(instance.as_raw())) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let Some(fp) = id.create_xcb_surface_khr else {
        return vk::Result::ERROR_EXTENSION_NOT_PRESENT;
    };
    let res = unsafe { fp(instance, p_create_info, p_allocator, p_surface) };
    if res == vk::Result::SUCCESS && !p_surface.is_null() {
        let surf = unsafe { *p_surface };
        let win = if p_create_info.is_null() {
            0
        } else {
            unsafe { (*p_create_info).window }
        };
        if surf != vk::SurfaceKHR::null()
            && let Ok(mut reg) = registry().lock()
        {
            reg.surfaces
                .insert(surf.as_raw(), (SurfaceBackendType::X11, win as u64));
        }
    }
    res
}

pub(crate) unsafe extern "system" fn hook_create_wayland_surface_khr(
    instance: vk::Instance,
    p_create_info: *const vk::WaylandSurfaceCreateInfoKHR<'_>,
    p_allocator: *const vk::AllocationCallbacks<'_>,
    p_surface: *mut vk::SurfaceKHR,
) -> vk::Result {
    let Some(id) = find_instance_by_key(dispatch_key_from_raw(instance.as_raw())) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let Some(fp) = id.create_wayland_surface_khr else {
        return vk::Result::ERROR_EXTENSION_NOT_PRESENT;
    };
    let res = unsafe { fp(instance, p_create_info, p_allocator, p_surface) };
    if res == vk::Result::SUCCESS && !p_surface.is_null() {
        let surf = unsafe { *p_surface };
        let wl_surf_raw = if p_create_info.is_null() {
            0_u64
        } else {
            unsafe { (*p_create_info).surface as usize as u64 }
        };
        if surf != vk::SurfaceKHR::null()
            && let Ok(mut reg) = registry().lock()
        {
            reg.surfaces
                .insert(surf.as_raw(), (SurfaceBackendType::Wayland, wl_surf_raw));
        }
        if let Ok(mut rt) = LayerRuntime::global().lock() {
            rt.input.attach_wayland_display_hint(true);
        }
    }
    res
}

pub(crate) unsafe extern "system" fn hook_destroy_surface_khr(
    instance: vk::Instance,
    surface: vk::SurfaceKHR,
    p_allocator: *const vk::AllocationCallbacks<'_>,
) {
    if surface != vk::SurfaceKHR::null() {
        let (info, any_wl_remaining) = registry()
            .lock()
            .ok()
            .map(|mut reg| {
                let info = reg.surfaces.remove(&surface.as_raw());
                let wl_left = reg
                    .surfaces
                    .values()
                    .any(|(backend, _)| *backend == SurfaceBackendType::Wayland);
                (info, wl_left)
            })
            .unwrap_or((None, false));
        match info {
            Some((SurfaceBackendType::X11, win_id)) if win_id != 0 => {
                if let Ok(mut rt) = LayerRuntime::global().lock() {
                    rt.input.clear_x11_target_window_if_matches(win_id as u32);
                }
            }
            Some((SurfaceBackendType::Wayland, _)) => {
                if !any_wl_remaining
                    && let Ok(mut rt) = LayerRuntime::global().lock()
                {
                    rt.input.attach_wayland_display_hint(false);
                }
            }
            _ => {}
        }
    }
    if let Some(id) = find_instance_by_key(dispatch_key_from_raw(instance.as_raw()))
        && let Some(fp) = id.destroy_surface_khr
    {
        unsafe { fp(instance, surface, p_allocator) };
    }
}

pub(crate) unsafe extern "system" fn hook_create_device(
    physical_device: vk::PhysicalDevice,
    p_create_info: *const vk::DeviceCreateInfo<'_>,
    p_allocator: *const vk::AllocationCallbacks<'_>,
    p_device: *mut vk::Device,
) -> vk::Result {
    if p_create_info.is_null() || p_device.is_null() {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    }

    let Some(id) = find_instance_by_key(dispatch_key_from_raw(physical_device.as_raw())) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };

    let mut cur = unsafe { (*p_create_info).p_next }
        .cast::<LayerDeviceCreateInfo>()
        .cast_mut();
    let mut link_info: *mut LayerDeviceCreateInfo = std::ptr::null_mut();
    let mut set_loader_data: Option<PfnVkSetDeviceLoaderData> = None;

    while !cur.is_null() {
        let info = unsafe { &*cur };
        if info.s_type == vk::StructureType::LOADER_DEVICE_CREATE_INFO {
            if info.function == LayerFunction::LAYER_LINK_INFO && link_info.is_null() {
                link_info = cur;
            } else if info.function == LayerFunction::LOADER_DATA_CALLBACK
                && set_loader_data.is_none()
            {
                set_loader_data = unsafe { info.u.pfn_set_device_loader_data };
            }
        }
        cur = info.p_next.cast::<LayerDeviceCreateInfo>().cast_mut();
    }

    if link_info.is_null() || unsafe { (*link_info).u.p_layer_info }.is_null() {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    }
    let layer_info = unsafe { &*(*link_info).u.p_layer_info };
    let Some(next_gipa) = layer_info.pfn_next_get_instance_proc_addr else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let Some(next_gdpa) = layer_info.pfn_next_get_device_proc_addr else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    unsafe {
        (*link_info).u.p_layer_info = layer_info.p_next;
    }

    let Some(fp_create) = (unsafe { next_gipa(vk::Instance::null(), c"vkCreateDevice".as_ptr()) })
    else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let create_fn: vk::PFN_vkCreateDevice = unsafe { std::mem::transmute(fp_create) };
    let res = unsafe { create_fn(physical_device, p_create_info, p_allocator, p_device) };
    if res != vk::Result::SUCCESS {
        return res;
    }

    let device_handle = unsafe { *p_device };
    let device = unsafe {
        ash::Device::load_with(
            |name| pfn_to_ptr(next_gdpa(device_handle, name.as_ptr())),
            device_handle,
        )
    };
    let swapchain_fn =
        ash::khr::swapchain::DeviceFn::load(|name| unsafe { pfn_to_ptr(next_gdpa(device_handle, name.as_ptr())) });

    let q_families =
        unsafe { id.instance.get_physical_device_queue_family_properties(physical_device) };
    let chosen_family = q_families
        .iter()
        .position(|q| q.queue_flags.contains(vk::QueueFlags::GRAPHICS))
        .unwrap_or(0) as u32;

    let graphics_queue = unsafe { device.get_device_queue(chosen_family, 0) };
    if graphics_queue != vk::Queue::null() {
        if let Some(set_loader) = set_loader_data {
            let _ = unsafe {
                set_loader(
                    device_handle,
                    graphics_queue.as_raw() as usize as *mut c_void,
                )
            };
        } else {
            unsafe { copy_dispatch_key_raw(graphics_queue.as_raw(), device_handle.as_raw()) };
        }
    }

    let mem_props = unsafe {
        id.instance
            .get_physical_device_memory_properties(physical_device)
    };
    let phys_props = unsafe {
        id.instance
            .get_physical_device_properties(physical_device)
    };
    if let Ok(rt) = LayerRuntime::global().lock() {
        rt.telemetry
            .notify_active_gpu(phys_props.vendor_id, phys_props.device_id);
    }

    let dd = Arc::new(DeviceData {
        handle: device_handle,
        physical_device,
        instance_data: id,
        next_gdpa,
        device,
        swapchain_fn,
        graphics_queue_family: chosen_family,
        graphics_queue,
        mem_props,
        set_loader_data,
        queue_mutex: Mutex::new(()),
    });

    if let Ok(mut reg) = registry().lock() {
        reg.devices
            .insert(dispatch_key_from_raw(device_handle.as_raw()), Arc::clone(&dd));
        if graphics_queue != vk::Queue::null() {
            reg.devices
                .insert(dispatch_key_from_raw(graphics_queue.as_raw()), dd);
        }
    }
    vk::Result::SUCCESS
}

pub(crate) unsafe extern "system" fn hook_destroy_device(
    device: vk::Device,
    p_allocator: *const vk::AllocationCallbacks<'_>,
) {
    if device == vk::Device::null() {
        return;
    }
    let key = dispatch_key_from_raw(device.as_raw());
    let (removed_dd, swapchains) = registry()
        .lock()
        .ok()
        .map_or((None, Vec::new()), |mut reg| {
            let dd = reg.devices.get(&key).cloned();
            let mut sc_list = Vec::new();
            if let Some(ref target) = dd {
                reg.devices.retain(|_, v| !Arc::ptr_eq(v, target));
                let sc_keys: Vec<u64> = reg
                    .swapchains
                    .iter()
                    .filter_map(|(k, v)| {
                        let same = v
                            .lock()
                            .ok()
                            .is_some_and(|s| Arc::ptr_eq(&s.device_data, target));
                        same.then_some(*k)
                    })
                    .collect();
                for sk in sc_keys {
                    if let Some(sc) = reg.swapchains.remove(&sk) {
                        sc_list.push(sc);
                    }
                }
            }
            (dd, sc_list)
        });

    for sc in swapchains {
        if let Ok(mut guard) = sc.lock() {
            guard.destroy_resources();
        }
    }
    if let Some(dd) = removed_dd {
        let cb = unsafe { p_allocator.as_ref() };
        unsafe { dd.device.destroy_device(cb) };
    }
}

pub(crate) unsafe extern "system" fn hook_get_device_queue(
    device: vk::Device,
    queue_family_index: u32,
    queue_index: u32,
    p_queue: *mut vk::Queue,
) {
    let Some(dd) = find_device_by_key(dispatch_key_from_raw(device.as_raw())) else {
        return;
    };
    unsafe { (dd.device.fp_v1_0().get_device_queue)(device, queue_family_index, queue_index, p_queue) };
    if !p_queue.is_null() && unsafe { *p_queue } != vk::Queue::null() {
        let q = unsafe { *p_queue };
        if let Some(set_loader) = dd.set_loader_data {
            let _ = unsafe { set_loader(device, q.as_raw() as usize as *mut c_void) };
        } else {
            unsafe { copy_dispatch_key_raw(q.as_raw(), device.as_raw()) };
        }
        if let Ok(mut reg) = registry().lock() {
            reg.devices.insert(dispatch_key_from_raw(q.as_raw()), dd);
        }
    }
}

pub(crate) unsafe extern "system" fn hook_get_device_queue2(
    device: vk::Device,
    p_queue_info: *const vk::DeviceQueueInfo2<'_>,
    p_queue: *mut vk::Queue,
) {
    let Some(dd) = find_device_by_key(dispatch_key_from_raw(device.as_raw())) else {
        return;
    };
    unsafe { (dd.device.fp_v1_1().get_device_queue2)(device, p_queue_info, p_queue) };
    if !p_queue.is_null() && unsafe { *p_queue } != vk::Queue::null() {
        let q = unsafe { *p_queue };
        if let Some(set_loader) = dd.set_loader_data {
            let _ = unsafe { set_loader(device, q.as_raw() as usize as *mut c_void) };
        } else {
            unsafe { copy_dispatch_key_raw(q.as_raw(), device.as_raw()) };
        }
        if let Ok(mut reg) = registry().lock() {
            reg.devices.insert(dispatch_key_from_raw(q.as_raw()), dd);
        }
    }
}

pub(crate) unsafe extern "system" fn hook_create_swapchain_khr(
    device: vk::Device,
    p_create_info: *const vk::SwapchainCreateInfoKHR<'_>,
    p_allocator: *const vk::AllocationCallbacks<'_>,
    p_swapchain: *mut vk::SwapchainKHR,
) -> vk::Result {
    if p_create_info.is_null() || p_swapchain.is_null() {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    }
    let Some(dd) = find_device_by_key(dispatch_key_from_raw(device.as_raw())) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };

    // Add TRANSFER_SRC_BIT opportunistically when supported so Clean/With-HUD screenshots work seamlessly
    let mut mod_info = unsafe { *p_create_info };
    mod_info.image_usage |= vk::ImageUsageFlags::TRANSFER_SRC;
    let res = unsafe {
        (dd.swapchain_fn.create_swapchain_khr)(device, &mod_info, p_allocator, p_swapchain)
    };
    let (res, supports_transfer_src) = if res == vk::Result::SUCCESS {
        (res, true)
    } else {
        let fallback_res = unsafe {
            (dd.swapchain_fn.create_swapchain_khr)(device, p_create_info, p_allocator, p_swapchain)
        };
        (
            fallback_res,
            mod_info
                .image_usage
                .contains(vk::ImageUsageFlags::TRANSFER_SRC)
                && unsafe { (*p_create_info).image_usage }
                    .contains(vk::ImageUsageFlags::TRANSFER_SRC),
        )
    };
    if res != vk::Result::SUCCESS {
        return res;
    }

    let swapchain = unsafe { *p_swapchain };
    let surf = unsafe { (*p_create_info).surface };
    if surf != vk::SurfaceKHR::null()
        && let Some((backend, win_id)) = registry()
            .lock()
            .ok()
            .and_then(|reg: std::sync::MutexGuard<'_, HookRegistry>| {
                reg.surfaces.get(&surf.as_raw()).copied()
            })
        && let Ok(mut rt) = LayerRuntime::global().lock()
    {
        rt.notify_surface_swapchain(backend, win_id as u32);
    }

    let mut sd = SwapchainData {
        swapchain,
        device_data: dd,
        format: unsafe { (*p_create_info).image_format },
        extent: unsafe { (*p_create_info).image_extent },
        supports_transfer_src,
        blit: None,
        frames: Vec::new(),
        initialized: false,
    };
    sd.init_resources();

    if let Ok(mut reg) = registry().lock() {
        reg.swapchains
            .insert(swapchain.as_raw(), Arc::new(Mutex::new(sd)));
    }
    vk::Result::SUCCESS
}

pub(crate) unsafe extern "system" fn hook_destroy_swapchain_khr(
    device: vk::Device,
    swapchain: vk::SwapchainKHR,
    p_allocator: *const vk::AllocationCallbacks<'_>,
) {
    if swapchain == vk::SwapchainKHR::null() {
        return;
    }
    let removed = registry()
        .lock()
        .ok()
        .and_then(|mut reg| reg.swapchains.remove(&swapchain.as_raw()));
    let dd = find_device_by_key(dispatch_key_from_raw(device.as_raw()));
    if let Some(sc_arc) = removed
        && let Ok(mut sd) = sc_arc.lock()
    {
        sd.destroy_resources();
    }
    if let Some(dd) = dd {
        unsafe { (dd.swapchain_fn.destroy_swapchain_khr)(device, swapchain, p_allocator) };
    }
}

pub(crate) unsafe extern "system" fn hook_queue_present_khr(
    queue: vk::Queue,
    p_present_info: *const vk::PresentInfoKHR<'_>,
) -> vk::Result {
    let Some(dd) = find_device_by_key(dispatch_key_from_raw(queue.as_raw())).or_else(|| {
        if p_present_info.is_null() {
            return None;
        }
        let pi = unsafe { &*p_present_info };
        if pi.swapchain_count == 0 || pi.p_swapchains.is_null() {
            return None;
        }
        let sc0 = unsafe { *pi.p_swapchains };
        let sd = find_swapchain(sc0)?;
        let dev = sd.lock().ok()?.device_data.clone();
        if let Ok(mut reg) = registry().lock() {
            reg.devices
                .insert(dispatch_key_from_raw(queue.as_raw()), Arc::clone(&dev));
        }
        Some(dev)
    }) else {
        return vk::Result::ERROR_DEVICE_LOST;
    };

    if p_present_info.is_null() || unsafe { (*p_present_info).swapchain_count } == 0 {
        return unsafe { (dd.swapchain_fn.queue_present_khr)(queue, p_present_info) };
    }

    let present_info = unsafe { &*p_present_info };
    let sc0 = unsafe { *present_info.p_swapchains };
    let img_idx = unsafe { *present_info.p_image_indices } as usize;

    let Some(sc_arc) = find_swapchain(sc0) else {
        return unsafe { (dd.swapchain_fn.queue_present_khr)(queue, p_present_info) };
    };
    let Ok(mut sd) = sc_arc.lock() else {
        return unsafe { (dd.swapchain_fn.queue_present_khr)(queue, p_present_info) };
    };

    let (enabled, hud_visible, gui_open, take_shot, overlay_frame) = {
        let Ok(mut rt) = LayerRuntime::global().lock() else {
            return unsafe { (dd.swapchain_fn.queue_present_khr)(queue, p_present_info) };
        };
        if !rt.config.enabled {
            return unsafe { (dd.swapchain_fn.queue_present_khr)(queue, p_present_info) };
        }
        rt.pump_frame(sd.extent.width, sd.extent.height, "VULKAN");
        let shot = if sd.supports_transfer_src {
            rt.consume_screenshot_request()
        } else {
            None
        };
        let buf = if rt.config.hud_visible || rt.config.gui_open {
            let l = &rt.overlay_sync.latest;
            (!l.rgba.is_empty()).then(|| l.clone())
        } else {
            None
        };
        (
            rt.config.enabled,
            rt.config.hud_visible,
            rt.config.gui_open,
            shot,
            buf,
        )
    };

    if !enabled || !sd.initialized || img_idx >= sd.frames.len() {
        return unsafe { (dd.swapchain_fn.queue_present_khr)(queue, p_present_info) };
    }

    let dev = &dd.device;
    let extent = sd.extent;
    let format = sd.format;
    let supports_transfer_src = sd.supports_transfer_src;
    let has_overlay = hud_visible || gui_open;

    // Wait bounded on previous slot fence (2s) before reusing command buffer/staging
    let fence = sd.frames[img_idx].fence;
    if unsafe { dev.wait_for_fences(&[fence], true, 2_000_000_000) }.is_err() {
        return unsafe { (dd.swapchain_fn.queue_present_khr)(queue, p_present_info) };
    }
    let _ = unsafe { dev.reset_fences(&[fence]) };

    let cmd = sd.frames[img_idx].command_buffer;
    let framebuffer = sd.frames[img_idx].framebuffer;
    let image = sd.frames[img_idx].image;
    let semaphore = sd.frames[img_idx].semaphore;

    unsafe {
        sd.frames[img_idx]
            .capture
            .flush_pending_screenshot(dev, extent.width, extent.height, format);
        let _ = dev.reset_command_buffer(cmd, vk::CommandBufferResetFlags::empty());
    }

    let begin =
        vk::CommandBufferBeginInfo::default().flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);
    if unsafe { dev.begin_command_buffer(cmd, &begin) }.is_err() {
        return unsafe { (dd.swapchain_fn.queue_present_khr)(queue, p_present_info) };
    }

    let mut clean_recorded = false;
    if let Some(false) = take_shot
        && supports_transfer_src
        && unsafe {
            sd.frames[img_idx]
                .capture
                .ensure_allocated(dev, &dd.mem_props, extent.width, extent.height, format)
        }
    {
        unsafe {
            sd.frames[img_idx].capture.record_copy_image_to_staging(
                dev,
                cmd,
                image,
                extent.width,
                extent.height,
            );
        }
        sd.frames[img_idx].capture.has_pending_screenshot = true;
        clean_recorded = true;
    }

    if has_overlay && let Some(ref mut blit) = sd.blit {
        if let Some(ref frame_buf) = overlay_frame {
            unsafe {
                blit.upload_if_changed(dev, &dd.mem_props, cmd, img_idx, frame_buf);
            }
        }
        unsafe {
            blit.record_draw(dev, cmd, framebuffer, extent, img_idx);
        }
    }

    if !clean_recorded
        && let Some(true) = take_shot
        && supports_transfer_src
        && unsafe {
            sd.frames[img_idx]
                .capture
                .ensure_allocated(dev, &dd.mem_props, extent.width, extent.height, format)
        }
    {
        unsafe {
            sd.frames[img_idx].capture.record_copy_image_to_staging(
                dev,
                cmd,
                image,
                extent.width,
                extent.height,
            );
        }
        sd.frames[img_idx].capture.has_pending_screenshot = true;
    }

    let _ = unsafe { dev.end_command_buffer(cmd) };

    let wait_sems = if present_info.wait_semaphore_count > 0 && !present_info.p_wait_semaphores.is_null() {
        unsafe {
            std::slice::from_raw_parts(
                present_info.p_wait_semaphores,
                present_info.wait_semaphore_count as usize,
            )
        }
    } else {
        &[]
    };
    let wait_stages = vec![vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT; wait_sems.len()];
    let cmds = [cmd];
    let sig_sems = [semaphore];
    let submit_info = vk::SubmitInfo::default()
        .wait_semaphores(wait_sems)
        .wait_dst_stage_mask(&wait_stages)
        .command_buffers(&cmds)
        .signal_semaphores(&sig_sems);

    let target_queue = if dd.graphics_queue != vk::Queue::null() {
        dd.graphics_queue
    } else {
        queue
    };

    let _qlock = dd.queue_mutex.lock();
    let sub_res = unsafe { dev.queue_submit(target_queue, &[submit_info], fence) };
    if sub_res == Ok(()) {
        let mut modified = *present_info;
        modified.wait_semaphore_count = 1;
        modified.p_wait_semaphores = sig_sems.as_ptr();
        unsafe { (dd.swapchain_fn.queue_present_khr)(queue, &modified) }
    } else {
        // Recover signaled fence cleanly on QueueSubmit failure to prevent next-frame stall
        let empty = vk::SubmitInfo::default();
        if unsafe { dev.queue_submit(target_queue, &[empty], fence) }.is_err() {
            unsafe {
                dev.destroy_fence(fence, None);
                if let Ok(new_fence) =
                    dev.create_fence(&vk::FenceCreateInfo::default().flags(vk::FenceCreateFlags::SIGNALED), None)
                {
                    sd.frames[img_idx].fence = new_fence;
                }
            }
        }
        unsafe { (dd.swapchain_fn.queue_present_khr)(queue, p_present_info) }
    }
}

pub(crate) unsafe fn lookup_device_proc(
    device: vk::Device,
    p_name: *const c_char,
) -> vk::PFN_vkVoidFunction {
    if p_name.is_null() {
        return None;
    }
    let name = unsafe { CStr::from_ptr(p_name) }.to_bytes();
    let ptr: *const c_void = match name {
        b"vkGetDeviceProcAddr" => (crate::vkGetDeviceProcAddr as *const ()).cast::<c_void>(),
        b"vkDestroyDevice" => (hook_destroy_device as *const ()).cast::<c_void>(),
        b"vkGetDeviceQueue" => (hook_get_device_queue as *const ()).cast::<c_void>(),
        b"vkGetDeviceQueue2" => (hook_get_device_queue2 as *const ()).cast::<c_void>(),
        b"vkCreateSwapchainKHR" => (hook_create_swapchain_khr as *const ()).cast::<c_void>(),
        b"vkDestroySwapchainKHR" => (hook_destroy_swapchain_khr as *const ()).cast::<c_void>(),
        b"vkQueuePresentKHR" => (hook_queue_present_khr as *const ()).cast::<c_void>(),
        _ => {
            if device == vk::Device::null() {
                return None;
            }
            let dd = find_device_by_key(dispatch_key_from_raw(device.as_raw()))?;
            return unsafe { (dd.next_gdpa)(device, p_name) };
        }
    };
    unsafe { ptr_to_pfn(ptr) }
}

pub(crate) unsafe fn lookup_instance_proc(
    instance: vk::Instance,
    p_name: *const c_char,
) -> vk::PFN_vkVoidFunction {
    if p_name.is_null() {
        return None;
    }
    let name = unsafe { CStr::from_ptr(p_name) }.to_bytes();
    let ptr: *const c_void = match name {
        b"vkGetInstanceProcAddr" => (crate::vkGetInstanceProcAddr as *const ()).cast::<c_void>(),
        b"vkCreateInstance" => (hook_create_instance as *const ()).cast::<c_void>(),
        b"vkDestroyInstance" => (hook_destroy_instance as *const ()).cast::<c_void>(),
        b"vkEnumeratePhysicalDevices" => {
            (hook_enumerate_physical_devices as *const ()).cast::<c_void>()
        }
        b"vkEnumeratePhysicalDeviceGroups" | b"vkEnumeratePhysicalDeviceGroupsKHR" => {
            (hook_enumerate_physical_device_groups as *const ()).cast::<c_void>()
        }
        b"vkCreateXlibSurfaceKHR" => (hook_create_xlib_surface_khr as *const ()).cast::<c_void>(),
        b"vkCreateXcbSurfaceKHR" => (hook_create_xcb_surface_khr as *const ()).cast::<c_void>(),
        b"vkCreateWaylandSurfaceKHR" => {
            (hook_create_wayland_surface_khr as *const ()).cast::<c_void>()
        }
        b"vkDestroySurfaceKHR" => (hook_destroy_surface_khr as *const ()).cast::<c_void>(),
        b"vkCreateDevice" => (hook_create_device as *const ()).cast::<c_void>(),
        b"vkGetDeviceProcAddr" => (crate::vkGetDeviceProcAddr as *const ()).cast::<c_void>(),
        b"vkDestroyDevice" => (hook_destroy_device as *const ()).cast::<c_void>(),
        b"vkGetDeviceQueue" => (hook_get_device_queue as *const ()).cast::<c_void>(),
        b"vkGetDeviceQueue2" => (hook_get_device_queue2 as *const ()).cast::<c_void>(),
        b"vkCreateSwapchainKHR" => (hook_create_swapchain_khr as *const ()).cast::<c_void>(),
        b"vkDestroySwapchainKHR" => (hook_destroy_swapchain_khr as *const ()).cast::<c_void>(),
        b"vkQueuePresentKHR" => (hook_queue_present_khr as *const ()).cast::<c_void>(),
        _ => {
            if instance == vk::Instance::null() {
                return None;
            }
            let id = find_instance_by_key(dispatch_key_from_raw(instance.as_raw()))?;
            return unsafe { (id.next_gipa)(instance, p_name) };
        }
    };
    unsafe { ptr_to_pfn(ptr) }
}
