use crate::runtime::LayerRuntime;
use ash::vk;
use fpshud_screenshot::PixelFormat;

/// Maps 32-bit swapchain `VkFormat` integers to screenshot `PixelFormat`.
#[must_use]
pub const fn map_vk_format_to_pixel_format(raw_vk_format: i32) -> Option<PixelFormat> {
    match raw_vk_format {
        37 | 43 | 51 | 57 => Some(PixelFormat::Rgba8), // R8G8B8A8 / A8B8G8R8_PACK32 UNORM / SRGB
        44 | 50 => Some(PixelFormat::Bgra8),           // B8G8R8A8 UNORM / SRGB
        58 => Some(PixelFormat::A2R10G10B10),          // A2R10G10B10_UNORM_PACK32 (10-bit Wayland)
        64 => Some(PixelFormat::A2B10G10R10),          // A2B10G10R10_UNORM_PACK32 (10-bit Wayland)
        _ => None,
    }
}

#[must_use]
pub fn find_memory_type_index(
    mem_props: &vk::PhysicalDeviceMemoryProperties,
    type_bits: u32,
    required: vk::MemoryPropertyFlags,
) -> Option<u32> {
    (0..mem_props.memory_type_count).find(|&i| {
        let supported = (type_bits & (1_u32 << i)) != 0;
        let props = mem_props.memory_types[i as usize].property_flags;
        supported && props.contains(required)
    })
}

#[must_use]
pub fn pick_host_readback_memory_type(
    mem_props: &vk::PhysicalDeviceMemoryProperties,
    type_bits: u32,
) -> Option<(u32, bool)> {
    let priorities = [
        (
            vk::MemoryPropertyFlags::HOST_VISIBLE
                | vk::MemoryPropertyFlags::HOST_COHERENT
                | vk::MemoryPropertyFlags::HOST_CACHED,
            true,
        ),
        (
            vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_CACHED,
            false,
        ),
        (
            vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
            true,
        ),
        (vk::MemoryPropertyFlags::HOST_VISIBLE, false),
    ];

    for (flags, coherent) in priorities {
        if let Some(idx) = find_memory_type_index(mem_props, type_bits, flags) {
            return Some((idx, coherent));
        }
    }
    None
}

pub struct FrameCaptureStaging {
    pub buffer: vk::Buffer,
    pub memory: vk::DeviceMemory,
    pub mapped_ptr: *mut u8,
    pub byte_size: vk::DeviceSize,
    pub is_coherent: bool,
    pub has_pending_screenshot: bool,
}

unsafe impl Send for FrameCaptureStaging {}
unsafe impl Sync for FrameCaptureStaging {}

impl Default for FrameCaptureStaging {
    fn default() -> Self {
        Self {
            buffer: vk::Buffer::null(),
            memory: vk::DeviceMemory::null(),
            mapped_ptr: std::ptr::null_mut(),
            byte_size: 0,
            is_coherent: false,
            has_pending_screenshot: false,
        }
    }
}

impl FrameCaptureStaging {
    pub(crate) unsafe fn ensure_allocated(
        &mut self,
        device: &ash::Device,
        mem_props: &vk::PhysicalDeviceMemoryProperties,
        width: u32,
        height: u32,
        format: vk::Format,
    ) -> bool {
        if width == 0 || height == 0 || map_vk_format_to_pixel_format(format.as_raw()).is_none() {
            return false;
        }
        let required = vk::DeviceSize::from(width) * vk::DeviceSize::from(height) * 4;
        if self.buffer != vk::Buffer::null() && self.byte_size >= required {
            return true;
        }

        unsafe { self.destroy(device) };

        let buf_info = vk::BufferCreateInfo::default()
            .size(required)
            .usage(vk::BufferUsageFlags::TRANSFER_DST)
            .sharing_mode(vk::SharingMode::EXCLUSIVE);
        let Ok(buffer) = (unsafe { device.create_buffer(&buf_info, None) }) else {
            return false;
        };

        let req = unsafe { device.get_buffer_memory_requirements(buffer) };
        let Some((mem_idx, is_coherent)) = pick_host_readback_memory_type(mem_props, req.memory_type_bits) else {
            unsafe { device.destroy_buffer(buffer, None) };
            return false;
        };

        let alloc_info = vk::MemoryAllocateInfo::default()
            .allocation_size(req.size)
            .memory_type_index(mem_idx);
        let Ok(memory) = (unsafe { device.allocate_memory(&alloc_info, None) }) else {
            unsafe { device.destroy_buffer(buffer, None) };
            return false;
        };

        if unsafe { device.bind_buffer_memory(buffer, memory, 0) }.is_err() {
            unsafe {
                device.free_memory(memory, None);
                device.destroy_buffer(buffer, None);
            }
            return false;
        }

        let Ok(ptr) =
            (unsafe { device.map_memory(memory, 0, vk::WHOLE_SIZE, vk::MemoryMapFlags::empty()) })
        else {
            unsafe {
                device.free_memory(memory, None);
                device.destroy_buffer(buffer, None);
            }
            return false;
        };

        self.buffer = buffer;
        self.memory = memory;
        self.mapped_ptr = ptr.cast::<u8>();
        self.byte_size = required;
        self.is_coherent = is_coherent;
        true
    }

    pub(crate) unsafe fn record_copy_image_to_staging(
        &self,
        device: &ash::Device,
        cmd: vk::CommandBuffer,
        image: vk::Image,
        width: u32,
        height: u32,
    ) {
        if self.buffer == vk::Buffer::null() {
            return;
        }
        let subresource_range = vk::ImageSubresourceRange::default()
            .aspect_mask(vk::ImageAspectFlags::COLOR)
            .base_mip_level(0)
            .level_count(1)
            .base_array_layer(0)
            .layer_count(1);

        let to_transfer = vk::ImageMemoryBarrier::default()
            .old_layout(vk::ImageLayout::PRESENT_SRC_KHR)
            .new_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
            .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .image(image)
            .subresource_range(subresource_range)
            .src_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE)
            .dst_access_mask(vk::AccessFlags::TRANSFER_READ);

        unsafe {
            device.cmd_pipeline_barrier(
                cmd,
                vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT,
                vk::PipelineStageFlags::TRANSFER,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &[to_transfer],
            );
        }

        let copy_region = vk::BufferImageCopy::default()
            .buffer_offset(0)
            .buffer_row_length(0)
            .buffer_image_height(0)
            .image_subresource(
                vk::ImageSubresourceLayers::default()
                    .aspect_mask(vk::ImageAspectFlags::COLOR)
                    .mip_level(0)
                    .base_array_layer(0)
                    .layer_count(1),
            )
            .image_offset(vk::Offset3D { x: 0, y: 0, z: 0 })
            .image_extent(vk::Extent3D {
                width,
                height,
                depth: 1,
            });

        unsafe {
            device.cmd_copy_image_to_buffer(
                cmd,
                image,
                vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                self.buffer,
                &[copy_region],
            );
        }

        let to_present = vk::ImageMemoryBarrier::default()
            .old_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
            .new_layout(vk::ImageLayout::PRESENT_SRC_KHR)
            .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .image(image)
            .subresource_range(subresource_range)
            .src_access_mask(vk::AccessFlags::TRANSFER_READ)
            .dst_access_mask(vk::AccessFlags::MEMORY_READ);

        unsafe {
            device.cmd_pipeline_barrier(
                cmd,
                vk::PipelineStageFlags::TRANSFER,
                vk::PipelineStageFlags::BOTTOM_OF_PIPE,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &[to_present],
            );
        }
    }

    pub(crate) unsafe fn flush_pending_screenshot(
        &mut self,
        device: &ash::Device,
        width: u32,
        height: u32,
        format: vk::Format,
    ) {
        if !self.has_pending_screenshot {
            return;
        }
        self.has_pending_screenshot = false;
        let Some(pixel_fmt) = map_vk_format_to_pixel_format(format.as_raw()) else {
            return;
        };
        if self.mapped_ptr.is_null() {
            return;
        }

        if !self.is_coherent {
            let range = vk::MappedMemoryRange::default()
                .memory(self.memory)
                .offset(0)
                .size(vk::WHOLE_SIZE);
            let _ = unsafe { device.invalidate_mapped_memory_ranges(&[range]) };
        }

        let len = (width as usize).saturating_mul(height as usize).saturating_mul(4);
        if len == 0 || (len as vk::DeviceSize) > self.byte_size {
            return;
        }
        let pixels = unsafe { std::slice::from_raw_parts(self.mapped_ptr, len) }.to_vec();
        if let Ok(rt) = LayerRuntime::global().lock() {
            rt.submit_captured_screenshot(width, height, pixel_fmt, pixels, false);
        }
    }

    pub(crate) unsafe fn destroy(&mut self, device: &ash::Device) {
        if !self.mapped_ptr.is_null() && self.memory != vk::DeviceMemory::null() {
            unsafe { device.unmap_memory(self.memory) };
            self.mapped_ptr = std::ptr::null_mut();
        }
        if self.buffer != vk::Buffer::null() {
            unsafe { device.destroy_buffer(self.buffer, None) };
            self.buffer = vk::Buffer::null();
        }
        if self.memory != vk::DeviceMemory::null() {
            unsafe { device.free_memory(self.memory, None) };
            self.memory = vk::DeviceMemory::null();
        }
        self.byte_size = 0;
        self.has_pending_screenshot = false;
    }
}
