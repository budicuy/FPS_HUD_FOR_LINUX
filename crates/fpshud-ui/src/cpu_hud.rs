//! Ultra-lightweight 100% safe-Rust CPU glyph-cached HUD renderer (`swash`, 0 MiB VRAM).
//!
//! Renders either:
//! - A compact bounding-box `CpuHudFrame` for positioned Vulkan/OpenGL viewport blitting (`HudOnly`), or
//! - Directly into a shared `CpuCanvas` alongside the Settings dashboard (`Full` pure-X11 fallback).

use crate::cpu_canvas::{CpuCanvas, Rect, TextRasterizer};
use crate::state::{
    MetricCategory, UiStore, category_color_rgba, category_label, color_to_rgba_u32,
    effective_text_color, group_metrics_ordered, rgba_u32,
};
use fpshud_core::{ArrangementMode, BackgroundStyle, FontFamily, LayoutPosition, MetricType};

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
struct TextSpan {
    text: String,
    color: u32,
}

#[derive(Clone)]
struct MeasuredCell {
    spans: Vec<TextSpan>,
    width: f32,
}

#[derive(Clone)]
struct MeasuredLine {
    prefix: Option<TextSpan>,
    cells: Vec<MeasuredCell>,
    width: f32,
}

/// Stateful CPU HUD rasterizer sharing `TextRasterizer` & `CpuCanvas` primitives.
pub struct CpuHudRenderer {
    fallback_rasterizer: TextRasterizer,
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
            fallback_rasterizer: TextRasterizer::new(),
        }
    }

    fn measure_spans(
        rasterizer: &mut TextRasterizer,
        font: FontFamily,
        size_px: f32,
        spans: &[TextSpan],
    ) -> f32 {
        let mut w = 0.0_f32;
        for s in spans {
            w += rasterizer.measure_text_font(font, size_px, &s.text);
        }
        w
    }

    fn draw_spans(
        canvas: &mut CpuCanvas<'_>,
        rasterizer: &mut TextRasterizer,
        font: FontFamily,
        size_logical: f32,
        start_x: f32,
        baseline_y: f32,
        spans: &[TextSpan],
    ) {
        let mut pen_x = start_x;
        for span in spans {
            pen_x = canvas.draw_text_font(
                rasterizer,
                pen_x,
                baseline_y,
                font,
                size_logical,
                span.color,
                &span.text,
            );
        }
    }

    /// Renders using an internal fallback `TextRasterizer` (convenience wrapper for tests/callers).
    pub fn render(
        &mut self,
        store: &UiStore,
        viewport_w: u32,
        viewport_h: u32,
        scale: f32,
    ) -> Option<CpuHudFrame> {
        let mut rast = std::mem::take(&mut self.fallback_rasterizer);
        let out = self.render_with_rasterizer(&mut rast, store, viewport_w, viewport_h, scale);
        self.fallback_rasterizer = rast;
        out
    }

    /// Renders either the active HUD card or the launch toast alone as a tight bounding-box frame.
    pub fn render_with_rasterizer(
        &self,
        rasterizer: &mut TextRasterizer,
        store: &UiStore,
        viewport_w: u32,
        viewport_h: u32,
        scale: f32,
    ) -> Option<CpuHudFrame> {
        let vp_w = viewport_w.max(1);
        let vp_h = viewport_h.max(1);
        let eff_scale = scale.clamp(0.5, 3.0);
        let toast_text = if !store.config.gui_open {
            store.current_toast_text()
        } else {
            None
        };

        if !store.config.hud_visible {
            let text = toast_text?;
            return Some(self.render_toast_only(rasterizer, &text, vp_w, vp_h, eff_scale));
        }

        let (box_w, box_h, layout) = self.measure_hud_card(rasterizer, store, eff_scale);
        let card_w_px = (box_w.ceil() as u32).clamp(1, vp_w);
        let card_h_px = (box_h.ceil() as u32).clamp(1, vp_h);

        let toast_metrics = toast_text.as_ref().map(|text| {
            let font_size = (13.0 * eff_scale).clamp(10.0, 28.0);
            let pad_x = (12.0 * eff_scale).round();
            let pad_y = (6.0 * eff_scale).round();
            let text_w = rasterizer.measure_text(false, font_size, text);
            let tw = ((text_w + pad_x * 2.0).ceil() as u32).clamp(1, vp_w);
            let th = (((font_size * 1.35) + pad_y * 2.0).ceil() as u32).clamp(1, vp_h);
            let gap = (6.0 * eff_scale).round() as u32;
            (text.as_str(), font_size, pad_x, tw, th, gap)
        });

        let total_w_px = toast_metrics
            .map_or(card_w_px, |(_, _, _, tw, _, _)| card_w_px.max(tw))
            .clamp(1, vp_w);
        let total_h_px = toast_metrics
            .map_or(card_h_px, |(_, _, _, _, th, gap)| card_h_px.saturating_add(gap).saturating_add(th))
            .clamp(1, vp_h);

        let (dst_x, dst_y) = resolve_hud_origin(
            store.config.position,
            store.config.custom_pos_x,
            store.config.custom_pos_y,
            vp_w,
            vp_h,
            total_w_px,
            total_h_px,
            eff_scale,
        );

        let mut rgba = vec![0_u8; (total_w_px as usize) * (total_h_px as usize) * 4];
        let mut canvas = CpuCanvas::new(&mut rgba, total_w_px, total_h_px, 1.0);
        self.paint_hud_card(
            &mut canvas,
            rasterizer,
            store,
            0.0,
            0.0,
            card_w_px as f32,
            card_h_px as f32,
            &layout,
        );

        if let Some((text, font_size, pad_x, tw, th, gap)) = toast_metrics {
            let ty = (card_h_px.saturating_add(gap)) as f32;
            let radius = (6.0 * eff_scale).round();
            let rect = Rect::new(0.0, ty, tw as f32, th as f32);
            canvas.fill_rounded_rect(rect, radius, rgba_u32(16, 22, 30, 225));
            canvas.stroke_rounded_rect(rect, radius, 1.0, rgba_u32(82, 168, 255, 180));
            canvas.draw_text_in_rect(
                rasterizer,
                Rect::new(pad_x, ty, (tw as f32 - pad_x * 2.0).max(1.0), th as f32),
                false,
                font_size,
                rgba_u32(232, 240, 250, 250),
                text,
                false,
            );
        }

        Some(CpuHudFrame {
            dst_x,
            dst_y,
            width: total_w_px,
            height: total_h_px,
            rgba,
        })
    }

    /// Composites the active HUD card AND launch toast (when visible) into a full-surface `CpuCanvas`.
    pub fn render_into_canvas(
        &self,
        canvas: &mut CpuCanvas<'_>,
        rasterizer: &mut TextRasterizer,
        store: &UiStore,
        logical_w: f32,
        logical_h: f32,
    ) {
        let vp_w = logical_w.max(1.0).round() as u32;
        let vp_h = logical_h.max(1.0).round() as u32;

        if store.config.hud_visible {
            let (box_w, box_h, layout) = self.measure_hud_card(rasterizer, store, 1.0);
            let box_w_u = (box_w.ceil() as u32).clamp(1, vp_w);
            let box_h_u = (box_h.ceil() as u32).clamp(1, vp_h);
            let (dst_x, dst_y) = resolve_hud_origin(
                store.config.position,
                store.config.custom_pos_x,
                store.config.custom_pos_y,
                vp_w,
                vp_h,
                box_w_u,
                box_h_u,
                1.0,
            );
            self.paint_hud_card(
                canvas,
                rasterizer,
                store,
                dst_x as f32,
                dst_y as f32,
                box_w_u as f32,
                box_h_u as f32,
                &layout,
            );
        }

        if !store.config.gui_open
            && let Some(text) = store.current_toast_text()
        {
            let font_size = 13.0;
            let text_w = rasterizer.measure_text(false, font_size, &text);
            let box_w = (text_w + 24.0).min((logical_w - 32.0).max(40.0));
            let box_h = 28.0;
            let rect = Rect::new(16.0, (logical_h - 16.0 - box_h).max(0.0), box_w, box_h);
            canvas.fill_rounded_rect(rect, 6.0, rgba_u32(16, 22, 30, 215));
            canvas.stroke_rounded_rect(rect, 6.0, 1.0, rgba_u32(82, 168, 255, 160));
            canvas.draw_text_in_rect(
                rasterizer,
                Rect::new(rect.x + 12.0, rect.y, (rect.w - 24.0).max(1.0), rect.h),
                false,
                font_size,
                rgba_u32(232, 240, 250, 245),
                &text,
                false,
            );
        }
    }

    fn measure_hud_card(
        &self,
        rasterizer: &mut TextRasterizer,
        store: &UiStore,
        eff_scale: f32,
    ) -> (f32, f32, HudCardLayout) {
        let font = store.config.font_family;
        let font_size =
            (store.config.font_size_px.clamp(11.0, 36.0) * 0.72 * eff_scale).clamp(9.0, 48.0);
        let line_h = (font_size * 1.38).ceil();
        let pad_x = (8.0 * eff_scale).round();
        let pad_y = (7.0 * eff_scale).round();
        let row_gap = (3.0 * eff_scale).round();
        let alpha = (store.config.text_opacity.clamp(0.1, 1.0) * 255.0).round() as u8;
        let groups = group_metrics_ordered(&store.config.metrics);

        let mut lines: Vec<MeasuredLine> = Vec::new();
        let mut right_lines: Vec<MeasuredLine> = Vec::new();
        let cell_gap = (16.0 * eff_scale).round();

        // Compute fixed category prefix column width for aligned Stack & Double layouts
        let mut prefix_col_w = 0.0_f32;
        if matches!(
            store.config.arrangement,
            ArrangementMode::Stack | ArrangementMode::Double
        ) {
            for (cat, _) in &groups {
                let pw = rasterizer.measure_text_font(font, font_size, category_label(*cat));
                prefix_col_w = prefix_col_w.max(pw);
            }
            if prefix_col_w > 0.0 {
                prefix_col_w += (14.0 * eff_scale).round();
            }
        }

        let mut left_col_widths: Vec<f32> = Vec::new();
        let mut right_col_widths: Vec<f32> = Vec::new();

        match store.config.arrangement {
            ArrangementMode::Stack => {
                for (cat, ms) in &groups {
                    let prefix = Some(TextSpan {
                        text: category_label(*cat).to_string(),
                        color: category_color_rgba(*cat, &store.config, alpha),
                    });
                    let cells = build_tabular_cells(rasterizer, font, font_size, store, ms, alpha);
                    update_col_widths(&mut left_col_widths, &cells);
                    lines.push(MeasuredLine {
                        prefix,
                        cells,
                        width: 0.0,
                    });
                }
                finalize_tabular_line_widths(&mut lines, prefix_col_w, &left_col_widths, cell_gap);
            }
            ArrangementMode::Double => {
                let mid = groups.len().div_ceil(2);
                for (cat, ms) in groups.iter().take(mid) {
                    let prefix = Some(TextSpan {
                        text: category_label(*cat).to_string(),
                        color: category_color_rgba(*cat, &store.config, alpha),
                    });
                    let cells = build_tabular_cells(rasterizer, font, font_size, store, ms, alpha);
                    update_col_widths(&mut left_col_widths, &cells);
                    lines.push(MeasuredLine {
                        prefix,
                        cells,
                        width: 0.0,
                    });
                }
                for (cat, ms) in groups.iter().skip(mid) {
                    let prefix = Some(TextSpan {
                        text: category_label(*cat).to_string(),
                        color: category_color_rgba(*cat, &store.config, alpha),
                    });
                    let cells = build_tabular_cells(rasterizer, font, font_size, store, ms, alpha);
                    update_col_widths(&mut right_col_widths, &cells);
                    right_lines.push(MeasuredLine {
                        prefix,
                        cells,
                        width: 0.0,
                    });
                }
                finalize_tabular_line_widths(&mut lines, prefix_col_w, &left_col_widths, cell_gap);
                finalize_tabular_line_widths(
                    &mut right_lines,
                    prefix_col_w,
                    &right_col_widths,
                    cell_gap,
                );
            }
            ArrangementMode::Linear => {
                let mut all_spans: Vec<TextSpan> = Vec::new();
                for (gi, (cat, ms)) in groups.iter().enumerate() {
                    if gi > 0 {
                        all_spans.push(TextSpan {
                            text: "  |  ".to_string(),
                            color: rgba_u32(130, 145, 168, alpha),
                        });
                    }
                    all_spans.extend(build_group_spans_inline(store, *cat, ms, alpha));
                }
                let w = Self::measure_spans(rasterizer, font, font_size, &all_spans);
                lines.push(MeasuredLine {
                    prefix: None,
                    cells: vec![MeasuredCell {
                        spans: all_spans,
                        width: w,
                    }],
                    width: w,
                });
            }
        }

        if store.telemetry.benchmark_active {
            let text = format!("REC {:.1}s", store.telemetry.benchmark_elapsed_sec);
            let spans = vec![TextSpan {
                text,
                color: rgba_u32(255, 95, 95, 255),
            }];
            let w = Self::measure_spans(rasterizer, font, font_size, &spans);
            lines.push(MeasuredLine {
                prefix: None,
                cells: vec![MeasuredCell { spans, width: w }],
                width: w,
            });
        }

        let col_gap = if right_lines.is_empty() {
            0.0
        } else {
            (18.0 * eff_scale).round()
        };
        let left_w = lines.iter().map(|l| l.width).fold(0.0_f32, f32::max);
        let right_w = right_lines.iter().map(|l| l.width).fold(0.0_f32, f32::max);
        let graph_h = if store.config.hud_graph_enabled {
            (42.0 * eff_scale).round()
        } else {
            0.0
        };
        let graph_w = if store.config.hud_graph_enabled {
            (196.0 * eff_scale).round()
        } else {
            0.0
        };

        let content_w = (left_w + col_gap + right_w).max(graph_w).max(40.0);
        let row_count = lines.len().max(right_lines.len()).max(1);
        let text_h = (row_count as f32) * line_h + ((row_count.saturating_sub(1)) as f32) * row_gap;
        let extra_graph = if graph_h > 0.0 {
            graph_h + (5.0 * eff_scale).round()
        } else {
            0.0
        };
        let box_w = content_w + pad_x * 2.0;
        let box_h = text_h + extra_graph + pad_y * 2.0;

        (
            box_w,
            box_h,
            HudCardLayout {
                font,
                font_size,
                line_h,
                pad_x,
                pad_y,
                row_gap,
                prefix_col_w,
                cell_gap,
                left_col_widths,
                right_col_widths,
                left_w,
                col_gap,
                graph_h,
                text_h,
                eff_scale,
                lines,
                right_lines,
            },
        )
    }

    #[allow(clippy::too_many_arguments)]
    fn paint_hud_card(
        &self,
        canvas: &mut CpuCanvas<'_>,
        rasterizer: &mut TextRasterizer,
        store: &UiStore,
        ox: f32,
        oy: f32,
        box_w: f32,
        box_h: f32,
        layout: &HudCardLayout,
    ) {
        let bg_a = match store.config.background_style {
            BackgroundStyle::None => 0_u8,
            BackgroundStyle::Translucent => {
                (store.config.bg_alpha.clamp(0.05, 1.0) * 255.0).round() as u8
            }
            BackgroundStyle::Solid => 242_u8,
        };
        let rect = Rect::new(ox, oy, box_w, box_h);
        let radius = (6.0 * layout.eff_scale).round();
        if bg_a > 0 {
            canvas.fill_rounded_rect(rect, radius, rgba_u32(12, 16, 22, bg_a));
        }
        if store.config.border_enabled {
            let border_a = (store.config.border_alpha.clamp(0.05, 1.0) * 255.0).round() as u8;
            let bc = store.config.border_color;
            canvas.stroke_rounded_rect(rect, radius, 1.0, rgba_u32(bc.r, bc.g, bc.b, border_a));
        }

        let baseline_off = layout.font_size * 0.98;
        for (idx, line) in layout.lines.iter().enumerate() {
            let by =
                oy + layout.pad_y + (idx as f32) * (layout.line_h + layout.row_gap) + baseline_off;
            paint_measured_line(
                canvas,
                rasterizer,
                layout,
                ox + layout.pad_x,
                by,
                line,
                &layout.left_col_widths,
            );
        }
        let right_x = ox + layout.pad_x + layout.left_w + layout.col_gap;
        for (idx, line) in layout.right_lines.iter().enumerate() {
            let by =
                oy + layout.pad_y + (idx as f32) * (layout.line_h + layout.row_gap) + baseline_off;
            paint_measured_line(
                canvas,
                rasterizer,
                layout,
                right_x,
                by,
                line,
                &layout.right_col_widths,
            );
        }

        if layout.graph_h > 0.0 {
            let gy = oy + layout.pad_y + layout.text_h + (5.0 * layout.eff_scale).round();
            let gw = (box_w - layout.pad_x * 2.0).max(16.0);
            canvas.draw_frametime_sparkline(
                rasterizer,
                Rect::new(ox + layout.pad_x, gy, gw, layout.graph_h),
                &store.telemetry,
                rgba_u32(72, 140, 220, 150),
            );
        }
    }

    fn render_toast_only(
        &self,
        rasterizer: &mut TextRasterizer,
        text: &str,
        vp_w: u32,
        vp_h: u32,
        eff_scale: f32,
    ) -> CpuHudFrame {
        let font_size = (13.0 * eff_scale).clamp(10.0, 28.0);
        let pad_x = (12.0 * eff_scale).round();
        let pad_y = (6.0 * eff_scale).round();
        let text_w = rasterizer.measure_text(false, font_size, text);
        let box_w = ((text_w + pad_x * 2.0).ceil() as u32).clamp(1, vp_w);
        let box_h = (((font_size * 1.35) + pad_y * 2.0).ceil() as u32).clamp(1, vp_h);
        let margin = (16.0 * eff_scale).round() as i32;
        let dst_x = margin.min(vp_w.saturating_sub(box_w) as i32);
        let dst_y = (vp_h.saturating_sub(box_h) as i32 - margin).max(0);

        let mut rgba = vec![0_u8; (box_w as usize) * (box_h as usize) * 4];
        let mut canvas = CpuCanvas::new(&mut rgba, box_w, box_h, 1.0);
        let radius = (6.0 * eff_scale).round();
        let rect = Rect::new(0.0, 0.0, box_w as f32, box_h as f32);
        canvas.fill_rounded_rect(rect, radius, rgba_u32(16, 22, 30, 215));
        canvas.stroke_rounded_rect(rect, radius, 1.0, rgba_u32(82, 168, 255, 160));
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(pad_x, 0.0, (box_w as f32 - pad_x * 2.0).max(1.0), box_h as f32),
            false,
            font_size,
            rgba_u32(232, 240, 250, 245),
            text,
            false,
        );

        CpuHudFrame {
            dst_x,
            dst_y,
            width: box_w,
            height: box_h,
            rgba,
        }
    }
}

struct HudCardLayout {
    font: FontFamily,
    font_size: f32,
    line_h: f32,
    pad_x: f32,
    pad_y: f32,
    row_gap: f32,
    prefix_col_w: f32,
    cell_gap: f32,
    left_col_widths: Vec<f32>,
    right_col_widths: Vec<f32>,
    left_w: f32,
    col_gap: f32,
    graph_h: f32,
    text_h: f32,
    eff_scale: f32,
    lines: Vec<MeasuredLine>,
    right_lines: Vec<MeasuredLine>,
}

fn paint_measured_line(
    canvas: &mut CpuCanvas<'_>,
    rasterizer: &mut TextRasterizer,
    layout: &HudCardLayout,
    start_x: f32,
    by: f32,
    line: &MeasuredLine,
    col_widths: &[f32],
) {
    let mut x = if let Some(prefix) = &line.prefix {
        canvas.draw_text_font(
            rasterizer,
            start_x,
            by,
            layout.font,
            layout.font_size,
            prefix.color,
            &prefix.text,
        );
        start_x + layout.prefix_col_w
    } else {
        start_x
    };

    for (ci, cell) in line.cells.iter().enumerate() {
        CpuHudRenderer::draw_spans(
            canvas,
            rasterizer,
            layout.font,
            layout.font_size,
            x,
            by,
            &cell.spans,
        );
        let col_w = col_widths.get(ci).copied().unwrap_or(cell.width);
        x += col_w + layout.cell_gap;
    }
}

fn update_col_widths(col_widths: &mut Vec<f32>, cells: &[MeasuredCell]) {
    if col_widths.len() < cells.len() {
        col_widths.resize(cells.len(), 0.0);
    }
    for (i, cell) in cells.iter().enumerate() {
        col_widths[i] = col_widths[i].max(cell.width);
    }
}

fn finalize_tabular_line_widths(
    lines: &mut [MeasuredLine],
    prefix_col_w: f32,
    col_widths: &[f32],
    cell_gap: f32,
) {
    for line in lines {
        if line.cells.is_empty() {
            line.width = prefix_col_w;
            continue;
        }
        let mut total = if line.prefix.is_some() {
            prefix_col_w
        } else {
            0.0
        };
        for (i, cell) in line.cells.iter().enumerate() {
            if i + 1 < line.cells.len() {
                total += col_widths.get(i).copied().unwrap_or(cell.width) + cell_gap;
            } else {
                total += cell.width;
            }
        }
        line.width = total;
    }
}

/// Optional compact muted prefix only for metrics whose unit isn't self-explanatory.
const fn clean_tabular_prefix(metric: MetricType) -> Option<&'static str> {
    match metric {
        MetricType::FpsAvg => Some("AVG: "),
        MetricType::Fps1Low => Some("1%: "),
        MetricType::Fps01Low => Some("0.1%: "),
        MetricType::GpuVramClock => Some("MEM: "),
        MetricType::GpuBandwidth => Some("BW: "),
        MetricType::GpuFan => Some("FAN: "),
        MetricType::CpuAllCoreLoad => Some("CORES: "),
        MetricType::CpuAllCoreClock => Some("ALL: "),
        MetricType::GameRam => Some("GAME: "),
        MetricType::SwapRam => Some("SWAP: "),
        MetricType::MemPressure => Some("PSI: "),
        MetricType::Battery => Some("BAT: "),
        MetricType::DiskIo => Some("DISK: "),
        MetricType::DiskTemp => Some("SSD: "),
        MetricType::NetSpeed => Some("NET: "),
        _ => None,
    }
}

fn split_value_and_unit(val: &str) -> (&str, Option<&str>) {
    const UNITS: &[&str] = &[
        " GB/s", " MB/s", " MHz", " GB", " ms", " W", "°C", "%",
    ];
    for &unit in UNITS {
        if let Some(num) = val.strip_suffix(unit)
            && !num.is_empty()
        {
            return (num, Some(unit));
        }
    }
    (val, None)
}

fn push_value_spans(
    spans: &mut Vec<TextSpan>,
    val: String,
    val_col: u32,
    dim_col: u32,
    dim_enabled: bool,
) {
    if dim_enabled && let (num, Some(unit)) = split_value_and_unit(&val) {
        spans.push(TextSpan {
            text: num.to_string(),
            color: val_col,
        });
        spans.push(TextSpan {
            text: unit.to_string(),
            color: dim_col,
        });
        return;
    }
    spans.push(TextSpan {
        text: val,
        color: val_col,
    });
}

fn build_tabular_cells(
    rasterizer: &mut TextRasterizer,
    font: FontFamily,
    font_size: f32,
    store: &UiStore,
    metrics: &[MetricType],
    alpha: u8,
) -> Vec<MeasuredCell> {
    let dim_enabled = store.config.dim_labels_enabled;
    let lbl_col = if dim_enabled {
        rgba_u32(150, 164, 184, alpha)
    } else {
        color_to_rgba_u32(effective_text_color(&store.config), alpha)
    };
    let mut cells = Vec::with_capacity(metrics.len());
    for &metric in metrics {
        let (_, val, val_col, _) = store.format_metric_row(metric);
        let mut spans = Vec::with_capacity(3);
        if let Some(prefix) = clean_tabular_prefix(metric) {
            spans.push(TextSpan {
                text: prefix.to_string(),
                color: lbl_col,
            });
        }
        push_value_spans(&mut spans, val, val_col, lbl_col, dim_enabled);
        let width = CpuHudRenderer::measure_spans(rasterizer, font, font_size, &spans);
        cells.push(MeasuredCell { spans, width });
    }
    cells
}

fn build_group_spans_inline(
    store: &UiStore,
    cat: MetricCategory,
    metrics: &[MetricType],
    alpha: u8,
) -> Vec<TextSpan> {
    let mut spans = Vec::new();
    let cat_col = category_color_rgba(cat, &store.config, alpha);
    let dim_enabled = store.config.dim_labels_enabled;
    let lbl_col = if dim_enabled {
        rgba_u32(150, 164, 184, alpha)
    } else {
        color_to_rgba_u32(effective_text_color(&store.config), alpha)
    };

    spans.push(TextSpan {
        text: format!("{} ", category_label(cat)),
        color: cat_col,
    });
    for (idx, &metric) in metrics.iter().enumerate() {
        if idx > 0 {
            spans.push(TextSpan {
                text: "  ".to_string(),
                color: lbl_col,
            });
        }
        let (_, val, val_col, _) = store.format_metric_row(metric);
        if let Some(prefix) = clean_tabular_prefix(metric) {
            spans.push(TextSpan {
                text: prefix.to_string(),
                color: lbl_col,
            });
        }
        push_value_spans(&mut spans, val, val_col, lbl_col, dim_enabled);
    }
    spans
}

#[allow(clippy::too_many_arguments)]
fn resolve_hud_origin(
    pos: LayoutPosition,
    custom_x: f32,
    custom_y: f32,
    vp_w: u32,
    vp_h: u32,
    box_w: u32,
    box_h: u32,
    eff_scale: f32,
) -> (i32, i32) {
    let margin = (14.0 * eff_scale).round() as i32;
    let max_x = vp_w.saturating_sub(box_w) as i32;
    let max_y = vp_h.saturating_sub(box_h) as i32;

    let (x, y) = match pos {
        LayoutPosition::TopLeft => (margin, margin),
        LayoutPosition::TopCenter => (max_x / 2, margin),
        LayoutPosition::TopRight => (max_x - margin, margin),
        LayoutPosition::BottomLeft => (margin, max_y - margin),
        LayoutPosition::BottomCenter => (max_x / 2, max_y - margin),
        LayoutPosition::BottomRight => (max_x - margin, max_y - margin),
        LayoutPosition::Custom => (
            (custom_x.clamp(0.0, 1.0) * (max_x as f32)).round() as i32,
            (custom_y.clamp(0.0, 1.0) * (max_y as f32)).round() as i32,
        ),
    };
    (x.clamp(0, max_x), y.clamp(0, max_y))
}
