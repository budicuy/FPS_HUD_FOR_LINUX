use fpshud_analytics::BenchmarkSummary;
use fpshud_core::{
    ColorRgba, FontFamily, HudConfig, Keybind, LayoutPosition, MetricType, PresetType,
    apply_preset_advance, apply_preset_custom, apply_preset_minimal, apply_preset_standard,
    keybind::format_keybind, reset_layout_to_default, save_config_to_disk,
};
use fpshud_input::KeybindTarget;
use fpshud_ipc::UiToLayerMsg;
use fpshud_telemetry::TelemetrySnapshot;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SensorFilter {
    All,
    Standard,
    Fps,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MetricCategory {
    Fps,
    Gpu,
    Cpu,
    Ram,
    Sys,
}

#[must_use]
pub const fn metric_category(metric: MetricType) -> MetricCategory {
    match metric {
        MetricType::Fps
        | MetricType::FpsAvg
        | MetricType::Fps1Low
        | MetricType::Fps01Low
        | MetricType::FrameTimes => MetricCategory::Fps,
        MetricType::GpuLoad
        | MetricType::GpuClock
        | MetricType::GpuVramClock
        | MetricType::GpuVram
        | MetricType::GpuBandwidth
        | MetricType::GpuTemp
        | MetricType::GpuFan
        | MetricType::GpuPower => MetricCategory::Gpu,
        MetricType::CpuLoad
        | MetricType::CpuAllCoreLoad
        | MetricType::CpuClock
        | MetricType::CpuAllCoreClock
        | MetricType::CpuPower
        | MetricType::CpuTemp => MetricCategory::Cpu,
        MetricType::Ram
        | MetricType::GameRam
        | MetricType::SwapRam
        | MetricType::MemPressure => MetricCategory::Ram,
        MetricType::EngineApi
        | MetricType::GpuModel
        | MetricType::CpuModel
        | MetricType::SystemClock
        | MetricType::Battery
        | MetricType::DiskIo
        | MetricType::DiskTemp
        | MetricType::NetSpeed => MetricCategory::Sys,
    }
}

#[must_use]
pub const fn category_label(cat: MetricCategory) -> &'static str {
    match cat {
        MetricCategory::Fps => "FPS",
        MetricCategory::Gpu => "GPU",
        MetricCategory::Cpu => "CPU",
        MetricCategory::Ram => "RAM",
        MetricCategory::Sys => "SYS",
    }
}

#[must_use]
pub fn rgba_u32(r: u8, g: u8, b: u8, a: u8) -> u32 {
    (u32::from(r) << 24) | (u32::from(g) << 16) | (u32::from(b) << 8) | u32::from(a)
}

#[must_use]
pub fn color_to_rgba_u32(c: ColorRgba, alpha: u8) -> u32 {
    rgba_u32(c.r, c.g, c.b, alpha)
}

#[must_use]
pub fn category_color_rgba(cat: MetricCategory, cfg: &HudConfig, alpha: u8) -> u32 {
    if !cfg.category_colors_enabled {
        return color_to_rgba_u32(cfg.text_color, alpha);
    }
    match cat {
        MetricCategory::Fps => rgba_u32(255, 215, 0, alpha),
        MetricCategory::Gpu => rgba_u32(118, 225, 0, alpha),
        MetricCategory::Cpu => rgba_u32(0, 229, 255, alpha),
        MetricCategory::Ram => rgba_u32(220, 135, 255, alpha),
        MetricCategory::Sys => rgba_u32(190, 210, 230, alpha),
    }
}

/// Evaluates thermal/load warning level `(color_rgba_u32, is_warning)`.
#[must_use]
pub fn resolve_metric_value_color(
    metric: MetricType,
    snap: &TelemetrySnapshot,
    cfg: &HudConfig,
    alpha: u8,
) -> (u32, bool) {
    let base = color_to_rgba_u32(cfg.text_color, alpha);
    if !cfg.thermal_warnings_enabled {
        return (base, false);
    }
    let col_red = rgba_u32(255, 70, 70, alpha);
    let col_yellow = rgba_u32(255, 185, 0, alpha);

    match metric {
        MetricType::Fps | MetricType::FpsAvg => {
            let val = if metric == MetricType::Fps { snap.fps } else { snap.fps_avg };
            if val.is_finite() && val > 0.1 {
                if val < 30.0 {
                    return (col_red, true);
                }
                if val < 45.0 {
                    return (col_yellow, true);
                }
            }
        }
        MetricType::Fps1Low | MetricType::Fps01Low => {
            let val = if metric == MetricType::Fps1Low {
                snap.fps_1pct_low
            } else {
                snap.fps_01pct_low
            };
            if val.is_finite() && val > 0.1 {
                if val < 25.0 || (snap.fps_avg > 10.0 && val < 0.45 * snap.fps_avg) {
                    return (col_red, true);
                }
                if val < 45.0 || (snap.fps_avg > 10.0 && val < 0.60 * snap.fps_avg) {
                    return (col_yellow, true);
                }
            }
        }
        MetricType::FrameTimes => {
            if snap.frametime_ms.is_finite() {
                if snap.frametime_ms > 33.3 {
                    return (col_red, true);
                }
                if snap.frametime_ms > 22.2 {
                    return (col_yellow, true);
                }
            }
        }
        MetricType::GpuLoad | MetricType::CpuLoad => {
            let load = if metric == MetricType::GpuLoad {
                snap.gpu_load
            } else {
                snap.cpu_load
            };
            if load.is_finite() {
                if load >= 95.0 {
                    return (col_red, true);
                }
                if load >= 80.0 {
                    return (col_yellow, true);
                }
            }
        }
        MetricType::GpuTemp | MetricType::CpuTemp => {
            let temp = if metric == MetricType::GpuTemp {
                snap.gpu_temp_c
            } else {
                snap.cpu_temp_c
            };
            if temp.is_finite() {
                if temp >= 82.0 {
                    return (col_red, true);
                }
                if temp >= 72.0 {
                    return (col_yellow, true);
                }
            }
        }
        MetricType::GpuVram => {
            if snap.gpu_vram_total_gb > 0.1 && snap.gpu_vram_used_gb.is_finite() {
                let pct = 100.0 * snap.gpu_vram_used_gb / snap.gpu_vram_total_gb;
                if pct >= 92.0 {
                    return (col_red, true);
                }
                if pct >= 80.0 {
                    return (col_yellow, true);
                }
            }
        }
        MetricType::GpuFan => {
            if snap.gpu_fan_pct >= 90 {
                return (col_red, true);
            }
            if snap.gpu_fan_pct >= 75 {
                return (col_yellow, true);
            }
        }
        MetricType::Ram | MetricType::GameRam => {
            if snap.ram_total_gb > 0.1 && snap.ram_used_gb.is_finite() {
                let pct = 100.0 * snap.ram_used_gb / snap.ram_total_gb;
                if pct >= 90.0 {
                    return (col_red, true);
                }
                if pct >= 80.0 {
                    return (col_yellow, true);
                }
            }
        }
        MetricType::SwapRam => {
            if snap.swap_total_gb > 0.0 && snap.swap_used_gb.is_finite() {
                let pct = 100.0 * snap.swap_used_gb / snap.swap_total_gb;
                if pct >= 80.0 || snap.swap_used_gb >= 4.0 {
                    return (col_red, true);
                }
                if pct >= 50.0 || snap.swap_used_gb >= 2.0 {
                    return (col_yellow, true);
                }
            }
        }
        MetricType::MemPressure => {
            if snap.mem_psi_some_pct.is_finite() {
                if snap.mem_psi_some_pct >= 20.0 {
                    return (col_red, true);
                }
                if snap.mem_psi_some_pct >= 5.0 {
                    return (col_yellow, true);
                }
            }
        }
        MetricType::DiskTemp => {
            if snap.nvme_temp_available && snap.nvme_temp_c.is_finite() {
                if snap.nvme_temp_c >= 75.0 {
                    return (col_red, true);
                }
                if snap.nvme_temp_c >= 65.0 {
                    return (col_yellow, true);
                }
            }
        }
        MetricType::DiskIo => {
            let total = snap.disk_read_mbs + snap.disk_write_mbs;
            if total.is_finite() {
                if total >= 1500.0 {
                    return (col_red, true);
                }
                if total >= 500.0 {
                    return (col_yellow, true);
                }
            }
        }
        MetricType::Battery
            if snap.battery_available && !snap.battery_charging && snap.battery_pct.is_finite() =>
        {
            if snap.battery_pct <= 12.0 {
                return (col_red, true);
            }
            if snap.battery_pct <= 25.0 {
                return (col_yellow, true);
            }
        }
        _ => {}
    }

    (base, false)
}

#[must_use]
pub const fn font_family_name(font: FontFamily) -> &'static str {
    match font {
        FontFamily::SansBold => "Open Sans",
        FontFamily::MonoBold | FontFamily::VeraMono | FontFamily::AdwaitaMono => {
            "Bitstream Vera Sans Mono"
        }
    }
}

#[must_use]
pub fn format_benchmark_summary(summary: &BenchmarkSummary) -> String {
    format!(
        "Bench {:.0} avg | 1% {:.0} | 0.1% {:.0} ({} frames)",
        summary.avg_fps, summary.fps_1pct_low, summary.fps_01pct_low, summary.total_frames
    )
}

/// Central pure-Rust UI state shared across HUD, toast hint, & 3-column dashboard.
#[derive(Debug, Clone)]
pub struct UiStore {
    pub config: HudConfig,
    pub telemetry: TelemetrySnapshot,
    pub sensor_filter: SensorFilter,
    pub active_keybind_capture: KeybindTarget,
    pub status_message: Option<String>,
    pub elapsed_sec: f32,
    outgoing: Vec<UiToLayerMsg>,
}

impl UiStore {
    #[must_use]
    pub fn new(config: HudConfig, telemetry: TelemetrySnapshot) -> Self {
        Self {
            config,
            telemetry,
            sensor_filter: SensorFilter::All,
            active_keybind_capture: KeybindTarget::None,
            status_message: None,
            elapsed_sec: 0.0,
            outgoing: Vec::new(),
        }
    }

    #[must_use]
    pub fn filtered_metrics(&self, filter: SensorFilter) -> Vec<MetricType> {
        match filter {
            SensorFilter::All => MetricType::ALL.to_vec(),
            SensorFilter::Standard => HudConfig::default().metrics,
            SensorFilter::Fps => vec![
                MetricType::Fps,
                MetricType::FpsAvg,
                MetricType::Fps1Low,
                MetricType::Fps01Low,
                MetricType::FrameTimes,
            ],
        }
    }

    /// Formats `(label, value, value_rgba_u32, is_warning)` for any of the 31 metrics.
    #[must_use]
    pub fn format_metric_row(&self, metric: MetricType) -> (String, String, u32, bool) {
        let label = metric.display_name().to_string();
        let raw = self.telemetry.format_metric_value(metric);
        let value = if raw.trim().is_empty() || raw.contains("NaN") {
            "N/A".to_string()
        } else {
            raw
        };
        let alpha = (self.config.text_opacity.clamp(0.1, 1.0) * 255.0).round() as u8;
        let (color, is_warning) =
            resolve_metric_value_color(metric, &self.telemetry, &self.config, alpha);
        (label, value, color, is_warning)
    }

    pub fn set_custom_pos(&mut self, x: f32, y: f32) {
        self.config.position = LayoutPosition::Custom;
        self.config.custom_pos_x = if x.is_finite() { x.clamp(0.0, 1.0) } else { 0.0 };
        self.config.custom_pos_y = if y.is_finite() { y.clamp(0.0, 1.0) } else { 0.0 };
        self.emit_config_updated();
    }

    pub fn set_position(&mut self, pos: LayoutPosition) {
        self.config.position = pos;
        self.emit_config_updated();
    }

    pub fn apply_preset(&mut self, preset: PresetType) {
        match preset {
            PresetType::Minimal => apply_preset_minimal(&mut self.config),
            PresetType::Standard => apply_preset_standard(&mut self.config),
            PresetType::Advance => apply_preset_advance(&mut self.config),
            PresetType::Custom => apply_preset_custom(&mut self.config),
        }
        self.status_message = Some(format!("Preset {} applied.", preset.as_str()));
        self.emit_config_updated();
    }

    pub fn toggle_metric(&mut self, metric: MetricType) {
        let active = !self.config.has_metric(metric);
        self.config.set_metric(metric, active);
        self.config.preset = PresetType::Custom;
        self.emit_config_updated();
    }

    pub fn step_scale(&mut self, delta: f32) {
        self.config.gui_scale = (self.config.gui_scale + delta).clamp(0.5, 2.5);
        self.emit_config_updated();
    }

    pub fn step_font_size(&mut self, delta: f32) {
        self.config.font_size_px = (self.config.font_size_px + delta).clamp(11.0, 36.0);
        self.emit_config_updated();
    }

    pub fn step_text_opacity(&mut self, delta: f32) {
        self.config.text_opacity = (self.config.text_opacity + delta).clamp(0.1, 1.0);
        self.emit_config_updated();
    }

    pub fn step_bg_alpha(&mut self, delta: f32) {
        self.config.bg_alpha = (self.config.bg_alpha + delta).clamp(0.05, 0.95);
        self.emit_config_updated();
    }

    pub fn toggle_hud_graph(&mut self) {
        self.config.hud_graph_enabled = !self.config.hud_graph_enabled;
        self.emit_config_updated();
    }

    pub fn cycle_text_color(&mut self) {
        const PALETTE: [ColorRgba; 4] = [
            ColorRgba::new(255, 255, 255, 255),
            ColorRgba::new(140, 225, 255, 255),
            ColorRgba::new(150, 255, 165, 255),
            ColorRgba::new(255, 220, 120, 255),
        ];
        let next_idx = PALETTE
            .iter()
            .position(|&c| c == self.config.text_color)
            .map_or(0, |i| (i + 1) % PALETTE.len());
        self.config.text_color = PALETTE[next_idx];
        self.emit_config_updated();
    }

    pub fn reset_layout(&mut self) {
        reset_layout_to_default(&mut self.config);
        self.status_message = Some("Layout reset to default.".to_string());
        self.emit_config_updated();
    }

    pub fn trigger_screenshot(&mut self) {
        self.outgoing.push(UiToLayerMsg::ScreenshotRequest {
            with_hud: self.config.screenshot_with_hud,
        });
        self.status_message = Some("Screenshot queued.".to_string());
    }

    pub fn trigger_benchmark(&mut self) {
        self.outgoing.push(UiToLayerMsg::BenchmarkToggle);
        self.status_message = Some(if self.telemetry.benchmark_active {
            "Benchmark stopping...".to_string()
        } else {
            "Benchmark started.".to_string()
        });
    }

    pub fn trigger_open_save_folder(&mut self) {
        self.outgoing.push(UiToLayerMsg::OpenSaveFolder);
        self.status_message = Some("Opening save folder...".to_string());
    }

    pub fn set_screenshot_mode(&mut self, with_hud: bool) {
        self.config.screenshot_with_hud = with_hud;
        self.emit_config_updated();
    }

    pub fn save_configuration(&mut self) {
        let msg = if save_config_to_disk(&self.config) {
            "Configuration saved.".to_string()
        } else {
            "Save failed.".to_string()
        };
        self.status_message = Some(msg);
        self.emit_config_updated();
    }

    pub fn request_keybind_capture(&mut self, target: KeybindTarget) {
        self.active_keybind_capture = target;
        self.outgoing.push(UiToLayerMsg::KeybindCaptureState {
            capturing: target != KeybindTarget::None,
            target,
        });
    }

    #[must_use]
    pub fn keybind_label(&self, target: KeybindTarget) -> String {
        if self.active_keybind_capture == target && target != KeybindTarget::None {
            return "Press keys...".to_string();
        }
        let kb: Keybind = match target {
            KeybindTarget::ToggleGui => self.config.bind_toggle_gui,
            KeybindTarget::ToggleHud => self.config.bind_toggle_hud,
            KeybindTarget::ToggleBenchmark => self.config.bind_benchmark,
            KeybindTarget::TakeScreenshot => self.config.bind_screenshot,
            KeybindTarget::None => return "None".to_string(),
        };
        format_keybind(kb)
    }

    pub fn take_outgoing(&mut self) -> Vec<UiToLayerMsg> {
        std::mem::take(&mut self.outgoing)
    }

    pub fn emit_config_updated(&mut self) {
        self.outgoing
            .push(UiToLayerMsg::ConfigUpdated(self.config.clone()));
    }
}
