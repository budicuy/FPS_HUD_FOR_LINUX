mod wayland_overlay;

use anyhow::{Context, Result};
use fpshud_core::HudConfig;
use fpshud_input::keycodes::KEY_ESC;
use fpshud_input::{InputAction, InputManager, KeybindTarget, UiInputEvent};
use fpshud_ipc::protocol::{FrameTickData, LayerToUiMsg, MAX_IPC_PAYLOAD, UiToLayerMsg};
use fpshud_ipc::{IpcClient, IpcConnection, ShmRingWriter};
use fpshud_telemetry::{SensorPollMask, TelemetryCollector, TelemetrySnapshot};
use fpshud_ui::cpu_canvas::TextRasterizer;
use fpshud_ui::cpu_dashboard::CpuDashboard;
use fpshud_ui::cpu_hud::CpuHudRenderer;
use fpshud_ui::state::UiRenderMode;
use fpshud_ui::toast::ToastState;
use fpshud_ui::UiStore;
use std::io;
use std::path::PathBuf;
use std::time::{Duration, Instant};
use wayland_overlay::{OverlayKeyEvent, WaylandOverlayPortal};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DaemonArgs {
    pub socket: PathBuf,
    pub parent_pid: i32,
}

pub fn parse_cli_args<I, S>(args: I) -> std::result::Result<DaemonArgs, String>
where
    I: IntoIterator<Item = S>,
    S: AsRef<str>,
{
    let mut socket: Option<PathBuf> = None;
    let mut parent_pid: Option<i32> = None;
    let mut iter = args.into_iter().skip(1);

    while let Some(arg) = iter.next() {
        let s = arg.as_ref();
        match s {
            "--socket" => {
                let val = iter
                    .next()
                    .ok_or_else(|| "missing value for --socket".to_string())?;
                if val.as_ref().is_empty() {
                    return Err("--socket path cannot be empty".to_string());
                }
                socket = Some(PathBuf::from(val.as_ref()));
            }
            "--parent-pid" => {
                let val = iter
                    .next()
                    .ok_or_else(|| "missing value for --parent-pid".to_string())?;
                let pid: i32 = val
                    .as_ref()
                    .parse()
                    .map_err(|_| format!("invalid --parent-pid value: {}", val.as_ref()))?;
                if pid <= 0 {
                    return Err("--parent-pid must be > 0".to_string());
                }
                parent_pid = Some(pid);
            }
            other => return Err(format!("unknown argument: {other}")),
        }
    }

    let socket = socket.ok_or_else(|| "missing required --socket <path>".to_string())?;
    let parent_pid =
        parent_pid.ok_or_else(|| "missing required --parent-pid <pid>".to_string())?;

    Ok(DaemonArgs { socket, parent_pid })
}

/// Ensures GPU scenes are only rendered when state/telemetry/input actually changed.
#[derive(Debug, Default)]
pub struct DirtyScheduler {
    dirty: bool,
}

impl DirtyScheduler {
    #[must_use]
    pub const fn new(initial_dirty: bool) -> Self {
        Self {
            dirty: initial_dirty,
        }
    }

    pub fn mark_dirty(&mut self) {
        self.dirty = true;
    }

    #[must_use]
    pub fn take_should_render(&mut self) -> bool {
        let should = self.dirty;
        self.dirty = false;
        should
    }
}

fn is_parent_alive(pid: i32) -> bool {
    if pid <= 0 {
        return false;
    }
    let Some(target) = rustix::process::Pid::from_raw(pid) else {
        return false;
    };
    matches!(
        rustix::process::test_kill_process(target),
        Ok(()) | Err(rustix::io::Errno::PERM)
    )
}

fn send_ui_msg(
    conn: &mut IpcConnection,
    msg: &UiToLayerMsg,
    shm: Option<&ShmRingWriter>,
    send_fd: bool,
) -> Result<()> {
    let encoded = msg.encode().context("encode UiToLayerMsg")?;
    if send_fd && let Some(writer) = shm {
        conn.send_msg(&encoded, &[writer.fd()])
            .context("send UiToLayerMsg with memfd")?;
    } else {
        conn.send_msg(&encoded, &[])
            .context("send UiToLayerMsg")?;
    }
    Ok(())
}

struct DaemonState {
    config: HudConfig,
    telemetry: TelemetrySnapshot,
    telemetry_collector: Option<TelemetryCollector>,
    pending_bench_loads: Option<(f32, f32)>,
    rasterizer: TextRasterizer,
    cpu_hud: CpuHudRenderer,
    dashboard: Option<CpuDashboard>,
    shm_writer: Option<ShmRingWriter>,
    shm_slot_bytes: usize,
    shm_fd_dirty: bool,
    scheduler: DirtyScheduler,
    toast_was_active: bool,
    active_toast_text: Option<String>,
    active_toast_until_sec: f32,
    folder_picker_rx: Option<std::sync::mpsc::Receiver<String>>,
    started_at: Instant,
    pub surface_scale: f32,
    surface_width: u32,
    surface_height: u32,
    software_cursor: bool,

    // Isolated Wayland compositor overlay (zwlr_layer_shell_v1 / xdg_toplevel fallback)
    overlay_portal: Option<WaylandOverlayPortal>,
    overlay_dirty: DirtyScheduler,
    overlay_input: InputManager,
    overlay_rgba_scratch: Vec<u8>,
    overlay_size: (u32, u32),
    external_overlay_reported: bool,
    last_overlay_telemetry_draw: Instant,
}

impl DaemonState {
    #[cfg(test)]
    fn new() -> Self {
        Self::new_for_test_headless()
    }

    fn new_for_test_headless() -> Self {
        Self {
            config: HudConfig::default(),
            telemetry: TelemetrySnapshot::default(),
            telemetry_collector: None,
            pending_bench_loads: None,
            rasterizer: TextRasterizer::new(),
            cpu_hud: CpuHudRenderer::new(),
            dashboard: None,
            shm_writer: None,
            shm_slot_bytes: 0,
            shm_fd_dirty: true,
            scheduler: DirtyScheduler::new(false),
            toast_was_active: true,
            active_toast_text: None,
            active_toast_until_sec: 0.0,
            folder_picker_rx: None,
            started_at: Instant::now(),
            surface_scale: 1.0,
            surface_width: 1280,
            surface_height: 720,
            software_cursor: false,
            overlay_portal: None,
            overlay_dirty: DirtyScheduler::new(false),
            overlay_input: InputManager::new_headless(),
            overlay_rgba_scratch: Vec::new(),
            overlay_size: (1280, 720),
            external_overlay_reported: false,
            last_overlay_telemetry_draw: Instant::now(),
        }
    }

    fn new_with_parent(parent_pid: u32) -> Self {
        let mut st = Self::new_for_test_headless();
        st.telemetry_collector = Some(TelemetryCollector::new_for_pid(
            (parent_pid > 0).then_some(parent_pid),
        ));
        st
    }

    fn sync_poll_mask(&self) {
        if let Some(tc) = &self.telemetry_collector {
            tc.set_poll_mask(SensorPollMask::from_metrics(
                &self.config.metrics,
                self.config.gui_open,
            ));
        }
    }

    fn merge_frame_tick(&mut self, tick: FrameTickData) {
        let was_bench_active = self.telemetry.benchmark_active;
        if let Some(tc) = &self.telemetry_collector {
            self.telemetry = tc.get_snapshot();
        }
        self.telemetry.fps = tick.fps;
        self.telemetry.fps_avg = tick.fps_avg;
        self.telemetry.fps_1pct_low = tick.fps_1pct_low;
        self.telemetry.fps_01pct_low = tick.fps_01pct_low;
        self.telemetry.frametime_ms = tick.frametime_ms;
        self.telemetry.frametime_history = tick.frametime_history;
        self.telemetry.render_api = tick.render_api;
        self.telemetry.benchmark_active = tick.benchmark_active;
        self.telemetry.benchmark_elapsed_sec = tick.benchmark_elapsed_sec;
        if tick.last_benchmark.is_some() {
            self.telemetry.last_benchmark = tick.last_benchmark;
        }
        if self.telemetry.benchmark_active {
            self.pending_bench_loads = Some((self.telemetry.cpu_load, self.telemetry.gpu_load));
        }
        if !was_bench_active && self.telemetry.benchmark_active {
            if let Some(dash) = &mut self.dashboard {
                dash.store.status_message = Some("Benchmark recording...".to_string());
            }
            self.overlay_dirty.mark_dirty();
        } else if was_bench_active && !self.telemetry.benchmark_active {
            let msg = if let Some(summary) = &self.telemetry.last_benchmark {
                format!(
                    "Benchmark saved ({:.0} FPS avg, {:.0} 1% low)",
                    summary.avg_fps, summary.fps_1pct_low
                )
            } else {
                "Benchmark stopped.".to_string()
            };
            if let Some(dash) = &mut self.dashboard {
                dash.store.status_message = Some(msg.clone());
            }
            self.overlay_dirty.mark_dirty();
            self.push_toast(msg);
        }
    }

    fn ensure_dashboard_for_mode(&mut self, mode: UiRenderMode) {
        let cfg = self.config.clone();
        let snap = self.telemetry.clone();
        let sw = mode == UiRenderMode::Full && self.software_cursor;
        if let Some(dash) = &mut self.dashboard {
            dash.set_render_mode(mode);
            dash.set_software_cursor(sw);
            dash.sync_all(cfg, snap);
        } else {
            let mut dash = CpuDashboard::new(UiStore::new(cfg, snap));
            dash.set_render_mode(mode);
            dash.set_software_cursor(sw);
            self.dashboard = Some(dash);
        }
    }

    fn release_dashboard_resources(&mut self) {
        self.dashboard = None;
        self.overlay_rgba_scratch = Vec::new();
    }

    fn render_dashboard_into_scratch(&mut self, width: u32, height: u32, scale: f32) {
        if let Some(dash) = &mut self.dashboard {
            dash.store.elapsed_sec = self.started_at.elapsed().as_secs_f32();
            dash.render_rgba(
                &mut self.rasterizer,
                width,
                height,
                scale,
                &mut self.overlay_rgba_scratch,
            );
        }
    }

    fn sync_dashboard(&mut self) {
        if let Some(dash) = &mut self.dashboard {
            let mode = if self.overlay_portal.is_some() {
                UiRenderMode::DashboardOnly
            } else {
                UiRenderMode::Full
            };
            dash.set_render_mode(mode);
            dash.set_software_cursor(mode == UiRenderMode::Full && self.software_cursor);
            dash.sync_all(self.config.clone(), self.telemetry.clone());
        }
    }

    pub fn apply_outgoing_msg(&mut self, msg: &UiToLayerMsg) {
        match msg {
            UiToLayerMsg::ConfigUpdated(cfg) => {
                self.config = cfg.clone();
                self.sync_poll_mask();
                if (cfg.gui_scale - self.surface_scale).abs() > 1e-4 {
                    self.surface_scale = cfg.gui_scale.clamp(0.5, 3.0);
                }
                if !self.config.gui_open && self.overlay_portal.is_none() {
                    self.release_dashboard_resources();
                } else {
                    self.sync_dashboard();
                }
                // Reflect live settings edits immediately into the in-game HUD behind the overlay.
                self.scheduler.mark_dirty();
                if self.overlay_portal.is_some() {
                    self.overlay_dirty.mark_dirty();
                }
            }
            UiToLayerMsg::KeybindCaptureState { capturing, target } => {
                if *capturing {
                    self.overlay_input.start_keybind_capture(*target);
                } else {
                    self.overlay_input.cancel_keybind_capture();
                }
            }
            _ => {}
        }
    }

    fn init_surface(
        &mut self,
        width: u32,
        height: u32,
        scale: f32,
        config: HudConfig,
    ) -> Result<()> {
        let w = width.max(1);
        let h = height.max(1);
        let s = scale.clamp(0.5, 3.0);
        self.config = config;
        self.surface_width = w;
        self.surface_height = h;
        self.surface_scale = s;
        self.sync_poll_mask();
        self.sync_dashboard();
        self.scheduler.mark_dirty();
        Ok(())
    }

    fn handle_layer_msg(&mut self, msg: LayerToUiMsg) -> Result<bool> {
        match msg {
            LayerToUiMsg::Init {
                width,
                height,
                scale,
                config,
            } => {
                self.init_surface(width, height, scale, config)?;
            }
            LayerToUiMsg::Resize {
                width,
                height,
                scale,
            } => {
                let w = width.max(1);
                let h = height.max(1);
                let s = scale.clamp(0.5, 3.0);
                self.surface_width = w;
                self.surface_height = h;
                self.surface_scale = s;
                if let Some(writer) = &mut self.shm_writer {
                    writer.resize(w, h)?;
                    self.shm_fd_dirty = true;
                }
                self.scheduler.mark_dirty();
            }
            LayerToUiMsg::TelemetryUpdate(snap) => {
                self.telemetry = *snap;
                self.sync_dashboard();
                let toast_active = self.is_toast_active();
                let in_game_needs_draw = self.config.hud_visible
                    || toast_active
                    || (self.config.gui_open && self.overlay_portal.is_none());
                if in_game_needs_draw {
                    self.scheduler.mark_dirty();
                }
                if self.overlay_portal.is_some()
                    && self.last_overlay_telemetry_draw.elapsed() >= Duration::from_millis(500)
                {
                    self.last_overlay_telemetry_draw = Instant::now();
                    self.overlay_dirty.mark_dirty();
                }
            }
            LayerToUiMsg::FrameTick(tick) => {
                self.merge_frame_tick(*tick);
                self.sync_dashboard();
                let toast_active = self.is_toast_active();
                let in_game_needs_draw = self.config.hud_visible
                    || toast_active
                    || (self.config.gui_open && self.overlay_portal.is_none());
                if in_game_needs_draw {
                    self.scheduler.mark_dirty();
                }
                if self.overlay_portal.is_some()
                    && self.last_overlay_telemetry_draw.elapsed() >= Duration::from_millis(500)
                {
                    self.last_overlay_telemetry_draw = Instant::now();
                    self.overlay_dirty.mark_dirty();
                }
            }
            LayerToUiMsg::ActiveGpu {
                vendor_id,
                device_id,
            } => {
                if let Some(tc) = &self.telemetry_collector {
                    tc.notify_active_gpu(vendor_id, device_id);
                }
            }
            LayerToUiMsg::ConfigSync(cfg) => {
                self.config = cfg;
                self.sync_poll_mask();
                if (self.config.gui_scale - self.surface_scale).abs() > 1e-4 {
                    self.surface_scale = self.config.gui_scale.clamp(0.5, 3.0);
                }
                if !self.config.gui_open && self.overlay_portal.is_none() {
                    self.release_dashboard_resources();
                } else {
                    self.sync_dashboard();
                }
                self.scheduler.mark_dirty();
                if self.overlay_portal.is_some() {
                    self.overlay_dirty.mark_dirty();
                }
            }
            LayerToUiMsg::InputEvents(events) => {
                // While the isolated Wayland overlay owns compositor focus, ignore stale
                // XWayland/in-process layer input so events never double-fire.
                if self.overlay_portal.is_none() && self.config.gui_open {
                    self.ensure_dashboard_for_mode(UiRenderMode::Full);
                    if let Some(dash) = &mut self.dashboard {
                        let dirty = dash.dispatch_input(
                            &events,
                            self.surface_width,
                            self.surface_height,
                            self.surface_scale,
                        );
                        if dirty {
                            self.scheduler.mark_dirty();
                        }
                    }
                }
            }
            LayerToUiMsg::SoftwareCursor(enabled) => {
                self.software_cursor = enabled;
                if let Some(dash) = &mut self.dashboard {
                    dash.set_software_cursor(self.overlay_portal.is_none() && enabled);
                    self.scheduler.mark_dirty();
                }
            }
            LayerToUiMsg::ToastNotification(text) => {
                if let Some(dash) = &mut self.dashboard {
                    dash.store.status_message = Some(text.clone());
                }
                self.overlay_dirty.mark_dirty();
                self.push_toast(text);
            }
            LayerToUiMsg::RecorderStatusUpdate(_) => {}
            LayerToUiMsg::Shutdown => return Ok(false),
        }
        Ok(true)
    }

    fn open_external_overlay(&mut self, conn: &mut IpcConnection) -> Result<()> {
        if self.overlay_portal.is_some() {
            return Ok(());
        }
        let Some(portal) = WaylandOverlayPortal::try_open() else {
            return Ok(());
        };
        let (ow, oh) = portal.size();
        self.overlay_size = (ow, oh);
        self.overlay_input = InputManager::new_headless();

        self.ensure_dashboard_for_mode(UiRenderMode::DashboardOnly);
        if let Some(dash) = &mut self.dashboard {
            dash.clear_keybind_capture();
            dash.clear_open_dropdown();
        }

        self.overlay_portal = Some(portal);
        self.overlay_dirty.mark_dirty();
        self.scheduler.mark_dirty();

        send_ui_msg(conn, &UiToLayerMsg::ExternalOverlayActive(true), None, false)?;
        self.external_overlay_reported = true;
        Ok(())
    }

    fn close_external_overlay(&mut self, conn: &mut IpcConnection) -> Result<()> {
        if self.overlay_portal.take().is_some() {
            self.release_dashboard_resources();
            self.overlay_input = InputManager::new_headless();
            self.overlay_dirty = DirtyScheduler::new(false);
            self.scheduler.mark_dirty();
        }
        if self.external_overlay_reported {
            self.external_overlay_reported = false;
            send_ui_msg(conn, &UiToLayerMsg::ExternalOverlayActive(false), None, false)?;
        }
        Ok(())
    }

    fn sync_external_overlay(&mut self, conn: &mut IpcConnection) -> Result<()> {
        if self.config.gui_open {
            self.open_external_overlay(conn)?;
        } else {
            self.close_external_overlay(conn)?;
        }
        Ok(())
    }

    /// Processes keyboard events delivered exclusively to the isolated Wayland overlay.
    pub fn handle_overlay_key_events(
        &mut self,
        key_events: &[OverlayKeyEvent],
    ) -> (Vec<UiToLayerMsg>, Vec<UiInputEvent>) {
        let mut outgoing = Vec::new();
        let mut ui_keys = Vec::new();

        for ev in key_events {
            match ev {
                OverlayKeyEvent::InitialHeld(held) => {
                    self.overlay_input.seed_keys_down(held);
                }
                OverlayKeyEvent::Key { code, down } => {
                    let was_capturing = self.overlay_input.is_capturing_keybind();
                    let prev_cfg = self.config.clone();
                    let actions =
                        self.overlay_input
                            .handle_key_event(*code, *down, &mut self.config);

                    if was_capturing {
                        if !self.overlay_input.is_capturing_keybind() {
                            outgoing.push(UiToLayerMsg::KeybindCaptureState {
                                capturing: false,
                                target: KeybindTarget::None,
                            });
                            if let Some(dash) = &mut self.dashboard {
                                dash.clear_keybind_capture();
                            }
                            if self.config != prev_cfg {
                                outgoing.push(UiToLayerMsg::ConfigUpdated(self.config.clone()));
                            }
                            self.sync_dashboard();
                            self.scheduler.mark_dirty();
                            self.overlay_dirty.mark_dirty();
                        }
                        continue;
                    }

                    if *down && *code == KEY_ESC && self.config.gui_open {
                        self.config.gui_open = false;
                    }

                    for action in actions {
                        match action {
                            InputAction::ToggleBenchmark => {
                                outgoing.push(UiToLayerMsg::BenchmarkToggle);
                            }
                            InputAction::TakeScreenshot => {
                                outgoing.push(UiToLayerMsg::ScreenshotRequest {
                                    with_hud: self.config.screenshot_with_hud,
                                });
                            }
                            InputAction::ToggleRecord => {
                                outgoing.push(UiToLayerMsg::ToggleRecord);
                            }
                            InputAction::ToggleGui | InputAction::ToggleHud => {}
                        }
                    }

                    if self.config != prev_cfg {
                        outgoing.push(UiToLayerMsg::ConfigUpdated(self.config.clone()));
                        self.sync_dashboard();
                        self.scheduler.mark_dirty();
                        if self.config.gui_open {
                            self.overlay_dirty.mark_dirty();
                        }
                    }

                    if self.config.gui_open {
                        ui_keys.push(if *down {
                            UiInputEvent::KeyDown {
                                code: *code,
                                mod_mask: 0,
                            }
                        } else {
                            UiInputEvent::KeyUp {
                                code: *code,
                                mod_mask: 0,
                            }
                        });
                    }
                }
            }
        }

        (outgoing, ui_keys)
    }

    fn forward_overlay_outgoing(
        &mut self,
        conn: &mut IpcConnection,
        outgoing: Vec<UiToLayerMsg>,
    ) -> Result<()> {
        for msg in outgoing {
            self.apply_outgoing_msg(&msg);
            if matches!(&msg, UiToLayerMsg::ConfigUpdated(c) if !c.gui_open) {
                self.close_external_overlay(conn)?;
            }
            send_ui_msg(conn, &msg, None, false)?;
        }
        Ok(())
    }

    fn poll_and_render_external_overlay(&mut self, conn: &mut IpcConnection) -> Result<bool> {
        let Some(poll) = self
            .overlay_portal
            .as_mut()
            .map(WaylandOverlayPortal::poll_events)
        else {
            return Ok(false);
        };

        let mut had_activity =
            !poll.ui_events.is_empty() || !poll.key_events.is_empty() || poll.resized.is_some();

        if poll.closed {
            self.config.gui_open = false;
            self.close_external_overlay(conn)?;
            send_ui_msg(
                conn,
                &UiToLayerMsg::ConfigUpdated(self.config.clone()),
                None,
                false,
            )?;
            return Ok(true);
        }

        if let Some((rw, rh)) = poll.resized {
            self.overlay_size = (rw, rh);
            self.overlay_dirty.mark_dirty();
        }

        let mut all_ui_events = poll.ui_events;
        if !poll.key_events.is_empty() {
            let (key_outgoing, ui_keys) = self.handle_overlay_key_events(&poll.key_events);
            all_ui_events.extend(ui_keys);
            if !self.config.gui_open {
                self.close_external_overlay(conn)?;
            }
            for msg in key_outgoing {
                send_ui_msg(conn, &msg, None, false)?;
            }
            if self.overlay_portal.is_none() {
                return Ok(true);
            }
        }

        if !all_ui_events.is_empty()
            && let Some(dash) = &mut self.dashboard
        {
            let scale = self.config.gui_scale.clamp(0.5, 3.0);
            let dirty =
                dash.dispatch_input(&all_ui_events, self.overlay_size.0, self.overlay_size.1, scale);
            if dirty {
                self.overlay_dirty.mark_dirty();
            }
            let outgoing = dash.take_outgoing();
            self.forward_overlay_outgoing(conn, outgoing)?;
            if self.overlay_portal.is_none() {
                return Ok(true);
            }
        }

        if self.overlay_dirty.take_should_render() && self.overlay_portal.is_some() {
            let (ow, oh) = self.overlay_size;
            let scale = self.config.gui_scale.clamp(0.5, 3.0);
            self.render_dashboard_into_scratch(ow, oh, scale);
            let presented = if let Some(portal) = &mut self.overlay_portal {
                portal.present_rgba(ow, oh, &self.overlay_rgba_scratch)?
            } else {
                false
            };
            let outgoing = self
                .dashboard
                .as_mut()
                .map_or_else(Vec::new, CpuDashboard::take_outgoing);
            self.forward_overlay_outgoing(conn, outgoing)?;
            if self.handle_overlay_present_outcome(presented) {
                had_activity = true;
            }
        }

        Ok(had_activity)
    }

    fn handle_overlay_present_outcome(&mut self, presented: bool) -> bool {
        if !presented {
            self.overlay_dirty.mark_dirty();
            return false;
        }
        true
    }

    fn push_toast(&mut self, text: impl Into<String>) {
        self.active_toast_text = Some(text.into());
        self.active_toast_until_sec = self.started_at.elapsed().as_secs_f32()
            + fpshud_ui::toast::NOTIFICATION_DURATION_SEC;
        self.toast_was_active = true;
        self.scheduler.mark_dirty();
    }

    fn is_toast_active(&self) -> bool {
        if self.config.gui_open {
            return false;
        }
        let elapsed = self.started_at.elapsed().as_secs_f32();
        (self.active_toast_text.is_some() && elapsed < self.active_toast_until_sec)
            || ToastState::should_show(elapsed)
    }

    fn check_toast_expiry(&mut self) {
        let now_active = self.is_toast_active();
        if self.toast_was_active && !now_active {
            self.toast_was_active = false;
            self.active_toast_text = None;
            self.scheduler.mark_dirty();
        }
    }

    fn poll_folder_picker(&mut self, conn: &mut IpcConnection) -> Result<()> {
        if let Some(dash) = &mut self.dashboard
            && dash.store.take_pick_screenshot_folder()
            && self.folder_picker_rx.is_none()
        {
            let initial_dir = self.config.resolved_screenshot_dir();
            let (tx, rx) = std::sync::mpsc::channel();
            self.folder_picker_rx = Some(rx);
            std::thread::spawn(move || {
                let initial_str = initial_dir.to_string_lossy().into_owned();
                let picked = std::process::Command::new("zenity")
                    .args([
                        "--file-selection",
                        "--directory",
                        "--title=Select FPSHUD Screenshot Folder",
                        &format!("--filename={initial_str}/"),
                    ])
                    .env_remove("LD_PRELOAD")
                    .env("DISABLE_FPS_HUD", "1")
                    .output()
                    .ok()
                    .filter(|o| o.status.success())
                    .or_else(|| {
                        std::process::Command::new("kdialog")
                            .args(["--getexistingdirectory", &initial_str])
                            .env_remove("LD_PRELOAD")
                            .env("DISABLE_FPS_HUD", "1")
                            .output()
                            .ok()
                            .filter(|o| o.status.success())
                    })
                    .and_then(|o| String::from_utf8(o.stdout).ok())
                    .map(|s| s.trim().to_string())
                    .filter(|s| !s.is_empty());
                if let Some(dir) = picked {
                    let _ = tx.send(dir);
                }
            });
        }

        if let Some(rx) = &self.folder_picker_rx {
            match rx.try_recv() {
                Ok(dir) => {
                    self.folder_picker_rx = None;
                    self.config.save_folder = dir;
                    fpshud_core::config::save_config_to_disk(&self.config);
                    self.sync_dashboard();
                    send_ui_msg(
                        conn,
                        &UiToLayerMsg::ConfigUpdated(self.config.clone()),
                        None,
                        false,
                    )?;
                    self.overlay_dirty.mark_dirty();
                    self.push_toast("Screenshot folder updated");
                }
                Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    self.folder_picker_rx = None;
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => {}
            }
        }
        Ok(())
    }

    fn ensure_shm_capacity(&mut self, width: u32, height: u32) -> Result<()> {
        let w = width.max(1);
        let h = height.max(1);
        let needed = (w as usize).saturating_mul(h as usize).saturating_mul(4);
        if let Some(writer) = &mut self.shm_writer {
            if needed > self.shm_slot_bytes {
                writer.resize(w, h)?;
                self.shm_slot_bytes = needed;
                self.shm_fd_dirty = true;
            }
        } else {
            let writer = ShmRingWriter::new(w, h)?;
            self.shm_slot_bytes = needed;
            self.shm_writer = Some(writer);
            self.shm_fd_dirty = true;
        }
        Ok(())
    }

    fn flush_pending_bench_loads(&mut self, conn: &mut IpcConnection) -> Result<()> {
        if let Some((cpu_load, gpu_load)) = self.pending_bench_loads.take() {
            send_ui_msg(
                conn,
                &UiToLayerMsg::BenchmarkLoads { cpu_load, gpu_load },
                None,
                false,
            )?;
        }
        Ok(())
    }

    fn render_if_dirty(&mut self, conn: &mut IpcConnection) -> Result<()> {
        self.flush_pending_bench_loads(conn)?;
        if !self.scheduler.take_should_render() {
            return Ok(());
        }

        // Fast CPU path: HudOnly (GUI closed OR GUI rendered on isolated Wayland overlay)
        if !self.config.gui_open || self.overlay_portal.is_some() {
            if !self.config.gui_open && self.dashboard.is_some() {
                self.release_dashboard_resources();
            }
            let mut store = UiStore::new(self.config.clone(), self.telemetry.clone());
            store.elapsed_sec = self.started_at.elapsed().as_secs_f32();
            store.active_toast_text = self.active_toast_text.clone();
            store.active_toast_until_sec = self.active_toast_until_sec;
            let Some(frame) = self.cpu_hud.render_with_rasterizer(
                &mut self.rasterizer,
                &store,
                self.surface_width,
                self.surface_height,
                self.surface_scale,
            ) else {
                return Ok(());
            };
            self.ensure_shm_capacity(frame.width, frame.height)?;
            let Some(writer) = &mut self.shm_writer else {
                return Ok(());
            };
            let meta = writer.write_next_slot_at(
                frame.dst_x,
                frame.dst_y,
                frame.width,
                frame.height,
                &frame.rgba,
            )?;
            let send_fd = self.shm_fd_dirty;
            send_ui_msg(conn, &UiToLayerMsg::FrameReady(meta), Some(writer), send_fd)?;
            self.shm_fd_dirty = false;
            return Ok(());
        }

        // Pure-X11 fallback path when GUI is open without WaylandOverlayPortal: 100% CPU Full composite
        self.ensure_dashboard_for_mode(UiRenderMode::Full);
        self.render_dashboard_into_scratch(
            self.surface_width,
            self.surface_height,
            self.surface_scale,
        );
        self.ensure_shm_capacity(self.surface_width, self.surface_height)?;
        let Some(writer) = &mut self.shm_writer else {
            return Ok(());
        };
        let meta = writer.write_next_slot_at(
            0,
            0,
            self.surface_width,
            self.surface_height,
            &self.overlay_rgba_scratch,
        )?;
        let send_fd = self.shm_fd_dirty;
        send_ui_msg(conn, &UiToLayerMsg::FrameReady(meta), Some(writer), send_fd)?;
        self.shm_fd_dirty = false;

        let outgoing = self
            .dashboard
            .as_mut()
            .map_or_else(Vec::new, CpuDashboard::take_outgoing);
        for msg in outgoing {
            self.apply_outgoing_msg(&msg);
            self.sync_poll_mask();
            send_ui_msg(conn, &msg, None, false)?;
        }
        Ok(())
    }
}

fn run_daemon(args: DaemonArgs) -> Result<()> {
    let mut conn = match IpcClient::connect(&args.socket) {
        Ok(c) => c,
        Err(_err) if !is_parent_alive(args.parent_pid) => return Ok(()),
        Err(err) => {
            return Err(err)
                .with_context(|| format!("connect IPC socket {}", args.socket.display()));
        }
    };
    conn.set_nonblocking(true)?;
    let _ = send_ui_msg(&mut conn, &UiToLayerMsg::Ready, None, false);

    if !is_parent_alive(args.parent_pid) {
        return Ok(());
    }

    let mut state = DaemonState::new_with_parent(u32::try_from(args.parent_pid).unwrap_or(0));
    let mut recv_buf = vec![0_u8; MAX_IPC_PAYLOAD + 64];

    loop {
        if !is_parent_alive(args.parent_pid) {
            break;
        }

        let mut received_any = false;
        loop {
            match conn.recv_msg(&mut recv_buf) {
                Ok((0, _)) => return Ok(()),
                Ok((n, _fds)) => {
                    received_any = true;
                    let msg = LayerToUiMsg::decode(&recv_buf[..n])?;
                    if !state.handle_layer_msg(msg)? {
                        return Ok(());
                    }
                }
                Err(err) if err.kind() == io::ErrorKind::WouldBlock => break,
                Err(err)
                    if matches!(
                        err.kind(),
                        io::ErrorKind::ConnectionReset
                            | io::ErrorKind::BrokenPipe
                            | io::ErrorKind::UnexpectedEof
                    ) =>
                {
                    return Ok(());
                }
                Err(err) => return Err(err.into()),
            }
        }

        state.sync_external_overlay(&mut conn)?;
        let overlay_active = state.poll_and_render_external_overlay(&mut conn)?;
        state.poll_folder_picker(&mut conn)?;
        state.check_toast_expiry();
        state.render_if_dirty(&mut conn)?;

        if !received_any && !overlay_active {
            let sleep_ms = if state.overlay_portal.is_some() { 4 } else { 25 };
            std::thread::sleep(Duration::from_millis(sleep_ms));
        }
    }

    Ok(())
}

fn main() -> std::process::ExitCode {
    let args = match parse_cli_args(std::env::args()) {
        Ok(a) => a,
        Err(msg) => {
            eprintln!("fpshud-ui error: {msg}");
            return std::process::ExitCode::FAILURE;
        }
    };

    match run_daemon(args) {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("fpshud-ui fatal: {err:#}");
            std::process::ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::wayland_overlay::{
        OverlayKeyEvent, WaylandOverlayPortal, decode_wl_enter_keys,
        discover_wayland_socket_candidates, map_wl_pointer_button, rgba_to_wl_argb8888,
    };
    use super::{DirtyScheduler, is_parent_alive, parse_cli_args};
    use fpshud_core::Keybind;
    use fpshud_input::KeybindTarget;
    use fpshud_ipc::protocol::UiToLayerMsg;
    use std::path::PathBuf;

    #[test]
    fn parse_cli_args_valid_and_invalid() {
        let parsed = parse_cli_args([
            "fpshud-ui",
            "--socket",
            "/run/user/1000/fpshud.sock",
            "--parent-pid",
            "4242",
        ])
        .unwrap();
        assert_eq!(parsed.socket, PathBuf::from("/run/user/1000/fpshud.sock"));
        assert_eq!(parsed.parent_pid, 4242);

        assert!(parse_cli_args(["fpshud-ui"]).is_err());
        assert!(parse_cli_args(["fpshud-ui", "--socket", "/tmp/a.sock"]).is_err());
        assert!(
            parse_cli_args(["fpshud-ui", "--socket", "/tmp/a.sock", "--parent-pid", "abc"]).is_err()
        );
        assert!(
            parse_cli_args(["fpshud-ui", "--socket", "/tmp/a.sock", "--parent-pid", "-5"]).is_err()
        );
        assert!(parse_cli_args(["fpshud-ui", "--unknown"]).is_err());
    }

    #[test]
    fn dirty_scheduler_renders_once_per_mark_and_zero_when_idle() {
        let mut sched = DirtyScheduler::new(false);
        assert!(!sched.take_should_render());

        sched.mark_dirty();
        assert!(sched.take_should_render());
        assert!(!sched.take_should_render());
        assert!(!sched.take_should_render());

        assert!(!is_parent_alive(999_999));
        assert!(is_parent_alive(std::process::id() as i32));
    }

    #[test]
    fn parent_alive_checks_pid_validity_via_rustix() {
        assert!(!is_parent_alive(0));
        assert!(!is_parent_alive(-5));
        assert!(is_parent_alive(std::process::id() as i32));
    }

    #[test]
    fn daemon_state_retains_ui_config_across_telemetry_update() {
        let mut st = super::DaemonState::new();
        let edited = fpshud_core::HudConfig {
            hud_visible: false,
            preset: fpshud_core::PresetType::Minimal,
            ..fpshud_core::HudConfig::default()
        };

        st.apply_outgoing_msg(&UiToLayerMsg::ConfigUpdated(edited.clone()));
        assert!(!st.config.hud_visible);
        assert_eq!(st.config.preset, fpshud_core::PresetType::Minimal);

        st.handle_layer_msg(fpshud_ipc::protocol::LayerToUiMsg::TelemetryUpdate(
            Box::default(),
        ))
        .unwrap();
        assert!(!st.config.hud_visible);
        assert_eq!(st.config.preset, fpshud_core::PresetType::Minimal);

        st.handle_layer_msg(fpshud_ipc::protocol::LayerToUiMsg::Resize {
            width: 1280,
            height: 720,
            scale: 1.5,
        })
        .unwrap();
        assert!((st.surface_scale - 1.5).abs() < 1e-5);
    }

    #[test]
    fn rgba_to_wl_argb8888_and_wayland_protocol_helpers() {
        let rgba = [255_u8, 128, 32, 200, 10, 20, 30, 0];
        let mut argb = [0_u8; 8];
        rgba_to_wl_argb8888(&rgba, &mut argb);
        assert_eq!(argb, [32, 128, 255, 200, 30, 20, 10, 0]);

        let raw_keys = [88_u32.to_le_bytes(), 29_u32.to_le_bytes()].concat();
        assert_eq!(decode_wl_enter_keys(&raw_keys), vec![88, 29]);

        assert_eq!(map_wl_pointer_button(0x110), Some(0));
        assert_eq!(map_wl_pointer_button(0x111), Some(1));
        assert_eq!(map_wl_pointer_button(0x112), Some(2));
        assert_eq!(map_wl_pointer_button(0x113), None);

        // Wayland runtime socket discovery filters out .lock & regular files, keeps real Unix sockets
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("wayland-0.lock"), b"").unwrap();
        std::fs::write(dir.path().join("wayland-regular"), b"").unwrap();
        std::fs::write(dir.path().join("other-sock"), b"").unwrap();
        let sock_path = dir.path().join("wayland-1");
        let _listener = std::os::unix::net::UnixListener::bind(&sock_path).unwrap();
        assert_eq!(
            discover_wayland_socket_candidates(dir.path()),
            vec![sock_path]
        );
    }

    #[test]
    fn overlay_keyboard_handles_held_f12_esc_close_and_keybind_capture() {
        let mut st = super::DaemonState::new();
        st.config.gui_open = true;

        // F12 held from wl_keyboard::enter must NOT close the overlay on initial release
        let (out0, _) = st.handle_overlay_key_events(&[
            OverlayKeyEvent::InitialHeld(vec![88]),
            OverlayKeyEvent::Key {
                code: 88,
                down: true,
            },
            OverlayKeyEvent::Key {
                code: 88,
                down: false,
            },
        ]);
        assert!(st.config.gui_open);
        assert!(out0.is_empty());

        // Keybind capture active: ESC cancels capture WITHOUT closing GUI
        st.apply_outgoing_msg(&UiToLayerMsg::KeybindCaptureState {
            capturing: true,
            target: KeybindTarget::TakeScreenshot,
        });
        let (out_cancel, _) = st.handle_overlay_key_events(&[
            OverlayKeyEvent::Key {
                code: 1,
                down: true,
            },
            OverlayKeyEvent::Key {
                code: 1,
                down: false,
            },
        ]);
        assert!(st.config.gui_open, "ESC cancelling keybind capture must keep GUI open");
        assert!(out_cancel.iter().any(|m| matches!(
            m,
            UiToLayerMsg::KeybindCaptureState {
                capturing: false,
                target: KeybindTarget::None
            }
        )));

        // Keybind capture active: LeftCtrl(29) + F10(68) commits new keybind cleanly
        st.apply_outgoing_msg(&UiToLayerMsg::KeybindCaptureState {
            capturing: true,
            target: KeybindTarget::TakeScreenshot,
        });
        let (out_bind, _) = st.handle_overlay_key_events(&[
            OverlayKeyEvent::Key {
                code: 29,
                down: true,
            },
            OverlayKeyEvent::Key {
                code: 68,
                down: true,
            },
            OverlayKeyEvent::Key {
                code: 68,
                down: false,
            },
            OverlayKeyEvent::Key {
                code: 29,
                down: false,
            },
        ]);
        assert_eq!(st.config.bind_screenshot, Keybind::new(1, 68));
        assert!(out_bind.iter().any(|m| matches!(m, UiToLayerMsg::ConfigUpdated(_))));

        // ESC when not capturing closes GUI cleanly
        let (out_esc, _) = st.handle_overlay_key_events(&[
            OverlayKeyEvent::Key {
                code: 1,
                down: true,
            },
            OverlayKeyEvent::Key {
                code: 1,
                down: false,
            },
        ]);
        assert!(!st.config.gui_open);
        assert!(
            out_esc
                .iter()
                .any(|m| matches!(m, UiToLayerMsg::ConfigUpdated(c) if !c.gui_open))
        );

        // Fresh F12 press also closes GUI cleanly
        st.config.gui_open = true;
        let (out_f12, _) = st.handle_overlay_key_events(&[
            OverlayKeyEvent::Key {
                code: 88,
                down: true,
            },
            OverlayKeyEvent::Key {
                code: 88,
                down: false,
            },
        ]);
        assert!(!st.config.gui_open);
        assert!(
            out_f12
                .iter()
                .any(|m| matches!(m, UiToLayerMsg::ConfigUpdated(c) if !c.gui_open))
        );
    }

    #[test]
    fn live_wayland_overlay_portal_roundtrips_when_wayland_present() {
        if std::env::var_os("WAYLAND_DISPLAY").is_none()
            || std::env::var("FPSHUD_DISABLE_WAYLAND_OVERLAY").as_deref() == Ok("1")
        {
            return;
        }
        let mut portal =
            WaylandOverlayPortal::try_open().expect("WaylandOverlayPortal must open on live Wayland");
        let (w, h) = portal.size();
        assert!(w >= 320 && h >= 240, "configured size unexpected: ({w}, {h})");
        let _layer_shell = portal.is_layer_shell();

        let frame = vec![0_u8; (w as usize) * (h as usize) * 4];
        portal.present_rgba(w, h, &frame).unwrap();
        let poll = portal.poll_events();
        assert!(!poll.closed);
        drop(portal);
    }

    #[test]
    fn overlay_buffer_slot_picker_never_overwrites_when_all_slots_busy() {
        use crate::wayland_overlay::pick_free_buffer_slot;

        assert_eq!(pick_free_buffer_slot(&[false, false, false]), Some(0));
        assert_eq!(pick_free_buffer_slot(&[true, false, false]), Some(1));
        assert_eq!(pick_free_buffer_slot(&[true, true, false]), Some(2));
        assert_eq!(pick_free_buffer_slot(&[true, true, true]), None);
    }

    #[test]
    fn overlay_present_backpressure_rearms_dirty_without_busy_spin() {
        let mut st = crate::DaemonState::new();
        assert!(!st.handle_overlay_present_outcome(false));
        assert!(st.overlay_dirty.take_should_render());
        assert!(st.handle_overlay_present_outcome(true));
        assert!(!st.overlay_dirty.take_should_render());
    }

    #[test]
    fn daemon_hud_only_keeps_dashboard_unallocated_and_merges_frame_tick() {
        use fpshud_core::HudConfig;
        use fpshud_ipc::protocol::{FrameTickData, LayerToUiMsg};

        let mut st = crate::DaemonState::new_for_test_headless();
        let cfg = HudConfig {
            hud_visible: true,
            gui_open: false,
            ..HudConfig::default()
        };
        st.handle_layer_msg(LayerToUiMsg::Init {
            width: 1280,
            height: 720,
            scale: 1.0,
            config: cfg,
        })
        .unwrap();
        assert!(
            st.dashboard.is_none(),
            "HudOnly mode must NOT allocate CpuDashboard"
        );
        assert_eq!(st.overlay_rgba_scratch.capacity(), 0);

        let tick = FrameTickData {
            fps: 144.0,
            fps_avg: 140.0,
            fps_1pct_low: 110.0,
            fps_01pct_low: 95.0,
            frametime_ms: 6.94,
            frametime_history: vec![6.9, 7.0],
            render_api: "VULKAN".to_string(),
            benchmark_active: false,
            benchmark_elapsed_sec: 0.0,
            last_benchmark: None,
        };
        st.handle_layer_msg(LayerToUiMsg::FrameTick(Box::new(tick)))
            .unwrap();
        assert!((st.telemetry.fps - 144.0).abs() < 0.01);
        assert_eq!(st.telemetry.render_api, "VULKAN");
    }

    #[test]
    fn daemon_cpu_gui_open_input_close_reopen_releases_memory_and_resets_transient_state() {
        use fpshud_core::HudConfig;
        use fpshud_input::KeybindTarget;
        use fpshud_ipc::protocol::LayerToUiMsg;
        use fpshud_ui::state::UiRenderMode;

        let mut state = crate::DaemonState::new_for_test_headless();
        let mut cfg = HudConfig {
            gui_open: true,
            ..HudConfig::default()
        };
        state
            .handle_layer_msg(LayerToUiMsg::Init {
                width: 1280,
                height: 720,
                scale: 1.0,
                config: cfg.clone(),
            })
            .unwrap();
        state.ensure_dashboard_for_mode(UiRenderMode::Full);
        assert!(state.dashboard.is_some());

        state.render_dashboard_into_scratch(1280, 720, 1.0);
        assert!(!state.overlay_rgba_scratch.is_empty());

        // Close GUI -> must drop dashboard and deallocate scratch buffer (capacity == 0)
        cfg.gui_open = false;
        state
            .handle_layer_msg(LayerToUiMsg::ConfigSync(cfg.clone()))
            .unwrap();
        assert!(state.dashboard.is_none());
        assert_eq!(state.overlay_rgba_scratch.capacity(), 0);

        // Reopen -> clean transient state (no lingering dropdown or keybind capture)
        cfg.gui_open = true;
        state
            .handle_layer_msg(LayerToUiMsg::ConfigSync(cfg))
            .unwrap();
        state.ensure_dashboard_for_mode(UiRenderMode::Full);
        let dash = state.dashboard.as_ref().unwrap();
        assert!(dash.open_dropdown().is_none());
        assert_eq!(dash.active_keybind_capture(), KeybindTarget::None);
    }
}
