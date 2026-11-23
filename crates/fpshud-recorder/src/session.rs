use crate::capture::{
    ColorTransfer, PixelBufferPool, RecorderPixelFormat, convert_frame_to_rgba8,
};
use crate::locate::{bundled_ffmpeg_command, tune_pipe_capacity};
use crate::pacer::compute_target_dimensions;
use crate::probe::{SelectedEncoder, probe_capabilities};
use chrono::Local;
use fpshud_core::{RecordAudioMode, RecordBitrate, RecordCodec, RecordFps, RecordResolution};
use std::fs;
use std::io::{self, Write};
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU8, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

#[derive(Debug, Clone)]
pub struct RecorderConfig {
    pub output_dir: PathBuf,
    pub resolution: RecordResolution,
    pub fps: RecordFps,
    pub codec: RecordCodec,
    pub bitrate: RecordBitrate,
    pub audio_mode: RecordAudioMode,
    pub target_gpu: Option<(u32, u32)>,
}

#[derive(Debug, Clone)]
pub enum RecorderEvent {
    Started {
        backend: String,
        output_path: PathBuf,
    },
    SegmentSplit {
        new_output_path: PathBuf,
    },
    Warning(String),
    Error(String),
    Stopped {
        output_paths: Vec<PathBuf>,
    },
}

/// RAII release token for zero-copy Vulkan staging slots (`state: AtomicU8`, resets to `0 = Free` on drop).
pub struct ZeroCopySlotToken {
    state: Arc<AtomicU8>,
}

impl ZeroCopySlotToken {
    #[must_use]
    pub fn new(state: Arc<AtomicU8>) -> Self {
        Self { state }
    }
}

impl Drop for ZeroCopySlotToken {
    fn drop(&mut self) {
        self.state.store(0, Ordering::Release);
    }
}

pub enum FramePayload {
    ZeroCopySlot {
        ptr: *const u8,
        len: usize,
        _token: ZeroCopySlotToken,
    },
    OwnedBuffer {
        data: Vec<u8>,
        pool: Option<PixelBufferPool>,
    },
}

// SAFETY: `ZeroCopySlot` pointer points to persistently mapped `HOST_CACHED` Vulkan staging memory
// whose lifetime is guarded by `ZeroCopySlotToken` (buffers are never destroyed until state == 0).
unsafe impl Send for FramePayload {}

pub struct SubmittedFrame {
    pub cap_w: u32,
    pub cap_h: u32,
    pub src_w: u32,
    pub src_h: u32,
    pub pix_fmt: RecorderPixelFormat,
    pub transfer: ColorTransfer,
    pub flip_y: bool,
    pub payload: FramePayload,
}

enum WorkerMsg {
    Frame(SubmittedFrame),
    Stop,
}

/// Builds the exact `fpshud-ffmpeg` CLI argument vector for recording.
#[allow(clippy::too_many_arguments)]
#[must_use]
pub fn build_record_ffmpeg_args(
    cap_w: u32,
    cap_h: u32,
    enc_w: u32,
    enc_h: u32,
    in_pix_fmt: &str,
    flip_y: bool,
    target_fps: u32,
    encoder: SelectedEncoder,
    vaapi_device: Option<&Path>,
    bitrate: RecordBitrate,
    audio_mode: RecordAudioMode,
    output_path: &Path,
) -> Vec<String> {
    let mut args = vec![
        "-y".to_string(),
        "-hide_banner".to_string(),
        "-loglevel".to_string(),
        "error".to_string(),
    ];

    if encoder.is_vaapi()
        && let Some(dev) = vaapi_device
    {
        args.push("-vaapi_device".to_string());
        args.push(dev.to_string_lossy().into_owned());
    } else if encoder.is_vulkan() {
        args.extend([
            "-init_hw_device".to_string(),
            "vulkan=vk:0".to_string(),
            "-filter_hw_device".to_string(),
            "vk".to_string(),
        ]);
    }

    args.extend([
        "-use_wallclock_as_timestamps".to_string(),
        "1".to_string(),
        "-f".to_string(),
        "rawvideo".to_string(),
        "-pix_fmt".to_string(),
        in_pix_fmt.to_string(),
        "-s".to_string(),
        format!("{cap_w}x{cap_h}"),
        "-i".to_string(),
        "pipe:0".to_string(),
    ]);

    match audio_mode {
        RecordAudioMode::Off => {}
        RecordAudioMode::GameAudio => {
            args.extend([
                "-f".to_string(),
                "pulse".to_string(),
                "-sample_rate".to_string(),
                "48000".to_string(),
                "-channels".to_string(),
                "2".to_string(),
                "-i".to_string(),
                "@DEFAULT_MONITOR@".to_string(),
            ]);
        }
        RecordAudioMode::MicOnly => {
            args.extend([
                "-f".to_string(),
                "pulse".to_string(),
                "-sample_rate".to_string(),
                "48000".to_string(),
                "-channels".to_string(),
                "2".to_string(),
                "-i".to_string(),
                "@DEFAULT_SOURCE@".to_string(),
            ]);
        }
        RecordAudioMode::GameAndMic => {
            args.extend([
                "-f".to_string(),
                "pulse".to_string(),
                "-sample_rate".to_string(),
                "48000".to_string(),
                "-channels".to_string(),
                "2".to_string(),
                "-i".to_string(),
                "@DEFAULT_MONITOR@".to_string(),
                "-f".to_string(),
                "pulse".to_string(),
                "-sample_rate".to_string(),
                "48000".to_string(),
                "-channels".to_string(),
                "2".to_string(),
                "-i".to_string(),
                "@DEFAULT_SOURCE@".to_string(),
            ]);
        }
    }

    let mut vf_parts: Vec<String> = Vec::new();
    if flip_y {
        vf_parts.push("vflip".to_string());
    }
    if (cap_w, cap_h) != (enc_w, enc_h) {
        vf_parts.push(format!("scale={enc_w}:{enc_h}:flags=bilinear"));
    }
    vf_parts.push(
        "scale=in_range=full:out_color_matrix=bt709:out_range=tv,format=nv12,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv"
            .to_string(),
    );
    if encoder.is_vaapi() || encoder.is_vulkan() {
        vf_parts.push("hwupload".to_string());
    }
    let vf_chain = vf_parts.join(",");

    if audio_mode == RecordAudioMode::GameAndMic {
        args.push("-filter_complex".to_string());
        args.push(format!(
            "[0:v]{vf_chain}[vout];[1:a][2:a]amix=inputs=2:duration=longest:dropout_transition=0,alimiter=limit=0.95,aresample=async=1[aout]"
        ));
        args.extend(["-map".to_string(), "[vout]".to_string()]);
        args.extend(["-map".to_string(), "[aout]".to_string()]);
    } else {
        args.push("-vf".to_string());
        args.push(vf_chain);
        if audio_mode != RecordAudioMode::Off {
            args.extend(["-af".to_string(), "aresample=async=1".to_string()]);
        }
    }

    args.push("-c:v".to_string());
    args.push(encoder.ffmpeg_name().to_string());

    match (encoder, bitrate.mbps()) {
        (SelectedEncoder::H264Nvenc | SelectedEncoder::HevcNvenc, None) => {
            args.extend([
                "-preset".to_string(),
                "p4".to_string(),
                "-tune".to_string(),
                "hq".to_string(),
                "-rc".to_string(),
                "vbr".to_string(),
                "-cq".to_string(),
                "20".to_string(),
                "-b:v".to_string(),
                "0".to_string(),
            ]);
        }
        (SelectedEncoder::H264Nvenc | SelectedEncoder::HevcNvenc, Some(mbps)) => {
            args.extend([
                "-preset".to_string(),
                "p4".to_string(),
                "-tune".to_string(),
                "hq".to_string(),
                "-rc".to_string(),
                "vbr".to_string(),
                "-b:v".to_string(),
                format!("{mbps}M"),
                "-maxrate".to_string(),
                format!("{}M", mbps * 3 / 2),
                "-bufsize".to_string(),
                format!("{}M", mbps * 2),
            ]);
        }
        (SelectedEncoder::H264Vaapi | SelectedEncoder::HevcVaapi, None) => {
            args.extend([
                "-rc_mode".to_string(),
                "CQP".to_string(),
                "-qp".to_string(),
                "20".to_string(),
            ]);
        }
        (SelectedEncoder::H264Vaapi | SelectedEncoder::HevcVaapi, Some(mbps)) => {
            args.extend([
                "-rc_mode".to_string(),
                "VBR".to_string(),
                "-b:v".to_string(),
                format!("{mbps}M"),
                "-maxrate".to_string(),
                format!("{}M", mbps * 3 / 2),
            ]);
        }
        (SelectedEncoder::H264Vulkan | SelectedEncoder::HevcVulkan, Some(mbps)) => {
            args.extend([
                "-rc_mode".to_string(),
                "vbr".to_string(),
                "-b:v".to_string(),
                format!("{mbps}M"),
            ]);
        }
        (SelectedEncoder::H264Vulkan | SelectedEncoder::HevcVulkan, None) => {}
        (SelectedEncoder::LibX264, None) => {
            args.extend([
                "-preset".to_string(),
                "veryfast".to_string(),
                "-crf".to_string(),
                "20".to_string(),
            ]);
        }
        (SelectedEncoder::LibX264, Some(mbps)) => {
            args.extend([
                "-preset".to_string(),
                "veryfast".to_string(),
                "-b:v".to_string(),
                format!("{mbps}M"),
                "-maxrate".to_string(),
                format!("{}M", mbps * 3 / 2),
                "-bufsize".to_string(),
                format!("{}M", mbps * 2),
            ]);
        }
    }

    let gop = (target_fps * 2).max(2);
    args.extend([
        "-g".to_string(),
        gop.to_string(),
        "-fps_mode".to_string(),
        "cfr".to_string(),
        "-r".to_string(),
        target_fps.to_string(),
    ]);

    if audio_mode != RecordAudioMode::Off {
        args.extend([
            "-c:a".to_string(),
            "aac".to_string(),
            "-b:a".to_string(),
            "192k".to_string(),
            "-shortest".to_string(),
        ]);
    }

    args.extend([
        "-movflags".to_string(),
        "+frag_keyframe+empty_moov+default_base_moof+write_colr".to_string(),
        output_path.to_string_lossy().into_owned(),
    ]);

    args
}

struct ChildGuard {
    child: Child,
    stdin: Option<ChildStdin>,
}

impl ChildGuard {
    fn finish(mut self) -> bool {
        drop(self.stdin.take());
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            match self.child.try_wait() {
                Ok(Some(status)) => return status.success(),
                Ok(None) => {
                    if Instant::now() >= deadline {
                        let _ = self.child.kill();
                        let _ = self.child.wait();
                        return false;
                    }
                    thread::sleep(Duration::from_millis(15));
                }
                Err(_) => return false,
            }
        }
    }
}

impl Drop for ChildGuard {
    fn drop(&mut self) {
        drop(self.stdin.take());
        if self.child.try_wait().ok().flatten().is_none() {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}

fn ensure_safe_output_dir(dir: &Path) -> io::Result<()> {
    if let Ok(meta) = fs::symlink_metadata(dir) {
        if meta.file_type().is_symlink() || !meta.is_dir() {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "recording output directory is a symlink or non-directory",
            ));
        }
        return Ok(());
    }
    fs::create_dir_all(dir)
}

pub struct RecorderHandle {
    tx: mpsc::SyncSender<WorkerMsg>,
    stop_requested: Arc<AtomicBool>,
    dropped_frames: Arc<AtomicU64>,
    started_at: Instant,
    backend_label: String,
    active_output: Arc<Mutex<Option<PathBuf>>>,
    worker: Option<JoinHandle<Vec<PathBuf>>>,
    finished_paths: Vec<PathBuf>,
}

impl RecorderHandle {
    #[must_use]
    pub fn try_submit_frame(&self, frame: SubmittedFrame) -> bool {
        if self.stop_requested.load(Ordering::Acquire) {
            return false;
        }
        match self.tx.try_send(WorkerMsg::Frame(frame)) {
            Ok(()) => true,
            Err(_) => {
                self.dropped_frames.fetch_add(1, Ordering::Relaxed);
                false
            }
        }
    }

    pub fn request_stop(&self) {
        self.stop_requested.store(true, Ordering::Release);
        let _ = self.tx.try_send(WorkerMsg::Stop);
    }

    #[must_use]
    pub fn is_stopping(&self) -> bool {
        self.stop_requested.load(Ordering::Acquire)
    }

    #[must_use]
    pub fn is_finished(&self) -> bool {
        self.worker
            .as_ref()
            .is_none_or(JoinHandle::is_finished)
    }

    pub fn wait_finished(&mut self) -> Vec<PathBuf> {
        self.request_stop();
        if let Some(handle) = self.worker.take()
            && let Ok(paths) = handle.join()
        {
            self.finished_paths = paths;
        }
        self.finished_paths.clone()
    }

    #[must_use]
    pub fn dropped_frames(&self) -> u64 {
        self.dropped_frames.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn elapsed_sec(&self) -> f32 {
        self.started_at.elapsed().as_secs_f32()
    }

    #[must_use]
    pub fn active_backend(&self) -> &str {
        &self.backend_label
    }

    #[must_use]
    pub fn active_output_path(&self) -> Option<PathBuf> {
        self.active_output.lock().ok().and_then(|g| g.clone())
    }
}

impl Drop for RecorderHandle {
    fn drop(&mut self) {
        let _ = self.wait_finished();
    }
}

pub struct RecorderSession;

impl RecorderSession {
    pub fn start<F>(cfg: RecorderConfig, on_event: F) -> io::Result<RecorderHandle>
    where
        F: Fn(RecorderEvent) + Send + 'static,
    {
        Self::start_with_pulse_server(cfg, None, on_event)
    }

    pub fn start_with_pulse_server<F>(
        cfg: RecorderConfig,
        pulse_server_override: Option<String>,
        on_event: F,
    ) -> io::Result<RecorderHandle>
    where
        F: Fn(RecorderEvent) + Send + 'static,
    {
        ensure_safe_output_dir(&cfg.output_dir)?;
        let caps = probe_capabilities(cfg.target_gpu);
        let ffmpeg_bin = caps.bundled_ffmpeg.ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                caps.warning.unwrap_or_else(|| {
                    "Bundled fpshud-ffmpeg not found".to_string()
                }),
            )
        })?;

        let (encoder, codec_warning) = match cfg.codec {
            RecordCodec::H265 => {
                if let Some(hevc) = caps.h265_encoder {
                    (hevc, None)
                } else if let Some(h264) = caps.h264_encoder {
                    (
                        h264,
                        Some(format!(
                            "Hardware H.265 unavailable — using {}",
                            h264.label()
                        )),
                    )
                } else {
                    return Err(io::Error::new(
                        io::ErrorKind::Unsupported,
                        "No usable video encoder found in bundled fpshud-ffmpeg",
                    ));
                }
            }
            RecordCodec::H264 => {
                let enc = caps.h264_encoder.ok_or_else(|| {
                    io::Error::new(
                        io::ErrorKind::Unsupported,
                        "No usable H.264 encoder found in bundled fpshud-ffmpeg",
                    )
                })?;
                (enc, None)
            }
        };

        let (tx, rx) = mpsc::sync_channel::<WorkerMsg>(4);
        let stop_requested = Arc::new(AtomicBool::new(false));
        let dropped_frames = Arc::new(AtomicU64::new(0));
        let active_output = Arc::new(Mutex::new(None));
        let backend_label = encoder.label().to_string();

        let active_out_w = Arc::clone(&active_output);
        let stop_w = Arc::clone(&stop_requested);
        let vaapi_dev = caps.vaapi_device;
        let backend_w = backend_label.clone();

        let worker = thread::Builder::new()
            .name("fpshud_rec".to_string())
            .spawn(move || {
                if let Some(w) = codec_warning {
                    on_event(RecorderEvent::Warning(w));
                }
                run_recorder_worker(
                    rx,
                    stop_w,
                    cfg,
                    ffmpeg_bin,
                    encoder,
                    vaapi_dev,
                    backend_w,
                    pulse_server_override,
                    active_out_w,
                    on_event,
                )
            })?;

        Ok(RecorderHandle {
            tx,
            stop_requested,
            dropped_frames,
            started_at: Instant::now(),
            backend_label,
            active_output,
            worker: Some(worker),
            finished_paths: Vec::new(),
        })
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct StreamShape {
    cap_w: u32,
    cap_h: u32,
    enc_w: u32,
    enc_h: u32,
    pix_fmt: RecorderPixelFormat,
    flip_y: bool,
}

#[allow(clippy::too_many_arguments)]
fn spawn_segment_ffmpeg(
    ffmpeg_bin: &Path,
    shape: StreamShape,
    fps: u32,
    encoder: SelectedEncoder,
    vaapi_dev: Option<&Path>,
    bitrate: RecordBitrate,
    audio_mode: RecordAudioMode,
    pulse_server_override: Option<&str>,
    output_path: &Path,
) -> io::Result<ChildGuard> {
    let in_pix_fmt = if shape.pix_fmt.is_direct_8bit() {
        shape.pix_fmt.ffmpeg_pix_fmt()
    } else {
        "rgba"
    };
    let args = build_record_ffmpeg_args(
        shape.cap_w,
        shape.cap_h,
        shape.enc_w,
        shape.enc_h,
        in_pix_fmt,
        shape.flip_y,
        fps,
        encoder,
        vaapi_dev,
        bitrate,
        audio_mode,
        output_path,
    );
    let mut cmd = bundled_ffmpeg_command(ffmpeg_bin);
    if let Some(ps) = pulse_server_override {
        cmd.env("PULSE_SERVER", ps);
    }
    let mut child = cmd
        .args(&args)
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()?;
    let stdin = child.stdin.take();
    if let Some(ref s) = stdin {
        tune_pipe_capacity(s.as_raw_fd());
    }
    Ok(ChildGuard { child, stdin })
}

#[allow(clippy::too_many_arguments)]
fn run_recorder_worker<F>(
    rx: mpsc::Receiver<WorkerMsg>,
    stop_requested: Arc<AtomicBool>,
    cfg: RecorderConfig,
    ffmpeg_bin: PathBuf,
    encoder: SelectedEncoder,
    vaapi_dev: Option<PathBuf>,
    backend_label: String,
    pulse_server_override: Option<String>,
    active_output: Arc<Mutex<Option<PathBuf>>>,
    on_event: F,
) -> Vec<PathBuf>
where
    F: Fn(RecorderEvent) + Send + 'static,
{
    let stamp = Local::now().format("%Y%m%d_%H%M%S").to_string();
    let mut segment_idx: u32 = 0;
    let mut current_shape: Option<StreamShape> = None;
    let mut current_child: Option<ChildGuard> = None;
    let mut current_audio_mode = cfg.audio_mode;
    let mut output_paths: Vec<PathBuf> = Vec::new();
    let mut hdr_scratch: Vec<u8> = Vec::new();

    let make_segment_path = |idx: u32| -> PathBuf {
        if idx <= 1 {
            cfg.output_dir.join(format!("fpshud_rec_{stamp}.mp4"))
        } else {
            cfg.output_dir
                .join(format!("fpshud_rec_{stamp}_part{idx}.mp4"))
        }
    };

    while let Ok(msg) = rx.recv() {
        let frame = match msg {
            WorkerMsg::Stop => break,
            WorkerMsg::Frame(f) => f,
        };
        if stop_requested.load(Ordering::Acquire) {
            break;
        }

        let (enc_w, enc_h) = compute_target_dimensions(frame.src_w, frame.src_h, cfg.resolution);
        let shape = StreamShape {
            cap_w: frame.cap_w,
            cap_h: frame.cap_h,
            enc_w,
            enc_h,
            pix_fmt: frame.pix_fmt,
            flip_y: frame.flip_y,
        };

        if current_shape != Some(shape) {
            if let Some(old) = current_child.take() {
                let _ = old.finish();
            }
            segment_idx += 1;
            let seg_path = make_segment_path(segment_idx);
            match spawn_segment_ffmpeg(
                &ffmpeg_bin,
                shape,
                cfg.fps.as_u32(),
                encoder,
                vaapi_dev.as_deref(),
                cfg.bitrate,
                current_audio_mode,
                pulse_server_override.as_deref(),
                &seg_path,
            ) {
                Ok(guard) => {
                    if let Ok(mut g) = active_output.lock() {
                        *g = Some(seg_path.clone());
                    }
                    if segment_idx == 1 {
                        on_event(RecorderEvent::Started {
                            backend: backend_label.clone(),
                            output_path: seg_path.clone(),
                        });
                    } else {
                        on_event(RecorderEvent::SegmentSplit {
                            new_output_path: seg_path.clone(),
                        });
                    }
                    output_paths.push(seg_path);
                    current_child = Some(guard);
                    current_shape = Some(shape);
                }
                Err(err) => {
                    on_event(RecorderEvent::Error(format!(
                        "Failed to start fpshud-ffmpeg: {err}"
                    )));
                    break;
                }
            }
        }

        let bytes_to_write: &[u8] = match &frame.payload {
            FramePayload::ZeroCopySlot { ptr, len, .. } => {
                // SAFETY: `ZeroCopySlot` guarantees valid slice for the lifetime of `frame`.
                let raw = unsafe { std::slice::from_raw_parts(*ptr, *len) };
                if frame.pix_fmt.is_direct_8bit() {
                    raw
                } else {
                    convert_frame_to_rgba8(
                        raw,
                        frame.cap_w,
                        frame.cap_h,
                        frame.pix_fmt,
                        frame.transfer,
                        &mut hdr_scratch,
                    );
                    &hdr_scratch
                }
            }
            FramePayload::OwnedBuffer { data, .. } => {
                if frame.pix_fmt.is_direct_8bit() {
                    data.as_slice()
                } else {
                    convert_frame_to_rgba8(
                        data,
                        frame.cap_w,
                        frame.cap_h,
                        frame.pix_fmt,
                        frame.transfer,
                        &mut hdr_scratch,
                    );
                    &hdr_scratch
                }
            }
        };

        let write_ok = current_child
            .as_mut()
            .and_then(|c| c.stdin.as_mut())
            .is_some_and(|stdin| stdin.write_all(bytes_to_write).is_ok());

        if !write_ok {
            // If audio was enabled, retry cleanly in video-only mode with a warning!
            if current_audio_mode != RecordAudioMode::Off {
                if let Some(old) = current_child.take() {
                    let _ = old.finish();
                }
                current_audio_mode = RecordAudioMode::Off;
                on_event(RecorderEvent::Warning(
                    "Audio capture unavailable — continuing video-only".to_string(),
                ));
                let retry_path = output_paths
                    .last()
                    .cloned()
                    .unwrap_or_else(|| make_segment_path(segment_idx.max(1)));
                if let Ok(mut guard) = spawn_segment_ffmpeg(
                    &ffmpeg_bin,
                    shape,
                    cfg.fps.as_u32(),
                    encoder,
                    vaapi_dev.as_deref(),
                    cfg.bitrate,
                    RecordAudioMode::Off,
                    pulse_server_override.as_deref(),
                    &retry_path,
                ) {
                    if let Some(stdin) = guard.stdin.as_mut() {
                        let _ = stdin.write_all(bytes_to_write);
                    }
                    current_child = Some(guard);
                } else {
                    on_event(RecorderEvent::Error(
                        "Recording failed even in video-only fallback".to_string(),
                    ));
                    break;
                }
            } else {
                on_event(RecorderEvent::Error(
                    "fpshud-ffmpeg pipe closed unexpectedly".to_string(),
                ));
                break;
            }
        }

        if let FramePayload::OwnedBuffer { data, pool: Some(p) } = frame.payload {
            p.put(data);
        }
    }

    if let Some(old) = current_child.take() {
        let _ = old.finish();
    }
    if let Ok(mut g) = active_output.lock() {
        *g = None;
    }
    output_paths.retain(|p| fs::metadata(p).is_ok_and(|m| m.len() > 0));
    on_event(RecorderEvent::Stopped {
        output_paths: output_paths.clone(),
    });
    output_paths
}
