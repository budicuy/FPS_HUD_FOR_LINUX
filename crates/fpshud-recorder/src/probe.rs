use crate::locate::{bundled_ffmpeg_command, locate_bundled_ffmpeg};
use fpshud_core::RecordCodec;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::OnceLock;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SelectedEncoder {
    H264Nvenc,
    HevcNvenc,
    H264Vaapi,
    HevcVaapi,
    H264Vulkan,
    HevcVulkan,
    LibX264,
}

impl SelectedEncoder {
    #[must_use]
    pub const fn ffmpeg_name(self) -> &'static str {
        match self {
            Self::H264Nvenc => "h264_nvenc",
            Self::HevcNvenc => "hevc_nvenc",
            Self::H264Vaapi => "h264_vaapi",
            Self::HevcVaapi => "hevc_vaapi",
            Self::H264Vulkan => "h264_vulkan",
            Self::HevcVulkan => "hevc_vulkan",
            Self::LibX264 => "libx264",
        }
    }

    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Self::H264Nvenc => "NVENC H.264",
            Self::HevcNvenc => "NVENC H.265",
            Self::H264Vaapi => "VA-API H.264",
            Self::HevcVaapi => "VA-API H.265",
            Self::H264Vulkan => "Vulkan H.264",
            Self::HevcVulkan => "Vulkan H.265",
            Self::LibX264 => "CPU x264",
        }
    }

    #[must_use]
    pub const fn is_vaapi(self) -> bool {
        matches!(self, Self::H264Vaapi | Self::HevcVaapi)
    }

    #[must_use]
    pub const fn is_vulkan(self) -> bool {
        matches!(self, Self::H264Vulkan | Self::HevcVulkan)
    }

    #[must_use]
    pub const fn is_nvenc(self) -> bool {
        matches!(self, Self::H264Nvenc | Self::HevcNvenc)
    }
}

#[derive(Debug, Clone, Default)]
pub struct FfmpegCapabilities {
    pub bundled_ffmpeg: Option<PathBuf>,
    pub h264_encoder: Option<SelectedEncoder>,
    pub h265_encoder: Option<SelectedEncoder>,
    pub vaapi_device: Option<PathBuf>,
    pub warning: Option<String>,
}

/// Returns ordered hardware/software encoder candidates for a given GPU vendor ID.
#[must_use]
pub fn encoder_priority_for_gpu(
    target_gpu: Option<(u32, u32)>,
    codec: RecordCodec,
) -> Vec<SelectedEncoder> {
    let vendor = target_gpu.map(|(v, _)| v);
    match (vendor, codec) {
        (Some(0x10DE), RecordCodec::H264) => vec![
            SelectedEncoder::H264Nvenc,
            SelectedEncoder::H264Vulkan,
            SelectedEncoder::LibX264,
        ],
        (Some(0x10DE), RecordCodec::H265) => {
            vec![SelectedEncoder::HevcNvenc, SelectedEncoder::HevcVulkan]
        }
        (Some(0x1002 | 0x8086), RecordCodec::H264) => vec![
            SelectedEncoder::H264Vaapi,
            SelectedEncoder::H264Vulkan,
            SelectedEncoder::LibX264,
        ],
        (Some(0x1002 | 0x8086), RecordCodec::H265) => {
            vec![SelectedEncoder::HevcVaapi, SelectedEncoder::HevcVulkan]
        }
        (_, RecordCodec::H264) => vec![
            SelectedEncoder::H264Nvenc,
            SelectedEncoder::H264Vaapi,
            SelectedEncoder::H264Vulkan,
            SelectedEncoder::LibX264,
        ],
        (_, RecordCodec::H265) => vec![
            SelectedEncoder::HevcNvenc,
            SelectedEncoder::HevcVaapi,
            SelectedEncoder::HevcVulkan,
        ],
    }
}

#[must_use]
fn detect_vaapi_render_node() -> Option<PathBuf> {
    for idx in 128..=135 {
        let p = PathBuf::from(format!("/dev/dri/renderD{idx}"));
        if p.exists() {
            return Some(p);
        }
    }
    None
}

#[must_use]
fn probe_single_encoder(bin: &Path, enc: SelectedEncoder, vaapi_dev: Option<&Path>) -> bool {
    let mut cmd = bundled_ffmpeg_command(bin);
    cmd.args(["-hide_banner", "-loglevel", "error"]);
    if enc.is_vaapi() {
        let Some(dev) = vaapi_dev else {
            return false;
        };
        cmd.arg("-vaapi_device").arg(dev);
    } else if enc.is_vulkan() {
        cmd.args(["-init_hw_device", "vulkan=vk:0", "-filter_hw_device", "vk"]);
    }
    cmd.args(["-f", "lavfi", "-i", "color=black:s=256x256:d=0.05"]);
    if enc.is_vaapi() || enc.is_vulkan() {
        cmd.args(["-vf", "format=nv12,hwupload"]);
    } else {
        cmd.args(["-vf", "format=nv12"]);
    }
    cmd.args(["-c:v", enc.ffmpeg_name(), "-f", "null", "-"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());

    cmd.status().is_ok_and(|s| s.success())
}

fn run_probe(target_gpu: Option<(u32, u32)>) -> FfmpegCapabilities {
    let Some(bin) = locate_bundled_ffmpeg() else {
        return FfmpegCapabilities {
            bundled_ffmpeg: None,
            h264_encoder: None,
            h265_encoder: None,
            vaapi_device: None,
            warning: Some(
                "Bundled fpshud-ffmpeg not found — run scripts/install.sh to enable recording/playback"
                    .to_string(),
            ),
        };
    };

    let vaapi_dev = detect_vaapi_render_node();
    let h264 = encoder_priority_for_gpu(target_gpu, RecordCodec::H264)
        .into_iter()
        .find(|&enc| probe_single_encoder(&bin, enc, vaapi_dev.as_deref()));
    let h265 = encoder_priority_for_gpu(target_gpu, RecordCodec::H265)
        .into_iter()
        .find(|&enc| probe_single_encoder(&bin, enc, vaapi_dev.as_deref()));

    FfmpegCapabilities {
        bundled_ffmpeg: Some(bin),
        h264_encoder: h264,
        h265_encoder: h265,
        vaapi_device: vaapi_dev,
        warning: None,
    }
}

static CACHED_CAPS: OnceLock<FfmpegCapabilities> = OnceLock::new();

/// Probes bundled `fpshud-ffmpeg` encoder capabilities once per process.
#[must_use]
pub fn probe_capabilities(target_gpu: Option<(u32, u32)>) -> FfmpegCapabilities {
    CACHED_CAPS.get_or_init(|| run_probe(target_gpu)).clone()
}
