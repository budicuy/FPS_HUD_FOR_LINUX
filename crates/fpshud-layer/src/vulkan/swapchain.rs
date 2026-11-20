use crate::vulkan::blit::VulkanBlitPipeline;
use crate::vulkan::capture::FrameCaptureStaging;
use ash::vk::{self, Handle};
use fpshud_input::SurfaceBackendType;
use std::collections::HashMap;
use std::sync::{Arc, Mutex, OnceLock};

#[must_use]
pub fn dispatch_key_from_raw(raw: u64) -> usize {
    if raw == 0 {
        return 0;
    }
    let ptr = raw as usize as *const usize;
    unsafe { *ptr }
}

pub(crate) unsafe fn copy_dispatch_key_raw(child_raw: u64, parent_raw: u64) {
    if child_raw != 0 && parent_raw != 0 {
        let dst = child_raw as usize as *mut usize;
        let src = parent_raw as usize as *const usize;
        unsafe {
            *dst = *src;
        }
    }
}

pub struct InstanceData {
    pub handle: vk::Instance,
    pub next_gipa: vk::PFN_vkGetInstanceProcAddr,
    pub instance: ash::Instance,
    pub create_xlib_surface_khr: Option<vk::PFN_vkCreateXlibSurfaceKHR>,
    pub create_xcb_surface_khr: Option<vk::PFN_vkCreateXcbSurfaceKHR>,
    pub create_wayland_surface_khr: Option<vk::PFN_vkCreateWaylandSurfaceKHR>,
    pub destroy_surface_khr: Option<vk::PFN_vkDestroySurfaceKHR>,
}

unsafe impl Send for InstanceData {}
unsafe impl Sync for InstanceData {}

pub type PfnVkSetDeviceLoaderData =
    unsafe extern "system" fn(device: vk::Device, object: *mut core::ffi::c_void) -> vk::Result;

pub struct DeviceData {
    pub handle: vk::Device,
    pub physical_device: vk::PhysicalDevice,
    pub instance_data: Arc<InstanceData>,
    pub next_gdpa: vk::PFN_vkGetDeviceProcAddr,
    pub device: ash::Device,
    pub swapchain_fn: ash::khr::swapchain::DeviceFn,
    pub graphics_queue_family: u32,
    pub graphics_queue: vk::Queue,
    pub mem_props: vk::PhysicalDeviceMemoryProperties,
    pub set_loader_data: Option<PfnVkSetDeviceLoaderData>,
    pub queue_mutex: Mutex<()>,
}

unsafe impl Send for DeviceData {}
unsafe impl Sync for DeviceData {}

pub struct FrameSlot {
    pub image: vk::Image,
    pub image_view: vk::ImageView,
    pub framebuffer: vk::Framebuffer,
    pub command_pool: vk::CommandPool,
    pub command_buffer: vk::CommandBuffer,
    pub fence: vk::Fence,
    pub semaphore: vk::Semaphore,
    pub capture: FrameCaptureStaging,
}

unsafe impl Send for FrameSlot {}
unsafe impl Sync for FrameSlot {}

pub struct SwapchainData {
    pub swapchain: vk::SwapchainKHR,
    pub device_data: Arc<DeviceData>,
    pub format: vk::Format,
    pub extent: vk::Extent2D,
    pub supports_transfer_src: bool,
    pub blit: Option<VulkanBlitPipeline>,
    pub frames: Vec<FrameSlot>,
    pub initialized: bool,
}

impl SwapchainData {
    pub fn init_resources(&mut self) {
        if self.extent.width == 0 || self.extent.height == 0 || self.device_data.graphics_queue == vk::Queue::null() {
            self.initialized = false;
            return;
        }
        let dev = &self.device_data.device;
        let swapchain_fn = &self.device_data.swapchain_fn;

        let mut img_count: u32 = 0;
        let res = unsafe {
            (swapchain_fn.get_swapchain_images_khr)(
                self.device_data.handle,
                self.swapchain,
                &mut img_count,
                std::ptr::null_mut(),
            )
        };
        if res != vk::Result::SUCCESS || img_count == 0 {
            return;
        }
        let mut images = vec![vk::Image::null(); img_count as usize];
        let res = unsafe {
            (swapchain_fn.get_swapchain_images_khr)(
                self.device_data.handle,
                self.swapchain,
                &mut img_count,
                images.as_mut_ptr(),
            )
        };
        if res != vk::Result::SUCCESS {
            return;
        }

        let Ok(blit) = (unsafe {
            VulkanBlitPipeline::new(dev, &self.device_data.mem_props, self.format)
        }) else {
            return;
        };

        let render_pass = blit.render_pass;
        let mut frames = Vec::with_capacity(images.len());

        for image in images {
            let view_info = vk::ImageViewCreateInfo::default()
                .image(image)
                .view_type(vk::ImageViewType::TYPE_2D)
                .format(self.format)
                .subresource_range(
                    vk::ImageSubresourceRange::default()
                        .aspect_mask(vk::ImageAspectFlags::COLOR)
                        .base_mip_level(0)
                        .level_count(1)
                        .base_array_layer(0)
                        .layer_count(1),
                );
            let Ok(image_view) = (unsafe { dev.create_image_view(&view_info, None) }) else {
                break;
            };

            let attachments = [image_view];
            let fb_info = vk::FramebufferCreateInfo::default()
                .render_pass(render_pass)
                .attachments(&attachments)
                .width(self.extent.width)
                .height(self.extent.height)
                .layers(1);
            let Ok(framebuffer) = (unsafe { dev.create_framebuffer(&fb_info, None) }) else {
                unsafe { dev.destroy_image_view(image_view, None) };
                break;
            };

            let pool_info = vk::CommandPoolCreateInfo::default()
                .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER)
                .queue_family_index(self.device_data.graphics_queue_family);
            let Ok(command_pool) = (unsafe { dev.create_command_pool(&pool_info, None) }) else {
                unsafe {
                    dev.destroy_framebuffer(framebuffer, None);
                    dev.destroy_image_view(image_view, None);
                }
                break;
            };

            let alloc_info = vk::CommandBufferAllocateInfo::default()
                .command_pool(command_pool)
                .level(vk::CommandBufferLevel::PRIMARY)
                .command_buffer_count(1);
            let Ok(cmds) = (unsafe { dev.allocate_command_buffers(&alloc_info) }) else {
                unsafe {
                    dev.destroy_command_pool(command_pool, None);
                    dev.destroy_framebuffer(framebuffer, None);
                    dev.destroy_image_view(image_view, None);
                }
                break;
            };
            let command_buffer = cmds[0];
            if let Some(set_loader) = self.device_data.set_loader_data {
                let _ = unsafe {
                    set_loader(
                        self.device_data.handle,
                        command_buffer.as_raw() as usize as *mut core::ffi::c_void,
                    )
                };
            } else {
                unsafe {
                    copy_dispatch_key_raw(command_buffer.as_raw(), self.device_data.handle.as_raw());
                }
            }

            let fence_info = vk::FenceCreateInfo::default().flags(vk::FenceCreateFlags::SIGNALED);
            let Ok(fence) = (unsafe { dev.create_fence(&fence_info, None) }) else {
                unsafe {
                    dev.destroy_command_pool(command_pool, None);
                    dev.destroy_framebuffer(framebuffer, None);
                    dev.destroy_image_view(image_view, None);
                }
                break;
            };
            let sem_info = vk::SemaphoreCreateInfo::default();
            let Ok(semaphore) = (unsafe { dev.create_semaphore(&sem_info, None) }) else {
                unsafe {
                    dev.destroy_fence(fence, None);
                    dev.destroy_command_pool(command_pool, None);
                    dev.destroy_framebuffer(framebuffer, None);
                    dev.destroy_image_view(image_view, None);
                }
                break;
            };

            frames.push(FrameSlot {
                image,
                image_view,
                framebuffer,
                command_pool,
                command_buffer,
                fence,
                semaphore,
                capture: FrameCaptureStaging::default(),
            });
        }

        if frames.len() != (img_count as usize) {
            self.blit = Some(blit);
            self.frames = frames;
            self.destroy_resources();
            return;
        }

        self.blit = Some(blit);
        self.frames = frames;
        self.initialized = true;
    }

    pub fn destroy_resources(&mut self) {
        let dev = &self.device_data.device;
        let _ = unsafe { dev.device_wait_idle() };

        for slot in &mut self.frames {
            unsafe {
                slot.capture.destroy(dev);
                dev.destroy_semaphore(slot.semaphore, None);
                dev.destroy_fence(slot.fence, None);
                dev.destroy_command_pool(slot.command_pool, None);
                dev.destroy_framebuffer(slot.framebuffer, None);
                dev.destroy_image_view(slot.image_view, None);
            }
        }
        self.frames.clear();
        if let Some(mut blit) = self.blit.take() {
            unsafe { blit.destroy(dev) };
        }
        self.initialized = false;
    }
}

#[derive(Default)]
pub struct HookRegistry {
    pub instances: HashMap<usize, Arc<InstanceData>>,
    pub devices: HashMap<usize, Arc<DeviceData>>,
    pub swapchains: HashMap<u64, Arc<Mutex<SwapchainData>>>,
    pub surfaces: HashMap<u64, (SurfaceBackendType, u32)>,
}

static REGISTRY: OnceLock<Mutex<HookRegistry>> = OnceLock::new();

pub fn registry() -> &'static Mutex<HookRegistry> {
    REGISTRY.get_or_init(|| Mutex::new(HookRegistry::default()))
}

#[must_use]
pub fn find_instance_by_key(key: usize) -> Option<Arc<InstanceData>> {
    registry().lock().ok()?.instances.get(&key).cloned()
}

#[must_use]
pub fn find_device_by_key(key: usize) -> Option<Arc<DeviceData>> {
    registry().lock().ok()?.devices.get(&key).cloned()
}

#[must_use]
pub fn find_swapchain(sc: vk::SwapchainKHR) -> Option<Arc<Mutex<SwapchainData>>> {
    registry()
        .lock()
        .ok()?
        .swapchains
        .get(&sc.as_raw())
        .cloned()
}
