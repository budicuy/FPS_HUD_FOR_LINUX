use fpshud_analytics::frametime::GRAPH_HISTORY_SIZE;
use fpshud_core::HudConfig;
use fpshud_input::{KeybindTarget, UiInputEvent};
use fpshud_telemetry::{MAX_CPU_CORES, TelemetrySnapshot};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use std::io;

pub const IPC_MAGIC: [u8; 4] = *b"FPSH";
pub const MAX_IPC_PAYLOAD: usize = 65_536;
const MAX_CONFIG_METRICS: usize = 32;
pub const MAX_INPUT_EVENTS: usize = 256;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum FrameBackendKind {
    MemfdRgba,
    DmaBuf,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
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

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
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
    SoftwareCursor(bool),
    Shutdown,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum UiToLayerMsg {
    Ready,
    FrameReady(ShmSlotMeta),
    ConfigUpdated(HudConfig),
    BenchmarkToggle,
    ScreenshotRequest { with_hud: bool },
    OpenSaveFolder,
    KeybindCaptureState { capturing: bool, target: KeybindTarget },
}

fn invalid_data(msg: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg.into())
}

fn encode_frame<T: Serialize>(msg: &T) -> io::Result<Vec<u8>> {
    let mut buf = Vec::with_capacity(64);
    buf.extend_from_slice(&IPC_MAGIC);
    let buf = postcard::to_extend(msg, buf).map_err(|e| invalid_data(e.to_string()))?;
    if buf.len() > MAX_IPC_PAYLOAD {
        return Err(invalid_data("encoded payload exceeds MAX_IPC_PAYLOAD"));
    }
    Ok(buf)
}

fn decode_frame<T: DeserializeOwned>(raw: &[u8]) -> io::Result<T> {
    if raw.len() > MAX_IPC_PAYLOAD {
        return Err(invalid_data("payload_len exceeds MAX_IPC_PAYLOAD"));
    }
    let body = raw
        .strip_prefix(&IPC_MAGIC)
        .ok_or_else(|| invalid_data("invalid IPC magic"))?;
    let (msg, trailing) =
        postcard::take_from_bytes(body).map_err(|e| invalid_data(e.to_string()))?;
    if !trailing.is_empty() {
        return Err(invalid_data("trailing bytes in IPC payload"));
    }
    Ok(msg)
}

fn validate_config(cfg: &HudConfig) -> io::Result<()> {
    if cfg.metrics.len() > MAX_CONFIG_METRICS {
        return Err(invalid_data("config metrics overflow"));
    }
    Ok(())
}

fn validate_snapshot(snap: &TelemetrySnapshot) -> io::Result<()> {
    if snap.frametime_history.len() > GRAPH_HISTORY_SIZE {
        return Err(invalid_data("frametime_history overflow"));
    }
    if snap.cpu_core_loads.len() > MAX_CPU_CORES || snap.cpu_core_clocks_mhz.len() > MAX_CPU_CORES
    {
        return Err(invalid_data("cpu core slice overflow"));
    }
    Ok(())
}

fn validate_layer_msg(msg: &LayerToUiMsg) -> io::Result<()> {
    match msg {
        LayerToUiMsg::Init { config, .. } | LayerToUiMsg::ConfigSync(config) => {
            validate_config(config)
        }
        LayerToUiMsg::TelemetryUpdate(snap) => validate_snapshot(snap),
        LayerToUiMsg::InputEvents(events) if events.len() > MAX_INPUT_EVENTS => {
            Err(invalid_data("input event batch overflow"))
        }
        _ => Ok(()),
    }
}

impl LayerToUiMsg {
    pub fn encode(&self) -> io::Result<Vec<u8>> {
        validate_layer_msg(self)?;
        encode_frame(self)
    }

    pub fn decode(raw: &[u8]) -> io::Result<Self> {
        let msg: Self = decode_frame(raw)?;
        validate_layer_msg(&msg)?;
        Ok(msg)
    }
}

impl UiToLayerMsg {
    pub fn encode(&self) -> io::Result<Vec<u8>> {
        if let Self::ConfigUpdated(cfg) = self {
            validate_config(cfg)?;
        }
        encode_frame(self)
    }

    pub fn decode(raw: &[u8]) -> io::Result<Self> {
        let msg: Self = decode_frame(raw)?;
        if let Self::ConfigUpdated(cfg) = &msg {
            validate_config(cfg)?;
        }
        Ok(msg)
    }
}
