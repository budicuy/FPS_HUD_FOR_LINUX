use std::sync::Arc;

use gpui::{DevicePixels, Scene, Size};

use crate::{WgpuAtlas, WgpuContext};

use super::{WgpuRenderer, WgpuSurfaceConfig};

struct OffscreenRgbaTarget {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    readback: wgpu::Buffer,
    padded_bytes_per_row: u32,
    size: Size<DevicePixels>,
}

impl OffscreenRgbaTarget {
    fn readback_copy(&self) -> super::frame::ReadbackCopy<'_> {
        super::frame::ReadbackCopy {
            texture: &self.texture,
            buffer: &self.readback,
            bytes_per_row: self.padded_bytes_per_row,
            width: self.size.width.0 as u32,
            height: self.size.height.0 as u32,
        }
    }
}

/// Transparent offscreen renderer producing tightly-packed RGBA8 images for IPC / DMA-BUF sharing.
pub struct WgpuOffscreenRenderer {
    renderer: WgpuRenderer,
    target: Option<OffscreenRgbaTarget>,
}

impl WgpuOffscreenRenderer {
    pub fn new(width: u32, height: u32) -> anyhow::Result<Self> {
        let context = WgpuContext::new_headless(None)?;
        let size = Size {
            width: DevicePixels(width.max(1) as i32),
            height: DevicePixels(height.max(1) as i32),
        };
        let renderer = WgpuRenderer::new_internal(
            None,
            &context,
            None,
            WgpuSurfaceConfig {
                size,
                transparent: true,
                preferred_present_mode: None,
            },
            None,
            None,
            Arc::new(WgpuAtlas::from_context(&context)),
        )?;
        Ok(Self {
            renderer,
            target: None,
        })
    }

    #[must_use]
    pub fn sprite_atlas(&self) -> Arc<WgpuAtlas> {
        self.renderer.sprite_atlas().clone()
    }

    #[must_use]
    pub fn max_texture_size(&self) -> u32 {
        self.renderer.max_texture_size()
    }

    #[must_use]
    pub fn backend(&self) -> wgpu::Backend {
        self.renderer.adapter_info.backend
    }

    fn create_target(&self) -> OffscreenRgbaTarget {
        let width = self.renderer.target.width().max(1);
        let height = self.renderer.target.height().max(1);
        let padded_bytes_per_row =
            (width * 4).next_multiple_of(wgpu::COPY_BYTES_PER_ROW_ALIGNMENT);
        let texture = self
            .renderer
            .resources()
            .device
            .create_texture(&wgpu::TextureDescriptor {
                label: Some("fpshud_gpui_offscreen_target"),
                size: wgpu::Extent3d {
                    width,
                    height,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: self.renderer.target.format(),
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        let readback = self
            .renderer
            .resources()
            .device
            .create_buffer(&wgpu::BufferDescriptor {
                label: Some("fpshud_gpui_offscreen_readback"),
                size: u64::from(padded_bytes_per_row) * u64::from(height),
                usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                mapped_at_creation: false,
            });
        OffscreenRgbaTarget {
            texture,
            view,
            readback,
            padded_bytes_per_row,
            size: self.renderer.viewport_size(),
        }
    }

    pub fn resize(&mut self, width: u32, height: u32) -> anyhow::Result<()> {
        let size = Size {
            width: DevicePixels(width.max(1) as i32),
            height: DevicePixels(height.max(1) as i32),
        };
        if self
            .target
            .as_ref()
            .is_some_and(|target| target.size == size)
        {
            return Ok(());
        }
        self.renderer.update_drawable_size(size);
        self.target = Some(self.create_target());
        Ok(())
    }

    pub fn render_scene_rgba(
        &mut self,
        scene: &Scene,
        width: u32,
        height: u32,
        out_rgba: &mut Vec<u8>,
    ) -> anyhow::Result<()> {
        self.resize(width, height)?;
        let target = self
            .target
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("offscreen render target missing"))?;
        let submission = super::frame::render_to_view(
            &mut self.renderer,
            scene,
            &target.view,
            Some(target.readback_copy()),
        )
        .ok_or_else(|| anyhow::anyhow!("failed to render GPUI offscreen scene"))?;

        let (sender, receiver) = std::sync::mpsc::sync_channel(1);
        target
            .readback
            .slice(..)
            .map_async(wgpu::MapMode::Read, move |res| {
                let _ = sender.send(res);
            });
        self.renderer
            .resources()
            .device
            .poll(wgpu::PollType::Wait {
                submission_index: Some(submission),
                timeout: None,
            })
            .map_err(|e| anyhow::anyhow!("offscreen wgpu poll error: {e}"))?;
        receiver
            .recv()
            .map_err(|e| anyhow::anyhow!("offscreen map channel dropped: {e}"))?
            .map_err(|e| anyhow::anyhow!("offscreen buffer map failed: {e}"))?;

        let w = target.size.width.0.max(1) as usize;
        let h = target.size.height.0.max(1) as usize;
        let bytes_per_row = w * 4;
        let total = bytes_per_row
            .checked_mul(h)
            .ok_or_else(|| anyhow::anyhow!("offscreen pixel buffer overflow"))?;
        out_rgba.clear();
        out_rgba.reserve(total);

        let mapped = target.readback.slice(..).get_mapped_range();
        for row in mapped
            .chunks_exact(target.padded_bytes_per_row as usize)
            .take(h)
        {
            out_rgba.extend_from_slice(&row[..bytes_per_row]);
        }
        drop(mapped);
        target.readback.unmap();

        if self.renderer.target.format() == wgpu::TextureFormat::Bgra8Unorm {
            for px in out_rgba.chunks_exact_mut(4) {
                px.swap(0, 2);
            }
        }
        Ok(())
    }
}
