use fpshud_ipc::{ShmRingWriter, ShmSlotMeta};
use gpui::PlatformAtlas;
use gpui_ce_wgpu::WgpuOffscreenRenderer;
use std::sync::Arc;

/// Transparent `wgpu` offscreen render target that publishes rendered RGBA frames to `ShmRingWriter`.
pub struct OffscreenSurfaceTarget {
    renderer: WgpuOffscreenRenderer,
    width: u32,
    height: u32,
    rgba_buf: Vec<u8>,
}

impl OffscreenSurfaceTarget {
    pub fn new(width: u32, height: u32) -> anyhow::Result<Self> {
        let w = width.max(1);
        let h = height.max(1);
        let mut renderer = WgpuOffscreenRenderer::new(w, h)?;
        renderer.resize(w, h)?;
        Ok(Self {
            renderer,
            width: w,
            height: h,
            rgba_buf: Vec::with_capacity((w as usize) * (h as usize) * 4),
        })
    }

    #[must_use]
    pub fn sprite_atlas(&self) -> Arc<dyn PlatformAtlas> {
        self.renderer.sprite_atlas()
    }

    pub fn resize(&mut self, width: u32, height: u32) -> anyhow::Result<()> {
        let w = width.max(1);
        let h = height.max(1);
        self.width = w;
        self.height = h;
        self.renderer.resize(w, h)
    }

    pub fn render_scene(&mut self, scene: &gpui::Scene) -> anyhow::Result<&[u8]> {
        self.renderer
            .render_scene_rgba(scene, self.width, self.height, &mut self.rgba_buf)?;
        Ok(&self.rgba_buf)
    }

    #[must_use]
    pub fn last_rgba(&self) -> &[u8] {
        &self.rgba_buf
    }

    pub fn write_to_shm(&self, writer: &mut ShmRingWriter) -> anyhow::Result<ShmSlotMeta> {
        if self.rgba_buf.is_empty() {
            let blank = vec![0_u8; (self.width as usize) * (self.height as usize) * 4];
            Ok(writer.write_next_slot(self.width, self.height, &blank)?)
        } else {
            Ok(writer.write_next_slot(self.width, self.height, &self.rgba_buf)?)
        }
    }
}
