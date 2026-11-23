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

use fpshud_recorder::capture::{ColorTransfer, RecorderPixelFormat};
use fpshud_recorder::pacer::{MAX_CAPTURE_RING_BYTES, adaptive_ring_slot_count};
use fpshud_recorder::session::{FramePayload, SubmittedFrame, ZeroCopySlotToken};
use std::sync::Arc;
use std::sync::atomic::{AtomicU8, Ordering};
use std::time::{Duration, Instant};

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecorderSlotState {
    Free = 0,
    InFlightGpu = 1,
    ReadingByWorker = 2,
}

/// Maps Vulkan swapchain `VkFormat` integers to `RecorderPixelFormat` (including FP16 HDR).
#[must_use]
pub const fn map_vk_format_to_recorder_format(raw_vk_format: i32) -> Option<RecorderPixelFormat> {
    match raw_vk_format {
        37 | 43 | 51 | 57 => Some(RecorderPixelFormat::Rgba8),
        44 | 50 => Some(RecorderPixelFormat::Bgra8),
        58 => Some(RecorderPixelFormat::A2R10G10B10),
        64 => Some(RecorderPixelFormat::A2B10G10R10),
        97 => Some(RecorderPixelFormat::R16G16B16A16Sfloat),
        _ => None,
    }
}

/// Maps `VkColorSpaceKHR` raw integer + format to `ColorTransfer`.
#[must_use]
pub const fn map_vk_color_transfer(
    raw_color_space: i32,
    fmt: RecorderPixelFormat,
) -> ColorTransfer {
    const VK_COLOR_SPACE_HDR10_ST2084_EXT: i32 = 1_000_104_008;
    const VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: i32 = 1_000_104_002;
    match (raw_color_space, fmt) {
        (VK_COLOR_SPACE_HDR10_ST2084_EXT, RecorderPixelFormat::A2B10G10R10 | RecorderPixelFormat::A2R10G10B10) => {
            ColorTransfer::PqHdr10
        }
        (VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, _) | (_, RecorderPixelFormat::R16G16B16A16Sfloat) => {
            ColorTransfer::LinearHdr
        }
        _ => ColorTransfer::Sdr,
    }
}

/// Synchronized barrier waiting until all worker-held `ReadingByWorker` slots return to `Free`.
#[must_use]
pub fn wait_for_slot_states_free(states: &[Arc<AtomicU8>], timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    loop {
        if states
            .iter()
            .all(|s| s.load(Ordering::Acquire) != RecorderSlotState::ReadingByWorker as u8)
        {
            return true;
        }
        if Instant::now() >= deadline {
            return false;
        }
        std::thread::sleep(Duration::from_millis(1));
    }
}

struct RecorderStagingSlot {
    buffer: vk::Buffer,
    memory: vk::DeviceMemory,
    mapped_ptr: *mut u8,
    byte_size: vk::DeviceSize,
    is_coherent: bool,
    state: Arc<AtomicU8>,
    frame_idx: usize,
    pts_ns: u64,
    cap_w: u32,
    cap_h: u32,
    src_w: u32,
    src_h: u32,
    pix_fmt: RecorderPixelFormat,
    transfer: ColorTransfer,
}

unsafe impl Send for RecorderStagingSlot {}
unsafe impl Sync for RecorderStagingSlot {}

#[derive(Default)]
pub struct RecorderCaptureRing {
    slots: Vec<RecorderStagingSlot>,
    cap_w: u32,
    cap_h: u32,
    pix_fmt: Option<RecorderPixelFormat>,
    blit_image: vk::Image,
    blit_memory: vk::DeviceMemory,
    blit_w: u32,
    blit_h: u32,
    pending_destroy: bool,
}

unsafe impl Send for RecorderCaptureRing {}
unsafe impl Sync for RecorderCaptureRing {}

impl RecorderCaptureRing {
    #[must_use]
    pub fn is_allocated(&self) -> bool {
        !self.slots.is_empty() || self.blit_image != vk::Image::null()
    }

    pub fn mark_pending_destroy(&mut self) {
        if self.is_allocated() {
            self.pending_destroy = true;
        }
    }

    /// Non-blocking cleanup when all worker slots have been released (0 MiB idle VRAM).
    pub(crate) unsafe fn try_cleanup_if_idle(&mut self, device: &ash::Device) {
        if !self.pending_destroy {
            return;
        }
        let all_free = self
            .slots
            .iter()
            .all(|s| s.state.load(Ordering::Acquire) == RecorderSlotState::Free as u8);
        if all_free {
            unsafe { self.destroy_resources(device) };
            self.pending_destroy = false;
        }
    }

    /// Synchronized drain barrier for `hook_destroy_swapchain_khr` (anti-UAF without stopping session).
    pub(crate) unsafe fn drain_in_flight_slots(&mut self, device: &ash::Device) {
        if !self.is_allocated() {
            return;
        }
        let states: Vec<Arc<AtomicU8>> = self.slots.iter().map(|s| Arc::clone(&s.state)).collect();
        let _ = wait_for_slot_states_free(&states, Duration::from_millis(500));
        unsafe { self.destroy_resources(device) };
        self.pending_destroy = false;
    }

    unsafe fn destroy_resources(&mut self, device: &ash::Device) {
        for slot in self.slots.drain(..) {
            if !slot.mapped_ptr.is_null() && slot.memory != vk::DeviceMemory::null() {
                unsafe { device.unmap_memory(slot.memory) };
            }
            if slot.buffer != vk::Buffer::null() {
                unsafe { device.destroy_buffer(slot.buffer, None) };
            }
            if slot.memory != vk::DeviceMemory::null() {
                unsafe { device.free_memory(slot.memory, None) };
            }
        }
        if self.blit_image != vk::Image::null() {
            unsafe { device.destroy_image(self.blit_image, None) };
            self.blit_image = vk::Image::null();
        }
        if self.blit_memory != vk::DeviceMemory::null() {
            unsafe { device.free_memory(self.blit_memory, None) };
            self.blit_memory = vk::DeviceMemory::null();
        }
        self.cap_w = 0;
        self.cap_h = 0;
        self.blit_w = 0;
        self.blit_h = 0;
        self.pix_fmt = None;
    }

    pub(crate) unsafe fn ensure_allocated(
        &mut self,
        device: &ash::Device,
        mem_props: &vk::PhysicalDeviceMemoryProperties,
        cap_w: u32,
        cap_h: u32,
        vk_format: vk::Format,
        pix_fmt: RecorderPixelFormat,
        use_gpu_blit: bool,
    ) -> bool {
        self.pending_destroy = false;
        if self.cap_w == cap_w
            && self.cap_h == cap_h
            && self.pix_fmt == Some(pix_fmt)
            && !self.slots.is_empty()
            && (!use_gpu_blit || self.blit_image != vk::Image::null())
        {
            return true;
        }

        unsafe { self.drain_in_flight_slots(device) };

        let frame_bytes = (cap_w as usize)
            .saturating_mul(cap_h as usize)
            .saturating_mul(pix_fmt.bytes_per_pixel());
        if frame_bytes == 0 {
            return false;
        }
        let slot_count = adaptive_ring_slot_count(frame_bytes, MAX_CAPTURE_RING_BYTES);

        for _ in 0..slot_count {
            let buf_info = vk::BufferCreateInfo::default()
                .size(frame_bytes as vk::DeviceSize)
                .usage(vk::BufferUsageFlags::TRANSFER_DST)
                .sharing_mode(vk::SharingMode::EXCLUSIVE);
            let Ok(buffer) = (unsafe { device.create_buffer(&buf_info, None) }) else {
                unsafe { self.destroy_resources(device) };
                return false;
            };
            let req = unsafe { device.get_buffer_memory_requirements(buffer) };
            let Some((mem_idx, is_coherent)) =
                pick_host_readback_memory_type(mem_props, req.memory_type_bits)
            else {
                unsafe {
                    device.destroy_buffer(buffer, None);
                    self.destroy_resources(device);
                }
                return false;
            };
            let alloc = vk::MemoryAllocateInfo::default()
                .allocation_size(req.size)
                .memory_type_index(mem_idx);
            let Ok(memory) = (unsafe { device.allocate_memory(&alloc, None) }) else {
                unsafe {
                    device.destroy_buffer(buffer, None);
                    self.destroy_resources(device);
                }
                return false;
            };
            if unsafe { device.bind_buffer_memory(buffer, memory, 0) }.is_err() {
                unsafe {
                    device.free_memory(memory, None);
                    device.destroy_buffer(buffer, None);
                    self.destroy_resources(device);
                }
                return false;
            }
            let Ok(ptr) = (unsafe { device.map_memory(memory, 0, req.size, vk::MemoryMapFlags::empty()) }) else {
                unsafe {
                    device.free_memory(memory, None);
                    device.destroy_buffer(buffer, None);
                    self.destroy_resources(device);
                }
                return false;
            };
            self.slots.push(RecorderStagingSlot {
                buffer,
                memory,
                mapped_ptr: ptr.cast::<u8>(),
                byte_size: req.size,
                is_coherent,
                state: Arc::new(AtomicU8::new(RecorderSlotState::Free as u8)),
                frame_idx: 0,
                pts_ns: 0,
                cap_w,
                cap_h,
                src_w: cap_w,
                src_h: cap_h,
                pix_fmt,
                transfer: ColorTransfer::Sdr,
            });
        }

        if use_gpu_blit {
            let img_info = vk::ImageCreateInfo::default()
                .image_type(vk::ImageType::TYPE_2D)
                .format(vk_format)
                .extent(vk::Extent3D {
                    width: cap_w,
                    height: cap_h,
                    depth: 1,
                })
                .mip_levels(1)
                .array_layers(1)
                .samples(vk::SampleCountFlags::TYPE_1)
                .tiling(vk::ImageTiling::OPTIMAL)
                .usage(vk::ImageUsageFlags::TRANSFER_DST | vk::ImageUsageFlags::TRANSFER_SRC)
                .initial_layout(vk::ImageLayout::UNDEFINED);
            if let Ok(img) = unsafe { device.create_image(&img_info, None) } {
                let req = unsafe { device.get_image_memory_requirements(img) };
                if let Some(mem_idx) = find_memory_type_index(
                    mem_props,
                    req.memory_type_bits,
                    vk::MemoryPropertyFlags::DEVICE_LOCAL,
                ) {
                    let alloc = vk::MemoryAllocateInfo::default()
                        .allocation_size(req.size)
                        .memory_type_index(mem_idx);
                    if let Ok(mem) = unsafe { device.allocate_memory(&alloc, None) } {
                        if unsafe { device.bind_image_memory(img, mem, 0) }.is_ok() {
                            self.blit_image = img;
                            self.blit_memory = mem;
                            self.blit_w = cap_w;
                            self.blit_h = cap_h;
                        } else {
                            unsafe {
                                device.free_memory(mem, None);
                                device.destroy_image(img, None);
                            }
                        }
                    } else {
                        unsafe { device.destroy_image(img, None) };
                    }
                } else {
                    unsafe { device.destroy_image(img, None) };
                }
            }
        }

        self.cap_w = cap_w;
        self.cap_h = cap_h;
        self.pix_fmt = Some(pix_fmt);
        true
    }

    /// Harvests completed GPU slots (zero-copy handoff to worker, ordered by `pts_ns`).
    pub(crate) unsafe fn harvest_completed_slots(
        &mut self,
        device: &ash::Device,
        completed_frame_idx: usize,
        frame_fences: &[vk::Fence],
        submit: &mut impl FnMut(SubmittedFrame),
    ) {
        let mut ready: Vec<(u64, usize)> = Vec::new();
        for (idx, slot) in self.slots.iter().enumerate() {
            if slot.state.load(Ordering::Acquire) != RecorderSlotState::InFlightGpu as u8 {
                continue;
            }
            let is_done = if slot.frame_idx == completed_frame_idx {
                true
            } else if let Some(&f) = frame_fences.get(slot.frame_idx) {
                f != vk::Fence::null()
                    && unsafe { device.get_fence_status(f) }.unwrap_or(false)
            } else {
                false
            };
            if is_done {
                ready.push((slot.pts_ns, idx));
            }
        }
        ready.sort_by_key(|&(pts, _)| pts);

        for (_, idx) in ready {
            let slot = &self.slots[idx];
            if !slot.is_coherent {
                let range = vk::MappedMemoryRange::default()
                    .memory(slot.memory)
                    .offset(0)
                    .size(vk::WHOLE_SIZE);
                let _ = unsafe { device.invalidate_mapped_memory_ranges(&[range]) };
            }
            let byte_len = (slot.cap_w as usize)
                .saturating_mul(slot.cap_h as usize)
                .saturating_mul(slot.pix_fmt.bytes_per_pixel());
            if byte_len == 0 || (byte_len as vk::DeviceSize) > slot.byte_size {
                slot.state
                    .store(RecorderSlotState::Free as u8, Ordering::Release);
                continue;
            }
            slot.state
                .store(RecorderSlotState::ReadingByWorker as u8, Ordering::Release);
            let token = ZeroCopySlotToken::new(Arc::clone(&slot.state));
            submit(SubmittedFrame {
                cap_w: slot.cap_w,
                cap_h: slot.cap_h,
                src_w: slot.src_w,
                src_h: slot.src_h,
                pix_fmt: slot.pix_fmt,
                transfer: slot.transfer,
                flip_y: false,
                payload: FramePayload::ZeroCopySlot {
                    ptr: slot.mapped_ptr,
                    len: byte_len,
                    _token: token,
                },
            });
        }
    }

    /// Records non-blocking GPU capture (optional `vkCmdBlitImage` downscale + `vkCmdCopyImageToBuffer`).
    #[allow(clippy::too_many_arguments)]
    pub(crate) unsafe fn record_capture(
        &mut self,
        device: &ash::Device,
        cmd: vk::CommandBuffer,
        swapchain_image: vk::Image,
        src_w: u32,
        src_h: u32,
        frame_idx: usize,
        pts_ns: u64,
        pix_fmt: RecorderPixelFormat,
        transfer: ColorTransfer,
    ) -> bool {
        let Some(slot) = self
            .slots
            .iter_mut()
            .find(|s| s.state.load(Ordering::Acquire) == RecorderSlotState::Free as u8)
        else {
            return false;
        };

        let subresource_range = vk::ImageSubresourceRange::default()
            .aspect_mask(vk::ImageAspectFlags::COLOR)
            .base_mip_level(0)
            .level_count(1)
            .base_array_layer(0)
            .layer_count(1);

        let sc_to_src = vk::ImageMemoryBarrier::default()
            .old_layout(vk::ImageLayout::PRESENT_SRC_KHR)
            .new_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
            .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .image(swapchain_image)
            .subresource_range(subresource_range)
            .src_access_mask(vk::AccessFlags::MEMORY_READ | vk::AccessFlags::COLOR_ATTACHMENT_WRITE)
            .dst_access_mask(vk::AccessFlags::TRANSFER_READ);

        unsafe {
            device.cmd_pipeline_barrier(
                cmd,
                vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT,
                vk::PipelineStageFlags::TRANSFER,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &[sc_to_src],
            );
        }

        let use_blit = self.blit_image != vk::Image::null()
            && self.blit_w == self.cap_w
            && self.blit_h == self.cap_h
            && (self.cap_w != src_w || self.cap_h != src_h);

        let copy_src_image = if use_blit {
            let blit_to_dst = vk::ImageMemoryBarrier::default()
                .old_layout(vk::ImageLayout::UNDEFINED)
                .new_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
                .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                .image(self.blit_image)
                .subresource_range(subresource_range)
                .src_access_mask(vk::AccessFlags::empty())
                .dst_access_mask(vk::AccessFlags::TRANSFER_WRITE);
            unsafe {
                device.cmd_pipeline_barrier(
                    cmd,
                    vk::PipelineStageFlags::TOP_OF_PIPE,
                    vk::PipelineStageFlags::TRANSFER,
                    vk::DependencyFlags::empty(),
                    &[],
                    &[],
                    &[blit_to_dst],
                );
            }
            let blit_region = vk::ImageBlit::default()
                .src_subresource(
                    vk::ImageSubresourceLayers::default()
                        .aspect_mask(vk::ImageAspectFlags::COLOR)
                        .mip_level(0)
                        .base_array_layer(0)
                        .layer_count(1),
                )
                .src_offsets([
                    vk::Offset3D { x: 0, y: 0, z: 0 },
                    vk::Offset3D {
                        x: src_w as i32,
                        y: src_h as i32,
                        z: 1,
                    },
                ])
                .dst_subresource(
                    vk::ImageSubresourceLayers::default()
                        .aspect_mask(vk::ImageAspectFlags::COLOR)
                        .mip_level(0)
                        .base_array_layer(0)
                        .layer_count(1),
                )
                .dst_offsets([
                    vk::Offset3D { x: 0, y: 0, z: 0 },
                    vk::Offset3D {
                        x: self.cap_w as i32,
                        y: self.cap_h as i32,
                        z: 1,
                    },
                ]);
            unsafe {
                device.cmd_blit_image(
                    cmd,
                    swapchain_image,
                    vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                    self.blit_image,
                    vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                    &[blit_region],
                    vk::Filter::LINEAR,
                );
            }
            let blit_to_src = vk::ImageMemoryBarrier::default()
                .old_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
                .new_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
                .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                .image(self.blit_image)
                .subresource_range(subresource_range)
                .src_access_mask(vk::AccessFlags::TRANSFER_WRITE)
                .dst_access_mask(vk::AccessFlags::TRANSFER_READ);
            unsafe {
                device.cmd_pipeline_barrier(
                    cmd,
                    vk::PipelineStageFlags::TRANSFER,
                    vk::PipelineStageFlags::TRANSFER,
                    vk::DependencyFlags::empty(),
                    &[],
                    &[],
                    &[blit_to_src],
                );
            }
            self.blit_image
        } else {
            swapchain_image
        };

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
                width: self.cap_w,
                height: self.cap_h,
                depth: 1,
            });
        unsafe {
            device.cmd_copy_image_to_buffer(
                cmd,
                copy_src_image,
                vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                slot.buffer,
                &[copy_region],
            );
        }

        let sc_to_present = vk::ImageMemoryBarrier::default()
            .old_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
            .new_layout(vk::ImageLayout::PRESENT_SRC_KHR)
            .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .image(swapchain_image)
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
                &[sc_to_present],
            );
        }

        slot.frame_idx = frame_idx;
        slot.pts_ns = pts_ns;
        slot.cap_w = self.cap_w;
        slot.cap_h = self.cap_h;
        slot.src_w = src_w;
        slot.src_h = src_h;
        slot.pix_fmt = pix_fmt;
        slot.transfer = transfer;
        slot.state
            .store(RecorderSlotState::InFlightGpu as u8, Ordering::Release);
        true
    }
}
