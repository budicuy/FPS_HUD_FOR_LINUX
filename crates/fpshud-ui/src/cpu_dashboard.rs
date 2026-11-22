//! 100% Pure-Rust CPU 3-column Settings dashboard renderer (`0` GPU / `0` MiB VRAM).
//!
//! Renders the complete interactive Settings panel directly into premultiplied RGBA8888 via
//! `CpuCanvas` + `TextRasterizer`, with 2-layer Z-order hit-testing (base panel + occluded
//! floating dropdown popovers) and Option-A effective scale clamping.

use crate::cpu_canvas::{CpuCanvas, Rect, TextRasterizer};
use crate::cpu_hud::CpuHudRenderer;
use crate::state::{
    DEFAULT_ACCENT_TEXT_COLOR, MetricCategory, SensorFilter, UiRenderMode, UiStore,
    arrangement_display_name, bg_style_display_name, category_color_rgba, category_display_title,
    font_dropdown_label, format_benchmark_summary, hud_metric_short_label, metric_category,
    position_display_name, rgba_u32,
};
use fpshud_core::{
    ArrangementMode, BackgroundStyle, ColorRgba, FontFamily, HudConfig, LayoutPosition, MetricType,
    PresetType,
};
use fpshud_input::{KeybindTarget, UiInputEvent};
use fpshud_ipc::UiToLayerMsg;
use fpshud_telemetry::TelemetrySnapshot;

const COL1_WIDTH_PX: f32 = 264.0;
const COL3_WIDTH_PX: f32 = 208.0;
const DASHBOARD_CHROME_PX: f32 = 44.0;
const PREVIEW_W: f32 = 248.0;
const PREVIEW_H: f32 = 116.0;
const PREVIEW_HANDLE_PX: f32 = 22.0;

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

#[must_use]
pub fn compute_dashboard_width(viewport_w: f32) -> f32 {
    (viewport_w - 24.0).clamp(720.0, 1140.0)
}

#[must_use]
pub fn compute_dashboard_col2_width(viewport_w: f32) -> f32 {
    (compute_dashboard_width(viewport_w) - COL1_WIDTH_PX - COL3_WIDTH_PX - DASHBOARD_CHROME_PX)
        .max(380.0)
}

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
    (
        (max_x * 0.5 + offset_x).clamp(0.0, max_x),
        (max_y * 0.5 + offset_y).clamp(0.0, max_y),
    )
}

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
    (
        ((pos_x - origin_x) / w).clamp(0.0, 1.0),
        ((pos_y - origin_y) / h).clamp(0.0, 1.0),
    )
}

/// Option-A effective scale factor for the Settings dashboard so 3 columns + Close never clip.
#[must_use]
pub fn resolve_dashboard_scale(
    requested_scale: f32,
    viewport_w_px: u32,
    viewport_h_px: u32,
    panel_w_logical: f32,
    panel_h_logical: f32,
) -> f32 {
    let req = requested_scale.clamp(0.5, 2.5);
    let max_sw = ((viewport_w_px as f32 - 16.0).max(1.0)) / panel_w_logical.max(1.0);
    let max_sh = ((viewport_h_px as f32 - 16.0).max(1.0)) / panel_h_logical.max(1.0);
    req.min(max_sw).min(max_sh).clamp(0.25, 2.5)
}

/// Computes floating popover `(x, y)` clamped inside viewport, flipping above anchor when needed.
#[must_use]
pub fn clamp_popover_origin_rect(
    anchor: Rect,
    popup_w: f32,
    popup_h: f32,
    viewport_w: f32,
    viewport_h: f32,
) -> (f32, f32) {
    const MARGIN: f32 = 8.0;
    const GAP: f32 = 4.0;
    let below_y = anchor.bottom() + GAP;
    let raw_y = if below_y + popup_h <= viewport_h - MARGIN {
        below_y
    } else {
        anchor.y - popup_h - GAP
    };
    let max_x = (viewport_w - popup_w - MARGIN).max(MARGIN);
    let max_y = (viewport_h - popup_h - MARGIN).max(MARGIN);
    (anchor.x.clamp(MARGIN, max_x), raw_y.clamp(MARGIN, max_y))
}

fn truncate_chars(text: &str, max_chars: usize) -> String {
    let count = text.chars().count();
    if count <= max_chars || max_chars <= 3 {
        return text.to_string();
    }
    let prefix: String = text.chars().take(max_chars - 3).collect();
    format!("{prefix}...")
}

#[derive(Debug, Clone, Copy)]
enum HitAction {
    DragHeader,
    DragPreview,
    ApplyPreset(PresetType),
    CloseGui,
    ToggleDropdown(DropdownId),
    SelectPosition(LayoutPosition),
    SelectArrangement(ArrangementMode),
    SelectScale(f32),
    SelectFont(FontFamily),
    SelectFontSize(u8),
    SelectTextOpacity(u8),
    SelectBgStyle(BackgroundStyle),
    SelectBgOpacity(u8),
    ToggleCategoryColors,
    SetTextColor(ColorRgba),
    ToggleCustomTextColor,
    ToggleThermalWarnings,
    ToggleHudGraph,
    ResetLayout,
    SetSensorFilter(SensorFilter),
    ToggleMetric(MetricType),
    TriggerScreenshot,
    TriggerBenchmark,
    TriggerOpenSaveFolder,
    SetScreenshotMode(bool),
    ResetSettings,
    CaptureKeybind(KeybindTarget),
}

/// Stateful 100% CPU Settings dashboard + optional pure-X11 `Full` compositor.
pub struct CpuDashboard {
    pub store: UiStore,
    pub render_mode: UiRenderMode,
    pub draw_software_cursor: bool,
    cpu_hud: CpuHudRenderer,
    open_dropdown: Option<DropdownId>,
    dropdown_anchors: [Rect; DropdownId::COUNT],
    dashboard_bounds: Rect,
    preview_bounds: Rect,
    popover_bounds: Rect,
    preset_bounds: [Rect; 4],
    close_bounds: Rect,
    base_hits: Vec<(Rect, HitAction)>,
    popover_hits: Vec<(Rect, HitAction)>,
    dragging_gui: bool,
    dragging_preview: bool,
    gui_pos: Option<(f32, f32)>,
    drag_gui_start_cursor: (f32, f32),
    drag_gui_start_origin: (f32, f32),
    cursor_logical: (f32, f32),
    last_eff_scale: f32,
    last_panel_size: (f32, f32),
}

impl CpuDashboard {
    #[must_use]
    pub fn new(store: UiStore) -> Self {
        Self {
            store,
            render_mode: UiRenderMode::Full,
            draw_software_cursor: false,
            cpu_hud: CpuHudRenderer::new(),
            open_dropdown: None,
            dropdown_anchors: [Rect::default(); DropdownId::COUNT],
            dashboard_bounds: Rect::default(),
            preview_bounds: Rect::default(),
            popover_bounds: Rect::default(),
            preset_bounds: [Rect::default(); 4],
            close_bounds: Rect::default(),
            base_hits: Vec::with_capacity(96),
            popover_hits: Vec::with_capacity(24),
            dragging_gui: false,
            dragging_preview: false,
            gui_pos: None,
            drag_gui_start_cursor: (0.0, 0.0),
            drag_gui_start_origin: (0.0, 0.0),
            cursor_logical: (0.0, 0.0),
            last_eff_scale: 1.0,
            last_panel_size: (960.0, 580.0),
        }
    }

    pub fn sync_all(&mut self, config: HudConfig, telemetry: TelemetrySnapshot) {
        let keys_changed = config.bind_toggle_gui != self.store.config.bind_toggle_gui
            || config.bind_toggle_hud != self.store.config.bind_toggle_hud
            || config.bind_benchmark != self.store.config.bind_benchmark
            || config.bind_screenshot != self.store.config.bind_screenshot;
        if keys_changed {
            self.store.active_keybind_capture = KeybindTarget::None;
        }
        if !config.gui_open {
            self.open_dropdown = None;
            self.dragging_gui = false;
            self.dragging_preview = false;
        }
        self.store.config = config;
        self.store.telemetry = telemetry;
    }

    pub fn set_render_mode(&mut self, mode: UiRenderMode) {
        self.render_mode = mode;
        if mode == UiRenderMode::HudOnly {
            self.open_dropdown = None;
            self.dragging_gui = false;
            self.dragging_preview = false;
            self.dashboard_bounds = Rect::default();
        }
    }

    pub fn set_software_cursor(&mut self, enabled: bool) {
        self.draw_software_cursor = enabled;
    }

    pub fn clear_keybind_capture(&mut self) {
        self.store.active_keybind_capture = KeybindTarget::None;
    }

    pub fn clear_open_dropdown(&mut self) {
        self.open_dropdown = None;
    }

    pub fn take_outgoing(&mut self) -> Vec<UiToLayerMsg> {
        self.store.take_outgoing()
    }

    #[must_use]
    pub const fn dashboard_bounds(&self) -> Rect {
        self.dashboard_bounds
    }

    #[must_use]
    pub const fn preview_bounds(&self) -> Rect {
        self.preview_bounds
    }

    #[must_use]
    pub const fn open_dropdown(&self) -> Option<DropdownId> {
        self.open_dropdown
    }

    #[must_use]
    pub const fn dropdown_anchor(&self, id: DropdownId) -> Rect {
        self.dropdown_anchors[id.index()]
    }

    #[must_use]
    pub const fn popover_bounds(&self) -> Rect {
        self.popover_bounds
    }

    #[must_use]
    pub const fn effective_scale(&self) -> f32 {
        self.last_eff_scale
    }

    #[must_use]
    pub const fn active_keybind_capture(&self) -> KeybindTarget {
        self.store.active_keybind_capture
    }

    #[must_use]
    pub const fn software_cursor_enabled(&self) -> bool {
        self.draw_software_cursor
    }

    #[must_use]
    pub fn preset_button_bounds(&self, preset: PresetType) -> Rect {
        let idx = match preset {
            PresetType::Minimal => 0,
            PresetType::Standard => 1,
            PresetType::Advance => 2,
            PresetType::Custom => 3,
        };
        self.preset_bounds[idx]
    }

    #[must_use]
    pub const fn close_button_bounds(&self) -> Rect {
        self.close_bounds
    }

    /// Dispatches pointer events in physical viewport pixels; returns `true` if redraw is needed.
    pub fn dispatch_input(
        &mut self,
        events: &[UiInputEvent],
        viewport_w_px: u32,
        viewport_h_px: u32,
        requested_scale: f32,
    ) -> bool {
        let eff_scale = resolve_dashboard_scale(
            requested_scale,
            viewport_w_px,
            viewport_h_px,
            self.last_panel_size.0,
            self.last_panel_size.1,
        );
        self.last_eff_scale = eff_scale;
        let logical_w = (viewport_w_px as f32 / eff_scale).max(1.0);
        let logical_h = (viewport_h_px as f32 / eff_scale).max(1.0);
        let mut dirty = false;

        for ev in events {
            match *ev {
                UiInputEvent::MouseMove { x, y } => {
                    let lx = x / eff_scale;
                    let ly = y / eff_scale;
                    self.cursor_logical = (lx, ly);
                    if self.draw_software_cursor {
                        dirty = true;
                    }
                    if self.dragging_gui {
                        let dx = lx - self.drag_gui_start_cursor.0;
                        let dy = ly - self.drag_gui_start_cursor.1;
                        let max_x = (logical_w - self.dashboard_bounds.w).max(0.0);
                        let max_y = (logical_h - self.dashboard_bounds.h).max(0.0);
                        self.gui_pos = Some((
                            (self.drag_gui_start_origin.0 + dx).clamp(0.0, max_x),
                            (self.drag_gui_start_origin.1 + dy).clamp(0.0, max_y),
                        ));
                        dirty = true;
                    }
                    if self.dragging_preview {
                        let (nx, ny) = map_preview_point_to_norm(
                            lx,
                            ly,
                            self.preview_bounds.x,
                            self.preview_bounds.y,
                            self.preview_bounds.w,
                            self.preview_bounds.h,
                        );
                        self.store.set_custom_pos(nx, ny);
                        dirty = true;
                    }
                }
                UiInputEvent::MouseDown { button: 0 } => {
                    let (lx, ly) = self.cursor_logical;
                    if self.handle_left_down(lx, ly) {
                        dirty = true;
                    }
                }
                UiInputEvent::MouseUp { button: 0 }
                    if self.dragging_gui || self.dragging_preview =>
                {
                    self.dragging_gui = false;
                    self.dragging_preview = false;
                    dirty = true;
                }
                _ => {}
            }
        }
        dirty
    }

    fn handle_left_down(&mut self, lx: f32, ly: f32) -> bool {
        if !self.store.config.gui_open || self.render_mode == UiRenderMode::HudOnly {
            return false;
        }
        // Layer 2 (top Z-order): active dropdown popover occludes everything underneath
        if self.open_dropdown.is_some() {
            for &(rect, action) in self.popover_hits.iter().rev() {
                if rect.contains(lx, ly) {
                    self.execute_action(action, lx, ly);
                    return true;
                }
            }
            if self.popover_bounds.contains(lx, ly) {
                return false;
            }
            self.open_dropdown = None;
            return true;
        }
        // Layer 1 (base panel): reverse order so foreground buttons beat background drag bar
        for &(rect, action) in self.base_hits.iter().rev() {
            if rect.contains(lx, ly) {
                self.execute_action(action, lx, ly);
                return true;
            }
        }
        false
    }

    fn execute_action(&mut self, action: HitAction, lx: f32, ly: f32) {
        match action {
            HitAction::DragHeader => {
                self.open_dropdown = None;
                self.dragging_gui = true;
                self.drag_gui_start_cursor = (lx, ly);
                self.drag_gui_start_origin = (self.dashboard_bounds.x, self.dashboard_bounds.y);
            }
            HitAction::DragPreview => {
                self.open_dropdown = None;
                self.dragging_preview = true;
                let (nx, ny) = map_preview_point_to_norm(
                    lx,
                    ly,
                    self.preview_bounds.x,
                    self.preview_bounds.y,
                    self.preview_bounds.w,
                    self.preview_bounds.h,
                );
                self.store.set_custom_pos(nx, ny);
            }
            HitAction::ApplyPreset(p) => {
                self.open_dropdown = None;
                self.store.apply_preset(p);
            }
            HitAction::CloseGui => {
                self.open_dropdown = None;
                self.store.config.gui_open = false;
                self.store.emit_config_updated();
            }
            HitAction::ToggleDropdown(id) => {
                self.open_dropdown = if self.open_dropdown == Some(id) {
                    None
                } else {
                    Some(id)
                };
            }
            HitAction::SelectPosition(pos) => {
                self.open_dropdown = None;
                self.store.set_position(pos);
            }
            HitAction::SelectArrangement(arr) => {
                self.open_dropdown = None;
                self.store.set_arrangement(arr);
            }
            HitAction::SelectScale(sc) => {
                self.open_dropdown = None;
                self.store.set_gui_scale(sc);
            }
            HitAction::SelectFont(font) => {
                self.open_dropdown = None;
                self.store.set_font_family(font);
            }
            HitAction::SelectFontSize(sz) => {
                self.open_dropdown = None;
                self.store.set_font_size_px(f32::from(sz));
            }
            HitAction::SelectTextOpacity(pct) => {
                self.open_dropdown = None;
                self.store.set_text_opacity_pct(pct);
            }
            HitAction::SelectBgStyle(style) => {
                self.open_dropdown = None;
                self.store.set_background_style(style);
            }
            HitAction::SelectBgOpacity(pct) => {
                self.open_dropdown = None;
                self.store.set_bg_alpha_pct(pct);
            }
            HitAction::ToggleCategoryColors => self.store.toggle_category_colors(),
            HitAction::SetTextColor(c) => self.store.set_text_color(c),
            HitAction::ToggleCustomTextColor => self.store.toggle_custom_text_color(),
            HitAction::ToggleThermalWarnings => self.store.toggle_thermal_warnings(),
            HitAction::ToggleHudGraph => self.store.toggle_hud_graph(),
            HitAction::ResetLayout => {
                self.open_dropdown = None;
                self.store.reset_layout();
            }
            HitAction::SetSensorFilter(f) => self.store.sensor_filter = f,
            HitAction::ToggleMetric(m) => self.store.toggle_metric(m),
            HitAction::TriggerScreenshot => self.store.trigger_screenshot(),
            HitAction::TriggerBenchmark => self.store.trigger_benchmark(),
            HitAction::TriggerOpenSaveFolder => self.store.trigger_open_save_folder(),
            HitAction::SetScreenshotMode(with_hud) => self.store.set_screenshot_mode(with_hud),
            HitAction::ResetSettings => {
                self.open_dropdown = None;
                self.store.reset_settings();
            }
            HitAction::CaptureKeybind(target) => {
                self.open_dropdown = None;
                self.store.request_keybind_capture(target);
            }
        }
    }

    /// Renders into `out_rgba` (resized to `width_px * height_px * 4`), premultiplied RGBA8888.
    pub fn render_rgba(
        &mut self,
        rasterizer: &mut TextRasterizer,
        width_px: u32,
        height_px: u32,
        requested_scale: f32,
        out_rgba: &mut Vec<u8>,
    ) {
        let w = width_px.max(1);
        let h = height_px.max(1);
        let byte_len = (w as usize) * (h as usize) * 4;
        if out_rgba.len() != byte_len {
            out_rgba.resize(byte_len, 0);
        }
        out_rgba.fill(0);

        let show_hud = self.render_mode != UiRenderMode::DashboardOnly;
        let show_dash = self.render_mode != UiRenderMode::HudOnly && self.store.config.gui_open;

        // Layer 0: HUD + Launch Toast at full requested `cfg.gui_scale` (pure-X11 Full / HudOnly)
        if show_hud {
            let hud_scale = requested_scale.clamp(0.5, 2.5);
            let mut canvas = CpuCanvas::new(out_rgba, w, h, hud_scale);
            self.cpu_hud.render_into_canvas(
                &mut canvas,
                rasterizer,
                &self.store,
                w as f32 / hud_scale,
                h as f32 / hud_scale,
            );
        }

        self.base_hits.clear();
        self.popover_hits.clear();
        self.popover_bounds = Rect::default();
        if !show_dash {
            self.dashboard_bounds = Rect::default();
            return;
        }

        // Compute natural panel dimensions at 1.0x, then Option-A effective scale
        let base_vp_w = (w as f32 / requested_scale.clamp(0.5, 2.5)).max(940.0);
        let dash_w = compute_dashboard_width(base_vp_w);
        let col2_w = compute_dashboard_col2_width(base_vp_w);
        let dash_h = self.estimate_dashboard_height();
        self.last_panel_size = (dash_w, dash_h);

        let eff_scale = resolve_dashboard_scale(requested_scale, w, h, dash_w, dash_h);
        self.last_eff_scale = eff_scale;
        let logical_w = (w as f32 / eff_scale).max(1.0);
        let logical_h = (h as f32 / eff_scale).max(1.0);

        let (dash_x, dash_y) = if let Some((gx, gy)) = self.gui_pos {
            (
                gx.clamp(0.0, (logical_w - dash_w).max(0.0)),
                gy.clamp(0.0, (logical_h - dash_h).max(0.0)),
            )
        } else {
            compute_dashboard_position(logical_w, logical_h, dash_w, dash_h, 0.0, 0.0)
        };
        self.dashboard_bounds = Rect::new(dash_x, dash_y, dash_w, dash_h);

        let mut canvas = CpuCanvas::new(out_rgba, w, h, eff_scale);

        // Main dashboard container
        canvas.fill_rounded_rect(self.dashboard_bounds, 10.0, rgba_u32(15, 19, 28, 240));
        canvas.stroke_rounded_rect(self.dashboard_bounds, 10.0, 1.0, rgba_u32(78, 128, 200, 190));

        let inner_x = dash_x + 8.0;
        let inner_w = dash_w - 16.0;
        let mut cy = dash_y + 8.0;

        // Title bar (drag handle)
        let title_rect = Rect::new(inner_x, cy, inner_w, 26.0);
        canvas.fill_rounded_rect(title_rect, 5.0, rgba_u32(22, 32, 48, 238));
        canvas.stroke_rounded_rect(title_rect, 5.0, 1.0, rgba_u32(66, 98, 146, 185));
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(title_rect.x + 10.0, title_rect.y, 200.0, title_rect.h),
            false,
            12.0,
            rgba_u32(190, 220, 255, 255),
            "FPSHUD Settings",
            false,
        );
        let hint_w = rasterizer.measure_text(false, 11.0, "Drag bar to move");
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(title_rect.right() - hint_w - 10.0, title_rect.y, hint_w + 4.0, title_rect.h),
            false,
            11.0,
            rgba_u32(138, 160, 188, 225),
            "Drag bar to move",
            false,
        );
        self.base_hits.push((title_rect, HitAction::DragHeader));
        cy += 32.0;

        // Header bar (Presets + live FPS/sparkline + Close)
        let header_rect = Rect::new(inner_x, cy, inner_w, 30.0);
        self.base_hits.push((header_rect, HitAction::DragHeader));
        self.paint_header_bar(&mut canvas, rasterizer, header_rect);
        canvas.draw_h_line(inner_x, header_rect.bottom(), inner_w, 1.0, rgba_u32(55, 70, 92, 180));
        cy += 36.0;

        // 3 Columns
        let col_h = (dash_y + dash_h - 8.0 - cy).max(320.0);
        let col1_rect = Rect::new(inner_x, cy, COL1_WIDTH_PX, col_h);
        let col2_rect = Rect::new(col1_rect.right() + 8.0, cy, col2_w, col_h);
        let col3_rect = Rect::new(col2_rect.right() + 8.0, cy, COL3_WIDTH_PX, col_h);

        self.paint_column_layout(&mut canvas, rasterizer, col1_rect);
        self.paint_column_sensors(&mut canvas, rasterizer, col2_rect);
        self.paint_column_actions(&mut canvas, rasterizer, col3_rect);

        // Floating dropdown popover (Z-order layer 2)
        if let Some(id) = self.open_dropdown {
            self.paint_dropdown_popover(&mut canvas, rasterizer, id, logical_w, logical_h);
        }

        // Software cursor (pure-X11 Full mode only)
        if self.render_mode == UiRenderMode::Full && self.draw_software_cursor {
            canvas.draw_software_cursor(self.cursor_logical.0, self.cursor_logical.1);
        }
    }

    fn estimate_dashboard_height(&self) -> f32 {
        // Title(26) + gap(6) + Header(30) + gap(6) + Col1 natural (~492) + pad(16) = 576 px (<= 684 px on 720p)
        576.0
    }

    #[allow(clippy::too_many_arguments)]
    fn paint_button(
        &mut self,
        canvas: &mut CpuCanvas<'_>,
        rasterizer: &mut TextRasterizer,
        rect: Rect,
        label: &str,
        active: bool,
        active_bg: u32,
        center_x: bool,
        action: HitAction,
        is_popover: bool,
    ) {
        let hovered = rect.contains(self.cursor_logical.0, self.cursor_logical.1);
        let bg = if active {
            active_bg
        } else if hovered {
            rgba_u32(38, 48, 66, 240)
        } else {
            rgba_u32(28, 35, 48, 232)
        };
        let border = if active {
            rgba_u32(130, 195, 255, 245)
        } else if hovered {
            rgba_u32(96, 138, 196, 225)
        } else {
            rgba_u32(64, 80, 106, 190)
        };
        canvas.fill_rounded_rect(rect, 4.0, bg);
        canvas.stroke_rounded_rect(rect, 4.0, 1.0, border);
        let text_rect = Rect::new(rect.x + 6.0, rect.y, (rect.w - 12.0).max(1.0), rect.h);
        canvas.draw_text_in_rect(
            rasterizer,
            text_rect,
            false,
            11.0,
            rgba_u32(236, 242, 252, 255),
            label,
            center_x,
        );
        if is_popover {
            self.popover_hits.push((rect, action));
        } else {
            self.base_hits.push((rect, action));
        }
    }

    fn paint_header_bar(
        &mut self,
        canvas: &mut CpuCanvas<'_>,
        rasterizer: &mut TextRasterizer,
        rect: Rect,
    ) {
        let mut x = rect.x;
        let lbl_w = rasterizer.measure_text(false, 12.0, "Presets:");
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(x, rect.y, lbl_w + 4.0, 24.0),
            false,
            12.0,
            rgba_u32(180, 212, 248, 255),
            "Presets:",
            false,
        );
        x += lbl_w + 8.0;

        let presets = [
            ("Minimal", PresetType::Minimal),
            ("Standard", PresetType::Standard),
            ("Advance", PresetType::Advance),
            ("Custom", PresetType::Custom),
        ];
        for (i, (title, preset)) in presets.into_iter().enumerate() {
            let bw = (rasterizer.measure_text(false, 11.0, title) + 18.0).max(58.0);
            let br = Rect::new(x, rect.y + 1.0, bw, 22.0);
            self.preset_bounds[i] = br;
            self.paint_button(
                canvas,
                rasterizer,
                br,
                title,
                self.store.config.preset == preset,
                rgba_u32(42, 114, 212, 235),
                true,
                HitAction::ApplyPreset(preset),
                false,
            );
            x += bw + 5.0;
        }

        // Right side: Close button + Sparkline + Live FPS
        let close_w = 54.0;
        let close_rect = Rect::new(rect.right() - close_w, rect.y + 1.0, close_w, 22.0);
        self.close_bounds = close_rect;
        self.paint_button(
            canvas,
            rasterizer,
            close_rect,
            "Close",
            false,
            rgba_u32(170, 50, 50, 230),
            true,
            HitAction::CloseGui,
            false,
        );

        let spark_rect = Rect::new(close_rect.x - 168.0, rect.y, 160.0, 24.0);
        canvas.draw_frametime_sparkline(
            spark_rect,
            &self.store.telemetry,
            rgba_u32(72, 140, 220, 140),
        );

        let live_txt = format!(
            "{:.0} FPS \u{00B7} {:.1} ms",
            self.store.telemetry.fps, self.store.telemetry.frametime_ms
        );
        let live_w = rasterizer.measure_text(false, 12.0, &live_txt);
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(spark_rect.x - live_w - 10.0, rect.y, live_w + 4.0, 24.0),
            false,
            12.0,
            rgba_u32(255, 215, 0, 255),
            &live_txt,
            false,
        );
    }

    fn paint_column_layout(
        &mut self,
        canvas: &mut CpuCanvas<'_>,
        rasterizer: &mut TextRasterizer,
        rect: Rect,
    ) {
        canvas.fill_rounded_rect(rect, 6.0, rgba_u32(20, 25, 36, 225));
        let ix = rect.x + 7.0;
        let iw = rect.w - 14.0;
        let mut y = rect.y + 7.0;

        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(ix, y, iw, 16.0),
            false,
            12.0,
            rgba_u32(165, 200, 240, 255),
            "1. Layout & Style",
            false,
        );
        y += 20.0;

        // Preview box (248x116)
        let prev_rect = Rect::new(ix, y, PREVIEW_W.min(iw), PREVIEW_H);
        self.preview_bounds = prev_rect;
        canvas.fill_rounded_rect(prev_rect, 6.0, rgba_u32(10, 14, 22, 242));
        canvas.stroke_rounded_rect(prev_rect, 6.0, 1.0, rgba_u32(78, 128, 200, 215));
        canvas.draw_v_line(
            prev_rect.x + prev_rect.w / 3.0,
            prev_rect.y,
            prev_rect.h,
            1.0,
            rgba_u32(78, 128, 200, 38),
        );
        canvas.draw_v_line(
            prev_rect.x + prev_rect.w * 2.0 / 3.0,
            prev_rect.y,
            prev_rect.h,
            1.0,
            rgba_u32(78, 128, 200, 38),
        );
        canvas.draw_h_line(
            prev_rect.x,
            prev_rect.y + prev_rect.h * 0.5,
            prev_rect.w,
            1.0,
            rgba_u32(78, 128, 200, 38),
        );

        let cfg = &self.store.config;
        let (norm_x, norm_y) = match cfg.position {
            LayoutPosition::TopLeft => (0.02, 0.04),
            LayoutPosition::TopCenter => (0.5, 0.04),
            LayoutPosition::TopRight => (0.98, 0.04),
            LayoutPosition::BottomLeft => (0.02, 0.96),
            LayoutPosition::BottomCenter => (0.5, 0.96),
            LayoutPosition::BottomRight => (0.98, 0.96),
            LayoutPosition::Custom => (
                cfg.custom_pos_x.clamp(0.0, 1.0),
                cfg.custom_pos_y.clamp(0.0, 1.0),
            ),
        };
        let dot_x = prev_rect.x
            + (norm_x * (prev_rect.w - PREVIEW_HANDLE_PX))
                .clamp(0.0, prev_rect.w - PREVIEW_HANDLE_PX);
        let dot_y = prev_rect.y
            + (norm_y * (prev_rect.h - PREVIEW_HANDLE_PX))
                .clamp(0.0, prev_rect.h - PREVIEW_HANDLE_PX);
        let handle = Rect::new(dot_x, dot_y, PREVIEW_HANDLE_PX, PREVIEW_HANDLE_PX);
        canvas.fill_rounded_rect(handle, 5.0, rgba_u32(95, 205, 255, 245));
        canvas.stroke_rounded_rect(handle, 5.0, 2.0, rgba_u32(255, 255, 255, 255));

        let pos_badge = if cfg.position == LayoutPosition::Custom {
            format!(
                "Drag HUD \u{00B7} Custom {:.0}%, {:.0}%",
                cfg.custom_pos_x * 100.0,
                cfg.custom_pos_y * 100.0
            )
        } else {
            format!("Drag HUD \u{00B7} {}", position_display_name(cfg.position))
        };
        let badge_w = rasterizer.measure_text(false, 10.0, &pos_badge);
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(
                prev_rect.right() - badge_w - 6.0,
                prev_rect.bottom() - 16.0,
                badge_w + 4.0,
                14.0,
            ),
            false,
            10.0,
            rgba_u32(150, 174, 206, 225),
            &pos_badge,
            false,
        );
        self.base_hits.push((prev_rect, HitAction::DragPreview));
        y += PREVIEW_H + 5.0;

        // 8 Dropdown trigger rows
        let rows: [(&str, String, DropdownId); 8] = [
            (
                "Position",
                position_display_name(self.store.config.position).to_string(),
                DropdownId::Position,
            ),
            (
                "Layout Mode",
                arrangement_display_name(self.store.config.arrangement).to_string(),
                DropdownId::Arrangement,
            ),
            (
                "UI Scale",
                format!("{:.2}x", self.store.config.gui_scale),
                DropdownId::Scale,
            ),
            (
                "Font",
                font_dropdown_label(self.store.config.font_family).to_string(),
                DropdownId::Font,
            ),
            (
                "Font Overlay",
                format!("{:.0} px", self.store.config.font_size_px),
                DropdownId::FontSize,
            ),
            (
                "Text Opacity",
                format!("{:.0}%", self.store.config.text_opacity * 100.0),
                DropdownId::TextOpacity,
            ),
            (
                "BG Style",
                bg_style_display_name(self.store.config.background_style).to_string(),
                DropdownId::BgStyle,
            ),
            (
                "BG Opacity",
                format!("{:.0}%", self.store.config.bg_alpha * 100.0),
                DropdownId::BgOpacity,
            ),
        ];
        for (label, value, id) in rows {
            let row_rect = Rect::new(ix, y, iw, 22.0);
            canvas.draw_text_in_rect(
                rasterizer,
                Rect::new(row_rect.x, row_rect.y, iw - 146.0, row_rect.h),
                false,
                11.0,
                rgba_u32(168, 188, 214, 245),
                label,
                false,
            );
            let trigger = Rect::new(row_rect.right() - 142.0, row_rect.y, 142.0, 21.0);
            self.dropdown_anchors[id.index()] = trigger;
            let is_open = self.open_dropdown == Some(id);
            let bg = if is_open {
                rgba_u32(34, 48, 72, 240)
            } else {
                rgba_u32(26, 33, 46, 230)
            };
            let border = if is_open {
                rgba_u32(110, 185, 255, 240)
            } else {
                rgba_u32(68, 86, 114, 195)
            };
            canvas.fill_rounded_rect(trigger, 4.0, bg);
            canvas.stroke_rounded_rect(trigger, 4.0, 1.0, border);
            canvas.draw_text_in_rect(
                rasterizer,
                Rect::new(trigger.x + 7.0, trigger.y, trigger.w - 24.0, trigger.h),
                false,
                11.0,
                rgba_u32(236, 242, 252, 255),
                &value,
                false,
            );
            canvas.draw_chevron(
                trigger.right() - 10.0,
                trigger.y + trigger.h * 0.5,
                is_open,
                rgba_u32(148, 180, 224, 240),
            );
            self.base_hits
                .push((trigger, HitAction::ToggleDropdown(id)));
            y += 24.0;
        }

        // Divider + 4 Toggle Switches
        y += 2.0;
        canvas.draw_h_line(ix, y, iw, 1.0, rgba_u32(52, 66, 88, 170));
        y += 5.0;

        self.paint_switch_row(
            canvas,
            rasterizer,
            Rect::new(ix, y, iw, 21.0),
            "Category Colors",
            self.store.config.category_colors_enabled,
            HitAction::ToggleCategoryColors,
        );
        y += 23.0;

        // Text Color row: 4 swatches + switch
        let tc_rect = Rect::new(ix, y, iw, 21.0);
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(tc_rect.x, tc_rect.y, 90.0, tc_rect.h),
            false,
            11.0,
            rgba_u32(168, 188, 214, 245),
            "Text Color",
            false,
        );
        let sw_rect = Rect::new(tc_rect.right() - 34.0, tc_rect.y + 1.5, 34.0, 18.0);
        canvas.draw_toggle_switch(sw_rect, self.store.config.custom_text_color_enabled);
        self.base_hits
            .push((sw_rect, HitAction::ToggleCustomTextColor));
        let mut sx = sw_rect.x - 18.0 * 4.0 - 6.0;
        for &(_, swatch) in &TEXT_COLOR_SWATCHES {
            let sr = Rect::new(sx, tc_rect.y + 3.0, 14.0, 14.0);
            canvas.fill_rounded_rect(sr, 3.0, rgba_u32(swatch.r, swatch.g, swatch.b, 255));
            let selected =
                self.store.config.custom_text_color_enabled && self.store.config.text_color == swatch;
            canvas.stroke_rounded_rect(
                sr,
                3.0,
                if selected { 2.0 } else { 1.0 },
                if selected {
                    rgba_u32(255, 255, 255, 255)
                } else {
                    rgba_u32(72, 90, 118, 200)
                },
            );
            self.base_hits.push((sr, HitAction::SetTextColor(swatch)));
            sx += 18.0;
        }
        y += 23.0;

        self.paint_switch_row(
            canvas,
            rasterizer,
            Rect::new(ix, y, iw, 21.0),
            "Warning Colors",
            self.store.config.thermal_warnings_enabled,
            HitAction::ToggleThermalWarnings,
        );
        y += 23.0;

        self.paint_switch_row(
            canvas,
            rasterizer,
            Rect::new(ix, y, iw, 21.0),
            "Frametime Graph",
            self.store.config.hud_graph_enabled,
            HitAction::ToggleHudGraph,
        );
        y += 25.0;

        self.paint_button(
            canvas,
            rasterizer,
            Rect::new(ix, y, iw, 23.0),
            "Reset Layout",
            false,
            rgba_u32(70, 82, 104, 230),
            true,
            HitAction::ResetLayout,
            false,
        );
    }

    fn paint_switch_row(
        &mut self,
        canvas: &mut CpuCanvas<'_>,
        rasterizer: &mut TextRasterizer,
        rect: Rect,
        label: &str,
        enabled: bool,
        action: HitAction,
    ) {
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(rect.x, rect.y, rect.w - 40.0, rect.h),
            false,
            11.0,
            rgba_u32(168, 188, 214, 245),
            label,
            false,
        );
        let sw = Rect::new(rect.right() - 34.0, rect.y + 1.5, 34.0, 18.0);
        canvas.draw_toggle_switch(sw, enabled);
        self.base_hits.push((rect, action));
    }

    fn paint_column_sensors(
        &mut self,
        canvas: &mut CpuCanvas<'_>,
        rasterizer: &mut TextRasterizer,
        rect: Rect,
    ) {
        canvas.fill_rounded_rect(rect, 6.0, rgba_u32(20, 25, 36, 225));
        let ix = rect.x + 7.0;
        let iw = rect.w - 14.0;
        let mut y = rect.y + 7.0;

        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(ix, y, iw, 16.0),
            false,
            12.0,
            rgba_u32(165, 200, 240, 255),
            "2. Telemetry Sensors",
            false,
        );
        y += 20.0;

        // 7 Filter tabs
        let filters = [
            ("All (31)", SensorFilter::All),
            ("Standard", SensorFilter::Standard),
            ("FPS", SensorFilter::Group(MetricCategory::Fps)),
            ("GPU", SensorFilter::Group(MetricCategory::Gpu)),
            ("CPU", SensorFilter::Group(MetricCategory::Cpu)),
            ("RAM", SensorFilter::Group(MetricCategory::Ram)),
            ("Sistem", SensorFilter::Group(MetricCategory::Sys)),
        ];
        let mut fx = ix;
        for (lbl, f) in filters {
            let bw = (rasterizer.measure_text(false, 11.0, lbl) + 14.0).max(40.0);
            self.paint_button(
                canvas,
                rasterizer,
                Rect::new(fx, y, bw, 22.0),
                lbl,
                self.store.sensor_filter == f,
                rgba_u32(42, 114, 212, 235),
                true,
                HitAction::SetSensorFilter(f),
                false,
            );
            fx += bw + 4.0;
        }
        y += 27.0;

        let visible = self.store.filtered_metrics(self.store.sensor_filter);
        match self.store.sensor_filter {
            SensorFilter::Group(cat) => {
                let _ = self.paint_category_card(canvas, rasterizer, ix, y, iw, cat, &visible, 2, 24);
            }
            SensorFilter::All | SensorFilter::Standard => {
                for (cat, cols, max_c) in [
                    (MetricCategory::Fps, 3, 14),
                    (MetricCategory::Gpu, 3, 14),
                    (MetricCategory::Cpu, 3, 14),
                ] {
                    if let Some(h) = self.paint_category_card(
                        canvas, rasterizer, ix, y, iw, cat, &visible, cols, max_c,
                    ) {
                        y += h + 5.0;
                    }
                }
                let has_ram = visible
                    .iter()
                    .any(|m| metric_category(*m) == MetricCategory::Ram);
                let has_sys = visible
                    .iter()
                    .any(|m| metric_category(*m) == MetricCategory::Sys);
                if has_ram && has_sys {
                    let ram_w = 185.0_f32.min(iw * 0.36);
                    let sys_w = (iw - ram_w - 5.0).max(160.0);
                    let _ = self.paint_category_card(
                        canvas,
                        rasterizer,
                        ix,
                        y,
                        ram_w,
                        MetricCategory::Ram,
                        &visible,
                        1,
                        16,
                    );
                    let _ = self.paint_category_card(
                        canvas,
                        rasterizer,
                        ix + ram_w + 5.0,
                        y,
                        sys_w,
                        MetricCategory::Sys,
                        &visible,
                        2,
                        13,
                    );
                } else if has_ram {
                    let _ = self.paint_category_card(
                        canvas,
                        rasterizer,
                        ix,
                        y,
                        iw,
                        MetricCategory::Ram,
                        &visible,
                        2,
                        18,
                    );
                } else if has_sys {
                    let _ = self.paint_category_card(
                        canvas,
                        rasterizer,
                        ix,
                        y,
                        iw,
                        MetricCategory::Sys,
                        &visible,
                        2,
                        16,
                    );
                }
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn paint_category_card(
        &mut self,
        canvas: &mut CpuCanvas<'_>,
        rasterizer: &mut TextRasterizer,
        x: f32,
        y: f32,
        w: f32,
        cat: MetricCategory,
        visible: &[MetricType],
        cols: usize,
        max_val_chars: usize,
    ) -> Option<f32> {
        let items: Vec<MetricType> = visible
            .iter()
            .copied()
            .filter(|m| metric_category(*m) == cat)
            .collect();
        if items.is_empty() {
            return None;
        }
        let cols = cols.max(1);
        let rows = items.len().div_ceil(cols);
        let chip_h = 21.0;
        let gap = 3.0;
        let card_h = 24.0 + (rows as f32) * chip_h + ((rows.saturating_sub(1)) as f32) * gap + 6.0;
        let card_rect = Rect::new(x, y, w, card_h);

        canvas.fill_rounded_rect(card_rect, 5.0, rgba_u32(15, 20, 30, 232));
        canvas.stroke_rounded_rect(card_rect, 5.0, 1.0, rgba_u32(48, 62, 86, 185));

        let accent = category_color_rgba(cat, &self.store.config, 255);
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(x + 7.0, y + 3.0, w - 50.0, 16.0),
            false,
            11.0,
            accent,
            category_display_title(cat),
            false,
        );
        let active_n = items
            .iter()
            .filter(|m| self.store.config.has_metric(**m))
            .count();
        let cnt_txt = format!("{active_n}/{}", items.len());
        let cnt_w = rasterizer.measure_text(false, 10.0, &cnt_txt);
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(x + w - cnt_w - 7.0, y + 3.0, cnt_w + 4.0, 16.0),
            false,
            10.0,
            rgba_u32(140, 160, 188, 220),
            &cnt_txt,
            false,
        );

        let inner_w = w - 12.0;
        let chip_w = (inner_w - gap * ((cols - 1) as f32)) / (cols as f32);
        for (idx, metric) in items.into_iter().enumerate() {
            let r = idx / cols;
            let c = idx % cols;
            let cx = x + 6.0 + (c as f32) * (chip_w + gap);
            let cy = y + 22.0 + (r as f32) * (chip_h + gap);
            let enabled = self.store.config.has_metric(metric);
            let (_, val, _, _) = self.store.format_metric_row(metric);
            let label = hud_metric_short_label(metric);
            let val_disp = truncate_chars(&val, max_val_chars);
            let mark = if enabled { "[x]" } else { "[ ]" };
            let txt = format!("{mark} {label}: {val_disp}");
            self.paint_button(
                canvas,
                rasterizer,
                Rect::new(cx, cy, chip_w, chip_h),
                &txt,
                enabled,
                rgba_u32(34, 96, 175, 225),
                false,
                HitAction::ToggleMetric(metric),
                false,
            );
        }
        Some(card_h)
    }

    fn paint_column_actions(
        &mut self,
        canvas: &mut CpuCanvas<'_>,
        rasterizer: &mut TextRasterizer,
        rect: Rect,
    ) {
        canvas.fill_rounded_rect(rect, 6.0, rgba_u32(20, 25, 36, 225));
        let ix = rect.x + 7.0;
        let iw = rect.w - 14.0;
        let mut y = rect.y + 7.0;

        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(ix, y, iw, 16.0),
            false,
            12.0,
            rgba_u32(165, 200, 240, 255),
            "3. Capture & Hotkeys",
            false,
        );
        y += 22.0;

        self.paint_button(
            canvas,
            rasterizer,
            Rect::new(ix, y, iw, 24.0),
            "Take Screenshot",
            false,
            rgba_u32(42, 114, 212, 235),
            false,
            HitAction::TriggerScreenshot,
            false,
        );
        y += 29.0;

        let bench_lbl = if self.store.telemetry.benchmark_active {
            "Stop Benchmark"
        } else {
            "Start Benchmark"
        };
        self.paint_button(
            canvas,
            rasterizer,
            Rect::new(ix, y, iw, 24.0),
            bench_lbl,
            self.store.telemetry.benchmark_active,
            rgba_u32(210, 75, 65, 235),
            false,
            HitAction::TriggerBenchmark,
            false,
        );
        y += 29.0;

        self.paint_button(
            canvas,
            rasterizer,
            Rect::new(ix, y, iw, 24.0),
            "Open Save Folder",
            false,
            rgba_u32(42, 114, 212, 235),
            false,
            HitAction::TriggerOpenSaveFolder,
            false,
        );
        y += 29.0;

        let half_w = (iw - 4.0) * 0.5;
        let clean = !self.store.config.screenshot_with_hud;
        self.paint_button(
            canvas,
            rasterizer,
            Rect::new(ix, y, half_w, 23.0),
            "Clean Game",
            clean,
            rgba_u32(42, 114, 212, 235),
            true,
            HitAction::SetScreenshotMode(false),
            false,
        );
        self.paint_button(
            canvas,
            rasterizer,
            Rect::new(ix + half_w + 4.0, y, half_w, 23.0),
            "With HUD",
            !clean,
            rgba_u32(42, 114, 212, 235),
            true,
            HitAction::SetScreenshotMode(true),
            false,
        );
        y += 28.0;

        self.paint_button(
            canvas,
            rasterizer,
            Rect::new(ix, y, iw, 24.0),
            "Reset Settings",
            false,
            rgba_u32(175, 58, 58, 235),
            false,
            HitAction::ResetSettings,
            false,
        );
        y += 30.0;

        // Status box
        let status = self
            .store
            .status_message
            .clone()
            .or_else(|| {
                self.store
                    .telemetry
                    .last_benchmark
                    .as_ref()
                    .map(format_benchmark_summary)
            })
            .unwrap_or_else(|| "Pure-Rust CPU UI Ready".to_string());
        let st_rect = Rect::new(ix, y, iw, 24.0);
        canvas.fill_rounded_rect(st_rect, 4.0, rgba_u32(14, 18, 26, 220));
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(st_rect.x + 6.0, st_rect.y, st_rect.w - 12.0, st_rect.h),
            false,
            10.5,
            rgba_u32(150, 225, 175, 255),
            &status,
            false,
        );
        y += 32.0;

        canvas.draw_h_line(ix, y, iw, 1.0, rgba_u32(52, 66, 88, 170));
        y += 6.0;
        canvas.draw_text_in_rect(
            rasterizer,
            Rect::new(ix, y, iw, 16.0),
            false,
            11.0,
            rgba_u32(165, 200, 240, 255),
            "Hotkeys",
            false,
        );
        y += 20.0;

        for (name, target) in [
            ("Toggle GUI", KeybindTarget::ToggleGui),
            ("Toggle HUD", KeybindTarget::ToggleHud),
            ("Benchmark", KeybindTarget::ToggleBenchmark),
            ("Screenshot", KeybindTarget::TakeScreenshot),
        ] {
            let txt = format!("{name}: {}", self.store.keybind_label(target));
            self.paint_button(
                canvas,
                rasterizer,
                Rect::new(ix, y, iw, 23.0),
                &txt,
                self.store.active_keybind_capture == target,
                rgba_u32(195, 130, 35, 240),
                false,
                HitAction::CaptureKeybind(target),
                false,
            );
            y += 27.0;
        }
    }

    fn paint_dropdown_popover(
        &mut self,
        canvas: &mut CpuCanvas<'_>,
        rasterizer: &mut TextRasterizer,
        id: DropdownId,
        logical_w: f32,
        logical_h: f32,
    ) {
        let anchor = self.dropdown_anchors[id.index()];
        let cfg = &self.store.config;

        let (popup_w, cols, items): (f32, usize, Vec<(String, bool, HitAction)>) = match id {
            DropdownId::Position => (
                156.0,
                1,
                POSITION_OPTIONS
                    .iter()
                    .map(|&(l, p)| (l.to_string(), cfg.position == p, HitAction::SelectPosition(p)))
                    .collect(),
            ),
            DropdownId::Arrangement => (
                148.0,
                1,
                ARRANGEMENT_OPTIONS
                    .iter()
                    .map(|&(l, a)| {
                        (
                            l.to_string(),
                            cfg.arrangement == a,
                            HitAction::SelectArrangement(a),
                        )
                    })
                    .collect(),
            ),
            DropdownId::Scale => (
                156.0,
                2,
                SCALE_OPTIONS
                    .iter()
                    .map(|&s| {
                        (
                            format!("{s:.2}x"),
                            (cfg.gui_scale - s).abs() < 0.03,
                            HitAction::SelectScale(s),
                        )
                    })
                    .collect(),
            ),
            DropdownId::Font => (
                156.0,
                1,
                FONT_OPTIONS
                    .iter()
                    .map(|&(l, f)| {
                        (
                            l.to_string(),
                            font_dropdown_label(cfg.font_family) == l,
                            HitAction::SelectFont(f),
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
                        (
                            format!("{sz} px"),
                            (cfg.font_size_px - f32::from(sz)).abs() < 0.6,
                            HitAction::SelectFontSize(sz),
                        )
                    })
                    .collect(),
            ),
            DropdownId::TextOpacity => (
                148.0,
                1,
                TEXT_OPACITY_PCTS
                    .iter()
                    .map(|&p| {
                        (
                            format!("{p}%"),
                            (cfg.text_opacity * 100.0 - f32::from(p)).abs() < 5.0,
                            HitAction::SelectTextOpacity(p),
                        )
                    })
                    .collect(),
            ),
            DropdownId::BgStyle => (
                148.0,
                1,
                BG_STYLE_OPTIONS
                    .iter()
                    .map(|&(l, s)| {
                        (
                            l.to_string(),
                            cfg.background_style == s,
                            HitAction::SelectBgStyle(s),
                        )
                    })
                    .collect(),
            ),
            DropdownId::BgOpacity => (
                220.0,
                4,
                BG_OPACITY_PCTS
                    .iter()
                    .map(|&p| {
                        (
                            format!("{p}%"),
                            (cfg.bg_alpha * 100.0 - f32::from(p)).abs() < 2.6,
                            HitAction::SelectBgOpacity(p),
                        )
                    })
                    .collect(),
            ),
        };

        let cols = cols.max(1);
        let rows = items.len().div_ceil(cols);
        let item_h = 22.0;
        let gap = 3.0;
        let pad = 5.0;
        let popup_h =
            pad * 2.0 + (rows as f32) * item_h + ((rows.saturating_sub(1)) as f32) * gap;
        let (ox, oy) = clamp_popover_origin_rect(anchor, popup_w, popup_h, logical_w, logical_h);
        let pop_rect = Rect::new(ox, oy, popup_w, popup_h);
        self.popover_bounds = pop_rect;

        canvas.fill_rounded_rect(pop_rect, 6.0, rgba_u32(16, 22, 34, 250));
        canvas.stroke_rounded_rect(pop_rect, 6.0, 1.0, rgba_u32(96, 156, 232, 235));

        let inner_w = popup_w - pad * 2.0;
        let item_w = (inner_w - gap * ((cols - 1) as f32)) / (cols as f32);
        for (idx, (label, selected, action)) in items.into_iter().enumerate() {
            let r = idx / cols;
            let c = idx % cols;
            let ix = ox + pad + (c as f32) * (item_w + gap);
            let iy = oy + pad + (r as f32) * (item_h + gap);
            self.paint_button(
                canvas,
                rasterizer,
                Rect::new(ix, iy, item_w, item_h),
                &label,
                selected,
                rgba_u32(42, 114, 212, 240),
                cols > 1,
                action,
                true,
            );
        }
    }
}
