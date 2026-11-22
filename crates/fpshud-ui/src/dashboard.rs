use crate::hud::render_frametime_sparkline;
use crate::state::{
    MetricCategory, SensorFilter, UiStore, arrangement_display_name, bg_style_display_name,
    category_color_rgba, font_dropdown_label, format_benchmark_summary, hud_metric_short_label,
    metric_category, position_display_name, rgba_u32,
};
use crate::widgets::{
    ARRANGEMENT_OPTIONS, BG_OPACITY_PCTS, BG_STYLE_OPTIONS, DropdownId, FONT_OPTIONS,
    FONT_SIZE_OPTIONS, POSITION_OPTIONS, SCALE_OPTIONS, TEXT_OPACITY_PCTS, clamp_popover_origin,
    dropdown_trigger_row, popover_grid_panel, popover_option_item, sensor_group_card,
    switch_toggle_row, text_color_switch_row,
};
use fpshud_core::{LayoutPosition, MetricType, PresetType};
use fpshud_input::KeybindTarget;
use gpui::{
    AnyElement, Bounds, Context, InteractiveElement, IntoElement, MouseButton, MouseDownEvent,
    ParentElement, Pixels, Styled, canvas, div, px, rgba,
};
use std::cell::Cell;
use std::rc::Rc;

const COL1_WIDTH_PX: f32 = 264.0;
const COL3_WIDTH_PX: f32 = 208.0;
const DASHBOARD_CHROME_PX: f32 = 44.0;
const PREVIEW_W: f32 = 248.0;
const PREVIEW_H: f32 = 116.0;
const PREVIEW_HANDLE_PX: f32 = 22.0;

#[must_use]
pub fn compute_dashboard_width(viewport_w: f32) -> f32 {
    (viewport_w - 24.0).clamp(720.0, 1140.0)
}

#[must_use]
pub fn compute_dashboard_col2_width(viewport_w: f32) -> f32 {
    (compute_dashboard_width(viewport_w) - COL1_WIDTH_PX - COL3_WIDTH_PX - DASHBOARD_CHROME_PX)
        .max(380.0)
}

/// Computes the top-left dashboard panel origin, centered by default and clamped inside the viewport.
#[must_use]
pub fn compute_dashboard_position(
    viewport_w: f32,
    viewport_h: f32,
    dash_w: f32,
    dash_h: f32,
    offset_x: f32,
    offset_y: f32,
) -> (f32, f32) {
    let max_x = (viewport_w - dash_w).max(0.0);
    let max_y = (viewport_h - dash_h).max(0.0);
    let base_x = max_x * 0.5;
    let base_y = max_y * 0.5;
    (
        (base_x + offset_x).clamp(0.0, max_x),
        (base_y + offset_y).clamp(0.0, max_y),
    )
}

fn truncate_chars(text: &str, max_chars: usize) -> String {
    let count = text.chars().count();
    if count <= max_chars || max_chars <= 3 {
        return text.to_string();
    }
    let prefix: String = text.chars().take(max_chars - 3).collect();
    format!("{prefix}...")
}

/// Maps an absolute window cursor coordinate inside preview box bounds into clamped `[0.0, 1.0]`.
#[must_use]
pub fn map_preview_point_to_norm(
    pos_x: f32,
    pos_y: f32,
    origin_x: f32,
    origin_y: f32,
    width: f32,
    height: f32,
) -> (f32, f32) {
    let w = width.max(1.0);
    let h = height.max(1.0);
    let nx = ((pos_x - origin_x) / w).clamp(0.0, 1.0);
    let ny = ((pos_y - origin_y) / h).clamp(0.0, 1.0);
    (nx, ny)
}

/// Selects which visual layers `FpshudRootView` paints inside a GPUI offscreen viewport.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum UiRenderMode {
    /// Renders HUD, launch toast, Settings dashboard, and fallback software cursor (X11/headless).
    #[default]
    Full,
    /// Renders only the in-game HUD and launch toast (while Settings dashboard runs in its own Wayland overlay).
    HudOnly,
    /// Renders only the interactive Settings dashboard inside the isolated Wayland overlay surface.
    DashboardOnly,
}

pub struct FpshudRootView {
    pub(crate) store: UiStore,
    pub(crate) last_applied_seq: u64,
    pub(crate) last_cursor_seq: u64,
    pub(crate) last_render_mode_seq: u64,
    pub(crate) last_clear_capture_seq: u64,
    pub(crate) render_mode: UiRenderMode,
    pub(crate) draw_software_cursor: bool,
    pub(crate) started_at: std::time::Instant,
    pub(crate) preview_bounds: Rc<Cell<Bounds<Pixels>>>,
    pub(crate) dashboard_bounds: Rc<Cell<Bounds<Pixels>>>,
    pub(crate) popover_bounds: Rc<Cell<Bounds<Pixels>>>,
    pub(crate) dropdown_anchors: [Rc<Cell<Bounds<Pixels>>>; DropdownId::COUNT],
    pub(crate) open_dropdown: Option<DropdownId>,
    pub(crate) dragging_preview: bool,
    pub(crate) dragging_gui: bool,
    pub(crate) gui_pos: Option<(f32, f32)>,
    pub(crate) drag_gui_start_cursor: (f32, f32),
    pub(crate) drag_gui_start_origin: (f32, f32),
}

impl FpshudRootView {
    #[must_use]
    pub fn new(store: UiStore) -> Self {
        Self {
            store,
            last_applied_seq: 0,
            last_cursor_seq: 0,
            last_render_mode_seq: 0,
            last_clear_capture_seq: 0,
            render_mode: UiRenderMode::Full,
            draw_software_cursor: false,
            started_at: std::time::Instant::now(),
            preview_bounds: Rc::new(Cell::new(Bounds::default())),
            dashboard_bounds: Rc::new(Cell::new(Bounds::default())),
            popover_bounds: Rc::new(Cell::new(Bounds::default())),
            dropdown_anchors: std::array::from_fn(|_| Rc::new(Cell::new(Bounds::default()))),
            open_dropdown: None,
            dragging_preview: false,
            dragging_gui: false,
            gui_pos: None,
            drag_gui_start_cursor: (0.0, 0.0),
            drag_gui_start_origin: (0.0, 0.0),
        }
    }

    #[must_use]
    pub fn software_cursor_enabled(&self) -> bool {
        self.draw_software_cursor
    }

    #[must_use]
    pub fn active_keybind_capture_snapshot(&self) -> KeybindTarget {
        self.store.active_keybind_capture
    }

    pub fn request_keybind_capture_for_test(&mut self, target: KeybindTarget) {
        self.store.request_keybind_capture(target);
    }

    #[must_use]
    pub fn dashboard_bounds_snapshot(&self) -> Bounds<Pixels> {
        self.dashboard_bounds.get()
    }

    #[must_use]
    pub fn preview_bounds_snapshot(&self) -> Bounds<Pixels> {
        self.preview_bounds.get()
    }

    #[must_use]
    pub fn open_dropdown_snapshot(&self) -> Option<DropdownId> {
        self.open_dropdown
    }

    #[must_use]
    pub fn dropdown_anchor_snapshot(&self, id: DropdownId) -> Bounds<Pixels> {
        self.dropdown_anchors[id.index()].get()
    }

    #[must_use]
    pub fn active_popover_bounds_snapshot(&self) -> Bounds<Pixels> {
        self.popover_bounds.get()
    }

    fn toggle_dropdown(&mut self, id: DropdownId, cx: &mut Context<Self>) {
        self.open_dropdown = if self.open_dropdown == Some(id) {
            None
        } else {
            Some(id)
        };
        cx.notify();
    }

    pub(crate) fn update_preview_pos_from_point(
        &mut self,
        pos_x: f32,
        pos_y: f32,
        cx: &mut Context<Self>,
    ) {
        let b = self.preview_bounds.get();
        let w = f32::from(b.size.width);
        let h = f32::from(b.size.height);
        let (nx, ny) = if w > 1.0 && h > 1.0 {
            map_preview_point_to_norm(
                pos_x,
                pos_y,
                f32::from(b.origin.x),
                f32::from(b.origin.y),
                w,
                h,
            )
        } else {
            (0.5, 0.5)
        };
        self.store.set_custom_pos(nx, ny);
        self.flush_outgoing(cx);
    }

    pub(crate) fn resolved_dashboard_size(&self, viewport_w: f32) -> (f32, f32) {
        let db = self.dashboard_bounds.get();
        let dash_w = f32::from(db.size.width).max(compute_dashboard_width(viewport_w));
        let h_raw = f32::from(db.size.height);
        let dash_h = if h_raw > 1.0 { h_raw } else { 600.0 };
        (dash_w, dash_h)
    }

    #[must_use]
    pub fn resolved_dashboard_position(&self, viewport_w: f32, viewport_h: f32) -> (f32, f32) {
        let (dash_w, dash_h) = self.resolved_dashboard_size(viewport_w);
        match self.gui_pos {
            Some((px_x, px_y)) => (
                px_x.clamp(0.0, (viewport_w - dash_w).max(0.0)),
                px_y.clamp(0.0, (viewport_h - dash_h).max(0.0)),
            ),
            None => compute_dashboard_position(viewport_w, viewport_h, dash_w, dash_h, 0.0, 0.0),
        }
    }

    pub(crate) fn begin_gui_drag(&mut self, ev: &MouseDownEvent) {
        let db = self.dashboard_bounds.get();
        let start_origin = if let Some(pos) = self.gui_pos {
            pos
        } else {
            (f32::from(db.origin.x), f32::from(db.origin.y))
        };
        self.open_dropdown = None;
        self.dragging_gui = true;
        self.drag_gui_start_cursor = (f32::from(ev.position.x), f32::from(ev.position.y));
        self.drag_gui_start_origin = start_origin;
    }

    pub(crate) fn update_gui_drag_from_point(
        &mut self,
        pos_x: f32,
        pos_y: f32,
        viewport_w: f32,
        viewport_h: f32,
        cx: &mut Context<Self>,
    ) {
        let (dash_w, dash_h) = self.resolved_dashboard_size(viewport_w);
        let max_x = (viewport_w - dash_w).max(0.0);
        let max_y = (viewport_h - dash_h).max(0.0);
        let next_x = (self.drag_gui_start_origin.0 + (pos_x - self.drag_gui_start_cursor.0))
            .clamp(0.0, max_x);
        let next_y = (self.drag_gui_start_origin.1 + (pos_y - self.drag_gui_start_cursor.1))
            .clamp(0.0, max_y);
        self.gui_pos = Some((next_x, next_y));
        cx.notify();
    }

    fn flush_outgoing(&mut self, cx: &mut Context<Self>) {
        let msgs = self.store.take_outgoing();
        if !msgs.is_empty() {
            crate::push_outgoing_global(cx, msgs);
        }
        cx.notify();
    }

    fn action_button(
        &self,
        label: impl Into<String>,
        active: bool,
        accent_hex: u32,
        on_click: impl Fn(&mut Self, &MouseDownEvent, &mut gpui::Window, &mut Context<Self>)
            + 'static,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        self.styled_button(label, active, accent_hex, false, on_click, cx)
    }

    fn styled_button(
        &self,
        label: impl Into<String>,
        active: bool,
        accent_hex: u32,
        full_width: bool,
        on_click: impl Fn(&mut Self, &MouseDownEvent, &mut gpui::Window, &mut Context<Self>)
            + 'static,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let bg = if active {
            accent_hex
        } else {
            rgba_u32(28, 35, 48, 225)
        };
        let border = if active {
            rgba_u32(140, 200, 255, 220)
        } else {
            rgba_u32(68, 82, 104, 180)
        };
        let handler = Rc::new(on_click);
        let mut btn = div()
            .px(px(if full_width { 5.0 } else { 7.0 }))
            .py(px(if full_width { 2.5 } else { 3.5 }))
            .rounded(px(4.0))
            .bg(rgba(bg))
            .border_1()
            .border_color(rgba(border))
            .text_size(px(11.0))
            .text_color(rgba(rgba_u32(236, 242, 252, 255)))
            .overflow_hidden()
            .on_mouse_down(
                MouseButton::Left,
                cx.listener(move |view, ev, window, cx| {
                    cx.stop_propagation();
                    handler(view, ev, window, cx);
                }),
            )
            .child(label.into());
        if full_width {
            btn = btn.w_full();
        }
        btn.into_any_element()
    }

    fn render_header_bar(&self, cx: &mut Context<Self>) -> AnyElement {
        let cur = self.store.config.preset;
        let presets = [
            ("Minimal", PresetType::Minimal),
            ("Standard", PresetType::Standard),
            ("Advance", PresetType::Advance),
            ("Custom", PresetType::Custom),
        ];

        let preset_btns: Vec<AnyElement> = presets
            .into_iter()
            .map(|(title, preset)| {
                self.action_button(
                    title,
                    cur == preset,
                    rgba_u32(42, 114, 212, 235),
                    move |view, _, _, cx| {
                        view.open_dropdown = None;
                        view.store.apply_preset(preset);
                        view.flush_outgoing(cx);
                    },
                    cx,
                )
            })
            .collect();

        let close_btn = self.action_button(
            "Close",
            false,
            rgba_u32(170, 50, 50, 230),
            |view, _, _, cx| {
                view.open_dropdown = None;
                view.store.config.gui_open = false;
                view.store.emit_config_updated();
                view.flush_outgoing(cx);
            },
            cx,
        );

        div()
            .flex()
            .flex_row()
            .items_center()
            .justify_between()
            .pb(px(6.0))
            .border_b_1()
            .border_color(rgba(rgba_u32(55, 70, 92, 180)))
            .on_mouse_down(
                MouseButton::Left,
                cx.listener(|view, ev: &MouseDownEvent, _, _| view.begin_gui_drag(ev)),
            )
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .gap(px(5.0))
                    .child(
                        div()
                            .text_size(px(12.0))
                            .text_color(rgba(rgba_u32(180, 212, 248, 255)))
                            .child("Presets:"),
                    )
                    .children(preset_btns),
            )
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .gap(px(8.0))
                    .child(
                        div()
                            .text_size(px(12.0))
                            .text_color(rgba(rgba_u32(255, 215, 0, 255)))
                            .child(format!(
                                "{:.0} FPS \u{00B7} {:.1} ms",
                                self.store.telemetry.fps, self.store.telemetry.frametime_ms
                            )),
                    )
                    .child(render_frametime_sparkline(
                        &self.store.telemetry,
                        160.0,
                        24.0,
                        rgba_u32(72, 140, 220, 140),
                    ))
                    .child(close_btn),
            )
            .into_any_element()
    }

    fn render_column_layout(&self, cx: &mut Context<Self>) -> AnyElement {
        let cfg = &self.store.config;
        let (norm_x, norm_y) = match cfg.position {
            LayoutPosition::TopLeft => (0.02, 0.04),
            LayoutPosition::TopCenter => (0.5, 0.04),
            LayoutPosition::TopRight => (0.98, 0.04),
            LayoutPosition::BottomLeft => (0.02, 0.96),
            LayoutPosition::BottomCenter => (0.5, 0.96),
            LayoutPosition::BottomRight => (0.98, 0.96),
            LayoutPosition::Custom => (cfg.custom_pos_x.clamp(0.0, 1.0), cfg.custom_pos_y.clamp(0.0, 1.0)),
        };
        let dot_x = (norm_x * (PREVIEW_W - PREVIEW_HANDLE_PX)).clamp(0.0, PREVIEW_W - PREVIEW_HANDLE_PX);
        let dot_y = (norm_y * (PREVIEW_H - PREVIEW_HANDLE_PX)).clamp(0.0, PREVIEW_H - PREVIEW_HANDLE_PX);
        let pos_badge = if cfg.position == LayoutPosition::Custom {
            format!(
                "Custom {:.0}%, {:.0}%",
                cfg.custom_pos_x * 100.0,
                cfg.custom_pos_y * 100.0
            )
        } else {
            position_display_name(cfg.position).to_string()
        };

        let bounds_measure = Rc::clone(&self.preview_bounds);
        let preview_box = div()
            .w(px(PREVIEW_W))
            .h(px(PREVIEW_H))
            .relative()
            .rounded(px(6.0))
            .bg(rgba(rgba_u32(10, 14, 22, 242)))
            .border_1()
            .border_color(rgba(rgba_u32(78, 128, 200, 215)))
            .on_mouse_down(
                MouseButton::Left,
                cx.listener(|view, ev: &MouseDownEvent, _, cx| {
                    cx.stop_propagation();
                    view.open_dropdown = None;
                    view.dragging_preview = true;
                    view.update_preview_pos_from_point(
                        f32::from(ev.position.x),
                        f32::from(ev.position.y),
                        cx,
                    );
                }),
            )
            .child(
                canvas(
                    move |bounds, _, _| bounds_measure.set(bounds),
                    |_, _, _, _| {},
                )
                .size_full()
                .absolute(),
            )
            // Subtle 3x3 guide lines for handheld 7" touch dragging
            .child(
                div()
                    .absolute()
                    .left(px(PREVIEW_W / 3.0))
                    .top(px(0.0))
                    .w(px(1.0))
                    .h_full()
                    .bg(rgba(rgba_u32(78, 128, 200, 38))),
            )
            .child(
                div()
                    .absolute()
                    .left(px(PREVIEW_W * 2.0 / 3.0))
                    .top(px(0.0))
                    .w(px(1.0))
                    .h_full()
                    .bg(rgba(rgba_u32(78, 128, 200, 38))),
            )
            .child(
                div()
                    .absolute()
                    .left(px(0.0))
                    .top(px(PREVIEW_H * 0.5))
                    .w_full()
                    .h(px(1.0))
                    .bg(rgba(rgba_u32(78, 128, 200, 38))),
            )
            .child(
                div()
                    .absolute()
                    .left(px(dot_x))
                    .top(px(dot_y))
                    .w(px(PREVIEW_HANDLE_PX))
                    .h(px(PREVIEW_HANDLE_PX))
                    .rounded(px(5.0))
                    .bg(rgba(rgba_u32(95, 205, 255, 245)))
                    .border_2()
                    .border_color(rgba(rgba_u32(255, 255, 255, 255))),
            )
            .child(
                div()
                    .absolute()
                    .right(px(6.0))
                    .bottom(px(4.0))
                    .text_size(px(10.0))
                    .text_color(rgba(rgba_u32(150, 174, 206, 225)))
                    .child(format!("Drag HUD \u{00B7} {pos_badge}")),
            );

        let dd_position = dropdown_trigger_row(
            "Position",
            position_display_name(cfg.position).to_string(),
            self.open_dropdown == Some(DropdownId::Position),
            Rc::clone(&self.dropdown_anchors[DropdownId::Position.index()]),
            |view, cx| view.toggle_dropdown(DropdownId::Position, cx),
            cx,
        );
        let dd_arrangement = dropdown_trigger_row(
            "Layout Mode",
            arrangement_display_name(cfg.arrangement).to_string(),
            self.open_dropdown == Some(DropdownId::Arrangement),
            Rc::clone(&self.dropdown_anchors[DropdownId::Arrangement.index()]),
            |view, cx| view.toggle_dropdown(DropdownId::Arrangement, cx),
            cx,
        );
        let dd_scale = dropdown_trigger_row(
            "UI Scale",
            format!("{:.2}x", cfg.gui_scale),
            self.open_dropdown == Some(DropdownId::Scale),
            Rc::clone(&self.dropdown_anchors[DropdownId::Scale.index()]),
            |view, cx| view.toggle_dropdown(DropdownId::Scale, cx),
            cx,
        );
        let dd_font = dropdown_trigger_row(
            "Font",
            font_dropdown_label(cfg.font_family).to_string(),
            self.open_dropdown == Some(DropdownId::Font),
            Rc::clone(&self.dropdown_anchors[DropdownId::Font.index()]),
            |view, cx| view.toggle_dropdown(DropdownId::Font, cx),
            cx,
        );
        let dd_font_size = dropdown_trigger_row(
            "Font Overlay",
            format!("{:.0} px", cfg.font_size_px),
            self.open_dropdown == Some(DropdownId::FontSize),
            Rc::clone(&self.dropdown_anchors[DropdownId::FontSize.index()]),
            |view, cx| view.toggle_dropdown(DropdownId::FontSize, cx),
            cx,
        );
        let dd_text_opacity = dropdown_trigger_row(
            "Text Opacity",
            format!("{:.0}%", cfg.text_opacity * 100.0),
            self.open_dropdown == Some(DropdownId::TextOpacity),
            Rc::clone(&self.dropdown_anchors[DropdownId::TextOpacity.index()]),
            |view, cx| view.toggle_dropdown(DropdownId::TextOpacity, cx),
            cx,
        );
        let dd_bg_style = dropdown_trigger_row(
            "BG Style",
            bg_style_display_name(cfg.background_style).to_string(),
            self.open_dropdown == Some(DropdownId::BgStyle),
            Rc::clone(&self.dropdown_anchors[DropdownId::BgStyle.index()]),
            |view, cx| view.toggle_dropdown(DropdownId::BgStyle, cx),
            cx,
        );
        let dd_bg_opacity = dropdown_trigger_row(
            "BG Opacity",
            format!("{:.0}%", cfg.bg_alpha * 100.0),
            self.open_dropdown == Some(DropdownId::BgOpacity),
            Rc::clone(&self.dropdown_anchors[DropdownId::BgOpacity.index()]),
            |view, cx| view.toggle_dropdown(DropdownId::BgOpacity, cx),
            cx,
        );

        let sw_cat = switch_toggle_row(
            "Category Colors",
            cfg.category_colors_enabled,
            |view, cx| {
                view.store.toggle_category_colors();
                view.flush_outgoing(cx);
            },
            cx,
        );
        let sw_text = text_color_switch_row(
            cfg.custom_text_color_enabled,
            cfg.text_color,
            |view, color, cx| {
                view.store.set_text_color(color);
                view.flush_outgoing(cx);
            },
            |view, cx| {
                view.store.toggle_custom_text_color();
                view.flush_outgoing(cx);
            },
            cx,
        );
        let sw_warn = switch_toggle_row(
            "Warning Colors",
            cfg.thermal_warnings_enabled,
            |view, cx| {
                view.store.toggle_thermal_warnings();
                view.flush_outgoing(cx);
            },
            cx,
        );
        let sw_graph = switch_toggle_row(
            "Frametime Graph",
            cfg.hud_graph_enabled,
            |view, cx| {
                view.store.toggle_hud_graph();
                view.flush_outgoing(cx);
            },
            cx,
        );

        let reset_btn = self.styled_button(
            "Reset Layout",
            false,
            rgba_u32(70, 82, 104, 230),
            true,
            |view, _, _, cx| {
                view.open_dropdown = None;
                view.store.reset_layout();
                view.flush_outgoing(cx);
            },
            cx,
        );

        div()
            .flex()
            .flex_col()
            .gap(px(3.0))
            .w(px(COL1_WIDTH_PX))
            .flex_shrink_0()
            .p(px(7.0))
            .rounded(px(6.0))
            .bg(rgba(rgba_u32(20, 25, 36, 225)))
            .child(
                div()
                    .text_size(px(12.0))
                    .text_color(rgba(rgba_u32(165, 200, 240, 255)))
                    .child("1. Layout & Style"),
            )
            .child(preview_box)
            .child(dd_position)
            .child(dd_arrangement)
            .child(dd_scale)
            .child(dd_font)
            .child(dd_font_size)
            .child(dd_text_opacity)
            .child(dd_bg_style)
            .child(dd_bg_opacity)
            .child(
                div()
                    .mt(px(2.0))
                    .pt(px(3.0))
                    .border_t_1()
                    .border_color(rgba(rgba_u32(52, 66, 88, 170)))
                    .flex()
                    .flex_col()
                    .gap(px(2.0))
                    .child(sw_cat)
                    .child(sw_text)
                    .child(sw_warn)
                    .child(sw_graph),
            )
            .child(div().mt(px(2.0)).child(reset_btn))
            .into_any_element()
    }

    fn build_sensor_chip(&self, metric: MetricType, max_val_chars: usize, cx: &mut Context<Self>) -> AnyElement {
        let enabled = self.store.config.has_metric(metric);
        let (_, val, _, _) = self.store.format_metric_row(metric);
        let label = hud_metric_short_label(metric);
        let val_disp = truncate_chars(&val, max_val_chars);
        let mark = if enabled { "[x]" } else { "[ ]" };
        self.styled_button(
            format!("{mark} {label}: {val_disp}"),
            enabled,
            rgba_u32(34, 96, 175, 225),
            true,
            move |view, _, _, cx| {
                view.store.toggle_metric(metric);
                view.flush_outgoing(cx);
            },
            cx,
        )
    }

    fn build_category_card(
        &self,
        cat: MetricCategory,
        visible_metrics: &[MetricType],
        columns: usize,
        max_val_chars: usize,
        cx: &mut Context<Self>,
    ) -> Option<AnyElement> {
        let group_items: Vec<MetricType> = visible_metrics
            .iter()
            .copied()
            .filter(|m| metric_category(*m) == cat)
            .collect();
        if group_items.is_empty() {
            return None;
        }
        let active_count = group_items
            .iter()
            .filter(|m| self.store.config.has_metric(**m))
            .count();
        let total_count = group_items.len();
        let accent = category_color_rgba(cat, &self.store.config, 255);
        let chips: Vec<AnyElement> = group_items
            .into_iter()
            .map(|m| self.build_sensor_chip(m, max_val_chars, cx))
            .collect();
        Some(sensor_group_card(cat, active_count, total_count, accent, columns, chips))
    }

    fn render_column_sensors(&self, col2_w: f32, cx: &mut Context<Self>) -> AnyElement {
        let filters = [
            ("All (31)", SensorFilter::All),
            ("Standard", SensorFilter::Standard),
            ("FPS", SensorFilter::Group(MetricCategory::Fps)),
            ("GPU", SensorFilter::Group(MetricCategory::Gpu)),
            ("CPU", SensorFilter::Group(MetricCategory::Cpu)),
            ("RAM", SensorFilter::Group(MetricCategory::Ram)),
            ("Sistem", SensorFilter::Group(MetricCategory::Sys)),
        ];
        let filter_btns: Vec<AnyElement> = filters
            .into_iter()
            .map(|(lbl, f)| {
                self.action_button(
                    lbl,
                    self.store.sensor_filter == f,
                    rgba_u32(42, 114, 212, 235),
                    move |view, _, _, cx| {
                        view.store.sensor_filter = f;
                        cx.notify();
                    },
                    cx,
                )
            })
            .collect();

        let visible = self.store.filtered_metrics(self.store.sensor_filter);
        let body: AnyElement = match self.store.sensor_filter {
            SensorFilter::Group(cat) => {
                let card = self
                    .build_category_card(cat, &visible, 2, 24, cx)
                    .unwrap_or_else(|| div().into_any_element());
                div().flex().flex_col().gap(px(5.0)).w_full().child(card).into_any_element()
            }
            SensorFilter::All | SensorFilter::Standard => {
                let mut cards: Vec<AnyElement> = Vec::new();
                if let Some(c) = self.build_category_card(MetricCategory::Fps, &visible, 3, 14, cx) {
                    cards.push(c);
                }
                if let Some(c) = self.build_category_card(MetricCategory::Gpu, &visible, 3, 14, cx) {
                    cards.push(c);
                }
                if let Some(c) = self.build_category_card(MetricCategory::Cpu, &visible, 3, 14, cx) {
                    cards.push(c);
                }
                let ram_card = self.build_category_card(MetricCategory::Ram, &visible, 1, 18, cx);
                let sys_card = self.build_category_card(MetricCategory::Sys, &visible, 2, 13, cx);
                match (ram_card, sys_card) {
                    (Some(ram), Some(sys)) => {
                        cards.push(
                            div()
                                .flex()
                                .flex_row()
                                .items_start()
                                .gap(px(5.0))
                                .w_full()
                                .child(div().w(px(195.0)).flex_shrink_0().child(ram))
                                .child(div().flex_1().min_w(px(0.0)).child(sys))
                                .into_any_element(),
                        );
                    }
                    (Some(ram), None) => cards.push(ram),
                    (None, Some(sys)) => cards.push(sys),
                    (None, None) => {}
                }
                div().flex().flex_col().gap(px(4.0)).w_full().children(cards).into_any_element()
            }
        };

        div()
            .flex()
            .flex_col()
            .gap(px(5.0))
            .w(px(col2_w))
            .flex_shrink_0()
            .p(px(7.0))
            .rounded(px(6.0))
            .bg(rgba(rgba_u32(20, 25, 36, 225)))
            .child(
                div()
                    .text_size(px(12.0))
                    .text_color(rgba(rgba_u32(165, 200, 240, 255)))
                    .child("2. Telemetry Sensors"),
            )
            .child(div().flex().flex_row().flex_wrap().gap(px(4.0)).children(filter_btns))
            .child(body)
            .into_any_element()
    }

    fn render_column_actions(&self, cx: &mut Context<Self>) -> AnyElement {
        let bench_label = if self.store.telemetry.benchmark_active {
            "Stop Benchmark"
        } else {
            "Start Benchmark"
        };
        let capture_clean = !self.store.config.screenshot_with_hud;

        let status = self
            .store
            .status_message
            .clone()
            .or_else(|| self.store.telemetry.last_benchmark.as_ref().map(format_benchmark_summary))
            .unwrap_or_else(|| "Pure-Rust GPUI-CE Ready".to_string());

        let keybind_buttons: Vec<AnyElement> = [
            ("Toggle GUI", KeybindTarget::ToggleGui),
            ("Toggle HUD", KeybindTarget::ToggleHud),
            ("Benchmark", KeybindTarget::ToggleBenchmark),
            ("Screenshot", KeybindTarget::TakeScreenshot),
        ]
        .into_iter()
        .map(|(name, target)| {
            let bind_txt = self.store.keybind_label(target);
            self.styled_button(
                format!("{name}: {bind_txt}"),
                self.store.active_keybind_capture == target,
                rgba_u32(195, 130, 35, 240),
                true,
                move |view, _, _, cx| {
                    view.open_dropdown = None;
                    view.store.request_keybind_capture(target);
                    view.flush_outgoing(cx);
                },
                cx,
            )
        })
        .collect();

        div()
            .flex()
            .flex_col()
            .gap(px(5.0))
            .w(px(COL3_WIDTH_PX))
            .flex_shrink_0()
            .p(px(7.0))
            .rounded(px(6.0))
            .bg(rgba(rgba_u32(20, 25, 36, 225)))
            .child(
                div()
                    .text_size(px(12.0))
                    .text_color(rgba(rgba_u32(165, 200, 240, 255)))
                    .child("3. Capture & Hotkeys"),
            )
            .child(self.styled_button(
                "Take Screenshot",
                false,
                rgba_u32(42, 114, 212, 235),
                true,
                |view, _, _, cx| {
                    view.store.trigger_screenshot();
                    view.flush_outgoing(cx);
                },
                cx,
            ))
            .child(self.styled_button(
                bench_label,
                self.store.telemetry.benchmark_active,
                rgba_u32(210, 75, 65, 235),
                true,
                |view, _, _, cx| {
                    view.store.trigger_benchmark();
                    view.flush_outgoing(cx);
                },
                cx,
            ))
            .child(self.styled_button(
                "Open Save Folder",
                false,
                rgba_u32(42, 114, 212, 235),
                true,
                |view, _, _, cx| {
                    view.store.trigger_open_save_folder();
                    view.flush_outgoing(cx);
                },
                cx,
            ))
            .child(
                div()
                    .flex()
                    .flex_row()
                    .gap(px(4.0))
                    .child(self.action_button(
                        "Clean Game",
                        capture_clean,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            view.store.set_screenshot_mode(false);
                            view.flush_outgoing(cx);
                        },
                        cx,
                    ))
                    .child(self.action_button(
                        "With HUD",
                        !capture_clean,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            view.store.set_screenshot_mode(true);
                            view.flush_outgoing(cx);
                        },
                        cx,
                    )),
            )
            .child(self.styled_button(
                "Save Configuration",
                false,
                rgba_u32(42, 145, 95, 235),
                true,
                |view, _, _, cx| {
                    view.store.save_configuration();
                    view.flush_outgoing(cx);
                },
                cx,
            ))
            .child(
                div()
                    .p(px(5.0))
                    .rounded(px(4.0))
                    .bg(rgba(rgba_u32(14, 18, 26, 220)))
                    .text_size(px(10.5))
                    .text_color(rgba(rgba_u32(150, 225, 175, 255)))
                    .child(status),
            )
            .child(
                div()
                    .mt(px(3.0))
                    .pt(px(5.0))
                    .border_t_1()
                    .border_color(rgba(rgba_u32(52, 66, 88, 170)))
                    .flex()
                    .flex_col()
                    .gap(px(4.0))
                    .child(
                        div()
                            .text_size(px(11.0))
                            .text_color(rgba(rgba_u32(165, 200, 240, 255)))
                            .child("Hotkeys"),
                    )
                    .children(keybind_buttons),
            )
            .into_any_element()
    }

    /// Renders the active floating dropdown popover + click-outside backdrop (mounted via `gpui::deferred`).
    pub fn render_active_dropdown_overlay(
        &self,
        viewport_w: f32,
        viewport_h: f32,
        cx: &mut Context<Self>,
    ) -> Option<AnyElement> {
        let id = self.open_dropdown?;
        let anchor = self.dropdown_anchors[id.index()].get();
        let cfg = &self.store.config;

        let (popup_w, columns, items): (f32, usize, Vec<AnyElement>) = match id {
            DropdownId::Position => (
                156.0,
                1,
                POSITION_OPTIONS
                    .iter()
                    .map(|&(label, pos)| {
                        popover_option_item(
                            label,
                            cfg.position == pos,
                            move |view, cx| {
                                view.open_dropdown = None;
                                view.store.set_position(pos);
                                view.flush_outgoing(cx);
                            },
                            cx,
                        )
                    })
                    .collect(),
            ),
            DropdownId::Arrangement => (
                148.0,
                1,
                ARRANGEMENT_OPTIONS
                    .iter()
                    .map(|&(label, arr)| {
                        popover_option_item(
                            label,
                            cfg.arrangement == arr,
                            move |view, cx| {
                                view.open_dropdown = None;
                                view.store.set_arrangement(arr);
                                view.flush_outgoing(cx);
                            },
                            cx,
                        )
                    })
                    .collect(),
            ),
            DropdownId::Scale => (
                156.0,
                2,
                SCALE_OPTIONS
                    .iter()
                    .map(|&sc| {
                        popover_option_item(
                            format!("{sc:.2}x"),
                            (cfg.gui_scale - sc).abs() < 0.03,
                            move |view, cx| {
                                view.open_dropdown = None;
                                view.store.set_gui_scale(sc);
                                view.flush_outgoing(cx);
                            },
                            cx,
                        )
                    })
                    .collect(),
            ),
            DropdownId::Font => (
                156.0,
                1,
                FONT_OPTIONS
                    .iter()
                    .map(|&(label, font)| {
                        let is_selected = font_dropdown_label(cfg.font_family) == label;
                        popover_option_item(
                            label,
                            is_selected,
                            move |view, cx| {
                                view.open_dropdown = None;
                                view.store.set_font_family(font);
                                view.flush_outgoing(cx);
                            },
                            cx,
                        )
                    })
                    .collect(),
            ),
            DropdownId::FontSize => (
                156.0,
                2,
                FONT_SIZE_OPTIONS
                    .iter()
                    .map(|&sz| {
                        popover_option_item(
                            format!("{sz} px"),
                            (cfg.font_size_px - f32::from(sz)).abs() < 0.6,
                            move |view, cx| {
                                view.open_dropdown = None;
                                view.store.set_font_size_px(f32::from(sz));
                                view.flush_outgoing(cx);
                            },
                            cx,
                        )
                    })
                    .collect(),
            ),
            DropdownId::TextOpacity => (
                148.0,
                1,
                TEXT_OPACITY_PCTS
                    .iter()
                    .map(|&pct| {
                        popover_option_item(
                            format!("{pct}%"),
                            (cfg.text_opacity * 100.0 - f32::from(pct)).abs() < 5.0,
                            move |view, cx| {
                                view.open_dropdown = None;
                                view.store.set_text_opacity_pct(pct);
                                view.flush_outgoing(cx);
                            },
                            cx,
                        )
                    })
                    .collect(),
            ),
            DropdownId::BgStyle => (
                148.0,
                1,
                BG_STYLE_OPTIONS
                    .iter()
                    .map(|&(label, style)| {
                        popover_option_item(
                            label,
                            cfg.background_style == style,
                            move |view, cx| {
                                view.open_dropdown = None;
                                view.store.set_background_style(style);
                                view.flush_outgoing(cx);
                            },
                            cx,
                        )
                    })
                    .collect(),
            ),
            DropdownId::BgOpacity => (
                220.0,
                4,
                BG_OPACITY_PCTS
                    .iter()
                    .map(|&pct| {
                        popover_option_item(
                            format!("{pct}%"),
                            (cfg.bg_alpha * 100.0 - f32::from(pct)).abs() < 2.6,
                            move |view, cx| {
                                view.open_dropdown = None;
                                view.store.set_bg_alpha_pct(pct);
                                view.flush_outgoing(cx);
                            },
                            cx,
                        )
                    })
                    .collect(),
            ),
        };

        let row_count = items.len().div_ceil(columns.max(1));
        let est_h = (row_count as f32) * 26.0 + 12.0;
        let (ox, oy) = clamp_popover_origin(anchor, popup_w, est_h, viewport_w, viewport_h);
        let panel = popover_grid_panel(
            ox,
            oy,
            popup_w,
            columns,
            Rc::clone(&self.popover_bounds),
            items,
        );

        Some(
            div()
                .absolute()
                .left(px(0.0))
                .top(px(0.0))
                .w(px(viewport_w))
                .h(px(viewport_h))
                .on_mouse_down(
                    MouseButton::Left,
                    cx.listener(|view, _ev: &MouseDownEvent, _, cx| {
                        cx.stop_propagation();
                        view.open_dropdown = None;
                        cx.notify();
                    }),
                )
                .child(panel)
                .into_any_element(),
        )
    }

    pub fn render_dashboard(&self, viewport_w: f32, cx: &mut Context<Self>) -> Option<AnyElement> {
        if !self.store.config.gui_open {
            return None;
        }
        let dash_w = compute_dashboard_width(viewport_w);
        let col2_w = compute_dashboard_col2_width(viewport_w);
        let dash_measure = Rc::clone(&self.dashboard_bounds);
        let title_bar = div()
            .flex()
            .flex_row()
            .items_center()
            .justify_between()
            .px(px(10.0))
            .py(px(6.0))
            .rounded(px(5.0))
            .bg(rgba(rgba_u32(22, 32, 48, 238)))
            .border_1()
            .border_color(rgba(rgba_u32(66, 98, 146, 185)))
            .on_mouse_down(
                MouseButton::Left,
                cx.listener(|view, ev: &MouseDownEvent, _, _| view.begin_gui_drag(ev)),
            )
            .child(
                div()
                    .text_size(px(12.0))
                    .text_color(rgba(rgba_u32(190, 220, 255, 255)))
                    .child("FPSHUD Settings"),
            )
            .child(
                div()
                    .text_size(px(11.0))
                    .text_color(rgba(rgba_u32(138, 160, 188, 225)))
                    .child("Drag bar to move"),
            );
        Some(
            div()
                .relative()
                .w(px(dash_w))
                .child(
                    canvas(
                        move |bounds, _, _| dash_measure.set(bounds),
                        |_, _, _, _| {},
                    )
                    .size_full()
                    .absolute(),
                )
                .child(
                    div()
                        .w(px(dash_w))
                        .flex()
                        .flex_col()
                        .gap(px(6.0))
                        .p(px(8.0))
                        .rounded(px(10.0))
                        .bg(rgba(rgba_u32(15, 19, 28, 240)))
                        .border_1()
                        .border_color(rgba(rgba_u32(78, 128, 200, 190)))
                        .child(title_bar)
                        .child(self.render_header_bar(cx))
                        .child(
                            div()
                                .flex()
                                .flex_row()
                                .items_start()
                                .gap(px(8.0))
                                .child(self.render_column_layout(cx))
                                .child(self.render_column_sensors(col2_w, cx))
                                .child(self.render_column_actions(cx)),
                        ),
                )
                .into_any_element(),
        )
    }
}
