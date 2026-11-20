use crate::hud::render_frametime_sparkline;
use crate::state::{
    SensorFilter, UiStore, font_family_name, format_benchmark_summary, rgba_u32,
};
use fpshud_core::{
    ArrangementMode, BackgroundStyle, FontFamily, LayoutPosition, PresetType,
};
use fpshud_input::KeybindTarget;
use gpui::{
    AnyElement, Context, InteractiveElement, IntoElement, MouseButton, MouseDownEvent,
    ParentElement, Styled, div, px, rgba,
};

pub struct FpshudRootView {
    pub(crate) store: UiStore,
    pub(crate) last_applied_seq: u64,
    pub(crate) started_at: std::time::Instant,
}

impl FpshudRootView {
    #[must_use]
    pub fn new(store: UiStore) -> Self {
        Self {
            store,
            last_applied_seq: 0,
            started_at: std::time::Instant::now(),
        }
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
        div()
            .px(px(8.0))
            .py(px(4.0))
            .rounded(px(4.0))
            .bg(rgba(bg))
            .border_1()
            .border_color(rgba(border))
            .text_size(px(12.0))
            .text_color(rgba(rgba_u32(236, 242, 252, 255)))
            .on_mouse_down(MouseButton::Left, cx.listener(on_click))
            .child(label.into())
            .into_any_element()
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
                cx.listener(|view, ev: &gpui::MouseDownEvent, window, cx| {
                    let win_w = f32::from(window.viewport_size().width).max(1.0);
                    let win_h = f32::from(window.viewport_size().height).max(1.0);
                    let norm_x = (f32::from(ev.position.x) / win_w).clamp(0.0, 1.0);
                    let norm_y = (f32::from(ev.position.y) / win_h).clamp(0.0, 1.0);
                    view.store.set_custom_pos(norm_x, norm_y);
                    view.flush_outgoing(cx);
                }),
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
            .w(px(230.0))
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
                    )),
            )
            .children(keybind_buttons)
            .into_any_element()
    }

    fn render_column_sensors(&self, cx: &mut Context<Self>) -> AnyElement {
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
                let mark = if enabled { "[x]" } else { "[ ]" };
                self.action_button(
                    format!("{mark} {label}: {val}"),
                    enabled,
                    rgba_u32(34, 96, 175, 225),
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
            .w(px(360.0))
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
                    .overflow_hidden()
                    .child(div().flex().flex_col().gap(px(3.0)).children(col_a))
                    .child(div().flex().flex_col().gap(px(3.0)).children(col_b)),
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
            .w(px(230.0))
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

    pub fn render_dashboard(&self, cx: &mut Context<Self>) -> Option<AnyElement> {
        if !self.store.config.gui_open {
            return None;
        }
        Some(
            div()
                .flex()
                .flex_col()
                .gap(px(10.0))
                .p(px(12.0))
                .rounded(px(10.0))
                .bg(rgba(rgba_u32(15, 19, 28, 240)))
                .border_1()
                .border_color(rgba(rgba_u32(78, 128, 200, 190)))
                .child(self.render_header_bar(cx))
                .child(
                    div()
                        .flex()
                        .flex_row()
                        .items_start()
                        .gap(px(10.0))
                        .child(self.render_column_layout(cx))
                        .child(self.render_column_sensors(cx))
                        .child(self.render_column_actions(cx)),
                )
                .into_any_element(),
        )
    }
}
