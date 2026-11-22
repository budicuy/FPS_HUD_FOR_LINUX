use crate::runtime::OverlayFrameBuffer;
use crate::vulkan::capture::find_memory_type_index;
use ash::vk;

pub const QUAD_VERT_SPV: &[u8] = include_bytes!("../../../../shaders/quad.vert.spv");
pub const QUAD_FRAG_SPV: &[u8] = include_bytes!("../../../../shaders/quad.frag.spv");

const SPIRV_MAGIC_LE: u32 = 0x0723_0203;

fn check_spirv_blob(bytes: &[u8]) -> bool {
    if bytes.len() < 4 || !bytes.len().is_multiple_of(4) {
        return false;
    }
    let word0 = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
    word0 == SPIRV_MAGIC_LE
}

/// Validates embedded SPIR-V vertex & fragment shader magic headers and alignment.
#[must_use]
pub fn verify_spirv_shaders_valid() -> bool {
    check_spirv_blob(QUAD_VERT_SPV) && check_spirv_blob(QUAD_FRAG_SPV)
}

fn bytes_to_u32_words(bytes: &[u8]) -> Option<Vec<u32>> {
    if bytes.is_empty() || !bytes.len().is_multiple_of(4) {
        return None;
    }
    Some(
        bytes
            .as_chunks::<4>()
            .0
            .iter()
            .map(|c| u32::from_le_bytes(*c))
            .collect(),
    )
}

#[must_use]
pub const fn is_srgb_swapchain_format(fmt: vk::Format) -> bool {
    matches!(fmt, vk::Format::B8G8R8A8_SRGB | vk::Format::R8G8B8A8_SRGB)
}

/// Pure per-swapchain-slot tracker ensuring concurrent GPU frames never alias one shared upload state.
#[derive(Debug, Clone)]
pub struct OverlaySlotSyncTracker {
    slots: Vec<(u32, u32, u64)>,
}

impl OverlaySlotSyncTracker {
    #[must_use]
    pub fn new(slot_count: usize) -> Self {
        Self {
            slots: vec![(0, 0, 0); slot_count.max(1)],
        }
    }

    #[must_use]
    pub fn needs_upload(&self, slot_idx: usize, width: u32, height: u32, generation: u64) -> bool {
        let Some(&(sw, sh, sgen)) = self.slots.get(slot_idx) else {
            return false;
        };
        sw != width || sh != height || sgen != generation
    }

    pub fn mark_uploaded(&mut self, slot_idx: usize, width: u32, height: u32, generation: u64) {
        if let Some(entry) = self.slots.get_mut(slot_idx) {
            *entry = (width, height, generation);
        }
    }

    pub fn invalidate(&mut self, slot_idx: usize) {
        if let Some(entry) = self.slots.get_mut(slot_idx) {
            *entry = (0, 0, 0);
        }
    }
}

struct OverlayGpuSlot {
    descriptor_set: vk::DescriptorSet,
    overlay_image: vk::Image,
    overlay_memory: vk::DeviceMemory,
    overlay_view: vk::ImageView,
    staging_buffer: vk::Buffer,
    staging_memory: vk::DeviceMemory,
    staging_mapped: *mut u8,
    staging_size: vk::DeviceSize,
    tex_width: u32,
    tex_height: u32,
    layout_initialized: bool,
}

impl OverlayGpuSlot {
    fn empty(descriptor_set: vk::DescriptorSet) -> Self {
        Self {
            descriptor_set,
            overlay_image: vk::Image::null(),
            overlay_memory: vk::DeviceMemory::null(),
            overlay_view: vk::ImageView::null(),
            staging_buffer: vk::Buffer::null(),
            staging_memory: vk::DeviceMemory::null(),
            staging_mapped: std::ptr::null_mut(),
            staging_size: 0,
            tex_width: 0,
            tex_height: 0,
            layout_initialized: false,
        }
    }

    unsafe fn destroy(&mut self, device: &ash::Device) {
        if !self.staging_mapped.is_null() && self.staging_memory != vk::DeviceMemory::null() {
            unsafe { device.unmap_memory(self.staging_memory) };
            self.staging_mapped = std::ptr::null_mut();
        }
        if self.staging_buffer != vk::Buffer::null() {
            unsafe { device.destroy_buffer(self.staging_buffer, None) };
            self.staging_buffer = vk::Buffer::null();
        }
        if self.staging_memory != vk::DeviceMemory::null() {
            unsafe { device.free_memory(self.staging_memory, None) };
            self.staging_memory = vk::DeviceMemory::null();
        }
        if self.overlay_view != vk::ImageView::null() {
            unsafe { device.destroy_image_view(self.overlay_view, None) };
            self.overlay_view = vk::ImageView::null();
        }
        if self.overlay_image != vk::Image::null() {
            unsafe { device.destroy_image(self.overlay_image, None) };
            self.overlay_image = vk::Image::null();
        }
        if self.overlay_memory != vk::DeviceMemory::null() {
            unsafe { device.free_memory(self.overlay_memory, None) };
            self.overlay_memory = vk::DeviceMemory::null();
        }
        self.staging_size = 0;
        self.tex_width = 0;
        self.tex_height = 0;
        self.layout_initialized = false;
    }

    unsafe fn ensure_texture(
        &mut self,
        device: &ash::Device,
        mem_props: &vk::PhysicalDeviceMemoryProperties,
        sampler: vk::Sampler,
        swapchain_format: vk::Format,
        width: u32,
        height: u32,
    ) -> bool {
        let w = width.max(1);
        let h = height.max(1);
        if self.overlay_image != vk::Image::null() && self.tex_width == w && self.tex_height == h {
            return true;
        }

        unsafe { self.destroy(device) };

        let tex_fmt = if is_srgb_swapchain_format(swapchain_format) {
            vk::Format::R8G8B8A8_SRGB
        } else {
            vk::Format::R8G8B8A8_UNORM
        };

        let img_info = vk::ImageCreateInfo::default()
            .image_type(vk::ImageType::TYPE_2D)
            .format(tex_fmt)
            .extent(vk::Extent3D {
                width: w,
                height: h,
                depth: 1,
            })
            .mip_levels(1)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::OPTIMAL)
            .usage(vk::ImageUsageFlags::TRANSFER_DST | vk::ImageUsageFlags::SAMPLED)
            .sharing_mode(vk::SharingMode::EXCLUSIVE)
            .initial_layout(vk::ImageLayout::UNDEFINED);

        let Ok(image) = (unsafe { device.create_image(&img_info, None) }) else {
            return false;
        };
        let req = unsafe { device.get_image_memory_requirements(image) };
        let Some(mem_idx) = find_memory_type_index(
            mem_props,
            req.memory_type_bits,
            vk::MemoryPropertyFlags::DEVICE_LOCAL,
        ) else {
            unsafe { device.destroy_image(image, None) };
            return false;
        };

        let Ok(memory) = (unsafe {
            device.allocate_memory(
                &vk::MemoryAllocateInfo::default()
                    .allocation_size(req.size)
                    .memory_type_index(mem_idx),
                None,
            )
        }) else {
            unsafe { device.destroy_image(image, None) };
            return false;
        };
        if unsafe { device.bind_image_memory(image, memory, 0) }.is_err() {
            unsafe {
                device.free_memory(memory, None);
                device.destroy_image(image, None);
            }
            return false;
        }

        let view_info = vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(tex_fmt)
            .subresource_range(
                vk::ImageSubresourceRange::default()
                    .aspect_mask(vk::ImageAspectFlags::COLOR)
                    .base_mip_level(0)
                    .level_count(1)
                    .base_array_layer(0)
                    .layer_count(1),
            );
        let Ok(view) = (unsafe { device.create_image_view(&view_info, None) }) else {
            unsafe {
                device.free_memory(memory, None);
                device.destroy_image(image, None);
            }
            return false;
        };

        let byte_size = vk::DeviceSize::from(w) * vk::DeviceSize::from(h) * 4;
        let Ok(buf) = (unsafe {
            device.create_buffer(
                &vk::BufferCreateInfo::default()
                    .size(byte_size)
                    .usage(vk::BufferUsageFlags::TRANSFER_SRC)
                    .sharing_mode(vk::SharingMode::EXCLUSIVE),
                None,
            )
        }) else {
            unsafe {
                device.destroy_image_view(view, None);
                device.free_memory(memory, None);
                device.destroy_image(image, None);
            }
            return false;
        };
        let buf_req = unsafe { device.get_buffer_memory_requirements(buf) };
        let Some(buf_mem_idx) = find_memory_type_index(
            mem_props,
            buf_req.memory_type_bits,
            vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
        ) else {
            unsafe {
                device.destroy_buffer(buf, None);
                device.destroy_image_view(view, None);
                device.free_memory(memory, None);
                device.destroy_image(image, None);
            }
            return false;
        };
        let Ok(buf_mem) = (unsafe {
            device.allocate_memory(
                &vk::MemoryAllocateInfo::default()
                    .allocation_size(buf_req.size)
                    .memory_type_index(buf_mem_idx),
                None,
            )
        }) else {
            unsafe {
                device.destroy_buffer(buf, None);
                device.destroy_image_view(view, None);
                device.free_memory(memory, None);
                device.destroy_image(image, None);
            }
            return false;
        };
        if unsafe { device.bind_buffer_memory(buf, buf_mem, 0) }.is_err() {
            unsafe {
                device.free_memory(buf_mem, None);
                device.destroy_buffer(buf, None);
                device.destroy_image_view(view, None);
                device.free_memory(memory, None);
                device.destroy_image(image, None);
            }
            return false;
        }
        let Ok(mapped) =
            (unsafe { device.map_memory(buf_mem, 0, vk::WHOLE_SIZE, vk::MemoryMapFlags::empty()) })
        else {
            unsafe {
                device.free_memory(buf_mem, None);
                device.destroy_buffer(buf, None);
                device.destroy_image_view(view, None);
                device.free_memory(memory, None);
                device.destroy_image(image, None);
            }
            return false;
        };

        let img_desc = vk::DescriptorImageInfo::default()
            .sampler(sampler)
            .image_view(view)
            .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL);
        let img_descs = [img_desc];
        let write = vk::WriteDescriptorSet::default()
            .dst_set(self.descriptor_set)
            .dst_binding(0)
            .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
            .image_info(&img_descs);
        unsafe { device.update_descriptor_sets(&[write], &[]) };

        self.overlay_image = image;
        self.overlay_memory = memory;
        self.overlay_view = view;
        self.staging_buffer = buf;
        self.staging_memory = buf_mem;
        self.staging_mapped = mapped.cast::<u8>();
        self.staging_size = byte_size;
        self.tex_width = w;
        self.tex_height = h;
        self.layout_initialized = false;
        true
    }
}

pub struct VulkanBlitPipeline {
    pub render_pass: vk::RenderPass,
    descriptor_set_layout: vk::DescriptorSetLayout,
    descriptor_pool: vk::DescriptorPool,
    sampler: vk::Sampler,
    pipeline_layout: vk::PipelineLayout,
    pipeline: vk::Pipeline,
    slots: Vec<OverlayGpuSlot>,
    tracker: OverlaySlotSyncTracker,
    swapchain_format: vk::Format,
}

unsafe impl Send for VulkanBlitPipeline {}
unsafe impl Sync for VulkanBlitPipeline {}

/// Computes a clamped Vulkan viewport and scissor for a positioned bounding-box HUD texture.
pub fn compute_vk_overlay_viewport(
    extent: vk::Extent2D,
    dst_x: i32,
    dst_y: i32,
    box_w: u32,
    box_h: u32,
) -> (vk::Viewport, vk::Rect2D) {
    let max_w = extent.width.max(1);
    let max_h = extent.height.max(1);
    let w = box_w.clamp(1, max_w);
    let h = box_h.clamp(1, max_h);
    let x = dst_x.clamp(0, (max_w.saturating_sub(w)) as i32);
    let y = dst_y.clamp(0, (max_h.saturating_sub(h)) as i32);

    let viewport = vk::Viewport::default()
        .x(x as f32)
        .y(y as f32)
        .width(w as f32)
        .height(h as f32)
        .min_depth(0.0)
        .max_depth(1.0);
    let scissor = vk::Rect2D {
        offset: vk::Offset2D { x, y },
        extent: vk::Extent2D {
            width: w,
            height: h,
        },
    };
    (viewport, scissor)
}

impl VulkanBlitPipeline {
    pub(crate) unsafe fn new(
        device: &ash::Device,
        mem_props: &vk::PhysicalDeviceMemoryProperties,
        swapchain_format: vk::Format,
        slot_count: usize,
    ) -> Result<Self, vk::Result> {
        let count = slot_count.max(1);
        let attachment = vk::AttachmentDescription::default()
            .format(swapchain_format)
            .samples(vk::SampleCountFlags::TYPE_1)
            .load_op(vk::AttachmentLoadOp::LOAD)
            .store_op(vk::AttachmentStoreOp::STORE)
            .stencil_load_op(vk::AttachmentLoadOp::DONT_CARE)
            .stencil_store_op(vk::AttachmentStoreOp::DONT_CARE)
            .initial_layout(vk::ImageLayout::PRESENT_SRC_KHR)
            .final_layout(vk::ImageLayout::PRESENT_SRC_KHR);

        let color_ref = vk::AttachmentReference::default()
            .attachment(0)
            .layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL);

        let color_refs = [color_ref];
        let subpass = vk::SubpassDescription::default()
            .pipeline_bind_point(vk::PipelineBindPoint::GRAPHICS)
            .color_attachments(&color_refs);

        let deps = [
            vk::SubpassDependency::default()
                .src_subpass(vk::SUBPASS_EXTERNAL)
                .dst_subpass(0)
                .src_stage_mask(
                    vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT
                        | vk::PipelineStageFlags::TRANSFER,
                )
                .dst_stage_mask(vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT)
                .src_access_mask(vk::AccessFlags::empty())
                .dst_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE),
            vk::SubpassDependency::default()
                .src_subpass(0)
                .dst_subpass(vk::SUBPASS_EXTERNAL)
                .src_stage_mask(vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT)
                .dst_stage_mask(vk::PipelineStageFlags::BOTTOM_OF_PIPE)
                .src_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE)
                .dst_access_mask(vk::AccessFlags::empty()),
        ];

        let attachments = [attachment];
        let subpasses = [subpass];
        let rp_info = vk::RenderPassCreateInfo::default()
            .attachments(&attachments)
            .subpasses(&subpasses)
            .dependencies(&deps);
        let render_pass = unsafe { device.create_render_pass(&rp_info, None)? };

        let sampler_info = vk::SamplerCreateInfo::default()
            .mag_filter(vk::Filter::LINEAR)
            .min_filter(vk::Filter::LINEAR)
            .mipmap_mode(vk::SamplerMipmapMode::NEAREST)
            .address_mode_u(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_v(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_w(vk::SamplerAddressMode::CLAMP_TO_EDGE);
        let sampler = match unsafe { device.create_sampler(&sampler_info, None) } {
            Ok(s) => s,
            Err(e) => {
                unsafe { device.destroy_render_pass(render_pass, None) };
                return Err(e);
            }
        };

        let binding = vk::DescriptorSetLayoutBinding::default()
            .binding(0)
            .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
            .descriptor_count(1)
            .stage_flags(vk::ShaderStageFlags::FRAGMENT);
        let bindings = [binding];
        let dsl_info = vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings);
        let descriptor_set_layout =
            match unsafe { device.create_descriptor_set_layout(&dsl_info, None) } {
                Ok(d) => d,
                Err(e) => {
                    unsafe {
                        device.destroy_sampler(sampler, None);
                        device.destroy_render_pass(render_pass, None);
                    }
                    return Err(e);
                }
            };

        let pool_size = vk::DescriptorPoolSize::default()
            .ty(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
            .descriptor_count(count as u32);
        let pool_sizes = [pool_size];
        let pool_info = vk::DescriptorPoolCreateInfo::default()
            .max_sets(count as u32)
            .pool_sizes(&pool_sizes);
        let descriptor_pool = match unsafe { device.create_descriptor_pool(&pool_info, None) } {
            Ok(p) => p,
            Err(e) => {
                unsafe {
                    device.destroy_descriptor_set_layout(descriptor_set_layout, None);
                    device.destroy_sampler(sampler, None);
                    device.destroy_render_pass(render_pass, None);
                }
                return Err(e);
            }
        };

        let cleanup_base = |dev: &ash::Device, pl: Option<vk::PipelineLayout>| unsafe {
            if let Some(layout) = pl {
                dev.destroy_pipeline_layout(layout, None);
            }
            dev.destroy_descriptor_pool(descriptor_pool, None);
            dev.destroy_descriptor_set_layout(descriptor_set_layout, None);
            dev.destroy_sampler(sampler, None);
            dev.destroy_render_pass(render_pass, None);
        };

        let set_layouts = vec![descriptor_set_layout; count];
        let alloc_ds = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(descriptor_pool)
            .set_layouts(&set_layouts);
        let descriptor_sets = match unsafe { device.allocate_descriptor_sets(&alloc_ds) } {
            Ok(ds) => ds,
            Err(e) => {
                cleanup_base(device, None);
                return Err(e);
            }
        };

        let pl_dsls = [descriptor_set_layout];
        let pl_info = vk::PipelineLayoutCreateInfo::default().set_layouts(&pl_dsls);
        let pipeline_layout = match unsafe { device.create_pipeline_layout(&pl_info, None) } {
            Ok(pl) => pl,
            Err(e) => {
                cleanup_base(device, None);
                return Err(e);
            }
        };

        let Some(vert_words) = bytes_to_u32_words(QUAD_VERT_SPV) else {
            cleanup_base(device, Some(pipeline_layout));
            return Err(vk::Result::ERROR_INVALID_SHADER_NV);
        };
        let Some(frag_words) = bytes_to_u32_words(QUAD_FRAG_SPV) else {
            cleanup_base(device, Some(pipeline_layout));
            return Err(vk::Result::ERROR_INVALID_SHADER_NV);
        };

        let vert_mod = match unsafe {
            device.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&vert_words), None)
        } {
            Ok(m) => m,
            Err(e) => {
                cleanup_base(device, Some(pipeline_layout));
                return Err(e);
            }
        };
        let frag_mod = match unsafe {
            device.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&frag_words), None)
        } {
            Ok(m) => m,
            Err(e) => {
                unsafe { device.destroy_shader_module(vert_mod, None) };
                cleanup_base(device, Some(pipeline_layout));
                return Err(e);
            }
        };

        let stages = [
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::VERTEX)
                .module(vert_mod)
                .name(c"main"),
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::FRAGMENT)
                .module(frag_mod)
                .name(c"main"),
        ];

        let vertex_input = vk::PipelineVertexInputStateCreateInfo::default();
        let input_assembly = vk::PipelineInputAssemblyStateCreateInfo::default()
            .topology(vk::PrimitiveTopology::TRIANGLE_LIST);
        let viewport_state = vk::PipelineViewportStateCreateInfo::default()
            .viewport_count(1)
            .scissor_count(1);
        let rasterizer = vk::PipelineRasterizationStateCreateInfo::default()
            .polygon_mode(vk::PolygonMode::FILL)
            .cull_mode(vk::CullModeFlags::NONE)
            .front_face(vk::FrontFace::COUNTER_CLOCKWISE)
            .line_width(1.0);
        let multisample = vk::PipelineMultisampleStateCreateInfo::default()
            .rasterization_samples(vk::SampleCountFlags::TYPE_1);

        let blend_attachment = vk::PipelineColorBlendAttachmentState::default()
            .blend_enable(true)
            .src_color_blend_factor(vk::BlendFactor::ONE)
            .dst_color_blend_factor(vk::BlendFactor::ONE_MINUS_SRC_ALPHA)
            .color_blend_op(vk::BlendOp::ADD)
            .src_alpha_blend_factor(vk::BlendFactor::ONE)
            .dst_alpha_blend_factor(vk::BlendFactor::ONE_MINUS_SRC_ALPHA)
            .alpha_blend_op(vk::BlendOp::ADD)
            .color_write_mask(vk::ColorComponentFlags::RGBA);
        let blend_attachments = [blend_attachment];
        let color_blend =
            vk::PipelineColorBlendStateCreateInfo::default().attachments(&blend_attachments);
        let dyn_states = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
        let dynamic_state =
            vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dyn_states);

        let gp_info = vk::GraphicsPipelineCreateInfo::default()
            .stages(&stages)
            .vertex_input_state(&vertex_input)
            .input_assembly_state(&input_assembly)
            .viewport_state(&viewport_state)
            .rasterization_state(&rasterizer)
            .multisample_state(&multisample)
            .color_blend_state(&color_blend)
            .dynamic_state(&dynamic_state)
            .layout(pipeline_layout)
            .render_pass(render_pass)
            .subpass(0);

        let pipelines =
            unsafe { device.create_graphics_pipelines(vk::PipelineCache::null(), &[gp_info], None) };
        unsafe {
            device.destroy_shader_module(vert_mod, None);
            device.destroy_shader_module(frag_mod, None);
        }
        let pipeline = match pipelines {
            Ok(list) => list[0],
            Err((_, err)) => {
                cleanup_base(device, Some(pipeline_layout));
                return Err(err);
            }
        };

        let mut slots: Vec<OverlayGpuSlot> =
            descriptor_sets.into_iter().map(OverlayGpuSlot::empty).collect();
        for slot in &mut slots {
            unsafe {
                slot.ensure_texture(device, mem_props, sampler, swapchain_format, 1, 1);
            }
        }

        Ok(Self {
            render_pass,
            descriptor_set_layout,
            descriptor_pool,
            sampler,
            pipeline_layout,
            pipeline,
            slots,
            tracker: OverlaySlotSyncTracker::new(count),
            swapchain_format,
        })
    }

    pub(crate) unsafe fn upload_if_changed(
        &mut self,
        device: &ash::Device,
        mem_props: &vk::PhysicalDeviceMemoryProperties,
        cmd: vk::CommandBuffer,
        slot_idx: usize,
        frame: &OverlayFrameBuffer,
    ) {
        let width = frame.width;
        let height = frame.height;
        let generation = frame.generation;
        let rgba = &frame.rgba;
        if width == 0 || height == 0 || rgba.is_empty() {
            return;
        }
        let Some(slot) = self.slots.get_mut(slot_idx) else {
            return;
        };
        if !self.tracker.needs_upload(slot_idx, width, height, generation) && slot.layout_initialized
        {
            return;
        }
        if !unsafe {
            slot.ensure_texture(device, mem_props, self.sampler, self.swapchain_format, width, height)
        } {
            self.tracker.invalidate(slot_idx);
            return;
        }
        let expected_bytes = (width as usize).saturating_mul(height as usize).saturating_mul(4);
        if rgba.len() < expected_bytes || slot.staging_size < (expected_bytes as vk::DeviceSize) {
            return;
        }

        unsafe {
            std::ptr::copy_nonoverlapping(rgba.as_ptr(), slot.staging_mapped, expected_bytes);
        }

        let subresource = vk::ImageSubresourceRange::default()
            .aspect_mask(vk::ImageAspectFlags::COLOR)
            .base_mip_level(0)
            .level_count(1)
            .base_array_layer(0)
            .layer_count(1);

        let old_layout = if slot.layout_initialized {
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL
        } else {
            vk::ImageLayout::UNDEFINED
        };
        let to_dst = vk::ImageMemoryBarrier::default()
            .old_layout(old_layout)
            .new_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
            .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .image(slot.overlay_image)
            .subresource_range(subresource)
            .src_access_mask(vk::AccessFlags::SHADER_READ)
            .dst_access_mask(vk::AccessFlags::TRANSFER_WRITE);

        unsafe {
            device.cmd_pipeline_barrier(
                cmd,
                vk::PipelineStageFlags::FRAGMENT_SHADER,
                vk::PipelineStageFlags::TRANSFER,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &[to_dst],
            );
        }

        let region = vk::BufferImageCopy::default()
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
            device.cmd_copy_buffer_to_image(
                cmd,
                slot.staging_buffer,
                slot.overlay_image,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                &[region],
            );
        }

        let to_shader = vk::ImageMemoryBarrier::default()
            .old_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
            .new_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
            .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
            .image(slot.overlay_image)
            .subresource_range(subresource)
            .src_access_mask(vk::AccessFlags::TRANSFER_WRITE)
            .dst_access_mask(vk::AccessFlags::SHADER_READ);
        unsafe {
            device.cmd_pipeline_barrier(
                cmd,
                vk::PipelineStageFlags::TRANSFER,
                vk::PipelineStageFlags::FRAGMENT_SHADER,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &[to_shader],
            );
        }

        slot.layout_initialized = true;
        self.tracker.mark_uploaded(slot_idx, width, height, generation);
    }

    pub(crate) unsafe fn record_draw(
        &self,
        device: &ash::Device,
        cmd: vk::CommandBuffer,
        framebuffer: vk::Framebuffer,
        extent: vk::Extent2D,
        rect: (i32, i32, u32, u32),
        slot_idx: usize,
    ) {
        let (dst_x, dst_y, box_w, box_h) = rect;
        let Some(slot) = self.slots.get(slot_idx) else {
            return;
        };
        if !slot.layout_initialized {
            return;
        }
        let rp_begin = vk::RenderPassBeginInfo::default()
            .render_pass(self.render_pass)
            .framebuffer(framebuffer)
            .render_area(vk::Rect2D {
                offset: vk::Offset2D { x: 0, y: 0 },
                extent,
            });

        let draw_w = if box_w > 0 { box_w } else { slot.tex_width };
        let draw_h = if box_h > 0 { box_h } else { slot.tex_height };
        let (viewport, scissor) = compute_vk_overlay_viewport(extent, dst_x, dst_y, draw_w, draw_h);

        unsafe {
            device.cmd_begin_render_pass(cmd, &rp_begin, vk::SubpassContents::INLINE);
            device.cmd_set_viewport(cmd, 0, &[viewport]);
            device.cmd_set_scissor(cmd, 0, &[scissor]);
            device.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, self.pipeline);
            device.cmd_bind_descriptor_sets(
                cmd,
                vk::PipelineBindPoint::GRAPHICS,
                self.pipeline_layout,
                0,
                &[slot.descriptor_set],
                &[],
            );
            device.cmd_draw(cmd, 3, 1, 0, 0);
            device.cmd_end_render_pass(cmd);
        }
    }

    pub(crate) unsafe fn destroy(&mut self, device: &ash::Device) {
        unsafe {
            for slot in &mut self.slots {
                slot.destroy(device);
            }
            self.slots.clear();
            device.destroy_pipeline(self.pipeline, None);
            device.destroy_pipeline_layout(self.pipeline_layout, None);
            device.destroy_descriptor_pool(self.descriptor_pool, None);
            device.destroy_descriptor_set_layout(self.descriptor_set_layout, None);
            device.destroy_sampler(self.sampler, None);
            device.destroy_render_pass(self.render_pass, None);
        }
    }
}
