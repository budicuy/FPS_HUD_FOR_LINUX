//! Shared 100% safe-Rust CPU 2D rasterizer & `swash` glyph cache (`0` GPU / `0` `wgpu` / `0` VRAM).
//!
//! Used by both `CpuHudRenderer` (compact bounding-box HUD) and `CpuDashboard` (3-column Settings UI).

use crate::state::rgba_u32;
use fpshud_telemetry::TelemetrySnapshot;
use std::collections::HashMap;
use swash::FontRef;
use swash::scale::{Render, ScaleContext, Source};
use swash::zeno::Format;

pub const OPEN_SANS_BOLD_TTF: &[u8] = include_bytes!("../../../assets/fonts/OpenSans-Bold.ttf");
pub const VERA_MONO_BOLD_TTF: &[u8] = include_bytes!("../../../assets/fonts/VeraMono-Bold.ttf");

/// Validates that embedded TTF font blobs carry recognized SFNT / OpenType headers.
#[must_use]
pub fn validate_embedded_font_headers() -> bool {
    let valid_sfnt = |bytes: &[u8]| -> bool {
        if bytes.len() < 12 {
            return false;
        }
        matches!(
            &bytes[0..4],
            [0x00, 0x01, 0x00, 0x00] | b"OTTO" | b"true" | b"ttcf"
        )
    };
    valid_sfnt(OPEN_SANS_BOLD_TTF) && valid_sfnt(VERA_MONO_BOLD_TTF)
}

/// Axis-aligned rectangle in logical UI coordinates.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

impl Rect {
    #[must_use]
    pub const fn new(x: f32, y: f32, w: f32, h: f32) -> Self {
        Self { x, y, w, h }
    }

    #[must_use]
    pub fn right(self) -> f32 {
        self.x + self.w
    }

    #[must_use]
    pub fn bottom(self) -> f32 {
        self.y + self.h
    }

    #[must_use]
    pub fn contains(self, px: f32, py: f32) -> bool {
        self.w > 0.0
            && self.h > 0.0
            && px >= self.x
            && px <= self.x + self.w
            && py >= self.y
            && py <= self.y + self.h
    }

    #[must_use]
    pub fn intersect(self, other: Self) -> Option<Self> {
        let x0 = self.x.max(other.x);
        let y0 = self.y.max(other.y);
        let x1 = self.right().min(other.right());
        let y1 = self.bottom().min(other.bottom());
        if x1 > x0 && y1 > y0 {
            Some(Self::new(x0, y0, x1 - x0, y1 - y0))
        } else {
            None
        }
    }
}

#[derive(Clone)]
pub(crate) struct CachedGlyph {
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) left: i32,
    pub(crate) top: i32,
    pub(crate) advance: f32,
    pub(crate) alpha: Vec<u8>,
}

/// Shared stateful `swash` glyph rasterizer & bitmap cache.
pub struct TextRasterizer {
    scale_ctx: ScaleContext,
    glyph_cache: HashMap<(bool, u16, char), CachedGlyph>,
}

impl Default for TextRasterizer {
    fn default() -> Self {
        Self::new()
    }
}

impl TextRasterizer {
    #[must_use]
    pub fn new() -> Self {
        Self {
            scale_ctx: ScaleContext::new(),
            glyph_cache: HashMap::with_capacity(256),
        }
    }

    #[must_use]
    fn font_bytes(is_mono: bool) -> &'static [u8] {
        if is_mono {
            VERA_MONO_BOLD_TTF
        } else {
            OPEN_SANS_BOLD_TTF
        }
    }

    pub(crate) fn get_glyph(&mut self, is_mono: bool, size_px: f32, ch: char) -> CachedGlyph {
        let size_key = (size_px.clamp(6.0, 96.0) * 4.0).round() as u16;
        let key = (is_mono, size_key, ch);
        if let Some(cached) = self.glyph_cache.get(&key) {
            return cached.clone();
        }
        if self.glyph_cache.len() > 2048 {
            self.glyph_cache.clear();
        }

        let actual_size = f32::from(size_key) / 4.0;
        let mut out = CachedGlyph {
            width: 0,
            height: 0,
            left: 0,
            top: 0,
            advance: actual_size * 0.55,
            alpha: Vec::new(),
        };

        if let Some(font) = FontRef::from_index(Self::font_bytes(is_mono), 0) {
            let gid = font.charmap().map(ch);
            let metrics = font.glyph_metrics(&[]).scale(actual_size);
            out.advance = metrics.advance_width(gid).max(actual_size * 0.25);

            let mut scaler = self
                .scale_ctx
                .builder(font)
                .size(actual_size)
                .hint(true)
                .build();
            if let Some(img) = Render::new(&[Source::Outline])
                .format(Format::Alpha)
                .render(&mut scaler, gid)
            {
                out.width = img.placement.width;
                out.height = img.placement.height;
                out.left = img.placement.left;
                out.top = img.placement.top;
                out.alpha = img.data;
            }
        }

        self.glyph_cache.insert(key, out.clone());
        out
    }

    /// Returns `(ascent, descent, line_height)` in logical pixels for the given font size.
    #[must_use]
    pub fn line_metrics(&self, is_mono: bool, size_px: f32) -> (f32, f32, f32) {
        if let Some(font) = FontRef::from_index(Self::font_bytes(is_mono), 0) {
            let m = font.metrics(&[]).scale(size_px.clamp(6.0, 96.0));
            let ascent = m.ascent.max(size_px * 0.72);
            let descent = m.descent.abs().max(size_px * 0.18);
            let line_h = (ascent + descent + m.leading.max(0.0)).max(size_px * 1.15);
            return (ascent, descent, line_h);
        }
        (size_px * 0.8, size_px * 0.2, size_px * 1.2)
    }

    /// Measures horizontal advance width of `text` in logical pixels.
    pub fn measure_text(&mut self, is_mono: bool, size_px: f32, text: &str) -> f32 {
        let mut width = 0.0_f32;
        for ch in text.chars() {
            width += self.get_glyph(is_mono, size_px, ch).advance;
        }
        width
    }
}

#[inline]
#[must_use]
pub const fn unpack_rgba(c: u32) -> [u8; 4] {
    [
        ((c >> 24) & 0xFF) as u8,
        ((c >> 16) & 0xFF) as u8,
        ((c >> 8) & 0xFF) as u8,
        (c & 0xFF) as u8,
    ]
}

/// Premultiplied RGBA8888 software canvas with DPI `scale` and optional `clip` rect.
pub struct CpuCanvas<'a> {
    pixels: &'a mut [u8],
    width: u32,
    height: u32,
    scale: f32,
    clip_px: Option<(i32, i32, i32, i32)>,
}

impl<'a> CpuCanvas<'a> {
    #[must_use]
    pub fn new(pixels: &'a mut [u8], width: u32, height: u32, scale: f32) -> Self {
        Self {
            pixels,
            width,
            height,
            scale: scale.clamp(0.25, 4.0),
            clip_px: None,
        }
    }

    #[must_use]
    pub const fn width_px(&self) -> u32 {
        self.width
    }

    #[must_use]
    pub const fn height_px(&self) -> u32 {
        self.height
    }

    #[must_use]
    pub const fn scale(&self) -> f32 {
        self.scale
    }

    pub fn set_scale(&mut self, scale: f32) {
        self.scale = scale.clamp(0.25, 4.0);
    }

    pub fn clear(&mut self) {
        self.pixels.fill(0);
    }

    pub fn with_clip<F: FnOnce(&mut Self)>(&mut self, clip_logical: Rect, f: F) {
        let prev = self.clip_px;
        let cx0 = (clip_logical.x * self.scale).floor() as i32;
        let cy0 = (clip_logical.y * self.scale).floor() as i32;
        let cx1 = (clip_logical.right() * self.scale).ceil() as i32;
        let cy1 = (clip_logical.bottom() * self.scale).ceil() as i32;
        let next = match prev {
            Some((px0, py0, px1, py1)) => (cx0.max(px0), cy0.max(py0), cx1.min(px1), cy1.min(py1)),
            None => (cx0, cy0, cx1, cy1),
        };
        self.clip_px = Some(next);
        f(self);
        self.clip_px = prev;
    }

    #[inline]
    fn active_bounds_px(&self) -> (i32, i32, i32, i32) {
        let w = self.width as i32;
        let h = self.height as i32;
        match self.clip_px {
            Some((x0, y0, x1, y1)) => (x0.clamp(0, w), y0.clamp(0, h), x1.clamp(0, w), y1.clamp(0, h)),
            None => (0, 0, w, h),
        }
    }

    #[inline]
    fn blend_px_unchecked(&mut self, px: i32, py: i32, r: u8, g: u8, b: u8, a: u8) {
        let idx = ((py as u32 * self.width + px as u32) * 4) as usize;
        if idx + 3 >= self.pixels.len() {
            return;
        }
        if a == 255 {
            self.pixels[idx] = r;
            self.pixels[idx + 1] = g;
            self.pixels[idx + 2] = b;
            self.pixels[idx + 3] = 255;
            return;
        }
        let sa = u32::from(a);
        let sr_p = (u32::from(r) * sa + 127) / 255;
        let sg_p = (u32::from(g) * sa + 127) / 255;
        let sb_p = (u32::from(b) * sa + 127) / 255;
        let inv = 255 - sa;

        let dr = u32::from(self.pixels[idx]);
        let dg = u32::from(self.pixels[idx + 1]);
        let db = u32::from(self.pixels[idx + 2]);
        let da = u32::from(self.pixels[idx + 3]);

        let out_a = (sa + (da * inv + 127) / 255).min(255);
        self.pixels[idx] = (sr_p + (dr * inv + 127) / 255).min(out_a) as u8;
        self.pixels[idx + 1] = (sg_p + (dg * inv + 127) / 255).min(out_a) as u8;
        self.pixels[idx + 2] = (sb_p + (db * inv + 127) / 255).min(out_a) as u8;
        self.pixels[idx + 3] = out_a as u8;
    }

    /// Fast horizontal span fill (premultiplied alpha-over) inside `[x0, x1)` at row `py`.
    #[inline]
    fn fill_span_px(&mut self, py: i32, x0: i32, x1: i32, rgba: [u8; 4]) {
        let [r, g, b, a] = rgba;
        if x1 <= x0 || a == 0 {
            return;
        }
        let row_start = ((py as u32 * self.width + x0 as u32) * 4) as usize;
        let count = (x1 - x0) as usize;
        let end = row_start + count * 4;
        if end > self.pixels.len() {
            return;
        }
        let (chunks, _) = self.pixels[row_start..end].as_chunks_mut::<4>();
        if a == 255 {
            for px in chunks {
                px[0] = r;
                px[1] = g;
                px[2] = b;
                px[3] = 255;
            }
            return;
        }
        let sa = u32::from(a);
        let sr_p = (u32::from(r) * sa + 127) / 255;
        let sg_p = (u32::from(g) * sa + 127) / 255;
        let sb_p = (u32::from(b) * sa + 127) / 255;
        let inv = 255 - sa;
        for px in chunks {
            let dr = u32::from(px[0]);
            let dg = u32::from(px[1]);
            let db = u32::from(px[2]);
            let da = u32::from(px[3]);
            let out_a = (sa + (da * inv + 127) / 255).min(255);
            px[0] = (sr_p + (dr * inv + 127) / 255).min(out_a) as u8;
            px[1] = (sg_p + (dg * inv + 127) / 255).min(out_a) as u8;
            px[2] = (sb_p + (db * inv + 127) / 255).min(out_a) as u8;
            px[3] = out_a as u8;
        }
    }

    pub fn fill_rect(&mut self, rect: Rect, color: u32) {
        let rgba = unpack_rgba(color);
        if rgba[3] == 0 || rect.w <= 0.0 || rect.h <= 0.0 {
            return;
        }
        let (bx0, by0, bx1, by1) = self.active_bounds_px();
        let x0 = (rect.x * self.scale).round() as i32;
        let y0 = (rect.y * self.scale).round() as i32;
        let x1 = (rect.right() * self.scale).round() as i32;
        let y1 = (rect.bottom() * self.scale).round() as i32;
        let cx0 = x0.clamp(bx0, bx1);
        let cx1 = x1.clamp(bx0, bx1);
        let cy0 = y0.clamp(by0, by1);
        let cy1 = y1.clamp(by0, by1);
        for py in cy0..cy1 {
            self.fill_span_px(py, cx0, cx1, rgba);
        }
    }

    pub fn draw_h_line(&mut self, x: f32, y: f32, w: f32, thickness: f32, color: u32) {
        self.fill_rect(Rect::new(x, y, w, thickness.max(1.0 / self.scale)), color);
    }

    pub fn draw_v_line(&mut self, x: f32, y: f32, h: f32, thickness: f32, color: u32) {
        self.fill_rect(Rect::new(x, y, thickness.max(1.0 / self.scale), h), color);
    }

    /// Fast flat-span + 1 px anti-aliased SDF rounded rectangle fill.
    pub fn fill_rounded_rect(&mut self, rect: Rect, radius: f32, color: u32) {
        let [r, g, b, a] = unpack_rgba(color);
        if a == 0 || rect.w <= 0.0 || rect.h <= 0.0 {
            return;
        }
        let rx0 = (rect.x * self.scale).round() as i32;
        let ry0 = (rect.y * self.scale).round() as i32;
        let rx1 = (rect.right() * self.scale).round() as i32;
        let ry1 = (rect.bottom() * self.scale).round() as i32;
        let pw = (rx1 - rx0).max(0) as f32;
        let ph = (ry1 - ry0).max(0) as f32;
        if pw < 1.0 || ph < 1.0 {
            return;
        }
        let rad = (radius * self.scale).clamp(0.0, pw.min(ph) * 0.5);
        if rad < 0.75 {
            self.fill_rect(rect, color);
            return;
        }
        let (bx0, by0, bx1, by1) = self.active_bounds_px();
        let cy0 = ry0.clamp(by0, by1);
        let cy1 = ry1.clamp(by0, by1);
        let cx0 = rx0.clamp(bx0, bx1);
        let cx1 = rx1.clamp(bx0, bx1);
        if cx0 >= cx1 || cy0 >= cy1 {
            return;
        }

        let rad_i = rad.ceil() as i32;
        let mid_y0 = (ry0 + rad_i).clamp(cy0, cy1);
        let mid_y1 = (ry1 - rad_i).clamp(mid_y0, cy1);

        // Fast flat middle band
        for py in mid_y0..mid_y1 {
            self.fill_span_px(py, cx0, cx1, [r, g, b, a]);
        }

        // Top and bottom rounded bands (only corners use AA SDF, inner span uses fast fill)
        let left_mid = (rx0 + rad_i).clamp(cx0, cx1);
        let right_mid = (rx1 - rad_i).clamp(left_mid, cx1);
        for py in (cy0..mid_y0).chain(mid_y1..cy1) {
            self.fill_span_px(py, left_mid, right_mid, [r, g, b, a]);
            let ly = (py - ry0) as f32 + 0.5;
            for px in (cx0..left_mid).chain(right_mid..cx1) {
                let lx = (px - rx0) as f32 + 0.5;
                let cov = rounded_rect_coverage(lx, ly, pw, ph, rad);
                if cov > 0.0 {
                    let eff_a = (f32::from(a) * cov + 0.5) as u8;
                    if eff_a > 0 {
                        self.blend_px_unchecked(px, py, r, g, b, eff_a);
                    }
                }
            }
        }
    }

    pub fn stroke_rounded_rect(&mut self, rect: Rect, radius: f32, thickness: f32, color: u32) {
        let [r, g, b, a] = unpack_rgba(color);
        if a == 0 || rect.w <= 0.0 || rect.h <= 0.0 {
            return;
        }
        let rx0 = (rect.x * self.scale).round() as i32;
        let ry0 = (rect.y * self.scale).round() as i32;
        let rx1 = (rect.right() * self.scale).round() as i32;
        let ry1 = (rect.bottom() * self.scale).round() as i32;
        let pw = (rx1 - rx0).max(0) as f32;
        let ph = (ry1 - ry0).max(0) as f32;
        let t = (thickness * self.scale).max(1.0);
        if pw < 2.0 || ph < 2.0 {
            return;
        }
        let rad = (radius * self.scale).clamp(0.0, pw.min(ph) * 0.5);
        let (bx0, by0, bx1, by1) = self.active_bounds_px();
        let cy0 = ry0.clamp(by0, by1);
        let cy1 = ry1.clamp(by0, by1);
        let cx0 = rx0.clamp(bx0, bx1);
        let cx1 = rx1.clamp(bx0, bx1);
        if cx0 >= cx1 || cy0 >= cy1 {
            return;
        }

        let band = (rad.ceil() as i32).max(t.ceil() as i32 + 1);
        let inner_y0 = (ry0 + band).clamp(cy0, cy1);
        let inner_y1 = (ry1 - band).clamp(inner_y0, cy1);
        let t_i = t.ceil() as i32;
        let left_end = (rx0 + t_i).clamp(cx0, cx1);
        let right_start = (rx1 - t_i).clamp(left_end, cx1);

        // Middle left/right vertical border spans
        for py in inner_y0..inner_y1 {
            self.fill_span_px(py, cx0, left_end, [r, g, b, a]);
            self.fill_span_px(py, right_start, cx1, [r, g, b, a]);
        }

        // Top and bottom border bands
        let inner_w = (pw - 2.0 * t).max(0.0);
        let inner_h = (ph - 2.0 * t).max(0.0);
        let inner_rad = (rad - t).max(0.0);
        for py in (cy0..inner_y0).chain(inner_y1..cy1) {
            let ly = (py - ry0) as f32 + 0.5;
            for px in cx0..cx1 {
                let lx = (px - rx0) as f32 + 0.5;
                let outer = rounded_rect_coverage(lx, ly, pw, ph, rad);
                if outer <= 0.0 {
                    continue;
                }
                let inner = if inner_w > 0.0 && inner_h > 0.0 {
                    rounded_rect_coverage(lx - t, ly - t, inner_w, inner_h, inner_rad)
                } else {
                    0.0
                };
                let cov = (outer - inner).clamp(0.0, 1.0);
                if cov > 0.0 {
                    let eff_a = (f32::from(a) * cov + 0.5) as u8;
                    if eff_a > 0 {
                        self.blend_px_unchecked(px, py, r, g, b, eff_a);
                    }
                }
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn draw_text(
        &mut self,
        rasterizer: &mut TextRasterizer,
        x_logical: f32,
        baseline_y_logical: f32,
        is_mono: bool,
        size_logical: f32,
        color: u32,
        text: &str,
    ) -> f32 {
        let [r, g, b, a] = unpack_rgba(color);
        if a == 0 || text.is_empty() {
            return x_logical;
        }
        let (bx0, by0, bx1, by1) = self.active_bounds_px();
        let size_px = (size_logical * self.scale).clamp(6.0, 96.0);
        let mut pen_x_px = x_logical * self.scale;
        let baseline_px = baseline_y_logical * self.scale;

        for ch in text.chars() {
            let glyph = rasterizer.get_glyph(is_mono, size_px, ch);
            if glyph.width > 0 && glyph.height > 0 {
                let gx0 = pen_x_px.round() as i32 + glyph.left;
                let gy0 = baseline_px.round() as i32 - glyph.top;
                for my in 0..glyph.height {
                    let py = gy0 + my as i32;
                    if py < by0 || py >= by1 {
                        continue;
                    }
                    let row_off = (my * glyph.width) as usize;
                    for mx in 0..glyph.width {
                        let px = gx0 + mx as i32;
                        if px < bx0 || px >= bx1 {
                            continue;
                        }
                        let m = glyph.alpha[row_off + mx as usize];
                        if m == 0 {
                            continue;
                        }
                        let eff_a = ((u32::from(m) * u32::from(a) + 127) / 255) as u8;
                        if eff_a > 0 {
                            self.blend_px_unchecked(px, py, r, g, b, eff_a);
                        }
                    }
                }
            }
            pen_x_px += glyph.advance;
        }
        pen_x_px / self.scale
    }

    /// Draws vertically centered text inside `rect`, optionally horizontally centered, clipped to `rect`.
    #[allow(clippy::too_many_arguments)]
    pub fn draw_text_in_rect(
        &mut self,
        rasterizer: &mut TextRasterizer,
        rect: Rect,
        is_mono: bool,
        size_logical: f32,
        color: u32,
        text: &str,
        center_x: bool,
    ) {
        let (ascent, descent, _) = rasterizer.line_metrics(is_mono, size_logical);
        let text_h = ascent + descent;
        let baseline_y = rect.y + (rect.h - text_h) * 0.5 + ascent;
        let start_x = if center_x {
            let w = rasterizer.measure_text(is_mono, size_logical, text);
            (rect.x + (rect.w - w) * 0.5).max(rect.x + 2.0)
        } else {
            rect.x
        };
        self.with_clip(rect, |c| {
            c.draw_text(rasterizer, start_x, baseline_y, is_mono, size_logical, color, text);
        });
    }

    /// Draws a crisp vector chevron (`▾` or `▴`) centered at `(cx, cy)` without font glyph dependency.
    pub fn draw_chevron(&mut self, cx: f32, cy: f32, up: bool, color: u32) {
        let half_w = 3.5_f32;
        for row in 0..4 {
            let frac = row as f32 / 3.0;
            let span = if up {
                (1.0 + frac * half_w * 2.0).round()
            } else {
                (1.0 + (1.0 - frac) * half_w * 2.0).round()
            };
            let y = cy - 1.5 + row as f32;
            self.fill_rect(Rect::new(cx - span * 0.5, y, span, 1.0), color);
        }
    }

    /// Draws a rounded toggle pill switch (`34×18` logical default) matching the Settings style.
    pub fn draw_toggle_switch(&mut self, rect: Rect, enabled: bool) {
        let track_bg = if enabled {
            rgba_u32(42, 132, 232, 245)
        } else {
            rgba_u32(44, 52, 68, 235)
        };
        let track_border = if enabled {
            rgba_u32(106, 186, 255, 240)
        } else {
            rgba_u32(82, 98, 122, 210)
        };
        let radius = rect.h * 0.5;
        self.fill_rounded_rect(rect, radius, track_bg);
        self.stroke_rounded_rect(rect, radius, 1.0, track_border);

        let knob_d = (rect.h - 4.0).max(8.0);
        let knob_x = if enabled {
            rect.right() - knob_d - 2.0
        } else {
            rect.x + 2.0
        };
        let knob_y = rect.y + (rect.h - knob_d) * 0.5;
        self.fill_rounded_rect(
            Rect::new(knob_x, knob_y, knob_d, knob_d),
            knob_d * 0.5,
            rgba_u32(244, 248, 255, 255),
        );
    }

    /// Draws the frametime bar sparkline inside `rect`.
    pub fn draw_frametime_sparkline(
        &mut self,
        rect: Rect,
        snap: &TelemetrySnapshot,
        border_color: u32,
    ) {
        self.fill_rounded_rect(rect, 3.0, rgba_u32(10, 14, 22, 210));
        self.stroke_rounded_rect(rect, 3.0, 1.0, border_color);

        let inner_w = (rect.w - 4.0).max(1.0);
        let inner_h = (rect.h - 4.0).max(1.0);
        let samples = &snap.frametime_history;
        if samples.is_empty() {
            return;
        }
        let max_bars = ((inner_w / 3.0).floor() as usize).clamp(1, 64);
        let start = samples.len().saturating_sub(max_bars);
        let slice = &samples[start..];
        let step_x = inner_w / (max_bars as f32);
        let bar_w = (step_x - 1.0).max(1.0);

        for (i, &ft) in slice.iter().enumerate() {
            let clamped = if ft.is_finite() { ft.clamp(1.0, 50.0) } else { 16.7 };
            let frac = (clamped / 50.0).clamp(0.08, 1.0);
            let bh = (inner_h * frac).max(2.0);
            let bx = rect.x + 2.0 + (i as f32) * step_x;
            let by = rect.bottom() - 2.0 - bh;
            let col = if clamped > 33.3 {
                rgba_u32(255, 80, 80, 235)
            } else if clamped > 22.2 {
                rgba_u32(255, 190, 40, 235)
            } else {
                rgba_u32(90, 220, 130, 235)
            };
            self.fill_rect(Rect::new(bx, by, bar_w, bh), col);
        }
    }

    /// Draws the pure-X11 software cursor indicator, safely clamped inside the viewport.
    pub fn draw_software_cursor(&mut self, x_logical: f32, y_logical: f32) {
        let max_x = (self.width as f32 / self.scale - 6.0).max(0.0);
        let max_y = (self.height as f32 / self.scale - 6.0).max(0.0);
        let cx = x_logical.clamp(0.0, max_x);
        let cy = y_logical.clamp(0.0, max_y);
        let outer = Rect::new(cx, cy, 14.0, 14.0);
        self.fill_rounded_rect(outer, 2.0, rgba_u32(12, 16, 24, 245));
        self.stroke_rounded_rect(outer, 2.0, 2.0, rgba_u32(255, 255, 255, 255));
        self.fill_rect(Rect::new(cx + 4.0, cy + 4.0, 6.0, 6.0), rgba_u32(95, 205, 255, 255));
    }
}

/// Signed-distance coverage `[0.0, 1.0]` with 1 px anti-aliasing for a rounded rectangle.
#[inline]
fn rounded_rect_coverage(lx: f32, ly: f32, w: f32, h: f32, r: f32) -> f32 {
    if lx < 0.0 || ly < 0.0 || lx > w || ly > h {
        return 0.0;
    }
    if r <= 0.0 {
        return 1.0;
    }
    let cx = if lx < r {
        r
    } else if lx > w - r {
        w - r
    } else {
        return 1.0;
    };
    let cy = if ly < r {
        r
    } else if ly > h - r {
        h - r
    } else {
        return 1.0;
    };
    let dx = lx - cx;
    let dy = ly - cy;
    let dist = (dx * dx + dy * dy).sqrt() - r;
    (0.5 - dist).clamp(0.0, 1.0)
}
