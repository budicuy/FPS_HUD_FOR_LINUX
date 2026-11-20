use crate::hud::render_frametime_sparkline;
use crate::state::{
    SensorFilter, UiStore, font_family_name, format_benchmark_summary, rgba_u32,
};
use fpshud_core::{
    ArrangementMode, BackgroundStyle, FontFamily, LayoutPosition, PresetType,
};
use fpshud_input::KeybindTarget;
use gpui::{
    AnyElement, Bounds, Context, InteractiveElement, IntoElement, MouseButton, MouseDownEvent,
    ParentElement, Pixels, Styled, canvas, div, px, rgba,
};
use std::cell::Cell;
use std::rc::Rc;

const COL1_WIDTH_PX: f32 = 244.0;
const COL3_WIDTH_PX: f32 = 224.0;
const DASHBOARD_CHROME_PX: f32 = 44.0;

#[must_use]
pub fn compute_dashboard_width(viewport_w: f32) -> f32 {
    (viewport_w - 24.0).clamp(680.0, 1080.0)
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

pub struct FpshudRootView {
    pub(crate) store: UiStore,
    pub(crate) last_applied_seq: u64,
    pub(crate) last_cursor_seq: u64,
    pub(crate) draw_software_cursor: bool,
    pub(crate) started_at: std::time::Instant,
    pub(crate) preview_bounds: Rc<Cell<Bounds<Pixels>>>,
    pub(crate) dashboard_bounds: Rc<Cell<Bounds<Pixels>>>,
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
            draw_software_cursor: false,
            started_at: std::time::Instant::now(),
            preview_bounds: Rc::new(Cell::new(Bounds::default())),
            dashboard_bounds: Rc::new(Cell::new(Bounds::default())),
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
    pub fn dashboard_bounds_snapshot(&self) -> Bounds<Pixels> {
        self.dashboard_bounds.get()
    }

    #[must_use]
    pub fn preview_bounds_snapshot(&self) -> Bounds<Pixels> {
        self.preview_bounds.get()
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
        let dash_h = if h_raw > 1.0 { h_raw } else { 660.0 };
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
            .px(px(if full_width { 6.0 } else { 8.0 }))
            .py(px(if full_width { 3.0 } else { 4.0 }))
            .rounded(px(4.0))
            .bg(rgba(bg))
            .border_1()
            .border_color(rgba(border))
            .text_size(px(12.0))
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
            .pb(px(8.0))
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
                    .gap(px(6.0))
                    .child(
                        div()
                            .text_size(px(14.0))
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
                    .gap(px(10.0))
                    .child(
                        div()
                            .text_size(px(13.0))
                            .text_color(rgba(rgba_u32(255, 215, 0, 255)))
                            .child(format!(
                                "{:.0} FPS | {:.1} ms",
                                self.store.telemetry.fps, self.store.telemetry.frametime_ms
                            )),
                    )
                    .child(render_frametime_sparkline(
                        &self.store.telemetry,
                        180.0,
                        28.0,
                        rgba_u32(72, 140, 220, 140),
                    ))
                    .child(close_btn),
            )
            .into_any_element()
    }

    fn render_column_layout(&self, cx: &mut Context<Self>) -> AnyElement {
        const PREVIEW_W: f32 = 160.0;
        const PREVIEW_H: f32 = 90.0;

        let dot_x = (self.store.config.custom_pos_x.clamp(0.0, 1.0) * (PREVIEW_W - 14.0)).max(0.0);
        let dot_y = (self.store.config.custom_pos_y.clamp(0.0, 1.0) * (PREVIEW_H - 14.0)).max(0.0);

        let bounds_measure = Rc::clone(&self.preview_bounds);
        let preview_box = div()
            .w(px(PREVIEW_W))
            .h(px(PREVIEW_H))
            .relative()
            .rounded(px(4.0))
            .bg(rgba(rgba_u32(10, 14, 22, 240)))
            .border_1()
            .border_color(rgba(rgba_u32(78, 128, 200, 200)))
            .on_mouse_down(
                MouseButton::Left,
                cx.listener(|view, ev: &MouseDownEvent, _, cx| {
                    cx.stop_propagation();
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
            .child(
                div()
                    .absolute()
                    .left(px(dot_x))
                    .top(px(dot_y))
                    .w(px(12.0))
                    .h(px(12.0))
                    .rounded(px(3.0))
                    .bg(rgba(rgba_u32(95, 205, 255, 245))),
            );

        let anchor_buttons: Vec<AnyElement> = [
            ("TL", LayoutPosition::TopLeft),
            ("TC", LayoutPosition::TopCenter),
            ("TR", LayoutPosition::TopRight),
            ("BL", LayoutPosition::BottomLeft),
            ("BC", LayoutPosition::BottomCenter),
            ("BR", LayoutPosition::BottomRight),
        ]
        .into_iter()
        .map(|(lbl, pos)| {
            self.action_button(
                lbl,
                self.store.config.position == pos,
                rgba_u32(42, 114, 212, 235),
                move |view, _, _, cx| {
                    view.store.set_position(pos);
                    view.flush_outgoing(cx);
                },
                cx,
            )
        })
        .collect();

        let arr_buttons: Vec<AnyElement> = [
            ("Stack", ArrangementMode::Stack),
            ("Double", ArrangementMode::Double),
            ("Linear", ArrangementMode::Linear),
        ]
        .into_iter()
        .map(|(lbl, arr)| {
            self.action_button(
                lbl,
                self.store.config.arrangement == arr,
                rgba_u32(42, 114, 212, 235),
                move |view, _, _, cx| {
                    view.store.config.arrangement = arr;
                    view.store.emit_config_updated();
                    view.flush_outgoing(cx);
                },
                cx,
            )
        })
        .collect();

        let bg_buttons: Vec<AnyElement> = [
            ("None", BackgroundStyle::None),
            ("Translucent", BackgroundStyle::Translucent),
            ("Solid", BackgroundStyle::Solid),
        ]
        .into_iter()
        .map(|(lbl, style)| {
            self.action_button(
                lbl,
                self.store.config.background_style == style,
                rgba_u32(42, 114, 212, 235),
                move |view, _, _, cx| {
                    view.store.config.background_style = style;
                    view.store.emit_config_updated();
                    view.flush_outgoing(cx);
                },
                cx,
            )
        })
        .collect();

        let keybind_buttons: Vec<AnyElement> = [
            ("Toggle GUI", KeybindTarget::ToggleGui),
            ("Toggle HUD", KeybindTarget::ToggleHud),
            ("Benchmark", KeybindTarget::ToggleBenchmark),
            ("Screenshot", KeybindTarget::TakeScreenshot),
        ]
        .into_iter()
        .map(|(name, target)| {
            let bind_txt = self.store.keybind_label(target);
            self.action_button(
                format!("{name}: {bind_txt}"),
                self.store.active_keybind_capture == target,
                rgba_u32(195, 130, 35, 240),
                move |view, _, _, cx| {
                    view.store.request_keybind_capture(target);
                    view.flush_outgoing(cx);
                },
                cx,
            )
        })
        .collect();

        let next_font = match self.store.config.font_family {
            FontFamily::SansBold => FontFamily::MonoBold,
            FontFamily::MonoBold => FontFamily::VeraMono,
            FontFamily::VeraMono | FontFamily::AdwaitaMono => FontFamily::SansBold,
        };

        div()
            .flex()
            .flex_col()
            .gap(px(6.0))
            .w(px(COL1_WIDTH_PX))
            .flex_shrink_0()
            .p(px(8.0))
            .rounded(px(6.0))
            .bg(rgba(rgba_u32(20, 25, 36, 225)))
            .child(
                div()
                    .text_size(px(13.0))
                    .text_color(rgba(rgba_u32(165, 200, 240, 255)))
                    .child("1. Layout, Style & Hotkeys"),
            )
            .child(preview_box)
            .child(div().flex().flex_row().flex_wrap().gap(px(4.0)).children(anchor_buttons))
            .child(div().flex().flex_row().flex_wrap().gap(px(4.0)).children(arr_buttons))
            .child(div().flex().flex_row().flex_wrap().gap(px(4.0)).children(bg_buttons))
            .child(
                div()
                    .flex()
                    .flex_row()
                    .flex_wrap()
                    .gap(px(4.0))
                    .child(self.action_button(
                        format!("Font: {}", font_family_name(self.store.config.font_family)),
                        false,
                        rgba_u32(42, 114, 212, 235),
                        move |view, _, _, cx| {
                            view.store.config.font_family = next_font;
                            view.store.emit_config_updated();
                            view.flush_outgoing(cx);
                        },
                        cx,
                    ))
                    .child(self.action_button(
                        "Reset Layout",
                        false,
                        rgba_u32(70, 82, 104, 230),
                        |view, _, _, cx| {
                            view.store.reset_layout();
                            view.flush_outgoing(cx);
                        },
                        cx,
                    ))
                    .child(self.action_button(
                        "Cat Colors",
                        self.store.config.category_colors_enabled,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            view.store.config.category_colors_enabled =
                                !view.store.config.category_colors_enabled;
                            view.store.emit_config_updated();
                            view.flush_outgoing(cx);
                        },
                        cx,
                    ))
                    .child(self.action_button(
                        "Warnings",
                        self.store.config.thermal_warnings_enabled,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            view.store.config.thermal_warnings_enabled =
                                !view.store.config.thermal_warnings_enabled;
                            view.store.emit_config_updated();
                            view.flush_outgoing(cx);
                        },
                        cx,
                    ))
                    .child(self.action_button(
                        "Graph",
                        self.store.config.hud_graph_enabled,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            view.store.toggle_hud_graph();
                            view.flush_outgoing(cx);
                        },
                        cx,
                    ))
                    .child(self.action_button(
                        "Text Color",
                        false,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            view.store.cycle_text_color();
                            view.flush_outgoing(cx);
                        },
                        cx,
                    )),
            )
            .child(
                div()
                    .flex()
                    .flex_row()
                    .flex_wrap()
                    .gap(px(4.0))
                    .child(self.action_button(
                        "Scale-",
                        false,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            view.store.step_scale(-0.1);
                            view.flush_outgoing(cx);
                        },
                        cx,
                    ))
                    .child(self.action_button(
                        format!("Scale {:.1}x+", self.store.config.gui_scale),
                        false,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            view.store.step_scale(0.1);
                            view.flush_outgoing(cx);
                        },
                        cx,
                    ))
                    .child(self.action_button(
                        "Font-",
                        false,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            view.store.step_font_size(-1.0);
                            view.flush_outgoing(cx);
                        },
                        cx,
                    ))
                    .child(self.action_button(
                        format!("{:.0}px+", self.store.config.font_size_px),
                        false,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            view.store.step_font_size(1.0);
                            view.flush_outgoing(cx);
                        },
                        cx,
                    ))
                    .child(self.action_button(
                        format!("Txt {:.0}%", self.store.config.text_opacity * 100.0),
                        false,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            let step = if view.store.config.text_opacity >= 0.99 { -0.8 } else { 0.2 };
                            view.store.step_text_opacity(step);
                            view.flush_outgoing(cx);
                        },
                        cx,
                    ))
                    .child(self.action_button(
                        format!("BG {:.0}%", self.store.config.bg_alpha * 100.0),
                        false,
                        rgba_u32(42, 114, 212, 235),
                        |view, _, _, cx| {
                            let step = if view.store.config.bg_alpha >= 0.90 { -0.7 } else { 0.15 };
                            view.store.step_bg_alpha(step);
                            view.flush_outgoing(cx);
                        },
                        cx,
                    )),
            )
            .children(keybind_buttons)
            .into_any_element()
    }

    fn render_column_sensors(&self, col2_w: f32, cx: &mut Context<Self>) -> AnyElement {
        let filters = [
            ("All (31)", SensorFilter::All),
            ("Standard", SensorFilter::Standard),
            ("FPS (5)", SensorFilter::Fps),
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

        let metric_rows: Vec<AnyElement> = self
            .store
            .filtered_metrics(self.store.sensor_filter)
            .into_iter()
            .map(|metric| {
                let enabled = self.store.config.has_metric(metric);
                let (label, val, _, _) = self.store.format_metric_row(metric);
                let val_disp = truncate_chars(&val, 26);
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
            })
            .collect();

        let split = metric_rows.len().div_ceil(2);
        let mut col_a = Vec::with_capacity(split);
        let mut col_b = Vec::with_capacity(metric_rows.len().saturating_sub(split));
        for (idx, row) in metric_rows.into_iter().enumerate() {
            if idx < split {
                col_a.push(row);
            } else {
                col_b.push(row);
            }
        }

        div()
            .flex()
            .flex_col()
            .gap(px(6.0))
            .w(px(col2_w))
            .flex_shrink_0()
            .p(px(8.0))
            .rounded(px(6.0))
            .bg(rgba(rgba_u32(20, 25, 36, 225)))
            .child(
                div()
                    .text_size(px(13.0))
                    .text_color(rgba(rgba_u32(165, 200, 240, 255)))
                    .child("2. Telemetry Sensors"),
            )
            .child(div().flex().flex_row().gap(px(4.0)).children(filter_btns))
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_start()
                    .gap(px(6.0))
                    .w_full()
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .flex_1()
                            .min_w(px(0.0))
                            .gap(px(3.0))
                            .children(col_a),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .flex_1()
                            .min_w(px(0.0))
                            .gap(px(3.0))
                            .children(col_b),
                    ),
            )
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
            .unwrap_or_else(|| "Pure-Rust GPUI-CE Session Ready".to_string());

        div()
            .flex()
            .flex_col()
            .gap(px(6.0))
            .w(px(COL3_WIDTH_PX))
            .flex_shrink_0()
            .p(px(8.0))
            .rounded(px(6.0))
            .bg(rgba(rgba_u32(20, 25, 36, 225)))
            .child(
                div()
                    .text_size(px(13.0))
                    .text_color(rgba(rgba_u32(165, 200, 240, 255)))
                    .child("3. Capture & Actions"),
            )
            .child(self.action_button(
                "Take Screenshot",
                false,
                rgba_u32(42, 114, 212, 235),
                |view, _, _, cx| {
                    view.store.trigger_screenshot();
                    view.flush_outgoing(cx);
                },
                cx,
            ))
            .child(self.action_button(
                bench_label,
                self.store.telemetry.benchmark_active,
                rgba_u32(210, 75, 65, 235),
                |view, _, _, cx| {
                    view.store.trigger_benchmark();
                    view.flush_outgoing(cx);
                },
                cx,
            ))
            .child(self.action_button(
                "Open Save Folder",
                false,
                rgba_u32(42, 114, 212, 235),
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
            .child(self.action_button(
                "Save Configuration",
                false,
                rgba_u32(42, 145, 95, 235),
                |view, _, _, cx| {
                    view.store.save_configuration();
                    view.flush_outgoing(cx);
                },
                cx,
            ))
            .child(
                div()
                    .p(px(6.0))
                    .rounded(px(4.0))
                    .bg(rgba(rgba_u32(14, 18, 26, 220)))
                    .text_size(px(11.0))
                    .text_color(rgba(rgba_u32(150, 225, 175, 255)))
                    .child(status),
            )
            .into_any_element()
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
            .px(px(8.0))
            .py(px(4.0))
            .rounded(px(5.0))
            .bg(rgba(rgba_u32(22, 32, 48, 235)))
            .border_1()
            .border_color(rgba(rgba_u32(66, 98, 146, 170)))
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
                        .gap(px(8.0))
                        .p(px(10.0))
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
                                .gap(px(10.0))
                                .child(self.render_column_layout(cx))
                                .child(self.render_column_sensors(col2_w, cx))
                                .child(self.render_column_actions(cx)),
                        ),
                )
                .into_any_element(),
        )
    }
}
