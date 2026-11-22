//! Reusable Pure-Rust GPUI-CE widgets: floating dropdown popovers, toggle switches,
//! inline color swatch selector, and grouped telemetry sensor cards.

use crate::state::{DEFAULT_ACCENT_TEXT_COLOR, MetricCategory, category_display_title, rgba_u32};
use fpshud_core::{ArrangementMode, BackgroundStyle, ColorRgba, FontFamily, LayoutPosition};
use gpui::{
    AnyElement, Bounds, Context, InteractiveElement, IntoElement, MouseButton, MouseDownEvent,
    ParentElement, Pixels, Styled, canvas, div, px, rgba,
};
use std::cell::Cell;
use std::rc::Rc;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum DropdownId {
    Position = 0,
    Arrangement = 1,
    Scale = 2,
    Font = 3,
    FontSize = 4,
    TextOpacity = 5,
    BgStyle = 6,
    BgOpacity = 7,
}

impl DropdownId {
    pub const COUNT: usize = 8;

    #[must_use]
    pub const fn index(self) -> usize {
        self as usize
    }
}

pub const POSITION_OPTIONS: &[(&str, LayoutPosition)] = &[
    ("Top Left", LayoutPosition::TopLeft),
    ("Top Center", LayoutPosition::TopCenter),
    ("Top Right", LayoutPosition::TopRight),
    ("Bottom Left", LayoutPosition::BottomLeft),
    ("Bottom Center", LayoutPosition::BottomCenter),
    ("Bottom Right", LayoutPosition::BottomRight),
    ("Custom", LayoutPosition::Custom),
];

pub const ARRANGEMENT_OPTIONS: &[(&str, ArrangementMode)] = &[
    ("Stack", ArrangementMode::Stack),
    ("Double", ArrangementMode::Double),
    ("Linear", ArrangementMode::Linear),
];

pub const SCALE_OPTIONS: &[f32] = &[0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0];

pub const FONT_OPTIONS: &[(&str, FontFamily)] = &[
    ("Open Sans Bold", FontFamily::SansBold),
    ("Vera Mono Bold", FontFamily::VeraMono),
];

pub const FONT_SIZE_OPTIONS: &[u8] = &[12, 14, 16, 18, 20, 22, 24, 28, 32, 36];

pub const TEXT_OPACITY_PCTS: [u8; 5] = [20, 40, 60, 80, 100];

pub const BG_STYLE_OPTIONS: &[(&str, BackgroundStyle)] = &[
    ("None", BackgroundStyle::None),
    ("Translucent", BackgroundStyle::Translucent),
    ("Solid", BackgroundStyle::Solid),
];

pub const BG_OPACITY_PCTS: [u8; 20] = [
    5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100,
];

pub const TEXT_COLOR_SWATCHES: [(&str, ColorRgba); 4] = [
    ("Ice Cyan", DEFAULT_ACCENT_TEXT_COLOR),
    ("Mint Green", ColorRgba::new(150, 255, 165, 255)),
    ("Amber Gold", ColorRgba::new(255, 220, 120, 255)),
    ("White", ColorRgba::new(255, 255, 255, 255)),
];

/// Computes floating popover top-left `(x, y)` clamped inside the viewport, flipping above the
/// anchor when opening downward would clip off-screen (e.g. bottom of 7-inch 720p displays).
#[must_use]
pub fn clamp_popover_origin(
    anchor: Bounds<Pixels>,
    popup_w: f32,
    popup_h: f32,
    viewport_w: f32,
    viewport_h: f32,
) -> (f32, f32) {
    const MARGIN: f32 = 8.0;
    const GAP: f32 = 4.0;
    let ax = f32::from(anchor.origin.x);
    let ay = f32::from(anchor.origin.y);
    let ah = f32::from(anchor.size.height);

    let below_y = ay + ah + GAP;
    let raw_y = if below_y + popup_h <= viewport_h - MARGIN {
        below_y
    } else {
        ay - popup_h - GAP
    };
    let max_x = (viewport_w - popup_w - MARGIN).max(MARGIN);
    let max_y = (viewport_h - popup_h - MARGIN).max(MARGIN);
    (ax.clamp(MARGIN, max_x), raw_y.clamp(MARGIN, max_y))
}

/// Renders a single labeled dropdown trigger row (`Label` on left, `Value ▾` button on right)
/// and records its screen bounds into `anchor_cell` for deferred popover positioning.
pub fn dropdown_trigger_row<V: 'static>(
    label: &str,
    value_text: String,
    is_open: bool,
    anchor_cell: Rc<Cell<Bounds<Pixels>>>,
    on_toggle: impl Fn(&mut V, &mut Context<V>) + 'static,
    cx: &mut Context<V>,
) -> AnyElement {
    let border = if is_open {
        rgba_u32(110, 185, 255, 240)
    } else {
        rgba_u32(68, 86, 114, 195)
    };
    let bg = if is_open {
        rgba_u32(34, 48, 72, 240)
    } else {
        rgba_u32(26, 33, 46, 230)
    };
    let handler = Rc::new(on_toggle);

    div()
        .flex()
        .flex_row()
        .items_center()
        .justify_between()
        .gap(px(6.0))
        .h(px(23.0))
        .child(
            div()
                .text_size(px(11.0))
                .text_color(rgba(rgba_u32(168, 188, 214, 245)))
                .child(label.to_string()),
        )
        .child(
            div()
                .relative()
                .w(px(142.0))
                .h(px(22.0))
                .px(px(7.0))
                .rounded(px(4.0))
                .bg(rgba(bg))
                .border_1()
                .border_color(rgba(border))
                .flex()
                .flex_row()
                .items_center()
                .justify_between()
                .text_size(px(11.0))
                .text_color(rgba(rgba_u32(236, 242, 252, 255)))
                .on_mouse_down(
                    MouseButton::Left,
                    cx.listener(move |view, _ev: &MouseDownEvent, _, cx| {
                        cx.stop_propagation();
                        handler(view, cx);
                    }),
                )
                .child(
                    canvas(
                        move |bounds, _, _| anchor_cell.set(bounds),
                        |_, _, _, _| {},
                    )
                    .size_full()
                    .absolute(),
                )
                .child(div().overflow_hidden().child(value_text))
                .child(
                    div()
                        .text_color(rgba(rgba_u32(148, 180, 224, 240)))
                        .child(if is_open { "\u{25B4}" } else { "\u{25BE}" }),
                ),
        )
        .into_any_element()
}

/// Renders an occluded floating dropdown menu panel arranged into `columns` per row.
pub fn popover_grid_panel(
    origin_x: f32,
    origin_y: f32,
    popup_w: f32,
    columns: usize,
    bounds_cell: Rc<Cell<Bounds<Pixels>>>,
    items: Vec<AnyElement>,
) -> AnyElement {
    let cols = columns.max(1);
    let mut rows: Vec<AnyElement> = Vec::new();
    let mut iter = items.into_iter();
    loop {
        let mut row_items = Vec::with_capacity(cols);
        for _ in 0..cols {
            if let Some(el) = iter.next() {
                row_items.push(div().flex_1().min_w(px(0.0)).child(el).into_any_element());
            } else {
                break;
            }
        }
        if row_items.is_empty() {
            break;
        }
        while row_items.len() < cols {
            row_items.push(div().flex_1().min_w(px(0.0)).into_any_element());
        }
        rows.push(
            div()
                .flex()
                .flex_row()
                .gap(px(3.0))
                .w_full()
                .children(row_items)
                .into_any_element(),
        );
    }

    div()
        .absolute()
        .left(px(origin_x))
        .top(px(origin_y))
        .w(px(popup_w))
        .p(px(5.0))
        .rounded(px(6.0))
        .bg(rgba(rgba_u32(16, 22, 34, 250)))
        .border_1()
        .border_color(rgba(rgba_u32(96, 156, 232, 235)))
        .occlude()
        .on_mouse_down(MouseButton::Left, |_, _, cx| {
            cx.stop_propagation();
        })
        .child(
            canvas(
                move |bounds, _, _| bounds_cell.set(bounds),
                |_, _, _, _| {},
            )
            .size_full()
            .absolute(),
        )
        .child(div().flex().flex_col().gap(px(3.0)).w_full().children(rows))
        .into_any_element()
}

/// Renders a single selectable option inside a dropdown popover.
pub fn popover_option_item<V: 'static>(
    label: impl Into<String>,
    selected: bool,
    on_select: impl Fn(&mut V, &mut Context<V>) + 'static,
    cx: &mut Context<V>,
) -> AnyElement {
    let bg = if selected {
        rgba_u32(42, 114, 212, 245)
    } else {
        rgba_u32(28, 36, 52, 235)
    };
    let border = if selected {
        rgba_u32(146, 206, 255, 240)
    } else {
        rgba_u32(58, 72, 96, 170)
    };
    let handler = Rc::new(on_select);
    div()
        .w_full()
        .h(px(23.0))
        .px(px(6.0))
        .rounded(px(4.0))
        .bg(rgba(bg))
        .border_1()
        .border_color(rgba(border))
        .flex()
        .items_center()
        .justify_center()
        .text_size(px(11.0))
        .text_color(rgba(rgba_u32(240, 246, 255, 255)))
        .on_mouse_down(
            MouseButton::Left,
            cx.listener(move |view, _ev: &MouseDownEvent, _, cx| {
                cx.stop_propagation();
                handler(view, cx);
            }),
        )
        .child(label.into())
        .into_any_element()
}

fn render_switch_pill(enabled: bool) -> AnyElement {
    let track_bg = if enabled {
        rgba_u32(42, 114, 212, 245)
    } else {
        rgba_u32(40, 49, 66, 230)
    };
    let track_border = if enabled {
        rgba_u32(140, 200, 255, 230)
    } else {
        rgba_u32(78, 92, 114, 190)
    };
    let thumb_bg = if enabled {
        rgba_u32(255, 255, 255, 255)
    } else {
        rgba_u32(156, 172, 194, 245)
    };
    let thumb_left = if enabled { 18.0 } else { 2.0 };

    div()
        .relative()
        .w(px(34.0))
        .h(px(18.0))
        .rounded(px(9.0))
        .bg(rgba(track_bg))
        .border_1()
        .border_color(rgba(track_border))
        .flex_shrink_0()
        .child(
            div()
                .absolute()
                .top(px(2.0))
                .left(px(thumb_left))
                .w(px(12.0))
                .h(px(12.0))
                .rounded(px(6.0))
                .bg(rgba(thumb_bg)),
        )
        .into_any_element()
}

/// Renders a standard label + `34×18` toggle switch row.
pub fn switch_toggle_row<V: 'static>(
    label: &str,
    enabled: bool,
    on_toggle: impl Fn(&mut V, &mut Context<V>) + 'static,
    cx: &mut Context<V>,
) -> AnyElement {
    let handler = Rc::new(on_toggle);
    div()
        .flex()
        .flex_row()
        .items_center()
        .justify_between()
        .h(px(22.0))
        .on_mouse_down(
            MouseButton::Left,
            cx.listener(move |view, _ev: &MouseDownEvent, _, cx| {
                cx.stop_propagation();
                handler(view, cx);
            }),
        )
        .child(
            div()
                .text_size(px(11.0))
                .text_color(rgba(rgba_u32(214, 226, 242, 250)))
                .child(label.to_string()),
        )
        .child(render_switch_pill(enabled))
        .into_any_element()
}

/// Renders the `Text Color` row with 4 inline `15×15` color swatches and a `34×18` ON/OFF switch.
pub fn text_color_switch_row<V: 'static>(
    enabled: bool,
    current_color: ColorRgba,
    on_pick_swatch: impl Fn(&mut V, ColorRgba, &mut Context<V>) + 'static,
    on_toggle: impl Fn(&mut V, &mut Context<V>) + 'static,
    cx: &mut Context<V>,
) -> AnyElement {
    let pick_cb = Rc::new(on_pick_swatch);
    let toggle_cb = Rc::new(on_toggle);

    let swatches: Vec<AnyElement> = TEXT_COLOR_SWATCHES
        .iter()
        .map(|(_, col)| {
            let color = *col;
            let active = enabled && current_color == color;
            let border = if active {
                rgba_u32(255, 255, 255, 255)
            } else {
                rgba_u32(72, 88, 112, 200)
            };
            let cb = Rc::clone(&pick_cb);
            div()
                .w(px(15.0))
                .h(px(15.0))
                .rounded(px(3.0))
                .bg(rgba(rgba_u32(color.r, color.g, color.b, 255)))
                .border_1()
                .border_color(rgba(border))
                .on_mouse_down(
                    MouseButton::Left,
                    cx.listener(move |view, _ev: &MouseDownEvent, _, cx| {
                        cx.stop_propagation();
                        cb(view, color, cx);
                    }),
                )
                .into_any_element()
        })
        .collect();

    div()
        .flex()
        .flex_row()
        .items_center()
        .justify_between()
        .gap(px(4.0))
        .h(px(24.0))
        .child(
            div()
                .text_size(px(11.0))
                .text_color(rgba(rgba_u32(214, 226, 242, 250)))
                .child("Text Color"),
        )
        .child(
            div()
                .flex()
                .flex_row()
                .items_center()
                .gap(px(6.0))
                .child(div().flex().flex_row().items_center().gap(px(4.0)).children(swatches))
                .child(
                    div()
                        .on_mouse_down(
                            MouseButton::Left,
                            cx.listener(move |view, _ev: &MouseDownEvent, _, cx| {
                                cx.stop_propagation();
                                toggle_cb(view, cx);
                            }),
                        )
                        .child(render_switch_pill(enabled)),
                ),
        )
        .into_any_element()
}

/// Renders a bordered sensor category card with colored title, `active/total` badge, and chip grid.
pub fn sensor_group_card(
    cat: MetricCategory,
    active_count: usize,
    total_count: usize,
    accent_rgba: u32,
    columns: usize,
    chips: Vec<AnyElement>,
) -> AnyElement {
    let cols = columns.max(1);
    let mut rows: Vec<AnyElement> = Vec::new();
    let mut iter = chips.into_iter();
    loop {
        let mut row_items = Vec::with_capacity(cols);
        for _ in 0..cols {
            if let Some(el) = iter.next() {
                row_items.push(div().flex_1().min_w(px(0.0)).child(el).into_any_element());
            } else {
                break;
            }
        }
        if row_items.is_empty() {
            break;
        }
        while row_items.len() < cols {
            row_items.push(div().flex_1().min_w(px(0.0)).into_any_element());
        }
        rows.push(
            div()
                .flex()
                .flex_row()
                .gap(px(3.0))
                .w_full()
                .children(row_items)
                .into_any_element(),
        );
    }

    div()
        .flex()
        .flex_col()
        .gap(px(3.0))
        .p(px(5.0))
        .rounded(px(6.0))
        .bg(rgba(rgba_u32(15, 20, 30, 215)))
        .border_1()
        .border_color(rgba(rgba_u32(62, 78, 102, 165)))
        .child(
            div()
                .flex()
                .flex_row()
                .items_center()
                .justify_between()
                .px(px(2.0))
                .child(
                    div()
                        .text_size(px(11.0))
                        .text_color(rgba(accent_rgba))
                        .child(category_display_title(cat)),
                )
                .child(
                    div()
                        .text_size(px(10.0))
                        .text_color(rgba(rgba_u32(146, 164, 190, 225)))
                        .child(format!("{active_count}/{total_count}")),
                ),
        )
        .children(rows)
        .into_any_element()
}
