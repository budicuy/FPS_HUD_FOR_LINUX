use crate::paths::get_config_file_path;
use serde::{Deserialize, Serialize};
use std::fs::{self, File};
use std::io::Write;
use std::path::Path;

/// Canonical 31 HUD telemetry metrics matching the legacy C++ config order.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum MetricType {
    Fps = 0,
    FpsAvg,
    Fps1Low,
    Fps01Low,
    FrameTimes,
    GpuLoad,
    GpuClock,
    GpuVramClock,
    GpuVram,
    GpuBandwidth,
    GpuTemp,
    GpuFan,
    GpuPower,
    CpuLoad,
    CpuAllCoreLoad,
    CpuClock,
    CpuAllCoreClock,
    CpuPower,
    CpuTemp,
    Ram,
    GameRam,
    SwapRam,
    MemPressure,
    EngineApi,
    GpuModel,
    CpuModel,
    SystemClock,
    Battery,
    DiskIo,
    DiskTemp,
    NetSpeed,
}

impl MetricType {
    /// Total number of supported telemetry metrics.
    pub const ALL: &'static [MetricType] = &[
        Self::Fps,
        Self::FpsAvg,
        Self::Fps1Low,
        Self::Fps01Low,
        Self::FrameTimes,
        Self::GpuLoad,
        Self::GpuClock,
        Self::GpuVramClock,
        Self::GpuVram,
        Self::GpuBandwidth,
        Self::GpuTemp,
        Self::GpuFan,
        Self::GpuPower,
        Self::CpuLoad,
        Self::CpuAllCoreLoad,
        Self::CpuClock,
        Self::CpuAllCoreClock,
        Self::CpuPower,
        Self::CpuTemp,
        Self::Ram,
        Self::GameRam,
        Self::SwapRam,
        Self::MemPressure,
        Self::EngineApi,
        Self::GpuModel,
        Self::CpuModel,
        Self::SystemClock,
        Self::Battery,
        Self::DiskIo,
        Self::DiskTemp,
        Self::NetSpeed,
    ];

    /// Stable INI key used in `config.ini`.
    #[must_use]
    pub const fn id_key(self) -> &'static str {
        match self {
            Self::Fps => "fps",
            Self::FpsAvg => "fpsavg",
            Self::Fps1Low => "fps1low",
            Self::Fps01Low => "fps01low",
            Self::FrameTimes => "frametimes",
            Self::GpuLoad => "gpuload",
            Self::GpuClock => "gpuclock",
            Self::GpuVramClock => "gpuvramclock",
            Self::GpuVram => "gpuvram",
            Self::GpuBandwidth => "gpubw",
            Self::GpuTemp => "gputemp",
            Self::GpuFan => "gpufan",
            Self::GpuPower => "gpuwatt",
            Self::CpuLoad => "cpuload",
            Self::CpuAllCoreLoad => "cpuallload",
            Self::CpuClock => "cpuclock",
            Self::CpuAllCoreClock => "cpuallclock",
            Self::CpuPower => "cpuwatt",
            Self::CpuTemp => "cputemp",
            Self::Ram => "ram",
            Self::GameRam => "gameram",
            Self::SwapRam => "swap",
            Self::MemPressure => "mempsi",
            Self::EngineApi => "api",
            Self::GpuModel => "gpumodel",
            Self::CpuModel => "cpumodel",
            Self::SystemClock => "clock",
            Self::Battery => "battery",
            Self::DiskIo => "disk_io",
            Self::DiskTemp => "disk_temp",
            Self::NetSpeed => "net_speed",
        }
    }

    /// Parses a metric key, preserving legacy aliases like `cpuhybrid`.
    #[must_use]
    pub fn from_id_key(key: &str) -> Option<Self> {
        let trimmed = key.trim();
        if trimmed.eq_ignore_ascii_case("cpuhybrid") {
            return Some(Self::CpuAllCoreLoad);
        }
        Self::ALL
            .iter()
            .copied()
            .find(|m| m.id_key().eq_ignore_ascii_case(trimmed))
    }

    /// Human-friendly display label for UI rows.
    #[must_use]
    pub const fn display_name(self) -> &'static str {
        match self {
            Self::Fps => "FPS",
            Self::FpsAvg => "FPS Avg",
            Self::Fps1Low => "1% Low",
            Self::Fps01Low => "0.1% Low",
            Self::FrameTimes => "Frametime",
            Self::GpuLoad => "GPU Load",
            Self::GpuClock => "GPU Clock",
            Self::GpuVramClock => "VRAM Clock",
            Self::GpuVram => "VRAM",
            Self::GpuBandwidth => "GPU BW",
            Self::GpuTemp => "GPU Temp",
            Self::GpuFan => "GPU Fan",
            Self::GpuPower => "GPU Power",
            Self::CpuLoad => "CPU Load",
            Self::CpuAllCoreLoad => "CPU Cores",
            Self::CpuClock => "CPU Clock",
            Self::CpuAllCoreClock => "CPU Clocks",
            Self::CpuPower => "CPU Power",
            Self::CpuTemp => "CPU Temp",
            Self::Ram => "RAM",
            Self::GameRam => "Game RAM",
            Self::SwapRam => "Swap",
            Self::MemPressure => "Mem PSI",
            Self::EngineApi => "API",
            Self::GpuModel => "GPU",
            Self::CpuModel => "CPU",
            Self::SystemClock => "Clock",
            Self::Battery => "Battery",
            Self::DiskIo => "Disk I/O",
            Self::DiskTemp => "Disk Temp",
            Self::NetSpeed => "Network",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum PresetType {
    Minimal = 0,
    Standard = 1,
    Advance = 2,
    Custom = 3,
}

impl PresetType {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Minimal => "minimal",
            Self::Standard => "standard",
            Self::Advance => "advance",
            Self::Custom => "custom",
        }
    }

    #[must_use]
    pub fn parse_name(s: &str) -> Self {
        match s.trim().to_ascii_lowercase().as_str() {
            "minimal" => Self::Minimal,
            "advance" => Self::Advance,
            "custom" => Self::Custom,
            _ => Self::Standard,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum LayoutPosition {
    TopLeft = 0,
    TopCenter = 1,
    TopRight = 2,
    BottomLeft = 3,
    BottomCenter = 4,
    BottomRight = 5,
    Custom = 6,
}

impl LayoutPosition {
    #[must_use]
    pub const fn as_i32(self) -> i32 {
        match self {
            Self::TopLeft => 0,
            Self::TopCenter => 1,
            Self::TopRight => 2,
            Self::BottomLeft => 3,
            Self::BottomCenter => 4,
            Self::BottomRight => 5,
            Self::Custom => 6,
        }
    }

    #[must_use]
    pub const fn from_i32(v: i32) -> Self {
        match v {
            1 => Self::TopCenter,
            2 => Self::TopRight,
            3 => Self::BottomLeft,
            4 => Self::BottomCenter,
            5 => Self::BottomRight,
            6 => Self::Custom,
            _ => Self::TopLeft,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ArrangementMode {
    Stack = 0,
    Double = 1,
    Linear = 2,
}

impl ArrangementMode {
    #[must_use]
    pub const fn as_i32(self) -> i32 {
        match self {
            Self::Stack => 0,
            Self::Double => 1,
            Self::Linear => 2,
        }
    }

    #[must_use]
    pub const fn from_i32(v: i32) -> Self {
        match v {
            1 => Self::Double,
            2 => Self::Linear,
            _ => Self::Stack,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum BackgroundStyle {
    None = 0,
    Translucent = 1,
    Solid = 2,
}

impl BackgroundStyle {
    #[must_use]
    pub const fn as_i32(self) -> i32 {
        match self {
            Self::None => 0,
            Self::Translucent => 1,
            Self::Solid => 2,
        }
    }

    #[must_use]
    pub const fn from_i32(v: i32) -> Self {
        match v {
            0 => Self::None,
            2 => Self::Solid,
            _ => Self::Translucent,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum FontFamily {
    SansBold = 0,
    MonoBold = 1,
    VeraMono = 2,
    AdwaitaMono = 3,
}

impl FontFamily {
    #[must_use]
    pub const fn as_i32(self) -> i32 {
        match self {
            Self::SansBold => 0,
            Self::MonoBold => 1,
            Self::VeraMono => 2,
            Self::AdwaitaMono => 3,
        }
    }

    #[must_use]
    pub const fn from_i32(v: i32) -> Self {
        match v {
            1 => Self::MonoBold,
            2 => Self::VeraMono,
            3 => Self::AdwaitaMono,
            _ => Self::SansBold,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct ColorRgba {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

impl ColorRgba {
    #[must_use]
    pub const fn new(r: u8, g: u8, b: u8, a: u8) -> Self {
        Self { r, g, b, a }
    }
}

impl Default for ColorRgba {
    fn default() -> Self {
        Self {
            r: 255,
            g: 255,
            b: 255,
            a: 255,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Keybind {
    pub mod_key: u32,
    pub main_key: u32,
}

impl Keybind {
    #[must_use]
    pub const fn new(mod_key: u32, main_key: u32) -> Self {
        Self { mod_key, main_key }
    }
}

/// Complete FPSHUD runtime and overlay configuration (zero recorder/CLI fields).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct HudConfig {
    pub enabled: bool,
    pub gui_open: bool,
    pub hud_visible: bool,

    pub preset: PresetType,
    pub metrics: Vec<MetricType>,
    pub position: LayoutPosition,
    pub arrangement: ArrangementMode,
    pub background_style: BackgroundStyle,
    pub font_family: FontFamily,
    pub text_color: ColorRgba,
    pub custom_text_color_enabled: bool,

    pub category_colors_enabled: bool,
    pub thermal_warnings_enabled: bool,
    pub dim_labels_enabled: bool,
    pub hud_graph_enabled: bool,

    // Legacy single-key dropdown indices
    pub key_toggle_gui: i32,
    pub key_toggle_hud: i32,
    pub key_benchmark: i32,

    // 4 Active Keybinds (Screen Recorder keybinds removed permanently)
    pub bind_toggle_gui: Keybind,
    pub bind_toggle_hud: Keybind,
    pub bind_benchmark: Keybind,
    pub bind_screenshot: Keybind,

    // Clean Game vs With HUD screenshot capture
    pub screenshot_with_hud: bool,
    pub save_folder: String,

    // Typography & Scale
    pub gui_scale: f32,
    pub font_size_px: f32,
    pub text_opacity: f32,
    pub bg_alpha: f32,
    pub border_enabled: bool,
    pub border_alpha: f32,
    pub border_color: ColorRgba,

    pub outline_enabled: bool,
    pub outline_thickness: f32,
    pub shadow_enabled: bool,
    pub shadow_offset: f32,
    pub shadow_opacity: f32,

    pub margin_x: f32,
    pub margin_y: f32,
    pub custom_pos_x: f32,
    pub custom_pos_y: f32,
}

impl Default for HudConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            gui_open: false,
            hud_visible: true,
            preset: PresetType::Standard,
            metrics: default_standard_metrics(),
            position: LayoutPosition::TopLeft,
            arrangement: ArrangementMode::Stack,
            background_style: BackgroundStyle::Translucent,
            font_family: FontFamily::SansBold,
            text_color: ColorRgba::default(),
            custom_text_color_enabled: false,
            category_colors_enabled: true,
            thermal_warnings_enabled: true,
            dim_labels_enabled: true,
            hud_graph_enabled: false,
            key_toggle_gui: 0,
            key_toggle_hud: 0,
            key_benchmark: 0,
            bind_toggle_gui: Keybind::new(0, 88),
            bind_toggle_hud: Keybind::new(0, 87),
            bind_benchmark: Keybind::new(0, 67),
            bind_screenshot: Keybind::new(0, 68),
            screenshot_with_hud: false,
            save_folder: String::new(),
            gui_scale: 1.0,
            font_size_px: 20.0,
            text_opacity: 1.0,
            bg_alpha: 0.38,
            border_enabled: true,
            border_alpha: 0.50,
            border_color: ColorRgba::new(72, 110, 155, 255),
            outline_enabled: true,
            outline_thickness: 1.4,
            shadow_enabled: true,
            shadow_offset: 2.0,
            shadow_opacity: 0.85,
            margin_x: 16.0,
            margin_y: 14.0,
            custom_pos_x: 0.01,
            custom_pos_y: 0.02,
        }
    }
}

impl HudConfig {
    #[must_use]
    pub fn has_metric(&self, m: MetricType) -> bool {
        self.metrics.contains(&m)
    }

    pub fn set_metric(&mut self, m: MetricType, active: bool) {
        if active {
            if !self.metrics.contains(&m) {
                self.metrics.push(m);
            }
        } else {
            self.metrics.retain(|&x| x != m);
        }
    }
}

fn default_standard_metrics() -> Vec<MetricType> {
    vec![
        MetricType::Fps,
        MetricType::FrameTimes,
        MetricType::GpuLoad,
        MetricType::GpuClock,
        MetricType::GpuTemp,
        MetricType::CpuLoad,
        MetricType::CpuClock,
        MetricType::CpuPower,
        MetricType::CpuTemp,
        MetricType::Ram,
    ]
}

pub fn apply_preset_minimal(cfg: &mut HudConfig) {
    cfg.preset = PresetType::Minimal;
    cfg.metrics = vec![MetricType::Fps, MetricType::FrameTimes];
    cfg.position = LayoutPosition::TopLeft;
    cfg.arrangement = ArrangementMode::Stack;
    cfg.background_style = BackgroundStyle::Translucent;
    cfg.bg_alpha = 0.35;
}

pub fn apply_preset_standard(cfg: &mut HudConfig) {
    cfg.preset = PresetType::Standard;
    cfg.metrics = default_standard_metrics();
    cfg.position = LayoutPosition::TopLeft;
    cfg.arrangement = ArrangementMode::Stack;
    cfg.background_style = BackgroundStyle::Translucent;
    cfg.bg_alpha = 0.38;
}

pub fn apply_preset_advance(cfg: &mut HudConfig) {
    cfg.preset = PresetType::Advance;
    cfg.metrics = MetricType::ALL.to_vec();
    cfg.position = LayoutPosition::TopLeft;
    cfg.arrangement = ArrangementMode::Stack;
    cfg.background_style = BackgroundStyle::Translucent;
    cfg.bg_alpha = 0.42;
}

pub fn apply_preset_custom(cfg: &mut HudConfig) {
    cfg.arrangement = ArrangementMode::Stack;
    if !load_config_from_disk(cfg) {
        apply_preset_standard(cfg);
        cfg.text_color = ColorRgba::new(0, 229, 255, 255);
        cfg.custom_text_color_enabled = true;
        cfg.font_size_px = 21.0;
    }
    cfg.preset = PresetType::Custom;
}

pub fn reset_layout_to_default(cfg: &mut HudConfig) {
    cfg.position = LayoutPosition::TopLeft;
    cfg.arrangement = ArrangementMode::Stack;
    cfg.custom_pos_x = 0.01;
    cfg.custom_pos_y = 0.02;
    cfg.gui_scale = 1.0;
    cfg.margin_x = 16.0;
    cfg.margin_y = 14.0;
}

#[must_use]
pub fn init_config_from_env(val: Option<&str>) -> HudConfig {
    let mut cfg = HudConfig::default();
    let Some(raw) = val else {
        cfg.enabled = false;
        return cfg;
    };
    if raw.trim() != "1" {
        cfg.enabled = false;
        return cfg;
    }
    cfg.enabled = true;
    if !load_config_from_disk(&mut cfg) {
        apply_preset_standard(&mut cfg);
    }
    cfg
}

pub fn save_config_to_disk(cfg: &HudConfig) -> bool {
    let path = get_config_file_path();
    save_ini_file(&path, cfg)
}

pub fn load_config_from_disk(cfg: &mut HudConfig) -> bool {
    let path = get_config_file_path();
    load_ini_file(&path, cfg)
}

fn sanitize_ini_str(val: &str) -> String {
    val.chars()
        .filter(|&c| c != '\r' && c != '\n')
        .collect()
}

fn parse_finite_clamped(val: &str, current: f32, min: f32, max: f32) -> f32 {
    match val.trim().parse::<f32>() {
        Ok(parsed) if parsed.is_finite() => parsed.clamp(min, max),
        _ => current,
    }
}

fn parse_i32_or(val: &str, fallback: i32) -> i32 {
    val.trim().parse::<i32>().unwrap_or(fallback)
}

fn parse_rgb(val: &str) -> Option<ColorRgba> {
    let mut parts = val.split(',');
    let r = parts.next()?.trim().parse::<i32>().ok()?.clamp(0, 255) as u8;
    let g = parts.next()?.trim().parse::<i32>().ok()?.clamp(0, 255) as u8;
    let b = parts.next()?.trim().parse::<i32>().ok()?.clamp(0, 255) as u8;
    if parts.next().is_some() {
        return None;
    }
    Some(ColorRgba::new(r, g, b, 255))
}

fn normalize_legacy_mod(raw_mod: u32) -> u32 {
    use crate::keybind::{MOD_ALT, MOD_CTRL, MOD_SHIFT, MOD_SUPER};
    match raw_mod {
        0 => 0,
        1..=15 => raw_mod,
        29 | 97 => MOD_CTRL,
        56 | 100 => MOD_ALT,
        42 | 54 => MOD_SHIFT,
        125 | 126 => MOD_SUPER,
        _ => raw_mod,
    }
}

fn parse_keybind(val: &str) -> Option<Keybind> {
    let mut parts = val.split(',');
    let raw_mod = parts.next()?.trim().parse::<u32>().ok()?;
    let main_key = parts.next()?.trim().parse::<u32>().ok()?;
    if parts.next().is_some() {
        return None;
    }
    if main_key == 0 || main_key >= 512 || raw_mod >= 512 {
        return None;
    }
    Some(Keybind::new(normalize_legacy_mod(raw_mod), main_key))
}

pub fn load_ini_file(path: &Path, cfg: &mut HudConfig) -> bool {
    let Ok(contents) = fs::read_to_string(path) else {
        return false;
    };

    let keep_enabled = cfg.enabled;
    let keep_open = cfg.gui_open;
    let mut parsed_any = false;
    let mut saw_text_color = false;
    let mut saw_custom_text_color_flag = false;

    for raw_line in contents.lines() {
        let line = raw_line.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') || line.starts_with('[')
        {
            continue;
        }
        let Some((raw_key, raw_val)) = line.split_once('=') else {
            continue;
        };
        let key = raw_key.trim();
        let val = raw_val.trim();

        match key {
            "preset" => {
                cfg.preset = PresetType::parse_name(val);
                parsed_any = true;
            }
            "gui_scale" => {
                cfg.gui_scale = parse_finite_clamped(val, cfg.gui_scale, 0.50, 2.00);
                parsed_any = true;
            }
            "hud_visible" => {
                cfg.hud_visible = parse_i32_or(val, 1) != 0;
                parsed_any = true;
            }
            "layout" => {
                cfg.position = LayoutPosition::from_i32(parse_i32_or(val, 0).clamp(0, 6));
                parsed_any = true;
            }
            "custom_pos_x" => {
                cfg.custom_pos_x = parse_finite_clamped(val, cfg.custom_pos_x, 0.0, 1.0);
                parsed_any = true;
            }
            "custom_pos_y" => {
                cfg.custom_pos_y = parse_finite_clamped(val, cfg.custom_pos_y, 0.0, 1.0);
                parsed_any = true;
            }
            "arrangement" => {
                cfg.arrangement = ArrangementMode::from_i32(parse_i32_or(val, 0).clamp(0, 2));
                parsed_any = true;
            }
            "background" => {
                cfg.background_style = BackgroundStyle::from_i32(parse_i32_or(val, 1).clamp(0, 2));
                parsed_any = true;
            }
            "font_family" => {
                cfg.font_family = FontFamily::from_i32(parse_i32_or(val, 0).clamp(0, 3));
                parsed_any = true;
            }
            "font_size_px" => {
                cfg.font_size_px = parse_finite_clamped(val, cfg.font_size_px, 10.0, 64.0);
                parsed_any = true;
            }
            "text_color" => {
                if let Some(c) = parse_rgb(val) {
                    cfg.text_color = c;
                    saw_text_color = true;
                }
                parsed_any = true;
            }
            "custom_text_color_enabled" => {
                cfg.custom_text_color_enabled = parse_i32_or(val, 0) != 0;
                saw_custom_text_color_flag = true;
                parsed_any = true;
            }
            "text_opacity" => {
                cfg.text_opacity = parse_finite_clamped(val, cfg.text_opacity, 0.05, 1.0);
                parsed_any = true;
            }
            "bg_alpha" => {
                cfg.bg_alpha = parse_finite_clamped(val, cfg.bg_alpha, 0.0, 1.0);
                parsed_any = true;
            }
            "border_enabled" => {
                cfg.border_enabled = parse_i32_or(val, 1) != 0;
                parsed_any = true;
            }
            "border_alpha" => {
                cfg.border_alpha = parse_finite_clamped(val, cfg.border_alpha, 0.05, 1.0);
                parsed_any = true;
            }
            "border_color" => {
                if let Some(c) = parse_rgb(val) {
                    cfg.border_color = c;
                    parsed_any = true;
                }
            }
            "outline_enabled" => {
                cfg.outline_enabled = parse_i32_or(val, 1) != 0;
                parsed_any = true;
            }
            "outline_thickness" => {
                cfg.outline_thickness = parse_finite_clamped(val, cfg.outline_thickness, 0.0, 6.0);
                parsed_any = true;
            }
            "shadow_enabled" => {
                cfg.shadow_enabled = parse_i32_or(val, 1) != 0;
                parsed_any = true;
            }
            "shadow_offset" => {
                cfg.shadow_offset = parse_finite_clamped(val, cfg.shadow_offset, 0.0, 10.0);
                parsed_any = true;
            }
            "shadow_opacity" => {
                cfg.shadow_opacity = parse_finite_clamped(val, cfg.shadow_opacity, 0.0, 1.0);
                parsed_any = true;
            }
            "category_colors_enabled" => {
                cfg.category_colors_enabled = parse_i32_or(val, 1) != 0;
                parsed_any = true;
            }
            "thermal_warnings_enabled" => {
                cfg.thermal_warnings_enabled = parse_i32_or(val, 1) != 0;
                parsed_any = true;
            }
            "dim_labels_enabled" => {
                cfg.dim_labels_enabled = parse_i32_or(val, 1) != 0;
                parsed_any = true;
            }
            "hud_graph_enabled" => {
                cfg.hud_graph_enabled = parse_i32_or(val, 0) != 0;
                parsed_any = true;
            }
            "key_toggle_gui" => {
                cfg.key_toggle_gui = parse_i32_or(val, 0).clamp(-1, 4);
                parsed_any = true;
            }
            "key_toggle_hud" => {
                cfg.key_toggle_hud = parse_i32_or(val, 0).clamp(-1, 2);
                parsed_any = true;
            }
            "key_benchmark" => {
                cfg.key_benchmark = parse_i32_or(val, 0).clamp(-1, 1);
                parsed_any = true;
            }
            "bind_toggle_gui" => {
                if let Some(kb) = parse_keybind(val) {
                    cfg.bind_toggle_gui = kb;
                }
                parsed_any = true;
            }
            "bind_toggle_hud" => {
                if let Some(kb) = parse_keybind(val) {
                    cfg.bind_toggle_hud = kb;
                }
                parsed_any = true;
            }
            "bind_benchmark" => {
                if let Some(kb) = parse_keybind(val) {
                    cfg.bind_benchmark = kb;
                }
                parsed_any = true;
            }
            "bind_screenshot" => {
                if let Some(kb) = parse_keybind(val) {
                    cfg.bind_screenshot = kb;
                }
                parsed_any = true;
            }
            "screenshot_with_hud" => {
                cfg.screenshot_with_hud = parse_i32_or(val, 0) != 0;
                parsed_any = true;
            }
            "save_folder" => {
                cfg.save_folder = sanitize_ini_str(val);
                parsed_any = true;
            }
            "metrics" => {
                let mut next = Vec::new();
                for token in val.split(',') {
                    if let Some(metric) = MetricType::from_id_key(token)
                        && !next.contains(&metric)
                    {
                        next.push(metric);
                    }
                }
                cfg.metrics = next;
                parsed_any = true;
            }
            // Legacy deleted recorder keys & other unknown fields are safely ignored without error
            _ => {
                parsed_any = true;
            }
        }
    }

    if !parsed_any {
        return false;
    }
    if saw_text_color && !saw_custom_text_color_flag {
        cfg.custom_text_color_enabled = cfg.text_color != ColorRgba::new(255, 255, 255, 255);
    }
    if cfg.metrics.is_empty() {
        cfg.metrics.push(MetricType::Fps);
    }
    cfg.enabled = keep_enabled;
    cfg.gui_open = keep_open;
    true
}

pub fn save_ini_file(path: &Path, cfg: &HudConfig) -> bool {
    if path.as_os_str().is_empty() {
        return false;
    }
    if let Some(parent) = path.parent()
        && !parent.as_os_str().is_empty()
        && fs::create_dir_all(parent).is_err()
    {
        return false;
    }

    let tmp_path = path.with_file_name(format!(
        "{}.tmp",
        path.file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("config.ini")
    ));

    let Ok(mut file) = File::create(&tmp_path) else {
        return false;
    };

    let metrics_csv: Vec<&'static str> = cfg.metrics.iter().map(|m| m.id_key()).collect();
    let content = format!(
        "# FPSHUD Configuration\n\
         preset={}\n\
         gui_scale={:.2}\n\
         hud_visible={}\n\
         layout={}\n\
         custom_pos_x={:.4}\n\
         custom_pos_y={:.4}\n\
         arrangement={}\n\
         background={}\n\
         font_family={}\n\
         font_size_px={:.2}\n\
         text_color={},{},{}\n\
         custom_text_color_enabled={}\n\
         text_opacity={:.3}\n\
         bg_alpha={:.3}\n\
         border_enabled={}\n\
         border_alpha={:.3}\n\
         border_color={},{},{}\n\
         outline_enabled={}\n\
         outline_thickness={:.2}\n\
         shadow_enabled={}\n\
         shadow_offset={:.2}\n\
         shadow_opacity={:.2}\n\
         category_colors_enabled={}\n\
         thermal_warnings_enabled={}\n\
         dim_labels_enabled={}\n\
         hud_graph_enabled={}\n\
         key_toggle_gui={}\n\
         key_toggle_hud={}\n\
         key_benchmark={}\n\
         bind_toggle_gui={},{}\n\
         bind_toggle_hud={},{}\n\
         bind_benchmark={},{}\n\
         bind_screenshot={},{}\n\
         screenshot_with_hud={}\n\
         save_folder={}\n\
         metrics={}\n",
        cfg.preset.as_str(),
        cfg.gui_scale,
        i32::from(cfg.hud_visible),
        cfg.position.as_i32(),
        cfg.custom_pos_x.clamp(0.0, 1.0),
        cfg.custom_pos_y.clamp(0.0, 1.0),
        cfg.arrangement.as_i32(),
        cfg.background_style.as_i32(),
        cfg.font_family.as_i32(),
        cfg.font_size_px,
        cfg.text_color.r,
        cfg.text_color.g,
        cfg.text_color.b,
        i32::from(cfg.custom_text_color_enabled),
        cfg.text_opacity,
        cfg.bg_alpha,
        i32::from(cfg.border_enabled),
        cfg.border_alpha,
        cfg.border_color.r,
        cfg.border_color.g,
        cfg.border_color.b,
        i32::from(cfg.outline_enabled),
        cfg.outline_thickness,
        i32::from(cfg.shadow_enabled),
        cfg.shadow_offset,
        cfg.shadow_opacity,
        i32::from(cfg.category_colors_enabled),
        i32::from(cfg.thermal_warnings_enabled),
        i32::from(cfg.dim_labels_enabled),
        i32::from(cfg.hud_graph_enabled),
        cfg.key_toggle_gui,
        cfg.key_toggle_hud,
        cfg.key_benchmark,
        cfg.bind_toggle_gui.mod_key,
        cfg.bind_toggle_gui.main_key,
        cfg.bind_toggle_hud.mod_key,
        cfg.bind_toggle_hud.main_key,
        cfg.bind_benchmark.mod_key,
        cfg.bind_benchmark.main_key,
        cfg.bind_screenshot.mod_key,
        cfg.bind_screenshot.main_key,
        i32::from(cfg.screenshot_with_hud),
        sanitize_ini_str(&cfg.save_folder),
        metrics_csv.join(","),
    );

    if file.write_all(content.as_bytes()).is_err() || file.flush().is_err() {
        let _ = fs::remove_file(&tmp_path);
        return false;
    }
    drop(file);

    if fs::rename(&tmp_path, path).is_err() {
        let _ = fs::remove_file(&tmp_path);
        return false;
    }
    true
}
