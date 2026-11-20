use crate::launcher::UiDaemonHandle;
use fpshud_core::config::{HudConfig, init_config_from_env, save_config_to_disk};
use fpshud_core::paths::{get_default_record_dir, get_secure_runtime_dir};
use fpshud_input::{InputAction, InputManager, SurfaceBackendType};
use fpshud_ipc::protocol::{LayerToUiMsg, MAX_IPC_PAYLOAD, ShmSlotMeta, UiToLayerMsg};
use fpshud_ipc::{IpcConnection, IpcServer, ShmRingReader};
use fpshud_screenshot::{PixelFormat, RawFrameCapture, ScreenshotWorker};
use fpshud_telemetry::TelemetryCollector;
use std::io;
use std::os::fd::AsFd;
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Default)]
pub struct OverlayFrameBuffer {
    pub width: u32,
    pub height: u32,
    pub generation: u64,
    pub rgba: Vec<u8>,
}

/// Validates monotonic generation and exact surface size, discarding stale pre-resize frames.
#[derive(Debug, Default)]
pub struct OverlayFrameSync {
    expected_size: (u32, u32),
    last_accepted_generation: u64,
    pub latest: OverlayFrameBuffer,
}

impl OverlayFrameSync {
    pub fn reset_for_resize(&mut self, width: u32, height: u32) {
        let next = (width.max(1), height.max(1));
        if self.expected_size != next {
            self.expected_size = next;
            self.latest.rgba.clear();
            self.latest.width = 0;
            self.latest.height = 0;
        }
    }

    #[must_use]
    pub fn expected_size(&self) -> (u32, u32) {
        self.expected_size
    }

    pub fn accept_metadata_only(&mut self, meta: &ShmSlotMeta, pixels: &[u8]) -> bool {
        if meta.width == 0 || meta.height == 0 {
            return false;
        }
        if self.expected_size != (0, 0) && (meta.width, meta.height) != self.expected_size {
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
        self.latest.width = meta.width;
        self.latest.height = meta.height;
        self.latest.generation = meta.generation;
        self.latest.rgba.clear();
        self.latest.rgba.extend_from_slice(&pixels[..expected_bytes]);
        true
    }

    pub fn try_consume_shm_slot(&mut self, meta: &ShmSlotMeta, reader: &ShmRingReader) -> bool {
        if meta.width == 0 || meta.height == 0 {
            return false;
        }
        if self.expected_size != (0, 0) && (meta.width, meta.height) != self.expected_size {
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
        if let Some((r_gen, _slot, slice)) = reader.read_latest() {
            if r_gen <= self.last_accepted_generation || slice.len() != expected_bytes {
                return false;
            }
            self.last_accepted_generation = r_gen.max(meta.generation);
            self.latest.width = meta.width;
            self.latest.height = meta.height;
            self.latest.generation = self.last_accepted_generation;
            self.latest.rgba.clear();
            self.latest.rgba.extend_from_slice(slice);
            return true;
        }
        false
    }
}

pub struct LayerRuntime {
    pub config: HudConfig,
    pub telemetry: TelemetryCollector,
    pub input: InputManager,
    pub screenshot_worker: ScreenshotWorker,
    pub overlay_sync: OverlayFrameSync,
    pending_screenshot: Option<bool>,
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
    socket_path: Option<PathBuf>,
}

static GLOBAL_RUNTIME: OnceLock<Mutex<LayerRuntime>> = OnceLock::new();

impl LayerRuntime {
    pub fn global() -> &'static Mutex<Self> {
        GLOBAL_RUNTIME.get_or_init(|| {
            let mut config = init_config_from_env(std::env::var("FPS_HUD").ok().as_deref());
            if std::env::var("DISABLE_FPS_HUD").as_deref() == Ok("1") {
                config.enabled = false;
            }
            let save_dir = if config.save_folder.trim().is_empty() {
                get_default_record_dir()
            } else {
                PathBuf::from(&config.save_folder)
            };
            Mutex::new(Self {
                config,
                telemetry: TelemetryCollector::new(),
                input: InputManager::new(),
                screenshot_worker: ScreenshotWorker::new(save_dir),
                overlay_sync: OverlayFrameSync::default(),
                pending_screenshot: None,
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
                socket_path: None,
            })
        })
    }

    #[must_use]
    pub fn new_for_test() -> Self {
        let config = HudConfig {
            enabled: true,
            ..HudConfig::default()
        };
        Self {
            config,
            telemetry: TelemetryCollector::with_mock_root(PathBuf::from("/nonexistent_fpshud_test")),
            input: InputManager::new_headless(),
            screenshot_worker: ScreenshotWorker::new(std::env::temp_dir().join("fpshud_test_shots")),
            overlay_sync: OverlayFrameSync::default(),
            pending_screenshot: None,
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
            socket_path: None,
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
                InputAction::ToggleBenchmark => {
                    self.telemetry.toggle_benchmark();
                }
                InputAction::TakeScreenshot => {
                    self.pending_screenshot = Some(self.config.screenshot_with_hud);
                }
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
                    config_changed = true;
                }
                InputAction::ToggleBenchmark => {
                    self.telemetry.toggle_benchmark();
                }
                InputAction::TakeScreenshot => {
                    self.pending_screenshot = Some(self.config.screenshot_with_hud);
                }
            }
        }
        if config_changed {
            let _ = self.send_to_ui(&LayerToUiMsg::ConfigSync(self.config.clone()));
        }
        config_changed
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
        self.screenshot_worker.submit(RawFrameCapture {
            width,
            height,
            format,
            flip_y,
            pixels,
        });
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
            return false;
        }
        true
    }

    fn ensure_ipc_started(&mut self) {
        if self.test_mode || !self.config.enabled || self.ipc_server.is_some() {
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
        let mut buf = vec![0_u8; MAX_IPC_PAYLOAD + 64];
        loop {
            let recv_res = {
                let Some(conn) = &mut self.ipc_conn else {
                    break;
                };
                conn.recv_msg(&mut buf)
            };
            match recv_res {
                Ok((0, _)) => {
                    self.ipc_conn = None;
                    self.init_sent = false;
                    break;
                }
                Ok((n, fds)) => {
                    for fd in fds {
                        if let Ok(reader) = ShmRingReader::from_borrowed_fd(fd.as_fd(), 0) {
                            self.shm_reader = Some(reader);
                        }
                    }
                    if let Ok(msg) = UiToLayerMsg::decode(&buf[..n]) {
                        self.handle_ui_msg(msg);
                    }
                }
                Err(err) if err.kind() == io::ErrorKind::WouldBlock => break,
                Err(_) => {
                    self.ipc_conn = None;
                    self.init_sent = false;
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
                self.config = cfg;
                let _ = save_config_to_disk(&self.config);
            }
            UiToLayerMsg::BenchmarkToggle => {
                self.telemetry.toggle_benchmark();
            }
            UiToLayerMsg::ScreenshotRequest { with_hud } => {
                self.pending_screenshot = Some(with_hud);
            }
            UiToLayerMsg::OpenSaveFolder => {
                let dir = if self.config.save_folder.trim().is_empty() {
                    get_default_record_dir()
                } else {
                    PathBuf::from(&self.config.save_folder)
                };
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
        }
    }

    pub fn pump_frame(&mut self, width: u32, height: u32, render_api: &str) {
        if !self.config.enabled {
            return;
        }
        let w = width.max(1);
        let h = height.max(1);
        self.telemetry.set_render_api(render_api);
        self.telemetry.on_frame();

        self.ensure_ipc_started();

        if self.ipc_conn.is_none()
            && let Some(server) = &self.ipc_server
            && let Ok(conn) = server.accept_verified()
            && conn.set_nonblocking(true).is_ok()
        {
            self.ipc_conn = Some(conn);
            self.init_sent = false;
            self.last_software_cursor = false;
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
        }

        let now = Instant::now();
        if now >= self.next_input_poll {
            self.next_input_poll = now + Duration::from_millis(8);
            let prev_config = self.config.clone();
            let (actions, ui_events, _grabbed) =
                self.input.poll_frame(&mut self.config, w as f32, h as f32);
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

        if (self.config.hud_visible || self.config.gui_open)
            && now.duration_since(self.last_telemetry_push) >= Duration::from_millis(100)
        {
            self.last_telemetry_push = now;
            let snap = self.telemetry.get_snapshot();
            let _ = self.send_to_ui(&LayerToUiMsg::TelemetryUpdate(Box::new(snap)));
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
        if !self.telemetry.get_snapshot().benchmark_active {
            return false;
        }
        if self.consume_screenshot_request().is_none() {
            return false;
        }

        // Resize to 1280x720 -> stale 800x600 generation 1 MUST be rejected, matching 1280x720 accepted
        self.overlay_sync.reset_for_resize(1280, 720);
        let stale_pixels = vec![0xAA_u8; 800 * 600 * 4];
        let stale_meta = ShmSlotMeta {
            modifier: 0,
            generation: 1,
            slot_index: 0,
            width: 800,
            height: 600,
            stride: 800 * 4,
            fourcc: 0,
            byte_len: stale_pixels.len() as u32,
            backend: fpshud_ipc::FrameBackendKind::MemfdRgba,
        };
        if self
            .overlay_sync
            .accept_metadata_only(&stale_meta, &stale_pixels)
        {
            return false;
        }

        let valid_pixels = vec![0xBB_u8; 1280 * 720 * 4];
        let valid_meta = ShmSlotMeta {
            modifier: 0,
            generation: 2,
            slot_index: 0,
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
