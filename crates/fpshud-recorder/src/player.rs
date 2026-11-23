use crate::library::parse_ffmpeg_probe_output;
use crate::locate::{bundled_ffmpeg_command, locate_bundled_ffmpeg, tune_pipe_capacity};
pub use crate::yuv::{PlayerColorSpace, PlayerPixelFormat, expected_frame_byte_len};
use std::io::{self, Read};
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::process::{Child, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::thread::{self, JoinHandle};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum HwDecodeBackend {
    CudaNvdec,
    Vaapi,
    Vulkan,
    #[default]
    Software,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PlayerOutputMode {
    #[default]
    NativeYuv,
    ViewportScaledYuv,
}

#[derive(Debug, Default, Clone)]
pub struct PlayerFrame {
    pub width: u32,
    pub height: u32,
    pub pixel_format: PlayerPixelFormat,
    pub color_space: PlayerColorSpace,
    pub pts_sec: f64,
    pub data: Vec<u8>,
    pub generation: u64,
}

#[derive(Debug, Clone, Default)]
pub struct PlayerSnapshot {
    pub ready: bool,
    pub playing: bool,
    pub at_eof: bool,
    pub muted: bool,
    pub has_audio: bool,
    pub duration_sec: f64,
    pub current_sec: f64,
    pub src_width: u32,
    pub src_height: u32,
    pub color_space: PlayerColorSpace,
    pub hw_backend: HwDecodeBackend,
    pub error: Option<String>,
}

/// Computes even-aligned aspect-fit output dimensions inside `(max_w, max_h)`.
#[must_use]
pub fn compute_player_target_size(src_w: u32, src_h: u32, max_w: u32, max_h: u32) -> (u32, u32) {
    let sw = src_w.max(2) as f64;
    let sh = src_h.max(2) as f64;
    let mw = max_w.max(16) as f64;
    let mh = max_h.max(16) as f64;
    let scale = (mw / sw).min(mh / sh);
    let w = ((sw * scale).round() as u32).max(2) & !1;
    let h = ((sh * scale).round() as u32).max(2) & !1;
    (w.max(2), h.max(2))
}

/// Builds the 1-process `fpshud-ffmpeg` playback or 1-frame shot CLI argument vector.
#[must_use]
pub fn build_player_ffmpeg_args(
    path: &Path,
    start_sec: f64,
    out_w: u32,
    out_h: u32,
    playback_fps: f32,
    with_audio: bool,
    single_frame: bool,
) -> Vec<String> {
    let mut args = vec![
        "-hide_banner".to_string(),
        "-loglevel".to_string(),
        "error".to_string(),
    ];
    if !single_frame {
        args.push("-re".to_string());
    }
    if start_sec > 0.001 {
        args.push("-ss".to_string());
        args.push(format!("{start_sec:.3}"));
    }
    args.push("-i".to_string());
    args.push(path.to_string_lossy().into_owned());

    args.extend([
        "-map".to_string(),
        "0:v:0".to_string(),
        "-vf".to_string(),
        format!("scale={out_w}:{out_h}:flags=bicubic,format=rgba"),
    ]);

    if single_frame {
        args.extend([
            "-frames:v".to_string(),
            "1".to_string(),
            "-f".to_string(),
            "rawvideo".to_string(),
            "pipe:1".to_string(),
        ]);
    } else {
        let fps = playback_fps.clamp(1.0, 60.0);
        args.extend([
            "-fps_mode".to_string(),
            "cfr".to_string(),
            "-r".to_string(),
            format!("{fps:.3}"),
            "-f".to_string(),
            "rawvideo".to_string(),
            "pipe:1".to_string(),
        ]);
        if with_audio {
            args.extend([
                "-map".to_string(),
                "0:a:0?".to_string(),
                "-f".to_string(),
                "pulse".to_string(),
                "-name".to_string(),
                "FPSHUD Player".to_string(),
                "-buffer_duration".to_string(),
                "60".to_string(),
                "default".to_string(),
            ]);
        }
    }

    args
}

/// Probes the best working hardware video decoder backend once per process (`OnceLock`).
pub fn probe_hw_decoder(ffmpeg_bin: &Path) -> HwDecodeBackend {
    static CACHED: OnceLock<HwDecodeBackend> = OnceLock::new();
    *CACHED.get_or_init(|| probe_hw_decoder_uncached(ffmpeg_bin))
}

fn probe_hw_decoder_uncached(ffmpeg_bin: &Path) -> HwDecodeBackend {
    let tmp = std::env::temp_dir().join(format!("fpshud_hwdec_probe_{}.mp4", std::process::id()));
    let enc_ok = bundled_ffmpeg_command(ffmpeg_bin)
        .args([
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "color=c=black:s=64x64:r=30:d=0.1",
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            "-frames:v",
            "2",
        ])
        .arg(&tmp)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|s| s.success());
    if !enc_ok {
        let _ = std::fs::remove_file(&tmp);
        return HwDecodeBackend::Software;
    }

    let candidates = [
        HwDecodeBackend::CudaNvdec,
        HwDecodeBackend::Vaapi,
        HwDecodeBackend::Vulkan,
    ];
    let mut chosen = HwDecodeBackend::Software;
    for backend in candidates {
        if backend == HwDecodeBackend::Vaapi && !Path::new("/dev/dri/renderD128").exists() {
            continue;
        }
        let (args, _, _, _) = build_player_video_ffmpeg_args(
            &tmp,
            0.0,
            64,
            64,
            (64, 64),
            30.0,
            PlayerOutputMode::NativeYuv,
            backend,
            true,
        );
        let ok = bundled_ffmpeg_command(ffmpeg_bin)
            .args(&args)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .output()
            .is_ok_and(|o| o.status.success() && o.stdout.len() >= 64 * 64 * 3 / 2);
        if ok {
            chosen = backend;
            break;
        }
    }
    let _ = std::fs::remove_file(&tmp);
    chosen
}

/// Builds the video-only `fpshud-ffmpeg` CLI argument vector for HW/SW decode to native `NV12` or `YUV420p`.
#[allow(clippy::too_many_arguments)]
#[must_use]
pub fn build_player_video_ffmpeg_args(
    path: &Path,
    start_sec: f64,
    src_w: u32,
    src_h: u32,
    viewport: (u32, u32),
    playback_fps: f32,
    mode: PlayerOutputMode,
    backend: HwDecodeBackend,
    single_frame: bool,
) -> (Vec<String>, u32, u32, PlayerPixelFormat) {
    let even_src_w = (src_w.max(2)) & !1;
    let even_src_h = (src_h.max(2)) & !1;
    let (out_w, out_h) = match mode {
        PlayerOutputMode::NativeYuv => (even_src_w, even_src_h),
        PlayerOutputMode::ViewportScaledYuv => {
            compute_player_target_size(even_src_w, even_src_h, viewport.0, viewport.1)
        }
    };
    let pix_fmt = match backend {
        HwDecodeBackend::CudaNvdec | HwDecodeBackend::Vaapi | HwDecodeBackend::Vulkan => {
            PlayerPixelFormat::Nv12
        }
        HwDecodeBackend::Software => PlayerPixelFormat::Yuv420p,
    };
    let pix_fmt_str = match pix_fmt {
        PlayerPixelFormat::Nv12 => "nv12",
        PlayerPixelFormat::Yuv420p => "yuv420p",
        PlayerPixelFormat::Rgba8 => "rgba",
    };

    let mut args = vec![
        "-hide_banner".to_string(),
        "-loglevel".to_string(),
        "error".to_string(),
    ];
    if !single_frame {
        args.push("-re".to_string());
    }
    match backend {
        HwDecodeBackend::CudaNvdec => {
            args.extend([
                "-hwaccel".to_string(),
                "cuda".to_string(),
                "-hwaccel_output_format".to_string(),
                "nv12".to_string(),
            ]);
        }
        HwDecodeBackend::Vaapi => {
            args.extend([
                "-hwaccel".to_string(),
                "vaapi".to_string(),
                "-hwaccel_device".to_string(),
                "/dev/dri/renderD128".to_string(),
                "-hwaccel_output_format".to_string(),
                "vaapi".to_string(),
            ]);
        }
        HwDecodeBackend::Vulkan => {
            args.extend([
                "-hwaccel".to_string(),
                "vulkan".to_string(),
                "-hwaccel_output_format".to_string(),
                "vulkan".to_string(),
            ]);
        }
        HwDecodeBackend::Software => {}
    }

    if start_sec > 0.001 {
        args.push("-ss".to_string());
        args.push(format!("{start_sec:.3}"));
    }
    args.push("-i".to_string());
    args.push(path.to_string_lossy().into_owned());
    args.extend(["-an".to_string(), "-map".to_string(), "0:v:0".to_string()]);

    let mut vf_parts: Vec<String> = Vec::new();
    if matches!(backend, HwDecodeBackend::Vaapi | HwDecodeBackend::Vulkan) {
        vf_parts.push("hwdownload".to_string());
        vf_parts.push("format=nv12".to_string());
    }
    if even_src_w != src_w || even_src_h != src_h {
        vf_parts.push(format!("crop={even_src_w}:{even_src_h}:0:0"));
    }
    if mode == PlayerOutputMode::ViewportScaledYuv && (out_w != even_src_w || out_h != even_src_h) {
        vf_parts.push(format!("scale={out_w}:{out_h}:flags=bicubic"));
    }
    if !vf_parts.is_empty() {
        args.push("-vf".to_string());
        args.push(vf_parts.join(","));
    }

    args.extend(["-pix_fmt".to_string(), pix_fmt_str.to_string()]);
    if single_frame {
        args.extend([
            "-frames:v".to_string(),
            "1".to_string(),
            "-f".to_string(),
            "rawvideo".to_string(),
            "pipe:1".to_string(),
        ]);
    } else {
        let fps = playback_fps.clamp(1.0, 60.0);
        args.extend([
            "-fps_mode".to_string(),
            "cfr".to_string(),
            "-r".to_string(),
            format!("{fps:.3}"),
            "-f".to_string(),
            "rawvideo".to_string(),
            "pipe:1".to_string(),
        ]);
    }

    (args, out_w, out_h, pix_fmt)
}

/// Builds the lightweight audio-only `fpshud-ffmpeg` CLI argument vector so PulseAudio
/// buffering never blocks rawvideo frame delivery on `pipe:1`.
#[must_use]
pub fn build_player_audio_ffmpeg_args(path: &Path, start_sec: f64) -> Vec<String> {
    let mut args = vec![
        "-hide_banner".to_string(),
        "-loglevel".to_string(),
        "error".to_string(),
        "-re".to_string(),
    ];
    if start_sec > 0.001 {
        args.push("-ss".to_string());
        args.push(format!("{start_sec:.3}"));
    }
    args.extend([
        "-i".to_string(),
        path.to_string_lossy().into_owned(),
        "-vn".to_string(),
        "-map".to_string(),
        "0:a:0?".to_string(),
        "-f".to_string(),
        "pulse".to_string(),
        "-name".to_string(),
        "FPSHUD Player".to_string(),
        "-buffer_duration".to_string(),
        "60".to_string(),
        "default".to_string(),
    ]);
    args
}

struct SharedPlayerState {
    snapshot: PlayerSnapshot,
    viewport: (u32, u32),
    desired_seek_sec: f64,
    single_frame_requested: bool,
    quit: bool,
    active_video_child: Option<Child>,
    active_audio_child: Option<Child>,
}

struct LatestFrameSlot {
    frame: PlayerFrame,
    dirty: bool,
}

pub struct VideoPlayerSession {
    path: PathBuf,
    mode: PlayerOutputMode,
    generation: Arc<AtomicU64>,
    state: Arc<(Mutex<SharedPlayerState>, Condvar)>,
    latest: Arc<Mutex<LatestFrameSlot>>,
    stopped: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

impl VideoPlayerSession {
    pub fn open(path: PathBuf) -> io::Result<Self> {
        Self::open_with_mode(path, PlayerOutputMode::NativeYuv)
    }

    pub fn open_with_mode(path: PathBuf, mode: PlayerOutputMode) -> io::Result<Self> {
        Self::open_with_options(path, mode, None)
    }

    pub fn open_with_pulse_server(
        path: PathBuf,
        pulse_server_override: Option<String>,
    ) -> io::Result<Self> {
        Self::open_with_options(path, PlayerOutputMode::NativeYuv, pulse_server_override)
    }

    fn open_with_options(
        path: PathBuf,
        mode: PlayerOutputMode,
        pulse_server_override: Option<String>,
    ) -> io::Result<Self> {
        let ffmpeg_bin = locate_bundled_ffmpeg().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                "Bundled fpshud-ffmpeg not found — run scripts/install.sh to enable playback",
            )
        })?;

        let generation = Arc::new(AtomicU64::new(1));
        let state = Arc::new((
            Mutex::new(SharedPlayerState {
                snapshot: PlayerSnapshot {
                    playing: true,
                    ..PlayerSnapshot::default()
                },
                viewport: (960, 540),
                desired_seek_sec: 0.0,
                single_frame_requested: false,
                quit: false,
                active_video_child: None,
                active_audio_child: None,
            }),
            Condvar::new(),
        ));
        let latest = Arc::new(Mutex::new(LatestFrameSlot {
            frame: PlayerFrame::default(),
            dirty: false,
        }));
        let stopped = Arc::new(AtomicBool::new(false));

        let path_w = path.clone();
        let gen_w = Arc::clone(&generation);
        let state_w = Arc::clone(&state);
        let latest_w = Arc::clone(&latest);
        let stopped_w = Arc::clone(&stopped);

        let worker = thread::Builder::new()
            .name("fpshud_player".to_string())
            .spawn(move || {
                run_player_worker(
                    ffmpeg_bin,
                    path_w,
                    mode,
                    pulse_server_override,
                    gen_w,
                    state_w,
                    latest_w,
                    stopped_w,
                );
            })?;

        Ok(Self {
            path,
            mode,
            generation,
            state,
            latest,
            stopped,
            worker: Some(worker),
        })
    }

    /// Sends non-blocking SIGKILL to active children; the worker thread reaps `wait()` so the UI thread never blocks!
    fn signal_kill_active_child_locked(st: &mut SharedPlayerState) {
        if let Some(ref mut child) = st.active_video_child {
            let _ = child.kill();
        }
        if let Some(ref mut child) = st.active_audio_child {
            let _ = child.kill();
        }
    }

    fn kill_and_reap_active_child_locked(st: &mut SharedPlayerState) {
        if let Some(mut child) = st.active_video_child.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
        if let Some(mut child) = st.active_audio_child.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }

    pub fn set_viewport_size(&self, max_w: u32, max_h: u32) {
        let next = (max_w.max(64), max_h.max(64));
        let (lock, cvar) = &*self.state;
        if let Ok(mut st) = lock.lock() {
            if st.viewport == next {
                return;
            }
            st.viewport = next;
            // In NativeYuv mode, GPU compositor (wp_viewporter) scales the native frame:
            // never kill/restart fpshud-ffmpeg on window resize or fullscreen toggle!
            if self.mode == PlayerOutputMode::NativeYuv {
                return;
            }
            if !st.snapshot.playing {
                st.single_frame_requested = true;
            }
            self.generation.fetch_add(1, Ordering::AcqRel);
            Self::signal_kill_active_child_locked(&mut st);
            cvar.notify_all();
        }
    }

    pub fn play(&self) {
        let (lock, cvar) = &*self.state;
        if let Ok(mut st) = lock.lock() {
            if st.snapshot.at_eof {
                st.snapshot.at_eof = false;
                st.snapshot.current_sec = 0.0;
                st.desired_seek_sec = 0.0;
            } else {
                st.desired_seek_sec = st.snapshot.current_sec;
            }
            st.snapshot.playing = true;
            st.single_frame_requested = false;
            self.generation.fetch_add(1, Ordering::AcqRel);
            Self::signal_kill_active_child_locked(&mut st);
            cvar.notify_all();
        }
    }

    pub fn pause(&self) {
        let (lock, cvar) = &*self.state;
        if let Ok(mut st) = lock.lock() {
            st.snapshot.playing = false;
            st.desired_seek_sec = st.snapshot.current_sec;
            self.generation.fetch_add(1, Ordering::AcqRel);
            Self::signal_kill_active_child_locked(&mut st);
            cvar.notify_all();
        }
    }

    pub fn toggle_play_pause(&self) {
        let playing = self.snapshot().playing;
        if playing {
            self.pause();
        } else {
            self.play();
        }
    }

    pub fn seek(&self, target_sec: f64) {
        let (lock, cvar) = &*self.state;
        if let Ok(mut st) = lock.lock() {
            let max_dur = st.snapshot.duration_sec.max(0.0);
            let clamped = if max_dur > 0.0 {
                target_sec.clamp(0.0, max_dur)
            } else {
                target_sec.max(0.0)
            };
            st.desired_seek_sec = clamped;
            st.snapshot.current_sec = clamped;
            st.snapshot.at_eof = false;
            if !st.snapshot.playing {
                st.single_frame_requested = true;
            }
            self.generation.fetch_add(1, Ordering::AcqRel);
            Self::signal_kill_active_child_locked(&mut st);
            cvar.notify_all();
        }
    }

    pub fn seek_relative(&self, delta_sec: f64) {
        let cur = self.snapshot().current_sec;
        self.seek(cur + delta_sec);
    }

    pub fn set_muted(&self, muted: bool) {
        let (lock, cvar) = &*self.state;
        if let Ok(mut st) = lock.lock() {
            if st.snapshot.muted == muted {
                return;
            }
            st.snapshot.muted = muted;
            if st.snapshot.playing {
                st.desired_seek_sec = st.snapshot.current_sec;
                self.generation.fetch_add(1, Ordering::AcqRel);
                Self::signal_kill_active_child_locked(&mut st);
                cvar.notify_all();
            }
        }
    }

    pub fn toggle_mute(&self) {
        let muted = self.snapshot().muted;
        self.set_muted(!muted);
    }

    pub fn take_latest_frame(&self, dst: &mut PlayerFrame) -> bool {
        if let Ok(mut slot) = self.latest.lock()
            && slot.dirty
        {
            std::mem::swap(dst, &mut slot.frame);
            slot.dirty = false;
            return true;
        }
        false
    }

    #[must_use]
    pub fn snapshot(&self) -> PlayerSnapshot {
        let (lock, _) = &*self.state;
        lock.lock()
            .map(|g| g.snapshot.clone())
            .unwrap_or_default()
    }

    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for VideoPlayerSession {
    fn drop(&mut self) {
        self.stopped.store(true, Ordering::Release);
        self.generation.fetch_add(1, Ordering::AcqRel);
        let (lock, cvar) = &*self.state;
        if let Ok(mut st) = lock.lock() {
            st.quit = true;
            st.snapshot.playing = false;
            Self::kill_and_reap_active_child_locked(&mut st);
            cvar.notify_all();
        }
        if let Some(handle) = self.worker.take() {
            let _ = handle.join();
        }
        if let Ok(mut slot) = self.latest.lock() {
            slot.frame.data.clear();
            slot.frame.data.shrink_to_fit();
            slot.dirty = false;
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn run_player_worker(
    ffmpeg_bin: PathBuf,
    path: PathBuf,
    mode: PlayerOutputMode,
    pulse_server_override: Option<String>,
    generation: Arc<AtomicU64>,
    state: Arc<(Mutex<SharedPlayerState>, Condvar)>,
    latest: Arc<Mutex<LatestFrameSlot>>,
    stopped: Arc<AtomicBool>,
) {
    // 1. Non-blocking probe inside worker thread
    let probe_out = bundled_ffmpeg_command(&ffmpeg_bin)
        .args(["-hide_banner", "-i"])
        .arg(&path)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .output();

    let (src_w, src_h, duration_sec, src_fps, has_audio, color_space) = match probe_out {
        Ok(out) => {
            let stderr = String::from_utf8_lossy(&out.stderr);
            let meta = parse_ffmpeg_probe_output(&stderr);
            if meta.width == 0 || meta.height == 0 {
                let (lock, _) = &*state;
                if let Ok(mut st) = lock.lock() {
                    st.snapshot.error = Some("Unable to probe video stream".to_string());
                    st.snapshot.playing = false;
                }
                return;
            }
            (
                meta.width,
                meta.height,
                meta.duration_sec,
                if meta.fps > 0.5 { meta.fps } else { 30.0 },
                meta.audio_codec.is_some(),
                meta.color_space,
            )
        }
        Err(err) => {
            let (lock, _) = &*state;
            if let Ok(mut st) = lock.lock() {
                st.snapshot.error = Some(format!("Failed to probe video: {err}"));
                st.snapshot.playing = false;
            }
            return;
        }
    };

    let probed_hw = probe_hw_decoder(&ffmpeg_bin);
    let (lock, cvar) = &*state;
    if let Ok(mut st) = lock.lock() {
        st.snapshot.ready = true;
        st.snapshot.src_width = src_w;
        st.snapshot.src_height = src_h;
        st.snapshot.duration_sec = duration_sec;
        st.snapshot.has_audio = has_audio;
        st.snapshot.color_space = color_space;
        st.snapshot.hw_backend = probed_hw;
    }

    let mut audio_fallback_disabled = false;
    let mut clip_force_software = false;
    let mut read_buf: Vec<u8> = Vec::new();

    loop {
        if stopped.load(Ordering::Acquire) {
            break;
        }

        let (my_gen, playing, single_frame, start_sec, viewport, want_audio) = {
            let Ok(mut st) = lock.lock() else {
                break;
            };
            while !st.quit && !st.snapshot.playing && !st.single_frame_requested {
                st = match cvar.wait(st) {
                    Ok(g) => g,
                    Err(_) => return,
                };
            }
            if st.quit {
                break;
            }
            let single = st.single_frame_requested && !st.snapshot.playing;
            st.single_frame_requested = false;
            let gen_now = generation.load(Ordering::Acquire);
            (
                gen_now,
                st.snapshot.playing,
                single,
                st.desired_seek_sec,
                st.viewport,
                has_audio && !st.snapshot.muted && !audio_fallback_disabled,
            )
        };

        let playback_fps = src_fps.clamp(1.0, 60.0);
        let eff_start = if single_frame && duration_sec > 0.05 {
            start_sec.min(duration_sec - 0.05).max(0.0)
        } else {
            start_sec.max(0.0)
        };
        let active_backend = if clip_force_software {
            HwDecodeBackend::Software
        } else {
            probed_hw
        };
        // Always run video decoding without PulseAudio muxing in the same process:
        // mixing `-f pulse` with `rawvideo pipe:1` causes ~1000ms stalls in FFmpeg.
        let (args, out_w, out_h, pix_fmt) = build_player_video_ffmpeg_args(
            &path,
            eff_start,
            src_w,
            src_h,
            viewport,
            playback_fps,
            mode,
            active_backend,
            single_frame,
        );

        let mut cmd = bundled_ffmpeg_command(&ffmpeg_bin);
        let Ok(mut video_child) = cmd
            .args(&args)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
        else {
            break;
        };

        let Some(mut stdout) = video_child.stdout.take() else {
            let _ = video_child.kill();
            let _ = video_child.wait();
            break;
        };
        tune_pipe_capacity(stdout.as_raw_fd());

        // Spawn decoupled audio-only sidecar when live playback has audio enabled.
        let mut audio_child = if !single_frame && want_audio {
            let audio_args = build_player_audio_ffmpeg_args(&path, eff_start);
            let mut acmd = bundled_ffmpeg_command(&ffmpeg_bin);
            if let Some(ref ps) = pulse_server_override {
                acmd.env("PULSE_SERVER", ps);
            }
            acmd.args(&audio_args)
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .spawn()
                .ok()
        } else {
            None
        };

        {
            let Ok(mut st) = lock.lock() else {
                let _ = video_child.kill();
                let _ = video_child.wait();
                if let Some(mut ac) = audio_child.take() {
                    let _ = ac.kill();
                    let _ = ac.wait();
                }
                break;
            };
            if st.quit || generation.load(Ordering::Acquire) != my_gen {
                let _ = video_child.kill();
                let _ = video_child.wait();
                if let Some(mut ac) = audio_child.take() {
                    let _ = ac.kill();
                    let _ = ac.wait();
                }
                continue;
            }
            st.active_video_child = Some(video_child);
            st.active_audio_child = audio_child;
        }

        let frame_bytes = expected_frame_byte_len(out_w, out_h, pix_fmt);
        read_buf.resize(frame_bytes, 0);
        let mut frame_idx: u64 = 0;

        loop {
            if generation.load(Ordering::Acquire) != my_gen || stopped.load(Ordering::Acquire) {
                break;
            }
            if stdout.read_exact(&mut read_buf).is_err() {
                break;
            }
            if generation.load(Ordering::Acquire) != my_gen {
                break;
            }

            let pts_sec = start_sec + (frame_idx as f64 / f64::from(playback_fps));
            frame_idx += 1;

            if let Ok(mut slot) = latest.lock() {
                std::mem::swap(&mut slot.frame.data, &mut read_buf);
                slot.frame.width = out_w;
                slot.frame.height = out_h;
                slot.frame.pixel_format = pix_fmt;
                slot.frame.color_space = color_space;
                slot.frame.pts_sec = pts_sec;
                slot.frame.generation = my_gen;
                slot.dirty = true;
                read_buf.resize(frame_bytes, 0);
            }

            if !single_frame
                && let Ok(mut st) = lock.lock()
                && generation.load(Ordering::Acquire) == my_gen
            {
                st.snapshot.current_sec = if duration_sec > 0.0 {
                    pts_sec.min(duration_sec)
                } else {
                    pts_sec
                };
            }

            if single_frame {
                break;
            }
        }

        let (exited_ok, audio_failed_early) = {
            let Ok(mut st) = lock.lock() else {
                break;
            };
            let v_ok = if let Some(mut c) = st.active_video_child.take() {
                let _ = c.kill();
                c.wait().is_ok_and(|s| s.success())
            } else {
                false
            };
            let a_failed = if let Some(mut ac) = st.active_audio_child.take() {
                let already_failed = ac.try_wait().ok().flatten().is_some_and(|s| !s.success());
                let _ = ac.kill();
                let _ = ac.wait();
                already_failed
            } else {
                false
            };
            (v_ok, a_failed)
        };

        // Per-clip automatic fallback: if HW decoder produced 0 frames (unsupported codec/profile),
        // seamlessly retry this clip with Software yuv420p!
        if frame_idx == 0
            && active_backend != HwDecodeBackend::Software
            && generation.load(Ordering::Acquire) == my_gen
        {
            clip_force_software = true;
            if let Ok(mut st) = lock.lock() {
                st.snapshot.hw_backend = HwDecodeBackend::Software;
            }
            continue;
        }

        // If audio sidecar failed (e.g. headless CI without PulseAudio), disable audio for future seeks.
        if audio_failed_early && generation.load(Ordering::Acquire) == my_gen {
            audio_fallback_disabled = true;
            if let Ok(mut st) = lock.lock() {
                st.snapshot.has_audio = false;
            }
        }
        let _ = exited_ok;

        // Normal EOF handling
        if !single_frame
            && playing
            && frame_idx > 0
            && generation.load(Ordering::Acquire) == my_gen
            && let Ok(mut st) = lock.lock()
        {
            st.snapshot.playing = false;
            st.snapshot.at_eof = true;
            if duration_sec > 0.0 {
                st.snapshot.current_sec = duration_sec;
            }
        }
    }
}
