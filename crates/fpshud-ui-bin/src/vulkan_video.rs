//! Pure-Rust Vulkan GPU YUV (NV12 / YUV420p) -> BGRA8 DMA-BUF video renderer.
//!
//! Loads `libvulkan.so.1` dynamically via `ash::Entry::load()` (`dlopen`, zero `DT_NEEDED`),
//! negotiates single-plane `DRM_FORMAT_XRGB8888` / `DRM_FORMAT_ARGB8888` modifiers with the
//! Wayland compositor (`VK_EXT_image_drm_format_modifier` + `VK_KHR_external_memory_fd`),
//! and renders decoded video frames directly into 3 DMA-BUF ring images.

#![allow(dead_code)]

use anyhow::{Context, Result, anyhow};
use ash::vk;
use fpshud_recorder::player::PlayerFrame;
use fpshud_recorder::yuv::{PlayerPixelFormat, expected_frame_byte_len};
use std::ffi::{CStr, c_void};
use std::os::fd::{FromRawFd, OwnedFd};
use std::ptr;

pub const DRM_FORMAT_XRGB8888: u32 = 0x3432_5258; // 'XR24'
pub const DRM_FORMAT_ARGB8888: u32 = 0x3432_5241; // 'AR24'
pub const DRM_FORMAT_MOD_INVALID: u64 = 0x00ff_ffff_ffff_ffff;

pub const YUV_VERT_SPV: &[u8] = include_bytes!("../../../shaders/yuv_to_bgra.vert.spv");
pub const YUV_FRAG_SPV: &[u8] = include_bytes!("../../../shaders/yuv_to_bgra.frag.spv");

const SPIRV_MAGIC_LE: u32 = 0x0723_0203;
const DMABUF_RING_LEN: usize = 3;

#[derive(Debug, Clone, Default)]
pub struct DmabufFormatSet {
    pub main_device: Option<u64>,
    pub modifiers: Vec<(u32, u64)>,
}

pub struct DmabufExportInfo {
    pub fd: OwnedFd,
    pub fourcc: u32,
    pub modifier: u64,
    pub offset: u32,
    pub stride: u32,
    pub width: u32,
    pub height: u32,
}

#[must_use]
pub fn validate_yuv_spirv_headers() -> bool {
    let check = |bytes: &[u8]| -> bool {
        bytes.len() >= 20
            && bytes.len().is_multiple_of(4)
            && u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) == SPIRV_MAGIC_LE
    };
    check(YUV_VERT_SPV) && check(YUV_FRAG_SPV)
}

/// Picks the best `(drm_fourcc, intersecting_valid_modifiers)` pair between the Wayland
/// compositor's `zwp_linux_dmabuf_v1` list and Vulkan's supported `B8G8R8A8_UNORM`
/// `COLOR_ATTACHMENT` single-plane modifiers. Prefers opaque `XRGB8888` over `ARGB8888`
/// and excludes `DRM_FORMAT_MOD_INVALID`.
#[must_use]
pub fn select_best_dmabuf_format(
    compositor: &DmabufFormatSet,
    vk_supported_bgra_mods: &[u64],
) -> Option<(u32, Vec<u64>)> {
    for &fourcc in &[DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888] {
        let mut mods: Vec<u64> = compositor
            .modifiers
            .iter()
            .filter(|&&(f, m)| {
                f == fourcc && m != DRM_FORMAT_MOD_INVALID && vk_supported_bgra_mods.contains(&m)
            })
            .map(|&(_, m)| m)
            .collect();
        mods.sort_unstable();
        mods.dedup();
        if !mods.is_empty() {
            return Some((fourcc, mods));
        }
    }
    None
}

fn bytes_to_spirv_words(bytes: &[u8]) -> Vec<u32> {
    let (chunks, _) = bytes.as_chunks::<4>();
    chunks.iter().copied().map(u32::from_le_bytes).collect()
}

fn find_memory_type(
    mem_props: &vk::PhysicalDeviceMemoryProperties,
    type_bits: u32,
    required: vk::MemoryPropertyFlags,
) -> Option<u32> {
    (0..mem_props.memory_type_count).find(|&i| {
        (type_bits & (1 << i)) != 0
            && mem_props.memory_types[i as usize]
                .property_flags
                .contains(required)
    })
}

struct ImageAlloc {
    image: vk::Image,
    memory: vk::DeviceMemory,
    view: vk::ImageView,
    width: u32,
    height: u32,
    layout: vk::ImageLayout,
}

struct DmabufSlot {
    image: vk::Image,
    memory: vk::DeviceMemory,
    view: vk::ImageView,
    framebuffer: vk::Framebuffer,
    layout: vk::ImageLayout,
}

struct FrameResources {
    width: u32,
    height: u32,
    pix_fmt: PlayerPixelFormat,
    staging_buf: vk::Buffer,
    staging_mem: vk::DeviceMemory,
    staging_ptr: *mut u8,
    staging_size: usize,
    tex_y: ImageAlloc,
    tex_uv_or_u: ImageAlloc,
    tex_v: ImageAlloc,
    slots: Vec<DmabufSlot>,
}

pub struct VulkanVideoRenderer {
    _entry: ash::Entry,
    instance: ash::Instance,
    physical_device: vk::PhysicalDevice,
    device: ash::Device,
    ext_mem_fd: ash::khr::external_memory_fd::Device,
    ext_drm_mod: ash::ext::image_drm_format_modifier::Device,
    queue: vk::Queue,
    mem_props: vk::PhysicalDeviceMemoryProperties,
    chosen_fourcc: u32,
    chosen_modifiers: Vec<u64>,
    render_pass: vk::RenderPass,
    sampler: vk::Sampler,
    desc_set_layout: vk::DescriptorSetLayout,
    pipeline_layout: vk::PipelineLayout,
    pipeline: vk::Pipeline,
    desc_pool: vk::DescriptorPool,
    desc_set: vk::DescriptorSet,
    cmd_pool: vk::CommandPool,
    cmd_buf: vk::CommandBuffer,
    fence: vk::Fence,
    resources: Option<FrameResources>,
}

impl VulkanVideoRenderer {
    pub fn new(compositor: &DmabufFormatSet) -> Result<Self> {
        if !validate_yuv_spirv_headers() {
            return Err(anyhow!("Embedded YUV SPIR-V shaders have invalid magic header"));
        }
        let entry = unsafe { ash::Entry::load() }.context("dlopen libvulkan.so.1 failed")?;
        let app_info = vk::ApplicationInfo::default()
            .api_version(vk::make_api_version(0, 1, 1, 0));
        let inst_ci = vk::InstanceCreateInfo::default().application_info(&app_info);
        let instance = unsafe { entry.create_instance(&inst_ci, None) }
            .context("vkCreateInstance failed")?;

        let required_exts: [&CStr; 3] = [
            ash::khr::external_memory_fd::NAME,
            ash::ext::external_memory_dma_buf::NAME,
            ash::ext::image_drm_format_modifier::NAME,
        ];

        let pdevs = unsafe { instance.enumerate_physical_devices() }
            .context("vkEnumeratePhysicalDevices failed")?;

        let mut candidates: Vec<(i32, vk::PhysicalDevice, u32, u32, Vec<u64>)> = Vec::new();
        for pdev in pdevs {
            let props = unsafe { instance.get_physical_device_properties(pdev) };
            let rank = match props.device_type {
                vk::PhysicalDeviceType::DISCRETE_GPU => 3,
                vk::PhysicalDeviceType::INTEGRATED_GPU => 2,
                vk::PhysicalDeviceType::VIRTUAL_GPU => 1,
                _ => continue, // Skip CPU (llvmpipe) so software Vulkan falls back to Tier 2 SHM
            };

            let Ok(exts) = (unsafe { instance.enumerate_device_extension_properties(pdev) }) else {
                continue;
            };
            let has_all = required_exts.iter().all(|req| {
                exts.iter().any(|e| {
                    (unsafe { CStr::from_ptr(e.extension_name.as_ptr()) }) == *req
                })
            });
            if !has_all {
                continue;
            }

            let q_props = unsafe { instance.get_physical_device_queue_family_properties(pdev) };
            let Some(q_idx) = q_props
                .iter()
                .position(|q| q.queue_flags.contains(vk::QueueFlags::GRAPHICS))
                .map(|i| i as u32)
            else {
                continue;
            };

            let vk_mods = query_vk_bgra_color_attachment_modifiers(&instance, pdev);
            if let Some((fourcc, mods)) = select_best_dmabuf_format(compositor, &vk_mods) {
                candidates.push((rank, pdev, q_idx, fourcc, mods));
            }
        }

        candidates.sort_by_key(|a| std::cmp::Reverse(a.0));
        let Some((_, physical_device, queue_family_index, chosen_fourcc, chosen_modifiers)) =
            candidates.into_iter().next()
        else {
            unsafe { instance.destroy_instance(None) };
            return Err(anyhow!(
                "No hardware Vulkan GPU with intersecting DMA-BUF modifiers found"
            ));
        };

        let priorities = [1.0f32];
        let q_ci = [vk::DeviceQueueCreateInfo::default()
            .queue_family_index(queue_family_index)
            .queue_priorities(&priorities)];
        let ext_ptrs: Vec<*const i8> = required_exts.iter().map(|c| c.as_ptr()).collect();
        let dev_ci = vk::DeviceCreateInfo::default()
            .queue_create_infos(&q_ci)
            .enabled_extension_names(&ext_ptrs);
        let device = unsafe { instance.create_device(physical_device, &dev_ci, None) }
            .context("vkCreateDevice failed")?;
        let queue = unsafe { device.get_device_queue(queue_family_index, 0) };
        let mem_props = unsafe { instance.get_physical_device_memory_properties(physical_device) };
        let ext_mem_fd = ash::khr::external_memory_fd::Device::new(&instance, &device);
        let ext_drm_mod = ash::ext::image_drm_format_modifier::Device::new(&instance, &device);

        // RenderPass: 1 BGRA8 color attachment (DONT_CARE -> STORE)
        let att = [vk::AttachmentDescription::default()
            .format(vk::Format::B8G8R8A8_UNORM)
            .samples(vk::SampleCountFlags::TYPE_1)
            .load_op(vk::AttachmentLoadOp::DONT_CARE)
            .store_op(vk::AttachmentStoreOp::STORE)
            .stencil_load_op(vk::AttachmentLoadOp::DONT_CARE)
            .stencil_store_op(vk::AttachmentStoreOp::DONT_CARE)
            .initial_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
            .final_layout(vk::ImageLayout::GENERAL)];
        let color_ref = [vk::AttachmentReference::default()
            .attachment(0)
            .layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)];
        let subpass = [vk::SubpassDescription::default()
            .pipeline_bind_point(vk::PipelineBindPoint::GRAPHICS)
            .color_attachments(&color_ref)];
        let rp_ci = vk::RenderPassCreateInfo::default()
            .attachments(&att)
            .subpasses(&subpass);
        let render_pass = unsafe { device.create_render_pass(&rp_ci, None) }
            .context("vkCreateRenderPass failed")?;

        // Linear clamp-to-edge sampler for hardware chroma upsampling
        let sampler_ci = vk::SamplerCreateInfo::default()
            .mag_filter(vk::Filter::LINEAR)
            .min_filter(vk::Filter::LINEAR)
            .address_mode_u(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_v(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_w(vk::SamplerAddressMode::CLAMP_TO_EDGE);
        let sampler = unsafe { device.create_sampler(&sampler_ci, None) }
            .context("vkCreateSampler failed")?;

        // Descriptor set layout: 3 combined image samplers (Y, UV/U, V)
        let bindings = [
            vk::DescriptorSetLayoutBinding::default()
                .binding(0)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT),
            vk::DescriptorSetLayoutBinding::default()
                .binding(1)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT),
            vk::DescriptorSetLayoutBinding::default()
                .binding(2)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT),
        ];
        let dsl_ci = vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings);
        let desc_set_layout = unsafe { device.create_descriptor_set_layout(&dsl_ci, None) }
            .context("vkCreateDescriptorSetLayout failed")?;

        let push_range = [vk::PushConstantRange::default()
            .stage_flags(vk::ShaderStageFlags::FRAGMENT)
            .offset(0)
            .size(8)];
        let set_layouts = [desc_set_layout];
        let pl_ci = vk::PipelineLayoutCreateInfo::default()
            .set_layouts(&set_layouts)
            .push_constant_ranges(&push_range);
        let pipeline_layout = unsafe { device.create_pipeline_layout(&pl_ci, None) }
            .context("vkCreatePipelineLayout failed")?;

        // Graphics pipeline from embedded SPIR-V
        let vert_words = bytes_to_spirv_words(YUV_VERT_SPV);
        let frag_words = bytes_to_spirv_words(YUV_FRAG_SPV);
        let vert_mod = unsafe {
            device.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&vert_words), None)
        }
        .context("create vert shader module failed")?;
        let frag_mod = unsafe {
            device.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&frag_words), None)
        }
        .context("create frag shader module failed")?;

        let entry_name = c"main";
        let stages = [
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::VERTEX)
                .module(vert_mod)
                .name(entry_name),
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::FRAGMENT)
                .module(frag_mod)
                .name(entry_name),
        ];
        let vi = vk::PipelineVertexInputStateCreateInfo::default();
        let ia = vk::PipelineInputAssemblyStateCreateInfo::default()
            .topology(vk::PrimitiveTopology::TRIANGLE_LIST);
        let vp_state = vk::PipelineViewportStateCreateInfo::default()
            .viewport_count(1)
            .scissor_count(1);
        let rast = vk::PipelineRasterizationStateCreateInfo::default()
            .polygon_mode(vk::PolygonMode::FILL)
            .cull_mode(vk::CullModeFlags::NONE)
            .front_face(vk::FrontFace::CLOCKWISE)
            .line_width(1.0);
        let msaa = vk::PipelineMultisampleStateCreateInfo::default()
            .rasterization_samples(vk::SampleCountFlags::TYPE_1);
        let blend_att = [vk::PipelineColorBlendAttachmentState::default()
            .color_write_mask(vk::ColorComponentFlags::RGBA)];
        let blend = vk::PipelineColorBlendStateCreateInfo::default().attachments(&blend_att);
        let dyn_states = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
        let dyn_state = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dyn_states);

        let gp_ci = [vk::GraphicsPipelineCreateInfo::default()
            .stages(&stages)
            .vertex_input_state(&vi)
            .input_assembly_state(&ia)
            .viewport_state(&vp_state)
            .rasterization_state(&rast)
            .multisample_state(&msaa)
            .color_blend_state(&blend)
            .dynamic_state(&dyn_state)
            .layout(pipeline_layout)
            .render_pass(render_pass)
            .subpass(0)];
        let pipeline = unsafe { device.create_graphics_pipelines(vk::PipelineCache::null(), &gp_ci, None) }
            .map_err(|(_, e)| e)
            .context("vkCreateGraphicsPipelines failed")?[0];
        unsafe {
            device.destroy_shader_module(vert_mod, None);
            device.destroy_shader_module(frag_mod, None);
        }

        // Descriptor pool & set
        let pool_sizes = [vk::DescriptorPoolSize::default()
            .ty(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
            .descriptor_count(3)];
        let dp_ci = vk::DescriptorPoolCreateInfo::default()
            .max_sets(1)
            .pool_sizes(&pool_sizes);
        let desc_pool = unsafe { device.create_descriptor_pool(&dp_ci, None) }
            .context("vkCreateDescriptorPool failed")?;
        let ds_ai = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(desc_pool)
            .set_layouts(&set_layouts);
        let desc_set = unsafe { device.allocate_descriptor_sets(&ds_ai) }
            .context("vkAllocateDescriptorSets failed")?[0];

        // Command pool, command buffer, fence
        let cp_ci = vk::CommandPoolCreateInfo::default()
            .queue_family_index(queue_family_index)
            .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER);
        let cmd_pool = unsafe { device.create_command_pool(&cp_ci, None) }
            .context("vkCreateCommandPool failed")?;
        let cb_ai = vk::CommandBufferAllocateInfo::default()
            .command_pool(cmd_pool)
            .level(vk::CommandBufferLevel::PRIMARY)
            .command_buffer_count(1);
        let cmd_buf = unsafe { device.allocate_command_buffers(&cb_ai) }
            .context("vkAllocateCommandBuffers failed")?[0];
        let fence = unsafe {
            device.create_fence(
                &vk::FenceCreateInfo::default().flags(vk::FenceCreateFlags::SIGNALED),
                None,
            )
        }
        .context("vkCreateFence failed")?;

        Ok(Self {
            _entry: entry,
            instance,
            physical_device,
            device,
            ext_mem_fd,
            ext_drm_mod,
            queue,
            mem_props,
            chosen_fourcc,
            chosen_modifiers,
            render_pass,
            sampler,
            desc_set_layout,
            pipeline_layout,
            pipeline,
            desc_pool,
            desc_set,
            cmd_pool,
            cmd_buf,
            fence,
            resources: None,
        })
    }

    /// Ensures internal staging, YUV textures, and 3 DMA-BUF output slots match `(width, height, fmt)`.
    /// Returns `Ok(Some(exports))` when new DMA-BUF slots were allocated (caller must wrap them into `wl_buffer`s).
    pub fn ensure_dimensions(
        &mut self,
        width: u32,
        height: u32,
        pix_fmt: PlayerPixelFormat,
    ) -> Result<Option<Vec<DmabufExportInfo>>> {
        if let Some(ref res) = self.resources
            && res.width == width
            && res.height == height
            && res.pix_fmt == pix_fmt
        {
            return Ok(None);
        }

        unsafe {
            let _ = self.device.device_wait_idle();
        }
        self.destroy_frame_resources();

        let w = width.max(2) & !1;
        let h = height.max(2) & !1;
        let cw = (w / 2).max(1);
        let ch = (h / 2).max(1);

        let staging_size = expected_frame_byte_len(w, h, pix_fmt).max(64);
        let (staging_buf, staging_mem, staging_ptr) = self.create_staging_buffer(staging_size)?;

        let tex_y = self.create_input_texture(w, h, vk::Format::R8_UNORM)?;
        let (tex_uv_or_u, tex_v) = match pix_fmt {
            PlayerPixelFormat::Nv12 => (
                self.create_input_texture(cw, ch, vk::Format::R8G8_UNORM)?,
                self.create_input_texture(1, 1, vk::Format::R8_UNORM)?,
            ),
            PlayerPixelFormat::Yuv420p | PlayerPixelFormat::Rgba8 => (
                self.create_input_texture(cw, ch, vk::Format::R8_UNORM)?,
                self.create_input_texture(cw, ch, vk::Format::R8_UNORM)?,
            ),
        };

        // Update descriptor set
        let img_infos = [
            vk::DescriptorImageInfo::default()
                .sampler(self.sampler)
                .image_view(tex_y.view)
                .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL),
            vk::DescriptorImageInfo::default()
                .sampler(self.sampler)
                .image_view(tex_uv_or_u.view)
                .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL),
            vk::DescriptorImageInfo::default()
                .sampler(self.sampler)
                .image_view(tex_v.view)
                .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL),
        ];
        let writes = [
            vk::WriteDescriptorSet::default()
                .dst_set(self.desc_set)
                .dst_binding(0)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .image_info(&img_infos[0..1]),
            vk::WriteDescriptorSet::default()
                .dst_set(self.desc_set)
                .dst_binding(1)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .image_info(&img_infos[1..2]),
            vk::WriteDescriptorSet::default()
                .dst_set(self.desc_set)
                .dst_binding(2)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .image_info(&img_infos[2..3]),
        ];
        unsafe { self.device.update_descriptor_sets(&writes, &[]) };

        let mut slots = Vec::with_capacity(DMABUF_RING_LEN);
        let mut exports = Vec::with_capacity(DMABUF_RING_LEN);
        for _ in 0..DMABUF_RING_LEN {
            let (slot, info) = self.create_dmabuf_slot(w, h)?;
            slots.push(slot);
            exports.push(info);
        }

        self.resources = Some(FrameResources {
            width: w,
            height: h,
            pix_fmt,
            staging_buf,
            staging_mem,
            staging_ptr,
            staging_size,
            tex_y,
            tex_uv_or_u,
            tex_v,
            slots,
        });
        Ok(Some(exports))
    }

    pub fn render_to_slot(&mut self, slot_idx: usize, frame: &PlayerFrame) -> Result<()> {
        let Some(res) = self.resources.as_mut() else {
            return Err(anyhow!("VulkanVideoRenderer resources not initialized"));
        };
        if slot_idx >= res.slots.len() {
            return Err(anyhow!("Invalid DMA-BUF slot index {slot_idx}"));
        }
        let needed = expected_frame_byte_len(res.width, res.height, res.pix_fmt);
        if frame.data.len() < needed || needed > res.staging_size {
            return Err(anyhow!("Frame payload size mismatch"));
        }

        unsafe {
            self.device
                .wait_for_fences(&[self.fence], true, u64::MAX)
                .context("vkWaitForFences failed")?;
            self.device
                .reset_fences(&[self.fence])
                .context("vkResetFences failed")?;

            ptr::copy_nonoverlapping(frame.data.as_ptr(), res.staging_ptr, needed);

            self.device
                .reset_command_buffer(self.cmd_buf, vk::CommandBufferResetFlags::empty())
                .context("vkResetCommandBuffer failed")?;
            self.device
                .begin_command_buffer(
                    self.cmd_buf,
                    &vk::CommandBufferBeginInfo::default()
                        .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT),
                )
                .context("vkBeginCommandBuffer failed")?;
        }

        let w = res.width;
        let h = res.height;
        let cw = w / 2;
        let ch = h / 2;
        let y_bytes = (w * h) as u64;
        let uv_bytes = (cw * ch) as u64;

        // 1. Transition input textures to TRANSFER_DST_OPTIMAL
        let mut to_dst = vec![
            image_barrier(
                res.tex_y.image,
                res.tex_y.layout,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                vk::AccessFlags::SHADER_READ,
                vk::AccessFlags::TRANSFER_WRITE,
            ),
            image_barrier(
                res.tex_uv_or_u.image,
                res.tex_uv_or_u.layout,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                vk::AccessFlags::SHADER_READ,
                vk::AccessFlags::TRANSFER_WRITE,
            ),
        ];
        if res.pix_fmt == PlayerPixelFormat::Yuv420p {
            to_dst.push(image_barrier(
                res.tex_v.image,
                res.tex_v.layout,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                vk::AccessFlags::SHADER_READ,
                vk::AccessFlags::TRANSFER_WRITE,
            ));
        } else if res.tex_v.layout == vk::ImageLayout::UNDEFINED {
            to_dst.push(image_barrier(
                res.tex_v.image,
                vk::ImageLayout::UNDEFINED,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                vk::AccessFlags::empty(),
                vk::AccessFlags::SHADER_READ,
            ));
            res.tex_v.layout = vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL;
        }
        unsafe {
            self.device.cmd_pipeline_barrier(
                self.cmd_buf,
                vk::PipelineStageFlags::FRAGMENT_SHADER | vk::PipelineStageFlags::TOP_OF_PIPE,
                vk::PipelineStageFlags::TRANSFER | vk::PipelineStageFlags::FRAGMENT_SHADER,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &to_dst,
            );
        }

        // 2. Copy staging -> Y, UV/U, V textures
        let copy_y = [buffer_image_copy(0, w, h)];
        let copy_uv = [buffer_image_copy(y_bytes, cw, ch)];
        unsafe {
            self.device.cmd_copy_buffer_to_image(
                self.cmd_buf,
                res.staging_buf,
                res.tex_y.image,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                &copy_y,
            );
            self.device.cmd_copy_buffer_to_image(
                self.cmd_buf,
                res.staging_buf,
                res.tex_uv_or_u.image,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                &copy_uv,
            );
            if res.pix_fmt == PlayerPixelFormat::Yuv420p {
                let copy_v = [buffer_image_copy(y_bytes + uv_bytes, cw, ch)];
                self.device.cmd_copy_buffer_to_image(
                    self.cmd_buf,
                    res.staging_buf,
                    res.tex_v.image,
                    vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                    &copy_v,
                );
            }
        }

        // 3. Transition input textures -> SHADER_READ_ONLY_OPTIMAL and target DMA-BUF -> COLOR_ATTACHMENT_OPTIMAL
        let slot = &mut res.slots[slot_idx];
        let mut to_draw = vec![
            image_barrier(
                res.tex_y.image,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                vk::AccessFlags::TRANSFER_WRITE,
                vk::AccessFlags::SHADER_READ,
            ),
            image_barrier(
                res.tex_uv_or_u.image,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                vk::AccessFlags::TRANSFER_WRITE,
                vk::AccessFlags::SHADER_READ,
            ),
            image_barrier(
                slot.image,
                slot.layout,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                vk::AccessFlags::empty(),
                vk::AccessFlags::COLOR_ATTACHMENT_WRITE,
            ),
        ];
        res.tex_y.layout = vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL;
        res.tex_uv_or_u.layout = vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL;
        if res.pix_fmt == PlayerPixelFormat::Yuv420p {
            to_draw.push(image_barrier(
                res.tex_v.image,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                vk::AccessFlags::TRANSFER_WRITE,
                vk::AccessFlags::SHADER_READ,
            ));
            res.tex_v.layout = vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL;
        }
        unsafe {
            self.device.cmd_pipeline_barrier(
                self.cmd_buf,
                vk::PipelineStageFlags::TRANSFER | vk::PipelineStageFlags::TOP_OF_PIPE,
                vk::PipelineStageFlags::FRAGMENT_SHADER
                    | vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &to_draw,
            );
        }

        // 4. Render fullscreen triangle (NV12 / YUV420p -> BGRA8)
        let rp_begin = vk::RenderPassBeginInfo::default()
            .render_pass(self.render_pass)
            .framebuffer(slot.framebuffer)
            .render_area(vk::Rect2D {
                offset: vk::Offset2D { x: 0, y: 0 },
                extent: vk::Extent2D { width: w, height: h },
            });
        let vp = [vk::Viewport {
            x: 0.0,
            y: 0.0,
            width: w as f32,
            height: h as f32,
            min_depth: 0.0,
            max_depth: 1.0,
        }];
        let scissor = [vk::Rect2D {
            offset: vk::Offset2D { x: 0, y: 0 },
            extent: vk::Extent2D { width: w, height: h },
        }];
        let fmt_mode: u32 = match res.pix_fmt {
            PlayerPixelFormat::Nv12 => 0,
            _ => 1,
        };
        let push_data = [fmt_mode, frame.color_space.as_shader_id()];
        let push_bytes: [u8; 8] = [
            push_data[0].to_le_bytes()[0],
            push_data[0].to_le_bytes()[1],
            push_data[0].to_le_bytes()[2],
            push_data[0].to_le_bytes()[3],
            push_data[1].to_le_bytes()[0],
            push_data[1].to_le_bytes()[1],
            push_data[1].to_le_bytes()[2],
            push_data[1].to_le_bytes()[3],
        ];
        unsafe {
            self.device
                .cmd_begin_render_pass(self.cmd_buf, &rp_begin, vk::SubpassContents::INLINE);
            self.device
                .cmd_bind_pipeline(self.cmd_buf, vk::PipelineBindPoint::GRAPHICS, self.pipeline);
            self.device.cmd_set_viewport(self.cmd_buf, 0, &vp);
            self.device.cmd_set_scissor(self.cmd_buf, 0, &scissor);
            self.device.cmd_bind_descriptor_sets(
                self.cmd_buf,
                vk::PipelineBindPoint::GRAPHICS,
                self.pipeline_layout,
                0,
                &[self.desc_set],
                &[],
            );
            self.device.cmd_push_constants(
                self.cmd_buf,
                self.pipeline_layout,
                vk::ShaderStageFlags::FRAGMENT,
                0,
                &push_bytes,
            );
            self.device.cmd_draw(self.cmd_buf, 3, 1, 0, 0);
            self.device.cmd_end_render_pass(self.cmd_buf);
            slot.layout = vk::ImageLayout::GENERAL;
            self.device
                .end_command_buffer(self.cmd_buf)
                .context("vkEndCommandBuffer failed")?;

            let bufs = [self.cmd_buf];
            let submits = [vk::SubmitInfo::default().command_buffers(&bufs)];
            self.device
                .queue_submit(self.queue, &submits, self.fence)
                .context("vkQueueSubmit failed")?;
            self.device
                .wait_for_fences(&[self.fence], true, u64::MAX)
                .context("vkWaitForFences after submit failed")?;
        }

        Ok(())
    }

    fn create_staging_buffer(&self, size: usize) -> Result<(vk::Buffer, vk::DeviceMemory, *mut u8)> {
        let buf_ci = vk::BufferCreateInfo::default()
            .size(size as u64)
            .usage(vk::BufferUsageFlags::TRANSFER_SRC)
            .sharing_mode(vk::SharingMode::EXCLUSIVE);
        let buf = unsafe { self.device.create_buffer(&buf_ci, None) }
            .context("create staging buffer failed")?;
        let req = unsafe { self.device.get_buffer_memory_requirements(buf) };
        let mem_idx = find_memory_type(
            &self.mem_props,
            req.memory_type_bits,
            vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
        )
        .ok_or_else(|| anyhow!("No host-visible coherent memory type for staging"))?;
        let alloc_ci = vk::MemoryAllocateInfo::default()
            .allocation_size(req.size)
            .memory_type_index(mem_idx);
        let mem = unsafe { self.device.allocate_memory(&alloc_ci, None) }
            .context("allocate staging memory failed")?;
        unsafe { self.device.bind_buffer_memory(buf, mem, 0) }
            .context("bind staging buffer failed")?;
        let ptr = unsafe {
            self.device
                .map_memory(mem, 0, req.size, vk::MemoryMapFlags::empty())
        }
        .context("map staging memory failed")? as *mut u8;
        Ok((buf, mem, ptr))
    }

    fn create_input_texture(
        &self,
        width: u32,
        height: u32,
        format: vk::Format,
    ) -> Result<ImageAlloc> {
        let img_ci = vk::ImageCreateInfo::default()
            .image_type(vk::ImageType::TYPE_2D)
            .format(format)
            .extent(vk::Extent3D {
                width,
                height,
                depth: 1,
            })
            .mip_levels(1)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::OPTIMAL)
            .usage(vk::ImageUsageFlags::TRANSFER_DST | vk::ImageUsageFlags::SAMPLED)
            .sharing_mode(vk::SharingMode::EXCLUSIVE)
            .initial_layout(vk::ImageLayout::UNDEFINED);
        let image = unsafe { self.device.create_image(&img_ci, None) }
            .context("create YUV input image failed")?;
        let req = unsafe { self.device.get_image_memory_requirements(image) };
        let mem_idx = find_memory_type(
            &self.mem_props,
            req.memory_type_bits,
            vk::MemoryPropertyFlags::DEVICE_LOCAL,
        )
        .or_else(|| find_memory_type(&self.mem_props, req.memory_type_bits, vk::MemoryPropertyFlags::empty()))
        .ok_or_else(|| anyhow!("No memory type for YUV texture"))?;
        let alloc_ci = vk::MemoryAllocateInfo::default()
            .allocation_size(req.size)
            .memory_type_index(mem_idx);
        let memory = unsafe { self.device.allocate_memory(&alloc_ci, None) }
            .context("allocate YUV texture memory failed")?;
        unsafe { self.device.bind_image_memory(image, memory, 0) }
            .context("bind YUV texture memory failed")?;
        let view_ci = vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(format)
            .subresource_range(vk::ImageSubresourceRange {
                aspect_mask: vk::ImageAspectFlags::COLOR,
                base_mip_level: 0,
                level_count: 1,
                base_array_layer: 0,
                layer_count: 1,
            });
        let view = unsafe { self.device.create_image_view(&view_ci, None) }
            .context("create YUV image view failed")?;
        Ok(ImageAlloc {
            image,
            memory,
            view,
            width,
            height,
            layout: vk::ImageLayout::UNDEFINED,
        })
    }

    fn create_dmabuf_slot(&self, width: u32, height: u32) -> Result<(DmabufSlot, DmabufExportInfo)> {
        let mut ext_img = vk::ExternalMemoryImageCreateInfo::default()
            .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
        let mut mod_list = vk::ImageDrmFormatModifierListCreateInfoEXT::default()
            .drm_format_modifiers(&self.chosen_modifiers);
        let img_ci = vk::ImageCreateInfo::default()
            .push_next(&mut ext_img)
            .push_next(&mut mod_list)
            .image_type(vk::ImageType::TYPE_2D)
            .format(vk::Format::B8G8R8A8_UNORM)
            .extent(vk::Extent3D {
                width,
                height,
                depth: 1,
            })
            .mip_levels(1)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
            .usage(vk::ImageUsageFlags::COLOR_ATTACHMENT)
            .sharing_mode(vk::SharingMode::EXCLUSIVE)
            .initial_layout(vk::ImageLayout::UNDEFINED);
        let image = unsafe { self.device.create_image(&img_ci, None) }
            .context("create DMA-BUF VkImage failed")?;

        let req = unsafe { self.device.get_image_memory_requirements(image) };
        let mem_idx = find_memory_type(
            &self.mem_props,
            req.memory_type_bits,
            vk::MemoryPropertyFlags::DEVICE_LOCAL,
        )
        .or_else(|| find_memory_type(&self.mem_props, req.memory_type_bits, vk::MemoryPropertyFlags::empty()))
        .ok_or_else(|| anyhow!("No memory type for DMA-BUF image"))?;

        let mut export_ai = vk::ExportMemoryAllocateInfo::default()
            .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
        let mut dedicated_ai = vk::MemoryDedicatedAllocateInfo::default().image(image);
        let alloc_ci = vk::MemoryAllocateInfo::default()
            .push_next(&mut export_ai)
            .push_next(&mut dedicated_ai)
            .allocation_size(req.size)
            .memory_type_index(mem_idx);
        let memory = unsafe { self.device.allocate_memory(&alloc_ci, None) }
            .context("allocate DMA-BUF VkDeviceMemory failed")?;
        unsafe { self.device.bind_image_memory(image, memory, 0) }
            .context("bind DMA-BUF image memory failed")?;

        let mut mod_props = vk::ImageDrmFormatModifierPropertiesEXT::default();
        unsafe {
            self.ext_drm_mod
                .get_image_drm_format_modifier_properties(image, &mut mod_props)
        }
        .context("vkGetImageDrmFormatModifierPropertiesEXT failed")?;
        let layout = unsafe {
            self.device.get_image_subresource_layout(
                image,
                vk::ImageSubresource {
                    aspect_mask: vk::ImageAspectFlags::MEMORY_PLANE_0_EXT,
                    mip_level: 0,
                    array_layer: 0,
                },
            )
        };
        let raw_fd = unsafe {
            self.ext_mem_fd.get_memory_fd(
                &vk::MemoryGetFdInfoKHR::default()
                    .memory(memory)
                    .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT),
            )
        }
        .context("vkGetMemoryFdKHR failed")?;
        let fd = unsafe { OwnedFd::from_raw_fd(raw_fd) };

        let view_ci = vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(vk::Format::B8G8R8A8_UNORM)
            .subresource_range(vk::ImageSubresourceRange {
                aspect_mask: vk::ImageAspectFlags::COLOR,
                base_mip_level: 0,
                level_count: 1,
                base_array_layer: 0,
                layer_count: 1,
            });
        let view = unsafe { self.device.create_image_view(&view_ci, None) }
            .context("create DMA-BUF image view failed")?;
        let attachments = [view];
        let fb_ci = vk::FramebufferCreateInfo::default()
            .render_pass(self.render_pass)
            .attachments(&attachments)
            .width(width)
            .height(height)
            .layers(1);
        let framebuffer = unsafe { self.device.create_framebuffer(&fb_ci, None) }
            .context("create DMA-BUF framebuffer failed")?;

        Ok((
            DmabufSlot {
                image,
                memory,
                view,
                framebuffer,
                layout: vk::ImageLayout::UNDEFINED,
            },
            DmabufExportInfo {
                fd,
                fourcc: self.chosen_fourcc,
                modifier: mod_props.drm_format_modifier,
                offset: layout.offset as u32,
                stride: layout.row_pitch as u32,
                width,
                height,
            },
        ))
    }

    fn destroy_frame_resources(&mut self) {
        if let Some(res) = self.resources.take() {
            unsafe {
                for slot in res.slots {
                    self.device.destroy_framebuffer(slot.framebuffer, None);
                    self.device.destroy_image_view(slot.view, None);
                    self.device.destroy_image(slot.image, None);
                    self.device.free_memory(slot.memory, None);
                }
                for tex in [res.tex_y, res.tex_uv_or_u, res.tex_v] {
                    self.device.destroy_image_view(tex.view, None);
                    self.device.destroy_image(tex.image, None);
                    self.device.free_memory(tex.memory, None);
                }
                self.device.unmap_memory(res.staging_mem);
                self.device.destroy_buffer(res.staging_buf, None);
                self.device.free_memory(res.staging_mem, None);
            }
        }
    }
}

impl Drop for VulkanVideoRenderer {
    fn drop(&mut self) {
        unsafe {
            let _ = self.device.device_wait_idle();
        }
        self.destroy_frame_resources();
        unsafe {
            self.device.destroy_fence(self.fence, None);
            self.device.destroy_command_pool(self.cmd_pool, None);
            self.device.destroy_descriptor_pool(self.desc_pool, None);
            self.device.destroy_pipeline(self.pipeline, None);
            self.device.destroy_pipeline_layout(self.pipeline_layout, None);
            self.device
                .destroy_descriptor_set_layout(self.desc_set_layout, None);
            self.device.destroy_sampler(self.sampler, None);
            self.device.destroy_render_pass(self.render_pass, None);
            self.device.destroy_device(None);
            self.instance.destroy_instance(None);
        }
    }
}

fn query_vk_bgra_color_attachment_modifiers(
    instance: &ash::Instance,
    pdev: vk::PhysicalDevice,
) -> Vec<u64> {
    let mut list = vk::DrmFormatModifierPropertiesListEXT::default();
    let mut props2 = vk::FormatProperties2::default().push_next(&mut list);
    unsafe {
        instance.get_physical_device_format_properties2(
            pdev,
            vk::Format::B8G8R8A8_UNORM,
            &mut props2,
        );
    }
    let count = list.drm_format_modifier_count as usize;
    if count == 0 {
        return Vec::new();
    }
    let mut mods = vec![vk::DrmFormatModifierPropertiesEXT::default(); count];
    let mut list2 =
        vk::DrmFormatModifierPropertiesListEXT::default().drm_format_modifier_properties(&mut mods);
    let mut props2_b = vk::FormatProperties2::default().push_next(&mut list2);
    unsafe {
        instance.get_physical_device_format_properties2(
            pdev,
            vk::Format::B8G8R8A8_UNORM,
            &mut props2_b,
        );
    }
    mods.into_iter()
        .filter(|m| {
            m.drm_format_modifier_plane_count == 1
                && m.drm_format_modifier_tiling_features
                    .contains(vk::FormatFeatureFlags::COLOR_ATTACHMENT)
        })
        .map(|m| m.drm_format_modifier)
        .collect()
}

fn image_barrier(
    image: vk::Image,
    old_layout: vk::ImageLayout,
    new_layout: vk::ImageLayout,
    src_access: vk::AccessFlags,
    dst_access: vk::AccessFlags,
) -> vk::ImageMemoryBarrier<'static> {
    vk::ImageMemoryBarrier::default()
        .old_layout(old_layout)
        .new_layout(new_layout)
        .src_access_mask(src_access)
        .dst_access_mask(dst_access)
        .image(image)
        .subresource_range(vk::ImageSubresourceRange {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            base_mip_level: 0,
            level_count: 1,
            base_array_layer: 0,
            layer_count: 1,
        })
}

fn buffer_image_copy(buffer_offset: u64, width: u32, height: u32) -> vk::BufferImageCopy {
    vk::BufferImageCopy::default()
        .buffer_offset(buffer_offset)
        .image_subresource(vk::ImageSubresourceLayers {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            mip_level: 0,
            base_array_layer: 0,
            layer_count: 1,
        })
        .image_extent(vk::Extent3D {
            width,
            height,
            depth: 1,
        })
}

// Keep unused import lint quiet
const _: *const c_void = ptr::null();

#[cfg(test)]
mod tests {
    use super::*;
    use fpshud_recorder::yuv::PlayerColorSpace;
    use std::os::fd::AsRawFd;

    #[test]
    fn test_yuv_spirv_headers_valid() {
        assert!(validate_yuv_spirv_headers());
    }

    #[test]
    fn test_select_best_dmabuf_format_prefers_xrgb_and_filters_invalid() {
        let comp = DmabufFormatSet {
            main_device: None,
            modifiers: vec![
                (DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID),
                (DRM_FORMAT_XRGB8888, 0x0300_0000_0060_6010),
                (DRM_FORMAT_ARGB8888, 0x0),
            ],
        };
        let vk_mods = [0x0300_0000_0060_6010, 0x0];
        let picked = select_best_dmabuf_format(&comp, &vk_mods).expect("must pick XR24");
        assert_eq!(picked.0, DRM_FORMAT_XRGB8888);
        assert_eq!(picked.1, vec![0x0300_0000_0060_6010]);

        let invalid_only = DmabufFormatSet {
            main_device: None,
            modifiers: vec![(DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID)],
        };
        assert!(select_best_dmabuf_format(&invalid_only, &vk_mods).is_none());
        assert!(select_best_dmabuf_format(&comp, &[0x9999]).is_none());
    }

    #[test]
    fn test_vulkan_video_renderer_live_dmabuf_smoke() {
        // Query real GPU modifiers if a hardware Vulkan device is present on host
        let Ok(entry) = (unsafe { ash::Entry::load() }) else {
            return;
        };
        let app_info = vk::ApplicationInfo::default().api_version(vk::make_api_version(0, 1, 1, 0));
        let Ok(inst) = (unsafe {
            entry.create_instance(&vk::InstanceCreateInfo::default().application_info(&app_info), None)
        }) else {
            return;
        };
        let pdevs = unsafe { inst.enumerate_physical_devices() }.unwrap_or_default();
        let mut host_mods = Vec::new();
        for pdev in pdevs {
            let props = unsafe { inst.get_physical_device_properties(pdev) };
            if matches!(
                props.device_type,
                vk::PhysicalDeviceType::DISCRETE_GPU | vk::PhysicalDeviceType::INTEGRATED_GPU
            ) {
                host_mods = query_vk_bgra_color_attachment_modifiers(&inst, pdev);
                if !host_mods.is_empty() {
                    break;
                }
            }
        }
        unsafe { inst.destroy_instance(None) };
        if host_mods.is_empty() {
            return; // Headless CI without HW GPU DMA-BUF
        }

        let comp = DmabufFormatSet {
            main_device: None,
            modifiers: host_mods
                .iter()
                .map(|&m| (DRM_FORMAT_XRGB8888, m))
                .collect(),
        };
        let mut renderer = VulkanVideoRenderer::new(&comp).expect("init VulkanVideoRenderer");
        let exports = renderer
            .ensure_dimensions(320, 180, PlayerPixelFormat::Nv12)
            .expect("allocate 3 DMA-BUF slots")
            .expect("must return 3 exports");
        assert_eq!(exports.len(), 3);
        for ex in &exports {
            assert!(ex.fd.as_raw_fd() >= 0);
            assert!(ex.stride >= 320 * 4);
        }

        let frame = PlayerFrame {
            width: 320,
            height: 180,
            pixel_format: PlayerPixelFormat::Nv12,
            color_space: PlayerColorSpace::Bt709Limited,
            pts_sec: 0.0,
            data: vec![128u8; 320 * 180 * 3 / 2],
            generation: 1,
        };
        renderer.render_to_slot(0, &frame).expect("GPU NV12->BGRA render must succeed");
    }
}
