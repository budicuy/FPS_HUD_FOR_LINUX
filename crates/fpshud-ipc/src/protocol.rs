use fpshud_analytics::{BenchmarkSummary, frametime::GRAPH_HISTORY_SIZE};
use fpshud_core::{
    ArrangementMode, BackgroundStyle, ColorRgba, FontFamily, HudConfig, Keybind, LayoutPosition,
    MetricType, PresetType,
};
use fpshud_input::{KeybindTarget, UiInputEvent};
use fpshud_telemetry::{CpuPowerSource, GpuBackendType, MAX_CPU_CORES, TelemetrySnapshot};
use std::io;
use std::path::PathBuf;

pub const IPC_MAGIC: u32 = 0x4650_5348; // "FPSH"
pub const MAX_IPC_PAYLOAD: usize = 65_536;
const MAX_STRING_BYTES: usize = 4096;

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WireHeader {
    pub magic: u32,
    pub msg_kind: u32,
    pub payload_len: u32,
    pub reserved: u32,
}

#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameBackendKind {
    MemfdRgba = 0,
    DmaBuf = 1,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ShmSlotMeta {
    pub modifier: u64,
    pub generation: u64,
    pub slot_index: u32,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub fourcc: u32,
    pub byte_len: u32,
    pub backend: FrameBackendKind,
}

#[derive(Debug, Clone, PartialEq)]
pub enum LayerToUiMsg {
    Init {
        width: u32,
        height: u32,
        scale: f32,
        config: HudConfig,
    },
    TelemetryUpdate(Box<TelemetrySnapshot>),
    ConfigSync(HudConfig),
    InputEvents(Vec<UiInputEvent>),
    Resize {
        width: u32,
        height: u32,
        scale: f32,
    },
    Shutdown,
}

#[derive(Debug, Clone, PartialEq)]
pub enum UiToLayerMsg {
    Ready,
    FrameReady(ShmSlotMeta),
    ConfigUpdated(HudConfig),
    BenchmarkToggle,
    ScreenshotRequest { with_hud: bool },
    OpenSaveFolder,
    KeybindCaptureState { capturing: bool, target: KeybindTarget },
}

fn invalid_data(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg)
}

struct WireWriter {
    buf: Vec<u8>,
}

impl WireWriter {
    fn new(kind: u32) -> Self {
        let mut buf = Vec::with_capacity(256);
        buf.extend_from_slice(&IPC_MAGIC.to_le_bytes());
        buf.extend_from_slice(&kind.to_le_bytes());
        buf.extend_from_slice(&0_u32.to_le_bytes()); // payload_len patched at finish
        buf.extend_from_slice(&0_u32.to_le_bytes()); // reserved
        Self { buf }
    }

    fn u8(&mut self, v: u8) {
        self.buf.push(v);
    }

    fn bool(&mut self, v: bool) {
        self.buf.push(u8::from(v));
    }

    fn u16(&mut self, v: u16) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    fn u32(&mut self, v: u32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    fn i32(&mut self, v: i32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    fn u64(&mut self, v: u64) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    fn f32(&mut self, v: f32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    fn str(&mut self, s: &str) {
        let bytes = s.as_bytes();
        let len = bytes.len().min(MAX_STRING_BYTES) as u32;
        self.u32(len);
        self.buf.extend_from_slice(&bytes[..len as usize]);
    }

    fn finish(mut self) -> io::Result<Vec<u8>> {
        let payload_len = self.buf.len() - size_of::<WireHeader>();
        if payload_len > MAX_IPC_PAYLOAD {
            return Err(invalid_data("encoded payload exceeds MAX_IPC_PAYLOAD"));
        }
        self.buf[8..12].copy_from_slice(&(payload_len as u32).to_le_bytes());
        Ok(self.buf)
    }
}

struct WireReader<'a> {
    slice: &'a [u8],
    pos: usize,
}

impl<'a> WireReader<'a> {
    fn open(raw: &'a [u8]) -> io::Result<(u32, Self)> {
        if raw.len() < size_of::<WireHeader>() {
            return Err(invalid_data("truncated IPC header"));
        }
        let magic = u32::from_le_bytes(raw[0..4].try_into().unwrap());
        if magic != IPC_MAGIC {
            return Err(invalid_data("invalid IPC magic"));
        }
        let kind = u32::from_le_bytes(raw[4..8].try_into().unwrap());
        let payload_len = u32::from_le_bytes(raw[8..12].try_into().unwrap()) as usize;
        if payload_len > MAX_IPC_PAYLOAD {
            return Err(invalid_data("payload_len exceeds MAX_IPC_PAYLOAD"));
        }
        let total = size_of::<WireHeader>() + payload_len;
        if raw.len() != total {
            return Err(invalid_data("IPC buffer length mismatch"));
        }
        Ok((
            kind,
            Self {
                slice: &raw[size_of::<WireHeader>()..total],
                pos: 0,
            },
        ))
    }

    fn take(&mut self, n: usize) -> io::Result<&'a [u8]> {
        let end = self
            .pos
            .checked_add(n)
            .ok_or_else(|| invalid_data("offset overflow"))?;
        if end > self.slice.len() {
            return Err(invalid_data("unexpected end of IPC payload"));
        }
        let chunk = &self.slice[self.pos..end];
        self.pos = end;
        Ok(chunk)
    }

    fn u8(&mut self) -> io::Result<u8> {
        Ok(self.take(1)?[0])
    }

    fn bool(&mut self) -> io::Result<bool> {
        Ok(self.u8()? != 0)
    }

    fn u16(&mut self) -> io::Result<u16> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }

    fn u32(&mut self) -> io::Result<u32> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }

    fn i32(&mut self) -> io::Result<i32> {
        Ok(i32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }

    fn u64(&mut self) -> io::Result<u64> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }

    fn f32(&mut self) -> io::Result<f32> {
        Ok(f32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }

    fn str(&mut self) -> io::Result<String> {
        let len = self.u32()? as usize;
        if len > MAX_STRING_BYTES {
            return Err(invalid_data("wire string exceeds max size"));
        }
        let bytes = self.take(len)?;
        std::str::from_utf8(bytes)
            .map(ToString::to_string)
            .map_err(|_| invalid_data("invalid UTF-8 string in IPC message"))
    }

    fn ensure_eof(&self) -> io::Result<()> {
        if self.pos != self.slice.len() {
            return Err(invalid_data("trailing bytes in IPC payload"));
        }
        Ok(())
    }
}

fn write_config(w: &mut WireWriter, c: &HudConfig) {
    w.bool(c.enabled);
    w.bool(c.gui_open);
    w.bool(c.hud_visible);
    w.u8(c.preset as u8);
    let m_len = c.metrics.len().min(32) as u32;
    w.u32(m_len);
    for &m in c.metrics.iter().take(32) {
        w.u8(m as u8);
    }
    w.u8(c.position as u8);
    w.u8(c.arrangement as u8);
    w.u8(c.background_style as u8);
    w.u8(c.font_family as u8);
    w.u8(c.text_color.r);
    w.u8(c.text_color.g);
    w.u8(c.text_color.b);
    w.u8(c.text_color.a);
    w.bool(c.category_colors_enabled);
    w.bool(c.thermal_warnings_enabled);
    w.bool(c.dim_labels_enabled);
    w.bool(c.hud_graph_enabled);
    w.i32(c.key_toggle_gui);
    w.i32(c.key_toggle_hud);
    w.i32(c.key_benchmark);
    for kb in [
        c.bind_toggle_gui,
        c.bind_toggle_hud,
        c.bind_benchmark,
        c.bind_screenshot,
    ] {
        w.u32(kb.mod_key);
        w.u32(kb.main_key);
    }
    w.bool(c.screenshot_with_hud);
    w.str(&c.save_folder);
    w.f32(c.gui_scale);
    w.f32(c.font_size_px);
    w.f32(c.text_opacity);
    w.f32(c.bg_alpha);
    w.bool(c.outline_enabled);
    w.f32(c.outline_thickness);
    w.bool(c.shadow_enabled);
    w.f32(c.shadow_offset);
    w.f32(c.shadow_opacity);
    w.f32(c.margin_x);
    w.f32(c.margin_y);
    w.f32(c.custom_pos_x);
    w.f32(c.custom_pos_y);
}

fn read_config(r: &mut WireReader<'_>) -> io::Result<HudConfig> {
    let enabled = r.bool()?;
    let gui_open = r.bool()?;
    let hud_visible = r.bool()?;
    let preset = match r.u8()? {
        0 => PresetType::Minimal,
        1 => PresetType::Standard,
        2 => PresetType::Advance,
        3 => PresetType::Custom,
        _ => return Err(invalid_data("invalid PresetType")),
    };
    let m_len = r.u32()? as usize;
    if m_len > 32 {
        return Err(invalid_data("invalid metrics length"));
    }
    let mut metrics = Vec::with_capacity(m_len);
    for _ in 0..m_len {
        let idx = r.u8()? as usize;
        let metric = *MetricType::ALL
            .get(idx)
            .ok_or_else(|| invalid_data("invalid MetricType"))?;
        metrics.push(metric);
    }
    let position = LayoutPosition::from_i32(i32::from(r.u8()?));
    let arrangement = ArrangementMode::from_i32(i32::from(r.u8()?));
    let background_style = BackgroundStyle::from_i32(i32::from(r.u8()?));
    let font_family = FontFamily::from_i32(i32::from(r.u8()?));
    let text_color = ColorRgba::new(r.u8()?, r.u8()?, r.u8()?, r.u8()?);
    let category_colors_enabled = r.bool()?;
    let thermal_warnings_enabled = r.bool()?;
    let dim_labels_enabled = r.bool()?;
    let hud_graph_enabled = r.bool()?;
    let key_toggle_gui = r.i32()?;
    let key_toggle_hud = r.i32()?;
    let key_benchmark = r.i32()?;
    let bind_toggle_gui = Keybind::new(r.u32()?, r.u32()?);
    let bind_toggle_hud = Keybind::new(r.u32()?, r.u32()?);
    let bind_benchmark = Keybind::new(r.u32()?, r.u32()?);
    let bind_screenshot = Keybind::new(r.u32()?, r.u32()?);
    let screenshot_with_hud = r.bool()?;
    let save_folder = r.str()?;
    let gui_scale = r.f32()?;
    let font_size_px = r.f32()?;
    let text_opacity = r.f32()?;
    let bg_alpha = r.f32()?;
    let outline_enabled = r.bool()?;
    let outline_thickness = r.f32()?;
    let shadow_enabled = r.bool()?;
    let shadow_offset = r.f32()?;
    let shadow_opacity = r.f32()?;
    let margin_x = r.f32()?;
    let margin_y = r.f32()?;
    let custom_pos_x = r.f32()?;
    let custom_pos_y = r.f32()?;

    Ok(HudConfig {
        enabled,
        gui_open,
        hud_visible,
        preset,
        metrics,
        position,
        arrangement,
        background_style,
        font_family,
        text_color,
        category_colors_enabled,
        thermal_warnings_enabled,
        dim_labels_enabled,
        hud_graph_enabled,
        key_toggle_gui,
        key_toggle_hud,
        key_benchmark,
        bind_toggle_gui,
        bind_toggle_hud,
        bind_benchmark,
        bind_screenshot,
        screenshot_with_hud,
        save_folder,
        gui_scale,
        font_size_px,
        text_opacity,
        bg_alpha,
        outline_enabled,
        outline_thickness,
        shadow_enabled,
        shadow_offset,
        shadow_opacity,
        margin_x,
        margin_y,
        custom_pos_x,
        custom_pos_y,
    })
}

fn write_snapshot(w: &mut WireWriter, s: &TelemetrySnapshot) {
    w.f32(s.fps);
    w.f32(s.fps_avg);
    w.f32(s.fps_1pct_low);
    w.f32(s.fps_01pct_low);
    w.f32(s.frametime_ms);
    let hlen = (s.frametime_history_len as usize).min(GRAPH_HISTORY_SIZE) as u8;
    w.u8(hlen);
    for &sample in &s.frametime_history[..usize::from(hlen)] {
        w.f32(sample);
    }
    w.bool(s.gpu_available);
    w.u8(s.gpu_backend as u8);
    w.f32(s.gpu_load);
    w.u32(s.gpu_clock_mhz);
    w.u32(s.gpu_vram_clock_mhz);
    w.f32(s.gpu_vram_used_gb);
    w.f32(s.gpu_vram_total_gb);
    w.f32(s.gpu_bandwidth_gbs);
    w.f32(s.gpu_temp_c);
    w.u32(s.gpu_fan_pct);
    w.f32(s.gpu_power_w);

    w.f32(s.cpu_load);
    w.u32(s.cpu_clock_mhz);
    w.f32(s.cpu_temp_c);
    w.f32(s.cpu_power_w);
    w.u8(s.cpu_power_source as u8);
    let cores = (s.cpu_core_count as usize).min(MAX_CPU_CORES) as u8;
    w.u8(cores);
    for i in 0..usize::from(cores) {
        w.f32(s.cpu_core_loads[i]);
        w.u16(s.cpu_core_clocks_mhz[i]);
    }

    w.f32(s.ram_used_gb);
    w.f32(s.ram_total_gb);
    w.f32(s.game_ram_gb);
    w.f32(s.swap_used_gb);
    w.f32(s.swap_total_gb);
    w.f32(s.mem_psi_some_pct);
    w.bool(s.battery_available);
    w.f32(s.battery_pct);
    w.f32(s.battery_power_w);
    w.bool(s.battery_charging);
    w.f32(s.disk_read_mbs);
    w.f32(s.disk_write_mbs);
    w.f32(s.nvme_temp_c);
    w.bool(s.nvme_temp_available);
    w.f32(s.net_rx_mbs);
    w.f32(s.net_tx_mbs);

    w.str(&s.render_api);
    w.str(&s.cpu_model);
    w.str(&s.gpu_model);
    w.str(&s.system_time);
    w.bool(s.benchmark_active);
    w.f32(s.benchmark_elapsed_sec);
    match &s.last_benchmark {
        Some(b) => {
            w.bool(true);
            w.f32(b.avg_fps);
            w.f32(b.fps_1pct_low);
            w.f32(b.fps_01pct_low);
            w.f32(b.min_fps);
            w.f32(b.max_fps);
            w.u64(b.total_frames);
            w.f32(b.duration_sec);
            w.str(&b.csv_path.to_string_lossy());
        }
        None => w.bool(false),
    }
}

fn read_snapshot(r: &mut WireReader<'_>) -> io::Result<TelemetrySnapshot> {
    let mut s = TelemetrySnapshot {
        fps: r.f32()?,
        fps_avg: r.f32()?,
        fps_1pct_low: r.f32()?,
        fps_01pct_low: r.f32()?,
        frametime_ms: r.f32()?,
        ..TelemetrySnapshot::default()
    };
    let hlen = r.u8()? as usize;
    if hlen > GRAPH_HISTORY_SIZE {
        return Err(invalid_data("frametime history len overflow"));
    }
    s.frametime_history_len = hlen as u8;
    for slot in &mut s.frametime_history[..hlen] {
        *slot = r.f32()?;
    }
    s.gpu_available = r.bool()?;
    s.gpu_backend = match r.u8()? {
        0 => GpuBackendType::Unknown,
        1 => GpuBackendType::DrmSysfs,
        2 => GpuBackendType::Nvml,
        3 => GpuBackendType::LevelZero,
        _ => return Err(invalid_data("invalid GpuBackendType")),
    };
    s.gpu_load = r.f32()?;
    s.gpu_clock_mhz = r.u32()?;
    s.gpu_vram_clock_mhz = r.u32()?;
    s.gpu_vram_used_gb = r.f32()?;
    s.gpu_vram_total_gb = r.f32()?;
    s.gpu_bandwidth_gbs = r.f32()?;
    s.gpu_temp_c = r.f32()?;
    s.gpu_fan_pct = r.u32()?;
    s.gpu_power_w = r.f32()?;

    s.cpu_load = r.f32()?;
    s.cpu_clock_mhz = r.u32()?;
    s.cpu_temp_c = r.f32()?;
    s.cpu_power_w = r.f32()?;
    s.cpu_power_source = match r.u8()? {
        0 => CpuPowerSource::Unavailable,
        1 => CpuPowerSource::DirectHwmon,
        2 => CpuPowerSource::DirectRapl,
        3 => CpuPowerSource::EstimatedDvfs,
        _ => return Err(invalid_data("invalid CpuPowerSource")),
    };
    let cores = r.u8()? as usize;
    if cores > MAX_CPU_CORES {
        return Err(invalid_data("cpu core count overflow"));
    }
    s.cpu_core_count = cores as u8;
    for i in 0..cores {
        s.cpu_core_loads[i] = r.f32()?;
        s.cpu_core_clocks_mhz[i] = r.u16()?;
    }

    s.ram_used_gb = r.f32()?;
    s.ram_total_gb = r.f32()?;
    s.game_ram_gb = r.f32()?;
    s.swap_used_gb = r.f32()?;
    s.swap_total_gb = r.f32()?;
    s.mem_psi_some_pct = r.f32()?;
    s.battery_available = r.bool()?;
    s.battery_pct = r.f32()?;
    s.battery_power_w = r.f32()?;
    s.battery_charging = r.bool()?;
    s.disk_read_mbs = r.f32()?;
    s.disk_write_mbs = r.f32()?;
    s.nvme_temp_c = r.f32()?;
    s.nvme_temp_available = r.bool()?;
    s.net_rx_mbs = r.f32()?;
    s.net_tx_mbs = r.f32()?;

    s.render_api = r.str()?;
    s.cpu_model = r.str()?;
    s.gpu_model = r.str()?;
    s.system_time = r.str()?;
    s.benchmark_active = r.bool()?;
    s.benchmark_elapsed_sec = r.f32()?;
    if r.bool()? {
        s.last_benchmark = Some(BenchmarkSummary {
            avg_fps: r.f32()?,
            fps_1pct_low: r.f32()?,
            fps_01pct_low: r.f32()?,
            min_fps: r.f32()?,
            max_fps: r.f32()?,
            total_frames: r.u64()?,
            duration_sec: r.f32()?,
            csv_path: PathBuf::from(r.str()?),
        });
    }
    Ok(s)
}

impl LayerToUiMsg {
    pub fn encode(&self) -> io::Result<Vec<u8>> {
        match self {
            Self::Init {
                width,
                height,
                scale,
                config,
            } => {
                let mut w = WireWriter::new(1);
                w.u32(*width);
                w.u32(*height);
                w.f32(*scale);
                write_config(&mut w, config);
                w.finish()
            }
            Self::TelemetryUpdate(snap) => {
                let mut w = WireWriter::new(2);
                write_snapshot(&mut w, snap);
                w.finish()
            }
            Self::ConfigSync(cfg) => {
                let mut w = WireWriter::new(3);
                write_config(&mut w, cfg);
                w.finish()
            }
            Self::InputEvents(events) => {
                let mut w = WireWriter::new(4);
                let count = events.len().min(256) as u32;
                w.u32(count);
                for ev in events.iter().take(256) {
                    match *ev {
                        UiInputEvent::MouseMove { x, y } => {
                            w.u8(0);
                            w.f32(x);
                            w.f32(y);
                        }
                        UiInputEvent::MouseDown { button } => {
                            w.u8(1);
                            w.u8(button);
                        }
                        UiInputEvent::MouseUp { button } => {
                            w.u8(2);
                            w.u8(button);
                        }
                        UiInputEvent::ScrollWheel { delta_x, delta_y } => {
                            w.u8(3);
                            w.f32(delta_x);
                            w.f32(delta_y);
                        }
                        UiInputEvent::KeyDown { code, mod_mask } => {
                            w.u8(4);
                            w.u32(code);
                            w.u32(mod_mask);
                        }
                        UiInputEvent::KeyUp { code, mod_mask } => {
                            w.u8(5);
                            w.u32(code);
                            w.u32(mod_mask);
                        }
                    }
                }
                w.finish()
            }
            Self::Resize {
                width,
                height,
                scale,
            } => {
                let mut w = WireWriter::new(5);
                w.u32(*width);
                w.u32(*height);
                w.f32(*scale);
                w.finish()
            }
            Self::Shutdown => WireWriter::new(6).finish(),
        }
    }

    pub fn decode(raw: &[u8]) -> io::Result<Self> {
        let (kind, mut r) = WireReader::open(raw)?;
        let msg = match kind {
            1 => Self::Init {
                width: r.u32()?,
                height: r.u32()?,
                scale: r.f32()?,
                config: read_config(&mut r)?,
            },
            2 => Self::TelemetryUpdate(Box::new(read_snapshot(&mut r)?)),
            3 => Self::ConfigSync(read_config(&mut r)?),
            4 => {
                let count = r.u32()? as usize;
                if count > 256 {
                    return Err(invalid_data("input event batch overflow"));
                }
                let mut events = Vec::with_capacity(count);
                for _ in 0..count {
                    let ev = match r.u8()? {
                        0 => UiInputEvent::MouseMove {
                            x: r.f32()?,
                            y: r.f32()?,
                        },
                        1 => UiInputEvent::MouseDown { button: r.u8()? },
                        2 => UiInputEvent::MouseUp { button: r.u8()? },
                        3 => UiInputEvent::ScrollWheel {
                            delta_x: r.f32()?,
                            delta_y: r.f32()?,
                        },
                        4 => UiInputEvent::KeyDown {
                            code: r.u32()?,
                            mod_mask: r.u32()?,
                        },
                        5 => UiInputEvent::KeyUp {
                            code: r.u32()?,
                            mod_mask: r.u32()?,
                        },
                        _ => return Err(invalid_data("invalid UiInputEvent discriminant")),
                    };
                    events.push(ev);
                }
                Self::InputEvents(events)
            }
            5 => Self::Resize {
                width: r.u32()?,
                height: r.u32()?,
                scale: r.f32()?,
            },
            6 => Self::Shutdown,
            _ => return Err(invalid_data("unknown LayerToUiMsg kind")),
        };
        r.ensure_eof()?;
        Ok(msg)
    }
}

impl UiToLayerMsg {
    pub fn encode(&self) -> io::Result<Vec<u8>> {
        match self {
            Self::Ready => WireWriter::new(101).finish(),
            Self::FrameReady(meta) => {
                let mut w = WireWriter::new(102);
                w.u64(meta.modifier);
                w.u64(meta.generation);
                w.u32(meta.slot_index);
                w.u32(meta.width);
                w.u32(meta.height);
                w.u32(meta.stride);
                w.u32(meta.fourcc);
                w.u32(meta.byte_len);
                w.u32(meta.backend as u32);
                w.finish()
            }
            Self::ConfigUpdated(cfg) => {
                let mut w = WireWriter::new(103);
                write_config(&mut w, cfg);
                w.finish()
            }
            Self::BenchmarkToggle => WireWriter::new(104).finish(),
            Self::ScreenshotRequest { with_hud } => {
                let mut w = WireWriter::new(105);
                w.bool(*with_hud);
                w.finish()
            }
            Self::OpenSaveFolder => WireWriter::new(106).finish(),
            Self::KeybindCaptureState { capturing, target } => {
                let mut w = WireWriter::new(107);
                w.bool(*capturing);
                let t = match target {
                    KeybindTarget::None => 0_u8,
                    KeybindTarget::ToggleGui => 1,
                    KeybindTarget::ToggleHud => 2,
                    KeybindTarget::ToggleBenchmark => 3,
                    KeybindTarget::TakeScreenshot => 4,
                };
                w.u8(t);
                w.finish()
            }
        }
    }

    pub fn decode(raw: &[u8]) -> io::Result<Self> {
        let (kind, mut r) = WireReader::open(raw)?;
        let msg = match kind {
            101 => Self::Ready,
            102 => {
                let modifier = r.u64()?;
                let generation = r.u64()?;
                let slot_index = r.u32()?;
                let width = r.u32()?;
                let height = r.u32()?;
                let stride = r.u32()?;
                let fourcc = r.u32()?;
                let byte_len = r.u32()?;
                let backend = match r.u32()? {
                    0 => FrameBackendKind::MemfdRgba,
                    1 => FrameBackendKind::DmaBuf,
                    _ => return Err(invalid_data("invalid FrameBackendKind")),
                };
                Self::FrameReady(ShmSlotMeta {
                    modifier,
                    generation,
                    slot_index,
                    width,
                    height,
                    stride,
                    fourcc,
                    byte_len,
                    backend,
                })
            }
            103 => Self::ConfigUpdated(read_config(&mut r)?),
            104 => Self::BenchmarkToggle,
            105 => Self::ScreenshotRequest {
                with_hud: r.bool()?,
            },
            106 => Self::OpenSaveFolder,
            107 => {
                let capturing = r.bool()?;
                let target = match r.u8()? {
                    0 => KeybindTarget::None,
                    1 => KeybindTarget::ToggleGui,
                    2 => KeybindTarget::ToggleHud,
                    3 => KeybindTarget::ToggleBenchmark,
                    4 => KeybindTarget::TakeScreenshot,
                    _ => return Err(invalid_data("invalid KeybindTarget")),
                };
                Self::KeybindCaptureState { capturing, target }
            }
            _ => return Err(invalid_data("unknown UiToLayerMsg kind")),
        };
        r.ensure_eof()?;
        Ok(msg)
    }
}
