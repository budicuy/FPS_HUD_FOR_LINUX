use crate::state::{
    UiStore, category_color_rgba, category_label, color_to_rgba_u32, font_family_name,
    metric_category, rgba_u32,
};
use fpshud_core::{BackgroundStyle, LayoutPosition, MetricType};
use fpshud_telemetry::TelemetrySnapshot;
use gpui::{AnyElement, Div, IntoElement, ParentElement, Styled, div, px, rgba};

/// Renders a bar sparkline from `snap.frametime_history`; handles `0`, `1`, or `120` points safely.
#[must_use]
pub fn render_frametime_sparkline(
    snap: &TelemetrySnapshot,
    width_px: f32,
    height_px: f32,
    border_col: u32,
) -> AnyElement {
    let count = usize::from(snap.frametime_history_len).min(snap.frametime_history.len());
    let samples = &snap.frametime_history[..count];
    let max_ms = samples
        .iter()
        .copied()
        .filter(|v| v.is_finite() && *v > 0.0)
        .fold(16.7_f32, f32::max);

    let inner_h = (height_px - 6.0).max(8.0);
    let inner_w = (width_px - 8.0).max(20.0);
    let bar_w = if count == 0 {
        2.0
    } else {
        (inner_w / (count as f32)).max(1.5)
    };

    let bars: Vec<AnyElement> = samples
        .iter()
        .map(|&ft| {
            let clean = if ft.is_finite() && ft > 0.0 { ft } else { 0.0 };
            let frac = (clean / max_ms).clamp(0.08, 1.0);
            let bar_h = (frac * inner_h).max(2.0);
            let bar_color = if clean > 33.3 {
                rgba_u32(255, 70, 70, 230)
            } else if clean > 22.2 {
                rgba_u32(255, 185, 0, 230)
            } else {
                rgba_u32(80, 225, 140, 225)
            };
            div()
                .w(px(bar_w))
                .h(px(bar_h))
                .bg(rgba(bar_color))
                .into_any_element()
        })
        .collect();

    div()
        .w(px(width_px))
        .h(px(height_px))
        .px(px(4.0))
        .py(px(3.0))
        .rounded(px(4.0))
        .bg(rgba(rgba_u32(10, 14, 20, 190)))
        .border_1()
        .border_color(rgba(border_col))
        .flex()
        .flex_row()
        .items_end()
        .gap(px(1.0))
        .overflow_hidden()
        .children(bars)
        .into_any_element()
}

fn hud_bg_color(store: &UiStore) -> u32 {
    match store.config.background_style {
        BackgroundStyle::None => rgba_u32(0, 0, 0, 0),
        BackgroundStyle::Translucent => {
            let alpha = (store.config.bg_alpha.clamp(0.05, 0.95) * 255.0).round() as u8;
            rgba_u32(12, 16, 22, alpha)
        }
        BackgroundStyle::Solid => rgba_u32(12, 16, 22, 238),
    }
}

fn build_metric_item(
    store: &UiStore,
    metric: MetricType,
    font_size: f32,
    alpha: u8,
) -> AnyElement {
    let (label, value, val_color, is_warn) = store.format_metric_row(metric);
    let cat = metric_category(metric);
    let label_color = if store.config.category_colors_enabled {
        category_color_rgba(cat, &store.config, alpha)
    } else if store.config.dim_labels_enabled {
        let mut c = store.config.text_color;
        c.r = (f32::from(c.r) * 0.72) as u8;
        c.g = (f32::from(c.g) * 0.72) as u8;
        c.b = (f32::from(c.b) * 0.72) as u8;
        color_to_rgba_u32(c, alpha)
    } else {
        color_to_rgba_u32(store.config.text_color, alpha)
    };

    let row_bg = if is_warn {
        rgba_u32(110, 24, 24, 70)
    } else {
        rgba_u32(0, 0, 0, 0)
    };

    div()
        .flex()
        .flex_row()
        .items_center()
        .gap(px(6.0))
        .px(px(4.0))
        .py(px(1.0))
        .rounded(px(3.0))
        .bg(rgba(row_bg))
        .text_size(px(font_size))
        .child(
            div()
                .text_color(rgba(label_color))
                .child(format!("[{}] {label}", category_label(cat))),
        )
        .child(div().text_color(rgba(val_color)).child(value))
        .into_any_element()
}

fn layout_hud_panel(store: &UiStore, viewport_w: f32, viewport_h: f32, card: Div) -> AnyElement {
    let mx = store.config.margin_x.clamp(0.0, 240.0);
    let my = store.config.margin_y.clamp(0.0, 240.0);

    let wrapper = div().w(px(viewport_w)).h(px(viewport_h)).flex();
    match store.config.position {
        LayoutPosition::TopLeft => wrapper.items_start().justify_start().pt(px(my)).pl(px(mx)).child(card),
        LayoutPosition::TopCenter => wrapper.items_start().justify_center().pt(px(my)).child(card),
        LayoutPosition::TopRight => wrapper.items_start().justify_end().pt(px(my)).pr(px(mx)).child(card),
        LayoutPosition::BottomLeft => wrapper.items_end().justify_start().pb(px(my)).pl(px(mx)).child(card),
        LayoutPosition::BottomCenter => wrapper.items_end().justify_center().pb(px(my)).child(card),
        LayoutPosition::BottomRight => wrapper.items_end().justify_end().pb(px(my)).pr(px(mx)).child(card),
        LayoutPosition::Custom => {
            let x = (store.config.custom_pos_x.clamp(0.0, 0.92) * viewport_w).max(0.0);
            let y = (store.config.custom_pos_y.clamp(0.0, 0.92) * viewport_h).max(0.0);
            wrapper
                .items_start()
                .justify_start()
                .pl(px(x))
                .pt(px(y))
                .child(card)
        }
    }
    .into_any_element()
}

#[must_use]
pub fn render_hud(store: &UiStore, viewport_w: f32, viewport_h: f32) -> Option<AnyElement> {
    if !store.config.hud_visible {
        return None;
    }

    let font_size = (store.config.font_size_px * store.config.gui_scale).clamp(11.0, 36.0);
    let alpha = (store.config.text_opacity.clamp(0.1, 1.0) * 255.0).round() as u8;
    let items: Vec<AnyElement> = store
        .config
        .metrics
        .iter()
        .copied()
        .map(|m| build_metric_item(store, m, font_size * 0.72, alpha))
        .collect();

    let metrics_body = match store.config.arrangement {
        fpshud_core::ArrangementMode::Stack => {
            div().flex().flex_col().gap(px(2.0)).children(items)
        }
        fpshud_core::ArrangementMode::Double => {
            let mid = items.len().div_ceil(2);
            let mut left = Vec::with_capacity(mid);
            let mut right = Vec::with_capacity(items.len().saturating_sub(mid));
            for (idx, item) in items.into_iter().enumerate() {
                if idx < mid {
                    left.push(item);
                } else {
                    right.push(item);
                }
            }
            div()
                .flex()
                .flex_row()
                .items_start()
                .gap(px(12.0))
                .child(div().flex().flex_col().gap(px(2.0)).children(left))
                .child(div().flex().flex_col().gap(px(2.0)).children(right))
        }
        fpshud_core::ArrangementMode::Linear => {
            div().flex().flex_row().flex_wrap().items_center().gap(px(8.0)).children(items)
        }
    };

    let bg = hud_bg_color(store);
    let border = if store.config.background_style == BackgroundStyle::None {
        rgba_u32(0, 0, 0, 0)
    } else {
        rgba_u32(90, 125, 165, 95)
    };

    let mut card = div()
        .flex()
        .flex_col()
        .gap(px(4.0))
        .p(px(8.0))
        .rounded(px(6.0))
        .bg(rgba(bg))
        .border_1()
        .border_color(rgba(border))
        .font_family(font_family_name(store.config.font_family))
        .child(metrics_body);

    if store.config.hud_graph_enabled {
        card = card.child(render_frametime_sparkline(
            &store.telemetry,
            180.0,
            34.0,
            rgba_u32(72, 140, 220, 110),
        ));
    }

    if store.telemetry.benchmark_active {
        card = card.child(
            div()
                .text_size(px(12.0))
                .text_color(rgba(rgba_u32(255, 200, 75, 240)))
                .child(format!("BENCHMARK REC: {:.1}s", store.telemetry.benchmark_elapsed_sec)),
        );
    }

    Some(layout_hud_panel(store, viewport_w, viewport_h, card))
}
