use crate::launcher::UiDaemonHandle;
use fpshud_analytics::{
    BenchmarkRecorder, BenchmarkSummary, FrametimeAnalyzer, frametime::GRAPH_HISTORY_SIZE,
};
use fpshud_core::config::{HudConfig, init_config_from_env, save_config_to_disk};
use fpshud_core::paths::{get_default_record_dir, get_secure_runtime_dir};
use fpshud_input::{InputAction, InputManager, SurfaceBackendType};
use fpshud_ipc::protocol::{
    FrameTickData, LayerToUiMsg, MAX_IPC_PAYLOAD, RecorderStatusSnapshot, ShmSlotMeta,
    UiToLayerMsg,
};
use fpshud_ipc::{IpcConnection, IpcServer, ShmRingReader};
use fpshud_recorder::pacer::{FramePacer, compute_target_dimensions};
use fpshud_recorder::session::{
    RecorderConfig, RecorderEvent, RecorderHandle, RecorderSession, SubmittedFrame,
};
use fpshud_screenshot::{PixelFormat, RawFrameCapture, ScreenshotWorker};
use std::io;
use std::os::fd::AsFd;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, OnceLock, mpsc};
use std::time::{Duration, Instant};

#[derive(Debug, Clone)]
pub struct OverlayFrameBuffer {
    pub dst_x: i32,
    pub dst_y: i32,
    pub width: u32,
    pub height: u32,
    pub generation: u64,
    pub rgba: Arc<[u8]>,
}

impl Default for OverlayFrameBuffer {
    fn default() -> Self {
        Self {
            dst_x: 0,
            dst_y: 0,
            width: 0,
            height: 0,
            generation: 0,
            rgba: Arc::from([]),
        }
    }
}

/// Validates monotonic generation and swapchain bounds, accepting compact bounding-box HUD frames
/// while discarding oversized pre-resize or stale frames.
#[derive(Debug, Default)]
pub struct OverlayFrameSync {
    expected_size: (u32, u32),
    last_accepted_generation: u64,
    scratch: Vec<u8>,
    pub latest: OverlayFrameBuffer,
}

impl OverlayFrameSync {
    pub fn reset_for_resize(&mut self, width: u32, height: u32) {
        let next = (width.max(1), height.max(1));
        if self.expected_size != next {
            self.expected_size = next;
            self.latest.rgba = Arc::from([]);
            self.latest.dst_x = 0;
            self.latest.dst_y = 0;
            self.latest.width = 0;
            self.latest.height = 0;
        }
    }

    #[must_use]
    pub fn expected_size(&self) -> (u32, u32) {
        self.expected_size
    }

    fn size_within_bounds(&self, width: u32, height: u32) -> bool {
        if width == 0 || height == 0 {
            return false;
        }
        self.expected_size == (0, 0)
            || (width <= self.expected_size.0 && height <= self.expected_size.1)
    }

    pub fn reset_generation(&mut self) {
        self.last_accepted_generation = 0;
    }

    pub fn accept_metadata_only(&mut self, meta: &ShmSlotMeta, pixels: &[u8]) -> bool {
        if !self.size_within_bounds(meta.width, meta.height) {
            return false;
        }
        if meta.generation <= self.last_accepted_generation {
            return false;
        }
        let expected_bytes = (meta.width as usize)
            .saturating_mul(meta.height as usize)
            .saturating_mul(4);
        if expected_bytes == 0 || pixels.len() < expected_bytes {
            return false;
        }
        self.last_accepted_generation = meta.generation;
        self.latest.dst_x = meta.dst_x;
        self.latest.dst_y = meta.dst_y;
        self.latest.width = meta.width;
        self.latest.height = meta.height;
        self.latest.generation = meta.generation;
        self.latest.rgba = Arc::from(&pixels[..expected_bytes]);
        true
    }

    pub fn try_consume_shm_slot(&mut self, meta: &ShmSlotMeta, reader: &ShmRingReader) -> bool {
        if !self.size_within_bounds(meta.width, meta.height) {
            return false;
        }
        if meta.generation <= self.last_accepted_generation {
            return false;
        }
        let expected_bytes = (meta.width as usize)
            .saturating_mul(meta.height as usize)
            .saturating_mul(4);
        if expected_bytes == 0 {
            return false;
        }
        if let Some((r_gen, _slot)) = reader.read_latest_into(&mut self.scratch) {
            if r_gen <= self.last_accepted_generation || self.scratch.len() != expected_bytes {
                return false;
            }
            self.last_accepted_generation = r_gen.max(meta.generation);
            self.latest.dst_x = meta.dst_x;
            self.latest.dst_y = meta.dst_y;
            self.latest.width = meta.width;
            self.latest.height = meta.height;
            self.latest.generation = self.last_accepted_generation;
            self.latest.rgba = Arc::from(self.scratch.as_slice());
            return true;
        }
        false
    }
}

#[derive(Debug, Clone, Copy)]
pub struct RecorderCapturePlan {
    pub enc_w: u32,
    pub enc_h: u32,
    pub with_hud: bool,
    pub pts_ns: u64,
}

pub struct LayerRuntime {
    pub config: HudConfig,
    pub input: InputManager,
    pub screenshot_worker: ScreenshotWorker,
    pub overlay_sync: OverlayFrameSync,
    frametime: FrametimeAnalyzer,
    last_frame_at: Option<Instant>,
    benchmark: BenchmarkRecorder,
    bench_started_at: Option<Instant>,
    last_benchmark: Option<BenchmarkSummary>,
    benchmark_dir: PathBuf,
    bench_loads: (f32, f32),
    active_gpu: Option<(u32, u32)>,
    active_gpu_sent: bool,
    pending_screenshot: Option<bool>,
    active_recorder: Option<RecorderHandle>,
    stopping_recorders: Vec<RecorderHandle>,
    recorder_pacer: FramePacer,
    recorder_events_rx: Option<mpsc::Receiver<RecorderEvent>>,
    last_recorder_status_push: Instant,
    recorder_epoch: Instant,
    active_toast_until: Option<Instant>,
    smoke_rec_sec: Option<f32>,
    smoke_rec_started: bool,
    smoke_rec_started_at: Option<Instant>,
    test_recorder_active_override: bool,
    test_mode: bool,
    ipc_server: Option<IpcServer>,
    ipc_conn: Option<IpcConnection>,
    shm_reader: Option<ShmRingReader>,
    _daemon: Option<UiDaemonHandle>,
    init_sent: bool,
    last_sent_size: (u32, u32),
    last_software_cursor: bool,
    last_telemetry_push: Instant,
    next_input_poll: Instant,
    gui_reopen_debounce_until: Option<Instant>,
    socket_path: Option<PathBuf>,
    ipc_recv_buf: Vec<u8>,
    last_render_api: String,
}

const GUI_REOPEN_DEBOUNCE: Duration = Duration::from_millis(350);

static GLOBAL_RUNTIME: OnceLock<Mutex<LayerRuntime>> = OnceLock::new();

impl LayerRuntime {
    pub fn global() -> &'static Mutex<Self> {
        GLOBAL_RUNTIME.get_or_init(|| {
            let mut config = init_config_from_env(std::env::var("FPS_HUD").ok().as_deref());
            if std::env::var("DISABLE_FPS_HUD").as_deref() == Ok("1") {
                config.enabled = false;
            }
            let screenshot_dir = config.resolved_screenshot_dir();
            let save_dir = if config.save_folder.trim().is_empty() {
                get_default_record_dir()
            } else {
                PathBuf::from(&config.save_folder)
            };
            Mutex::new(Self {
                config,
                input: InputManager::new(),
                screenshot_worker: ScreenshotWorker::new(screenshot_dir),
                overlay_sync: OverlayFrameSync::default(),
                frametime: FrametimeAnalyzer::new(),
                last_frame_at: None,
                benchmark: BenchmarkRecorder::default(),
                bench_started_at: None,
                last_benchmark: None,
                benchmark_dir: save_dir,
                bench_loads: (0.0, 0.0),
                active_gpu: None,
                active_gpu_sent: false,
                pending_screenshot: None,
                active_recorder: None,
                stopping_recorders: Vec::new(),
                recorder_pacer: FramePacer::new(60),
                recorder_events_rx: None,
                last_recorder_status_push: Instant::now() - Duration::from_secs(1),
                recorder_epoch: Instant::now(),
                active_toast_until: None,
                smoke_rec_sec: std::env::var("FPS_HUD_REC_SMOKE_SEC")
                    .ok()
                    .and_then(|v| v.trim().parse::<f32>().ok())
                    .filter(|v| v.is_finite() && *v > 0.0),
                smoke_rec_started: false,
                smoke_rec_started_at: None,
                test_recorder_active_override: false,
                test_mode: false,
                ipc_server: None,
                ipc_conn: None,
                shm_reader: None,
                _daemon: None,
                init_sent: false,
                last_sent_size: (0, 0),
                last_software_cursor: false,
                last_telemetry_push: Instant::now() - Duration::from_secs(1),
                next_input_poll: Instant::now(),
                gui_reopen_debounce_until: None,
                socket_path: None,
                ipc_recv_buf: vec![0_u8; MAX_IPC_PAYLOAD + 64],
                last_render_api: String::new(),
            })
        })
    }

    #[must_use]
    pub fn new_for_test() -> Self {
        let config = HudConfig {
            enabled: true,
            ..HudConfig::default()
        };
        let bench_dir = std::env::temp_dir().join("fpshud_test_bench");
        Self {
            config,
            input: InputManager::new_headless(),
            screenshot_worker: ScreenshotWorker::new(std::env::temp_dir().join("fpshud_test_shots")),
            overlay_sync: OverlayFrameSync::default(),
            frametime: FrametimeAnalyzer::new(),
            last_frame_at: None,
            benchmark: BenchmarkRecorder::default(),
            bench_started_at: None,
            last_benchmark: None,
            benchmark_dir: bench_dir,
            bench_loads: (0.0, 0.0),
            active_gpu: None,
            active_gpu_sent: false,
            pending_screenshot: None,
            active_recorder: None,
            stopping_recorders: Vec::new(),
            recorder_pacer: FramePacer::new(60),
            recorder_events_rx: None,
            last_recorder_status_push: Instant::now(),
            recorder_epoch: Instant::now(),
            active_toast_until: None,
            smoke_rec_sec: None,
            smoke_rec_started: false,
            smoke_rec_started_at: None,
            test_recorder_active_override: false,
            test_mode: true,
            ipc_server: None,
            ipc_conn: None,
            shm_reader: None,
            _daemon: None,
            init_sent: false,
            last_sent_size: (0, 0),
            last_software_cursor: false,
            last_telemetry_push: Instant::now(),
            next_input_poll: Instant::now(),
            gui_reopen_debounce_until: None,
            socket_path: None,
            ipc_recv_buf: vec![0_u8; MAX_IPC_PAYLOAD + 64],
            last_render_api: String::new(),
        }
    }

    pub fn notify_active_gpu(&mut self, vendor_id: u32, device_id: u32) {
        self.active_gpu = Some((vendor_id, device_id));
        self.active_gpu_sent = false;
    }

    fn toggle_benchmark(&mut self) {
        if self.benchmark.is_active() {
            let summary = self.benchmark.stop();
            self.bench_started_at = None;
            if summary.is_some() {
                self.last_benchmark = summary;
            }
        } else {
            self.benchmark.start(&self.benchmark_dir);
            self.bench_started_at = Some(Instant::now());
        }
    }

    #[must_use]
    pub fn is_gui_reopen_debounced(&self) -> bool {
        self.gui_reopen_debounce_until
            .is_some_and(|until| Instant::now() < until)
    }

    pub fn clear_gui_reopen_debounce_for_test(&mut self) {
        self.gui_reopen_debounce_until = None;
    }

    fn arm_gui_reopen_debounce(&mut self) {
        self.gui_reopen_debounce_until = Some(Instant::now() + GUI_REOPEN_DEBOUNCE);
    }

    fn suppress_debounced_gui_reopen(&mut self, prev_gui_open: bool, actions: &[InputAction]) {
        if !prev_gui_open
            && self.config.gui_open
            && actions.contains(&InputAction::ToggleGui)
            && self.is_gui_reopen_debounced()
        {
            self.config.gui_open = false;
        }
    }

    pub fn notify_surface_swapchain(&mut self, backend: SurfaceBackendType, window_id: u32) {
        match backend {
            SurfaceBackendType::X11 => self.input.notify_x11_swapchain(window_id),
            SurfaceBackendType::Wayland => self.input.notify_wayland_swapchain(),
            SurfaceBackendType::Unknown => {}
        }
    }

    pub fn handle_side_effect_actions(&mut self, actions: &[InputAction]) {
        for &action in actions {
            match action {
                InputAction::ToggleBenchmark => self.toggle_benchmark(),
                InputAction::TakeScreenshot => {
                    self.pending_screenshot = Some(self.config.screenshot_with_hud);
                }
                InputAction::ToggleRecord => self.toggle_recorder(),
                InputAction::ToggleGui | InputAction::ToggleHud => {}
            }
        }
    }

    pub fn apply_input_actions(&mut self, actions: &[InputAction]) -> bool {
        let mut config_changed = false;
        for &action in actions {
            match action {
                InputAction::ToggleGui => {
                    self.config.gui_open = !self.config.gui_open;
                    config_changed = true;
                }
                InputAction::ToggleHud => {
                    self.config.hud_visible = !self.config.hud_visible;
                    if !self.config.hud_visible
                        && !self.is_recorder_active()
                        && !self.has_active_toast()
                    {
                        self.overlay_sync.latest.rgba = std::sync::Arc::from([]);
                    }
                    config_changed = true;
                }
                InputAction::ToggleBenchmark => self.toggle_benchmark(),
                InputAction::TakeScreenshot => {
                    self.pending_screenshot = Some(self.config.screenshot_with_hud);
                }
                InputAction::ToggleRecord => self.toggle_recorder(),
            }
        }
        if config_changed {
            let _ = self.send_to_ui(&LayerToUiMsg::ConfigSync(self.config.clone()));
        }
        config_changed
    }

    #[must_use]
    pub fn is_recorder_active(&self) -> bool {
        self.test_recorder_active_override || self.active_recorder.is_some()
    }

    pub fn set_recorder_active_for_test(&mut self, active: bool) {
        self.test_recorder_active_override = active;
    }

    #[must_use]
    pub fn has_active_toast(&self) -> bool {
        self.active_toast_until
            .is_some_and(|until| Instant::now() < until)
    }

    #[must_use]
    pub fn should_blit_overlay(&self) -> bool {
        self.config.hud_visible
            || self.is_recorder_active()
            || self.has_active_toast()
            || (self.config.gui_open && !self.input.is_external_overlay_active())
    }

    fn emit_toast(&mut self, msg: String) {
        self.active_toast_until = Some(Instant::now() + Duration::from_secs(4));
        let _ = self.send_to_ui(&LayerToUiMsg::ToastNotification(msg));
    }

    fn build_recorder_status(&self) -> RecorderStatusSnapshot {
        if let Some(ref handle) = self.active_recorder {
            RecorderStatusSnapshot {
                is_recording: true,
                elapsed_sec: handle.elapsed_sec(),
                dropped_frames: handle.dropped_frames(),
                active_backend: handle.active_backend().to_string(),
                active_output_path: handle
                    .active_output_path()
                    .map(|p| p.to_string_lossy().into_owned()),
            }
        } else {
            RecorderStatusSnapshot::default()
        }
    }

    pub fn toggle_recorder(&mut self) {
        if let Some(handle) = self.active_recorder.take() {
            handle.request_stop();
            self.stopping_recorders.push(handle);
            self.emit_toast("Stopping recording...".to_string());
            let snap = self.build_recorder_status();
            let _ = self.send_to_ui(&LayerToUiMsg::RecorderStatusUpdate(snap));
            return;
        }

        self.reap_finished_recorders();
        if !self.stopping_recorders.is_empty() {
            self.emit_toast("Finalizing previous recording...".to_string());
            return;
        }

        let (ev_tx, ev_rx) = mpsc::channel::<RecorderEvent>();
        let cfg = RecorderConfig {
            output_dir: self.config.resolved_record_dir(),
            resolution: self.config.record_resolution,
            fps: self.config.record_fps,
            codec: self.config.record_codec,
            bitrate: self.config.record_bitrate,
            audio_mode: self.config.record_audio_mode,
            target_gpu: self.active_gpu,
        };

        match RecorderSession::start(cfg, move |ev| {
            let _ = ev_tx.send(ev);
        }) {
            Ok(handle) => {
                self.recorder_pacer.reset(self.config.record_fps.as_u32());
                self.recorder_epoch = Instant::now();
                let backend = handle.active_backend().to_string();
                self.active_recorder = Some(handle);
                self.recorder_events_rx = Some(ev_rx);
                self.emit_toast(format!("Recording started ({backend})"));
                let snap = self.build_recorder_status();
                let _ = self.send_to_ui(&LayerToUiMsg::RecorderStatusUpdate(snap));
            }
            Err(err) => {
                self.emit_toast(format!("Recorder error: {err}"));
            }
        }
    }

    fn reap_finished_recorders(&mut self) {
        self.stopping_recorders.retain_mut(|h| {
            if h.is_finished() {
                let _ = h.wait_finished();
                false
            } else {
                true
            }
        });
    }

    pub fn flush_and_stop_recorders(&mut self) {
        if let Some(mut handle) = self.active_recorder.take() {
            let _ = handle.wait_finished();
        }
        for mut handle in self.stopping_recorders.drain(..) {
            let _ = handle.wait_finished();
        }
        self.drain_recorder_events();
    }

    fn drain_recorder_events(&mut self) {
        let mut drained = Vec::new();
        if let Some(ref rx) = self.recorder_events_rx {
            while let Ok(ev) = rx.try_recv() {
                drained.push(ev);
            }
        }
        for ev in drained {
            match ev {
                RecorderEvent::Started { .. } => {
                    let snap = self.build_recorder_status();
                    let _ = self.send_to_ui(&LayerToUiMsg::RecorderStatusUpdate(snap));
                }
                RecorderEvent::SegmentSplit { new_output_path } => {
                    let name = new_output_path
                        .file_name()
                        .map(|n| n.to_string_lossy().into_owned())
                        .unwrap_or_default();
                    self.emit_toast(format!("Recording split: {name}"));
                    let snap = self.build_recorder_status();
                    let _ = self.send_to_ui(&LayerToUiMsg::RecorderStatusUpdate(snap));
                }
                RecorderEvent::Warning(w) => self.emit_toast(w),
                RecorderEvent::Error(e) => {
                    self.emit_toast(format!("Recorder error: {e}"));
                    if let Some(h) = self.active_recorder.take() {
                        h.request_stop();
                        self.stopping_recorders.push(h);
                    }
                    let snap = self.build_recorder_status();
                    let _ = self.send_to_ui(&LayerToUiMsg::RecorderStatusUpdate(snap));
                }
                RecorderEvent::Stopped { output_paths } => {
                    if let Some(first) = output_paths.first()
                        && let Some(name) = first.file_name()
                    {
                        self.emit_toast(format!("Recording saved: {}", name.to_string_lossy()));
                    }
                    let snap = self.build_recorder_status();
                    let _ = self.send_to_ui(&LayerToUiMsg::RecorderStatusUpdate(snap));
                }
            }
        }
    }

    #[must_use]
    pub fn recorder_capture_plan(&mut self, src_w: u32, src_h: u32) -> Option<RecorderCapturePlan> {
        if self.active_recorder.is_none() || src_w == 0 || src_h == 0 {
            return None;
        }
        let now_ns = self.recorder_epoch.elapsed().as_nanos() as u64;
        if !self.recorder_pacer.should_capture(now_ns) {
            return None;
        }
        let (enc_w, enc_h) =
            compute_target_dimensions(src_w, src_h, self.config.record_resolution);
        Some(RecorderCapturePlan {
            enc_w,
            enc_h,
            with_hud: self.config.record_with_hud,
            pts_ns: now_ns,
        })
    }

    pub fn submit_recorder_frame(&self, frame: SubmittedFrame) {
        if let Some(ref handle) = self.active_recorder {
            let _ = handle.try_submit_frame(frame);
        }
    }

    #[must_use]
    pub fn consume_screenshot_request(&mut self) -> Option<bool> {
        self.pending_screenshot.take()
    }

    pub fn submit_captured_screenshot(
        &self,
        width: u32,
        height: u32,
        format: PixelFormat,
        pixels: Vec<u8>,
        flip_y: bool,
    ) {
        self.screenshot_worker.submit_to(
            RawFrameCapture {
                width,
                height,
                format,
                flip_y,
                pixels,
            },
            Some(self.config.resolved_screenshot_dir()),
            self.config.screenshot_quality,
        );
    }

    fn send_to_ui(&mut self, msg: &LayerToUiMsg) -> bool {
        let Some(conn) = &mut self.ipc_conn else {
            return false;
        };
        let Ok(bytes) = msg.encode() else {
            return false;
        };
        if conn.send_msg(&bytes, &[]).is_err() {
            self.ipc_conn = None;
            self.init_sent = false;
            self.active_gpu_sent = false;
            self.input.set_external_overlay_active(false);
            return false;
        }
        true
    }

    fn ensure_ipc_started(&mut self) {
        if self.test_mode || self.ipc_server.is_some() {
            return;
        }
        let Ok(runtime_dir) = get_secure_runtime_dir() else {
            return;
        };
        let sock_path = runtime_dir.join(format!("fpshud-{}.sock", std::process::id()));
        let Ok(server) = IpcServer::bind(&sock_path) else {
            return;
        };
        if server.set_nonblocking(true).is_err() {
            return;
        }
        self.socket_path = Some(sock_path.clone());
        self.ipc_server = Some(server);
        if let Ok(handle) = UiDaemonHandle::spawn(&sock_path) {
            self._daemon = Some(handle);
        }
    }

    fn drain_ui_messages(&mut self) {
        if self.ipc_conn.is_none() {
            return;
        }
        loop {
            let recv_res = {
                let Some(conn) = &mut self.ipc_conn else {
                    break;
                };
                conn.recv_msg(&mut self.ipc_recv_buf)
            };
            match recv_res {
                Ok((0, _)) => {
                    self.ipc_conn = None;
                    self.init_sent = false;
                    self.active_gpu_sent = false;
                    self.input.set_external_overlay_active(false);
                    break;
                }
                Ok((n, fds)) => {
                    for fd in fds {
                        if let Ok(reader) = ShmRingReader::from_borrowed_fd(fd.as_fd(), 0) {
                            self.shm_reader = Some(reader);
                            self.overlay_sync.reset_generation();
                        }
                    }
                    if let Ok(msg) = UiToLayerMsg::decode(&self.ipc_recv_buf[..n]) {
                        self.handle_ui_msg(msg);
                    }
                }
                Err(err) if err.kind() == io::ErrorKind::WouldBlock => break,
                Err(_) => {
                    self.ipc_conn = None;
                    self.init_sent = false;
                    self.active_gpu_sent = false;
                    self.input.set_external_overlay_active(false);
                    break;
                }
            }
        }
    }

    fn handle_ui_msg(&mut self, msg: UiToLayerMsg) {
        match msg {
            UiToLayerMsg::Ready => {}
            UiToLayerMsg::FrameReady(meta) => {
                if let Some(reader) = &self.shm_reader {
                    let _ = self.overlay_sync.try_consume_shm_slot(&meta, reader);
                }
            }
            UiToLayerMsg::ConfigUpdated(cfg) => {
                if self.config.gui_open && !cfg.gui_open {
                    self.arm_gui_reopen_debounce();
                }
                let keep_enabled = self.config.enabled;
                self.config = cfg;
                self.config.enabled = keep_enabled;
                let _ = save_config_to_disk(&self.config);
            }
            UiToLayerMsg::BenchmarkToggle => self.toggle_benchmark(),
            UiToLayerMsg::BenchmarkLoads { cpu_load, gpu_load } => {
                self.bench_loads = (cpu_load, gpu_load);
            }
            UiToLayerMsg::ScreenshotRequest { with_hud } => {
                self.pending_screenshot = Some(with_hud);
            }
            UiToLayerMsg::ToggleRecord => self.toggle_recorder(),
            UiToLayerMsg::OpenSaveFolder => {
                let dir = self.config.resolved_screenshot_dir();
                let _ = std::fs::create_dir_all(&dir);
                let _ = std::process::Command::new("xdg-open")
                    .arg(&dir)
                    .env_remove("LD_PRELOAD")
                    .env("DISABLE_FPS_HUD", "1")
                    .stdin(std::process::Stdio::null())
                    .stdout(std::process::Stdio::null())
                    .stderr(std::process::Stdio::null())
                    .spawn();
            }
            UiToLayerMsg::KeybindCaptureState { capturing, target } => {
                if capturing {
                    self.input.start_keybind_capture(target);
                } else {
                    self.input.cancel_keybind_capture();
                }
            }
            UiToLayerMsg::ExternalOverlayActive(active) => {
                self.input.set_external_overlay_active(active);
                if !active {
                    self.arm_gui_reopen_debounce();
                }
            }
        }
    }

    fn record_frame_tick(&mut self, now: Instant) {
        let dt_sec = self
            .last_frame_at
            .map_or(1.0 / 60.0, |prev| now.duration_since(prev).as_secs_f32())
            .clamp(0.000_05, 0.25);
        self.last_frame_at = Some(now);
        self.frametime.record_frame(dt_sec);
        if self.benchmark.is_active() {
            let fps_inst = if dt_sec > 0.0 { 1.0 / dt_sec } else { 0.0 };
            self.benchmark.push_sample(
                fps_inst,
                dt_sec * 1000.0,
                self.bench_loads.0,
                self.bench_loads.1,
            );
        }
    }

    fn build_frame_tick(&self) -> FrameTickData {
        let metrics = self.frametime.compute_metrics();
        FrameTickData {
            fps: metrics.fps,
            fps_avg: metrics.fps_avg,
            fps_1pct_low: metrics.fps_1pct_low,
            fps_01pct_low: metrics.fps_01pct_low,
            frametime_ms: metrics.frametime_ms,
            frametime_history: self
                .frametime
                .history_slice()
                .iter()
                .copied()
                .take(GRAPH_HISTORY_SIZE)
                .collect(),
            render_api: self.last_render_api.clone(),
            benchmark_active: self.benchmark.is_active(),
            benchmark_elapsed_sec: self
                .bench_started_at
                .map_or(0.0, |t| t.elapsed().as_secs_f32()),
            last_benchmark: self.last_benchmark.clone(),
        }
    }

    pub fn pump_frame(&mut self, width: u32, height: u32, render_api: &str) {
        if !self.config.enabled {
            return;
        }
        let w = width.max(1);
        let h = height.max(1);
        let now = Instant::now();
        if self.last_render_api != render_api {
            self.last_render_api.clear();
            self.last_render_api.push_str(render_api);
        }
        self.record_frame_tick(now);

        self.ensure_ipc_started();

        if self.ipc_conn.is_none()
            && let Some(server) = &self.ipc_server
            && let Ok(conn) = server.accept_verified()
            && conn.set_nonblocking(true).is_ok()
        {
            self.ipc_conn = Some(conn);
            self.init_sent = false;
            self.active_gpu_sent = false;
            self.last_software_cursor = false;
        }

        for saved_name in self.screenshot_worker.drain_saved_notifications() {
            self.send_to_ui(&LayerToUiMsg::ToastNotification(format!(
                "Screenshot saved: {saved_name}"
            )));
        }

        self.overlay_sync.reset_for_resize(w, h);

        if self.ipc_conn.is_some() {
            if !self.init_sent {
                let init_msg = LayerToUiMsg::Init {
                    width: w,
                    height: h,
                    scale: self.config.gui_scale,
                    config: self.config.clone(),
                };
                if self.send_to_ui(&init_msg) {
                    self.init_sent = true;
                    self.last_sent_size = (w, h);
                }
            } else if self.last_sent_size != (w, h) {
                let resize_msg = LayerToUiMsg::Resize {
                    width: w,
                    height: h,
                    scale: self.config.gui_scale,
                };
                if self.send_to_ui(&resize_msg) {
                    self.last_sent_size = (w, h);
                }
            }

            if self.init_sent
                && !self.active_gpu_sent
                && let Some((vendor_id, device_id)) = self.active_gpu
                && self.send_to_ui(&LayerToUiMsg::ActiveGpu {
                    vendor_id,
                    device_id,
                })
            {
                self.active_gpu_sent = true;
            }
        }

        if now >= self.next_input_poll {
            let poll_interval_ms = if self.config.gui_open { 8 } else { 32 };
            self.next_input_poll = now + Duration::from_millis(poll_interval_ms);
            let prev_config = self.config.clone();
            let (actions, ui_events, _grabbed) =
                self.input.poll_frame(&mut self.config, w as f32, h as f32);
            self.suppress_debounced_gui_reopen(prev_config.gui_open, &actions);
            self.handle_side_effect_actions(&actions);
            if self.config != prev_config {
                let _ = self.send_to_ui(&LayerToUiMsg::ConfigSync(self.config.clone()));
            }
            if !ui_events.is_empty() {
                let _ = self.push_ui_events(&ui_events);
            }
            let sw_cursor = self.input.should_draw_software_cursor(&self.config);
            if self.init_sent
                && sw_cursor != self.last_software_cursor
                && self.send_to_ui(&LayerToUiMsg::SoftwareCursor(sw_cursor))
            {
                self.last_software_cursor = sw_cursor;
            }
        }

        self.drain_ui_messages();
        self.reap_finished_recorders();
        self.drain_recorder_events();

        // Automated live recording smoke test hook (`FPS_HUD_REC_SMOKE_SEC=<N>`)
        if let Some(smoke_sec) = self.smoke_rec_sec {
            if !self.smoke_rec_started {
                self.smoke_rec_started = true;
                self.smoke_rec_started_at = Some(now);
                self.toggle_recorder();
            } else if let Some(started_at) = self.smoke_rec_started_at
                && started_at.elapsed().as_secs_f32() >= smoke_sec
            {
                self.smoke_rec_sec = None;
                self.flush_and_stop_recorders();
            }
        }

        if self.is_recorder_active()
            && now.duration_since(self.last_recorder_status_push) >= Duration::from_millis(400)
        {
            self.last_recorder_status_push = now;
            let snap = self.build_recorder_status();
            let _ = self.send_to_ui(&LayerToUiMsg::RecorderStatusUpdate(snap));
        }

        let telemetry_interval = if self.config.hud_graph_enabled {
            Duration::from_millis(333)
        } else {
            Duration::from_millis(500)
        };
        if (self.config.hud_visible || self.config.gui_open)
            && now.duration_since(self.last_telemetry_push) >= telemetry_interval
        {
            self.last_telemetry_push = now;
            let tick = self.build_frame_tick();
            let _ = self.send_to_ui(&LayerToUiMsg::FrameTick(Box::new(tick)));
        }
    }

    pub fn push_ui_events(&mut self, ui_events: &[fpshud_input::UiInputEvent]) -> usize {
        let mut sent = 0;
        for chunk in ui_events.chunks(fpshud_ipc::protocol::MAX_INPUT_EVENTS) {
            if self.send_to_ui(&LayerToUiMsg::InputEvents(chunk.to_vec())) {
                sent += chunk.len();
            }
        }
        sent
    }

    pub fn attach_ipc_for_test(&mut self, conn: IpcConnection) {
        self.ipc_conn = Some(conn);
        self.init_sent = false;
    }

    pub fn test_key_event_triggers_config_sync_once(&mut self, key_code: u32) -> bool {
        let prev = self.config.clone();
        let actions = self.input.handle_key_event(key_code, true, &mut self.config);
        let _ = self.input.handle_key_event(key_code, false, &mut self.config);
        self.suppress_debounced_gui_reopen(prev.gui_open, &actions);
        self.handle_side_effect_actions(&actions);
        if self.config != prev {
            return self.send_to_ui(&LayerToUiMsg::ConfigSync(self.config.clone()));
        }
        false
    }

    pub fn test_actions_and_generation_resize_guard(&mut self) -> bool {
        let initial_gui = self.config.gui_open;
        let initial_hud = self.config.hud_visible;
        self.apply_input_actions(&[
            InputAction::ToggleGui,
            InputAction::ToggleHud,
            InputAction::ToggleBenchmark,
            InputAction::TakeScreenshot,
        ]);
        if self.config.gui_open == initial_gui || self.config.hud_visible == initial_hud {
            return false;
        }
        if !self.benchmark.is_active() {
            return false;
        }
        if self.consume_screenshot_request().is_none() {
            return false;
        }

        // Resize to 1280x720 -> oversized 1920x1080 generation 1 MUST be rejected, valid <=1280x720 accepted
        self.overlay_sync.reset_for_resize(1280, 720);
        let oversized_pixels = vec![0xAA_u8; 1920 * 1080 * 4];
        let oversized_meta = ShmSlotMeta {
            modifier: 0,
            generation: 1,
            slot_index: 0,
            dst_x: 0,
            dst_y: 0,
            width: 1920,
            height: 1080,
            stride: 1920 * 4,
            fourcc: 0,
            byte_len: oversized_pixels.len() as u32,
            backend: fpshud_ipc::FrameBackendKind::MemfdRgba,
        };
        if self
            .overlay_sync
            .accept_metadata_only(&oversized_meta, &oversized_pixels)
        {
            return false;
        }

        let valid_pixels = vec![0xBB_u8; 1280 * 720 * 4];
        let valid_meta = ShmSlotMeta {
            modifier: 0,
            generation: 2,
            slot_index: 0,
            dst_x: 0,
            dst_y: 0,
            width: 1280,
            height: 720,
            stride: 1280 * 4,
            fourcc: 0,
            byte_len: valid_pixels.len() as u32,
            backend: fpshud_ipc::FrameBackendKind::MemfdRgba,
        };
        if !self
            .overlay_sync
            .accept_metadata_only(&valid_meta, &valid_pixels)
        {
            return false;
        }

        // Duplicate/older generation 2 must also be rejected!
        if self
            .overlay_sync
            .accept_metadata_only(&valid_meta, &valid_pixels)
        {
            return false;
        }

        true
    }
}
