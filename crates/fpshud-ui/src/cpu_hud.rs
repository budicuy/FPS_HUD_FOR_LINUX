//! Ultra-lightweight 100% safe CPU glyph-cached HUD renderer (`swash`, 0 `wgpu`, 0 MiB VRAM).
//!
//! Renders the compact bounding-box HUD card (and 5.5s launch toast) directly to premultiplied
//! RGBA8888 pixels for positioned Vulkan/OpenGL viewport blitting while the Settings GUI is closed.

use crate::state::{
    MetricCategory, UiStore, category_color_rgba, category_label, color_to_rgba_u32,
    effective_text_color, group_metrics_ordered, hud_metric_short_label, rgba_u32,
};
use crate::toast::ToastState;
use fpshud_core::keybind::format_keybind;
use fpshud_core::{ArrangementMode, BackgroundStyle, FontFamily, LayoutPosition, MetricType};
use std::collections::HashMap;
use swash::FontRef;
use swash::scale::{Render, ScaleContext, Source};
use swash::zeno::Format;

const OPEN_SANS_BOLD_TTF: &[u8] = include_bytes!("../../../assets/fonts/OpenSans-Bold.ttf");
const VERA_MONO_BOLD_TTF: &[u8] = include_bytes!("../../../assets/fonts/VeraMono-Bold.ttf");

/// Premultiplied RGBA8888 bounding-box frame ready for positioned swapchain blitting.
#[derive(Debug, Clone)]
pub struct CpuHudFrame {
    pub dst_x: i32,
    pub dst_y: i32,
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}

#[derive(Clone)]
struct CachedGlyph {
    width: u32,
    height: u32,
    left: i32,
    top: i32,
    advance: f32,
    alpha: Vec<u8>,
}

#[derive(Clone)]
struct TextSpan {
    text: String,
    color: u32,
}

#[derive(Clone)]
struct MeasuredLine {
    spans: Vec<TextSpan>,
    width: f32,
}

/// Stateful CPU HUD rasterizer with per-glyph alpha bitmap cache.
pub struct CpuHudRenderer {
    scale_ctx: ScaleContext,
    glyph_cache: HashMap<(bool, u16, char), CachedGlyph>,
}

impl Default for CpuHudRenderer {
    fn default() -> Self {
        Self::new()
    }
}

impl CpuHudRenderer {
    #[must_use]
    pub fn new() -> Self {
        Self {
            scale_ctx: ScaleContext::new(),
            glyph_cache: HashMap::with_capacity(256),
        }
    }

    fn font_bytes(is_mono: bool) -> &'static [u8] {
        if is_mono {
            VERA_MONO_BOLD_TTF
        } else {
            OPEN_SANS_BOLD_TTF
        }
    }

    fn is_mono_font(family: FontFamily) -> bool {
        !matches!(family, FontFamily::SansBold)
    }

    fn get_glyph(&mut self, is_mono: bool, size_px: f32, ch: char) -> CachedGlyph {
        let size_key = (size_px.clamp(8.0, 64.0) * 4.0).round() as u16;
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

    fn measure_text(&mut self, is_mono: bool, size_px: f32, text: &str) -> f32 {
        let mut w = 0.0_f32;
        for ch in text.chars() {
            w += self.get_glyph(is_mono, size_px, ch).advance;
        }
        w
    }

    fn measure_spans(&mut self, is_mono: bool, size_px: f32, spans: &[TextSpan]) -> f32 {
        let mut w = 0.0_f32;
        for s in spans {
            w += self.measure_text(is_mono, size_px, &s.text);
        }
        w
    }

    fn draw_spans(
        &mut self,
        canvas: &mut Canvas,
        is_mono: bool,
        size_px: f32,
        start_x: f32,
        baseline_y: f32,
        spans: &[TextSpan],
    ) {
        let mut pen_x = start_x;
        for span in spans {
            let [r, g, b, a] = unpack_rgba(span.color);
            for ch in span.text.chars() {
                let glyph = self.get_glyph(is_mono, size_px, ch);
                if glyph.width > 0 && glyph.height > 0 {
                    let gx = pen_x.round() as i32 + glyph.left;
                    let gy = baseline_y.round() as i32 - glyph.top;
                    canvas.blend_alpha_mask(
                        gx,
                        gy,
                        glyph.width,
                        glyph.height,
                        &glyph.alpha,
                        r,
                        g,
                        b,
                        a,
                    );
                }
                pen_x += glyph.advance;
            }
        }
    }

    /// Renders either the active HUD card (with optional graph/benchmark/toast) or the launch toast alone.
    /// Returns `None` when both HUD and launch toast are hidden.
    pub fn render(
        &mut self,
        store: &UiStore,
        viewport_w: u32,
        viewport_h: u32,
        scale: f32,
    ) -> Option<CpuHudFrame> {
        let vp_w = viewport_w.max(1);
        let vp_h = viewport_h.max(1);
        let eff_scale = scale.clamp(0.5, 3.0);
        let show_toast = !store.config.gui_open && ToastState::should_show(store.elapsed_sec);

        if !store.config.hud_visible {
            if !show_toast {
                return None;
            }
            return Some(self.render_toast_only(store, vp_w, vp_h, eff_scale));
        }

        let is_mono = Self::is_mono_font(store.config.font_family);
        let font_size = (store.config.font_size_px.clamp(11.0, 36.0) * 0.72 * eff_scale).clamp(9.0, 48.0);
        let line_h = (font_size * 1.38).ceil();
        let pad_x = (8.0 * eff_scale).round();
        let pad_y = (7.0 * eff_scale).round();
        let row_gap = (3.0 * eff_scale).round();
        let alpha = (store.config.text_opacity.clamp(0.1, 1.0) * 255.0).round() as u8;
        let groups = group_metrics_ordered(&store.config.metrics);

        let mut lines: Vec<MeasuredLine> = Vec::new();
        let mut right_lines: Vec<MeasuredLine> = Vec::new();

        match store.config.arrangement {
            ArrangementMode::Stack => {
                for (cat, ms) in &groups {
                    let spans = build_group_spans(store, *cat, ms, alpha);
                    let w = self.measure_spans(is_mono, font_size, &spans);
                    lines.push(MeasuredLine { spans, width: w });
                }
            }
            ArrangementMode::Double => {
                let mid = groups.len().div_ceil(2);
                for (cat, ms) in groups.iter().take(mid) {
                    let spans = build_group_spans(store, *cat, ms, alpha);
                    let w = self.measure_spans(is_mono, font_size, &spans);
                    lines.push(MeasuredLine { spans, width: w });
                }
                for (cat, ms) in groups.iter().skip(mid) {
                    let spans = build_group_spans(store, *cat, ms, alpha);
                    let w = self.measure_spans(is_mono, font_size, &spans);
                    right_lines.push(MeasuredLine { spans, width: w });
                }
            }
            ArrangementMode::Linear => {
                let sep_color = rgba_u32(110, 128, 152, (u16::from(alpha) * 3 / 5) as u8);
                let mut spans = Vec::new();
                for (idx, (cat, ms)) in groups.iter().enumerate() {
                    if idx > 0 {
                        spans.push(TextSpan {
                            text: "  |  ".to_string(),
                            color: sep_color,
                        });
                    }
                    spans.extend(build_group_spans(store, *cat, ms, alpha));
                }
                let w = self.measure_spans(is_mono, font_size, &spans);
                lines.push(MeasuredLine { spans, width: w });
            }
        }

        let col_gap = (14.0 * eff_scale).round();
        let left_w = lines.iter().map(|l| l.width).fold(0.0_f32, f32::max);
        let right_w = right_lines.iter().map(|l| l.width).fold(0.0_f32, f32::max);
        let body_w = if right_lines.is_empty() {
            left_w
        } else {
            left_w + col_gap + right_w
        };
        let row_count = lines.len().max(right_lines.len()).max(1);
        let body_h = (row_count as f32) * line_h + ((row_count - 1) as f32) * row_gap;

        let graph_w = (180.0 * eff_scale).round();
        let graph_h = (34.0 * eff_scale).round();
        let graph_gap = (5.0 * eff_scale).round();

        let bench_text = store.telemetry.benchmark_active.then(|| {
            format!("BENCHMARK REC: {:.1}s", store.telemetry.benchmark_elapsed_sec)
        });
        let bench_font = (12.0 * eff_scale).clamp(9.0, 28.0);
        let bench_w = bench_text
            .as_deref()
            .map_or(0.0, |t| self.measure_text(is_mono, bench_font, t));
        let bench_h = if bench_text.is_some() {
            (bench_font * 1.35).ceil()
        } else {
            0.0
        };

        let toast_text = show_toast.then(|| {
            let gui_key = format_keybind(store.config.bind_toggle_gui);
            let hud_key = format_keybind(store.config.bind_toggle_hud);
            format!("FPSHUD active | Dashboard: {gui_key} | HUD: {hud_key}")
        });
        let toast_font = (11.5 * eff_scale).clamp(9.0, 26.0);
        let toast_w = toast_text
            .as_deref()
            .map_or(0.0, |t| self.measure_text(false, toast_font, t));
        let toast_h = if toast_text.is_some() {
            (toast_font * 1.45).ceil()
        } else {
            0.0
        };

        let mut content_w = body_w.max(bench_w).max(toast_w);
        let mut content_h = body_h;
        if store.config.hud_graph_enabled {
            content_w = content_w.max(graph_w);
            content_h += graph_gap + graph_h;
        }
        if bench_h > 0.0 {
            content_h += row_gap + bench_h;
        }
        if toast_h > 0.0 {
            content_h += row_gap + toast_h;
        }

        let box_w = ((content_w + pad_x * 2.0).ceil() as u32).clamp(1, vp_w);
        let box_h = ((content_h + pad_y * 2.0).ceil() as u32).clamp(1, vp_h);

        let mut canvas = Canvas::new(box_w, box_h);
        let radius = (6.0 * eff_scale).round();
        let bg = hud_bg_color(store);
        if (bg & 0xff) > 0 {
            canvas.fill_rounded_rect(0, 0, box_w, box_h, radius, bg);
            canvas.stroke_rounded_rect(0, 0, box_w, box_h, radius, rgba_u32(90, 125, 165, 95));
        }

        let ascent = font_size * 0.82;
        for (i, line) in lines.iter().enumerate() {
            let y = pad_y + (i as f32) * (line_h + row_gap) + ascent;
            self.draw_spans(&mut canvas, is_mono, font_size, pad_x, y, &line.spans);
        }
        for (i, line) in right_lines.iter().enumerate() {
            let y = pad_y + (i as f32) * (line_h + row_gap) + ascent;
            self.draw_spans(
                &mut canvas,
                is_mono,
                font_size,
                pad_x + left_w + col_gap,
                y,
                &line.spans,
            );
        }

        let mut cur_y = pad_y + body_h;
        if store.config.hud_graph_enabled {
            cur_y += graph_gap;
            draw_sparkline(
                &mut canvas,
                pad_x.round() as i32,
                cur_y.round() as i32,
                graph_w.round() as u32,
                graph_h.round() as u32,
                &store.telemetry.frametime_history,
            );
            cur_y += graph_h;
        }

        if let Some(ref text) = bench_text {
            cur_y += row_gap;
            let span = [TextSpan {
                text: text.clone(),
                color: rgba_u32(255, 200, 75, 240),
            }];
            self.draw_spans(&mut canvas, is_mono, bench_font, pad_x, cur_y + bench_font * 0.82, &span);
            cur_y += bench_h;
        }

        if let Some(ref text) = toast_text {
            cur_y += row_gap;
            let span = [TextSpan {
                text: text.clone(),
                color: rgba_u32(160, 210, 255, 235),
            }];
            self.draw_spans(&mut canvas, false, toast_font, pad_x, cur_y + toast_font * 0.82, &span);
        }

        let (dst_x, dst_y) = compute_hud_position(store, vp_w, vp_h, box_w, box_h);
        Some(CpuHudFrame {
            dst_x,
            dst_y,
            width: box_w,
            height: box_h,
            rgba: canvas.pixels,
        })
    }

    fn render_toast_only(
        &mut self,
        store: &UiStore,
        vp_w: u32,
        vp_h: u32,
        scale: f32,
    ) -> CpuHudFrame {
        let gui_key = format_keybind(store.config.bind_toggle_gui);
        let hud_key = format_keybind(store.config.bind_toggle_hud);
        let text = format!("FPSHUD active | Dashboard: {gui_key} | HUD: {hud_key}");
        let font_px = (13.0 * scale).clamp(10.0, 28.0);
        let pad_x = (12.0 * scale).round();
        let pad_y = (6.0 * scale).round();
        let tw = self.measure_text(false, font_px, &text);
        let th = (font_px * 1.4).ceil();

        let box_w = ((tw + pad_x * 2.0).ceil() as u32).clamp(1, vp_w);
        let box_h = ((th + pad_y * 2.0).ceil() as u32).clamp(1, vp_h);
        let mut canvas = Canvas::new(box_w, box_h);
        let radius = (6.0 * scale).round();
        canvas.fill_rounded_rect(0, 0, box_w, box_h, radius, rgba_u32(16, 22, 30, 215));
        canvas.stroke_rounded_rect(0, 0, box_w, box_h, radius, rgba_u32(82, 168, 255, 160));
        let span = [TextSpan {
            text,
            color: rgba_u32(232, 240, 250, 245),
        }];
        self.draw_spans(&mut canvas, false, font_px, pad_x, pad_y + font_px * 0.82, &span);

        let dst_x = 16_i32.clamp(0, vp_w.saturating_sub(box_w) as i32);
        let dst_y = (vp_h.saturating_sub(box_h + 16)) as i32;
        CpuHudFrame {
            dst_x,
            dst_y: dst_y.max(0),
            width: box_w,
            height: box_h,
            rgba: canvas.pixels,
        }
    }
}

fn hud_bg_color(store: &UiStore) -> u32 {
    match store.config.background_style {
        BackgroundStyle::None => rgba_u32(0, 0, 0, 0),
        BackgroundStyle::Translucent => {
            let alpha = (store.config.bg_alpha.clamp(0.05, 1.0) * 255.0).round() as u8;
            rgba_u32(12, 16, 22, alpha)
        }
        BackgroundStyle::Solid => rgba_u32(12, 16, 22, 255),
    }
}

fn dimmed_text_rgba(store: &UiStore, alpha: u8) -> u32 {
    let mut c = effective_text_color(&store.config);
    if store.config.dim_labels_enabled {
        c.r = (f32::from(c.r) * 0.72) as u8;
        c.g = (f32::from(c.g) * 0.72) as u8;
        c.b = (f32::from(c.b) * 0.72) as u8;
    }
    color_to_rgba_u32(c, alpha)
}

fn build_group_spans(
    store: &UiStore,
    cat: MetricCategory,
    metrics: &[MetricType],
    alpha: u8,
) -> Vec<TextSpan> {
    let cat_color = if store.config.category_colors_enabled {
        category_color_rgba(cat, &store.config, alpha)
    } else {
        dimmed_text_rgba(store, alpha)
    };
    let sub_color = dimmed_text_rgba(store, alpha);
    let dot_color = rgba_u32(130, 148, 172, alpha);

    let mut spans = Vec::with_capacity(metrics.len() * 3 + 2);
    spans.push(TextSpan {
        text: format!("{}  ", category_label(cat)),
        color: cat_color,
    });
    for (idx, &metric) in metrics.iter().enumerate() {
        if idx > 0 {
            spans.push(TextSpan {
                text: " \u{00B7} ".to_string(),
                color: dot_color,
            });
        }
        let (_, value, val_color, _) = store.format_metric_row(metric);
        if metric != MetricType::Fps {
            spans.push(TextSpan {
                text: format!("{} ", hud_metric_short_label(metric)),
                color: sub_color,
            });
        }
        spans.push(TextSpan {
            text: value,
            color: val_color,
        });
    }
    spans
}

fn compute_hud_position(
    store: &UiStore,
    vp_w: u32,
    vp_h: u32,
    box_w: u32,
    box_h: u32,
) -> (i32, i32) {
    let m = 16_i32;
    let max_x = vp_w.saturating_sub(box_w) as i32;
    let max_y = vp_h.saturating_sub(box_h) as i32;
    let (x, y) = match store.config.position {
        LayoutPosition::TopLeft => (m, m),
        LayoutPosition::TopCenter => (max_x / 2, m),
        LayoutPosition::TopRight => (max_x - m, m),
        LayoutPosition::BottomLeft => (m, max_y - m),
        LayoutPosition::BottomCenter => (max_x / 2, max_y - m),
        LayoutPosition::BottomRight => (max_x - m, max_y - m),
        LayoutPosition::Custom => (
            (store.config.custom_pos_x.clamp(0.0, 0.95) * (vp_w as f32)).round() as i32,
            (store.config.custom_pos_y.clamp(0.0, 0.95) * (vp_h as f32)).round() as i32,
        ),
    };
    (x.clamp(0, max_x), y.clamp(0, max_y))
}

fn draw_sparkline(
    canvas: &mut Canvas,
    x: i32,
    y: i32,
    w: u32,
    h: u32,
    samples: &[f32],
) {
    canvas.fill_rounded_rect(x, y, w, h, 4.0, rgba_u32(10, 14, 20, 190));
    canvas.stroke_rounded_rect(x, y, w, h, 4.0, rgba_u32(72, 140, 220, 110));
    if samples.is_empty() || w <= 8 || h <= 6 {
        return;
    }
    let max_ms = samples
        .iter()
        .copied()
        .filter(|v| v.is_finite() && *v > 0.0)
        .fold(16.7_f32, f32::max);
    let inner_w = (w - 8) as f32;
    let inner_h = (h - 6) as f32;
    let count = samples.len();
    let step = inner_w / (count as f32);
    let bar_w = (step - 1.0).max(1.0).ceil() as u32;

    for (i, &ft) in samples.iter().enumerate() {
        let clean = if ft.is_finite() && ft > 0.0 { ft } else { 0.0 };
        let frac = (clean / max_ms).clamp(0.08, 1.0);
        let bh = (frac * inner_h).max(2.0).round() as u32;
        let col = if clean > 33.3 {
            rgba_u32(255, 70, 70, 230)
        } else if clean > 22.2 {
            rgba_u32(255, 185, 0, 230)
        } else {
            rgba_u32(80, 225, 140, 225)
        };
        let bx = x + 4 + ((i as f32) * step).floor() as i32;
        let by = y + (h as i32) - 3 - (bh as i32);
        canvas.fill_rect(bx, by, bar_w, bh, col);
    }
}

fn unpack_rgba(c: u32) -> [u8; 4] {
    [
        ((c >> 24) & 0xff) as u8,
        ((c >> 16) & 0xff) as u8,
        ((c >> 8) & 0xff) as u8,
        (c & 0xff) as u8,
    ]
}

struct Canvas {
    width: u32,
    height: u32,
    pixels: Vec<u8>,
}

impl Canvas {
    fn new(width: u32, height: u32) -> Self {
        let len = (width as usize).saturating_mul(height as usize).saturating_mul(4);
        Self {
            width,
            height,
            pixels: vec![0_u8; len],
        }
    }

    #[inline]
    fn blend_px(&mut self, x: i32, y: i32, r: u8, g: u8, b: u8, a: u8) {
        if a == 0 || x < 0 || y < 0 || x >= self.width as i32 || y >= self.height as i32 {
            return;
        }
        let idx = ((y as usize) * (self.width as usize) + (x as usize)) * 4;
        if idx + 3 >= self.pixels.len() {
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

    fn fill_rect(&mut self, x: i32, y: i32, w: u32, h: u32, color: u32) {
        let [r, g, b, a] = unpack_rgba(color);
        if a == 0 {
            return;
        }
        let x0 = x.max(0);
        let y0 = y.max(0);
        let x1 = x.saturating_add(w as i32).min(self.width as i32);
        let y1 = y.saturating_add(h as i32).min(self.height as i32);
        for py in y0..y1 {
            for px in x0..x1 {
                self.blend_px(px, py, r, g, b, a);
            }
        }
    }

    fn fill_rounded_rect(&mut self, x: i32, y: i32, w: u32, h: u32, radius: f32, color: u32) {
        let [r, g, b, a] = unpack_rgba(color);
        if a == 0 || w == 0 || h == 0 {
            return;
        }
        let rad = radius.min((w.min(h) as f32) * 0.5);
        let x0 = x.max(0);
        let y0 = y.max(0);
        let x1 = x.saturating_add(w as i32).min(self.width as i32);
        let y1 = y.saturating_add(h as i32).min(self.height as i32);
        for py in y0..y1 {
            for px in x0..x1 {
                let lx = (px - x) as f32 + 0.5;
                let ly = (py - y) as f32 + 0.5;
                if inside_rounded_rect(lx, ly, w as f32, h as f32, rad) {
                    self.blend_px(px, py, r, g, b, a);
                }
            }
        }
    }

    fn stroke_rounded_rect(&mut self, x: i32, y: i32, w: u32, h: u32, radius: f32, color: u32) {
        let [r, g, b, a] = unpack_rgba(color);
        if a == 0 || w < 2 || h < 2 {
            return;
        }
        let rad = radius.min((w.min(h) as f32) * 0.5);
        let x0 = x.max(0);
        let y0 = y.max(0);
        let x1 = x.saturating_add(w as i32).min(self.width as i32);
        let y1 = y.saturating_add(h as i32).min(self.height as i32);
        for py in y0..y1 {
            for px in x0..x1 {
                let lx = (px - x) as f32 + 0.5;
                let ly = (py - y) as f32 + 0.5;
                let in_outer = inside_rounded_rect(lx, ly, w as f32, h as f32, rad);
                let in_inner = lx >= 1.0
                    && ly >= 1.0
                    && lx <= (w as f32) - 1.0
                    && ly <= (h as f32) - 1.0
                    && inside_rounded_rect(
                        lx - 1.0,
                        ly - 1.0,
                        (w - 2) as f32,
                        (h - 2) as f32,
                        (rad - 1.0).max(0.0),
                    );
                if in_outer && !in_inner {
                    self.blend_px(px, py, r, g, b, a);
                }
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn blend_alpha_mask(
        &mut self,
        dst_x: i32,
        dst_y: i32,
        mask_w: u32,
        mask_h: u32,
        mask: &[u8],
        r: u8,
        g: u8,
        b: u8,
        a: u8,
    ) {
        for my in 0..mask_h {
            let py = dst_y + my as i32;
            if py < 0 || py >= self.height as i32 {
                continue;
            }
            for mx in 0..mask_w {
                let px = dst_x + mx as i32;
                if px < 0 || px >= self.width as i32 {
                    continue;
                }
                let m = mask[(my * mask_w + mx) as usize];
                if m == 0 {
                    continue;
                }
                let eff_a = ((u32::from(m) * u32::from(a) + 127) / 255) as u8;
                self.blend_px(px, py, r, g, b, eff_a);
            }
        }
    }
}

fn inside_rounded_rect(lx: f32, ly: f32, w: f32, h: f32, r: f32) -> bool {
    if r <= 0.0 {
        return true;
    }
    let cx = if lx < r {
        r
    } else if lx > w - r {
        w - r
    } else {
        return true;
    };
    let cy = if ly < r {
        r
    } else if ly > h - r {
        h - r
    } else {
        return true;
    };
    let dx = lx - cx;
    let dy = ly - cy;
    dx * dx + dy * dy <= r * r
}
