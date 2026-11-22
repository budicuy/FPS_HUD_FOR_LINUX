use crate::state::{
    MetricCategory, UiStore, category_color_rgba, category_label, color_to_rgba_u32,
    effective_text_color, font_family_name, group_metrics_ordered, hud_metric_short_label,
    rgba_u32,
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
    let count = snap.frametime_history.len();
    let samples = snap.frametime_history.as_slice();
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

fn build_metric_chip(
    store: &UiStore,
    metric: MetricType,
    item_font_px: f32,
    alpha: u8,
) -> AnyElement {
    let (_, value, val_color, _is_warn) = store.format_metric_row(metric);
    let sub_label = hud_metric_short_label(metric);
    let sub_color = dimmed_text_rgba(store, alpha);

    let mut chip = div()
        .flex()
        .flex_row()
        .items_center()
        .gap(px(4.0))
        .px(px(1.0))
        .text_size(px(item_font_px));

    // For instant FPS, render the bare value cleanly after the FPS group badge (e.g. "FPS | 59 · Avg 60 · FT 16.8 ms").
    if metric != MetricType::Fps {
        chip = chip.child(div().text_color(rgba(sub_color)).child(sub_label));
    }
    chip.child(div().text_color(rgba(val_color)).child(value))
        .into_any_element()
}

fn build_group_items_row(
    store: &UiStore,
    metrics: &[MetricType],
    item_font_px: f32,
    alpha: u8,
    max_val_w: Option<f32>,
) -> AnyElement {
    let dot_color = rgba_u32(130, 148, 172, alpha);
    let mut children: Vec<AnyElement> = Vec::with_capacity(metrics.len() * 2);
    for (idx, &metric) in metrics.iter().enumerate() {
        if idx > 0 {
            children.push(
                div()
                    .text_size(px(item_font_px))
                    .text_color(rgba(dot_color))
                    .px(px(2.0))
                    .child("\u{00B7}")
                    .into_any_element(),
            );
        }
        children.push(build_metric_chip(store, metric, item_font_px, alpha));
    }
    let mut row = div()
        .flex()
        .flex_row()
        .flex_wrap()
        .items_center()
        .gap_x(px(3.0))
        .gap_y(px(1.0))
        .children(children);
    if let Some(mw) = max_val_w {
        row = row.max_w(px(mw));
    }
    row.into_any_element()
}

fn render_grouped_row(
    store: &UiStore,
    cat: MetricCategory,
    metrics: &[MetricType],
    item_font_px: f32,
    alpha: u8,
    max_val_w: f32,
) -> AnyElement {
    let cat_color = if store.config.category_colors_enabled {
        category_color_rgba(cat, &store.config, alpha)
    } else {
        dimmed_text_rgba(store, alpha)
    };
    let badge_w = (item_font_px * 2.55).max(34.0);

    div()
        .flex()
        .flex_row()
        .items_start()
        .gap(px(6.0))
        .py(px(1.0))
        .child(
            div()
                .w(px(badge_w))
                .flex_shrink_0()
                .text_size(px(item_font_px))
                .text_color(rgba(cat_color))
                .child(category_label(cat)),
        )
        .child(build_group_items_row(
            store,
            metrics,
            item_font_px,
            alpha,
            Some(max_val_w),
        ))
        .into_any_element()
}

fn render_linear_group_chip(
    store: &UiStore,
    cat: MetricCategory,
    metrics: &[MetricType],
    item_font_px: f32,
    alpha: u8,
) -> AnyElement {
    let cat_color = if store.config.category_colors_enabled {
        category_color_rgba(cat, &store.config, alpha)
    } else {
        dimmed_text_rgba(store, alpha)
    };

    div()
        .flex()
        .flex_row()
        .items_center()
        .gap(px(5.0))
        .child(
            div()
                .text_size(px(item_font_px))
                .text_color(rgba(cat_color))
                .child(category_label(cat)),
        )
        .child(build_group_items_row(store, metrics, item_font_px, alpha, None))
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

    let font_size = store.config.font_size_px.clamp(11.0, 36.0);
    let item_font_px = font_size * 0.72;
    let alpha = (store.config.text_opacity.clamp(0.1, 1.0) * 255.0).round() as u8;
    let groups = group_metrics_ordered(&store.config.metrics);

    let metrics_body = match store.config.arrangement {
        fpshud_core::ArrangementMode::Stack => {
            let max_val_w = (viewport_w * 0.62).max(400.0);
            let rows: Vec<AnyElement> = groups
                .iter()
                .map(|(cat, ms)| render_grouped_row(store, *cat, ms, item_font_px, alpha, max_val_w))
                .collect();
            div().flex().flex_col().gap(px(3.0)).children(rows)
        }
        fpshud_core::ArrangementMode::Double => {
            let max_val_w = (viewport_w * 0.42).max(280.0);
            let mid = groups.len().div_ceil(2);
            let left: Vec<AnyElement> = groups
                .iter()
                .take(mid)
                .map(|(cat, ms)| render_grouped_row(store, *cat, ms, item_font_px, alpha, max_val_w))
                .collect();
            let right: Vec<AnyElement> = groups
                .iter()
                .skip(mid)
                .map(|(cat, ms)| render_grouped_row(store, *cat, ms, item_font_px, alpha, max_val_w))
                .collect();
            div()
                .flex()
                .flex_row()
                .items_start()
                .gap(px(14.0))
                .child(div().flex().flex_col().gap(px(3.0)).children(left))
                .child(div().flex().flex_col().gap(px(3.0)).children(right))
        }
        fpshud_core::ArrangementMode::Linear => {
            let sep_color = rgba_u32(110, 128, 152, (u16::from(alpha) * 3 / 5) as u8);
            let mut chips: Vec<AnyElement> = Vec::with_capacity(groups.len() * 2);
            for (idx, (cat, ms)) in groups.iter().enumerate() {
                if idx > 0 {
                    chips.push(
                        div()
                            .text_size(px(item_font_px))
                            .text_color(rgba(sep_color))
                            .px(px(3.0))
                            .child("|")
                            .into_any_element(),
                    );
                }
                chips.push(render_linear_group_chip(store, *cat, ms, item_font_px, alpha));
            }
            div()
                .flex()
                .flex_row()
                .flex_wrap()
                .items_center()
                .gap_x(px(6.0))
                .gap_y(px(4.0))
                .max_w(px((viewport_w - 32.0).max(320.0)))
                .children(chips)
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
                .child(format!(
                    "BENCHMARK REC: {:.1}s",
                    store.telemetry.benchmark_elapsed_sec
                )),
        );
    }

    Some(layout_hud_panel(store, viewport_w, viewport_h, card))
}
