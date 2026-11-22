#![forbid(unsafe_code)]
mod wayland_overlay;

use anyhow::{Context, Result};
use fpshud_core::HudConfig;
use fpshud_gpui_platform::OffscreenGpuiRuntime;
use fpshud_input::keycodes::KEY_ESC;
use fpshud_input::{InputAction, InputManager, KeybindTarget, UiInputEvent};
use fpshud_ipc::protocol::{LayerToUiMsg, MAX_IPC_PAYLOAD, UiToLayerMsg};
use fpshud_ipc::{IpcClient, IpcConnection, ShmRingWriter};
use fpshud_telemetry::TelemetrySnapshot;
use fpshud_ui::toast::ToastState;
use fpshud_ui::{FpshudRootView, UiRenderMode, UiStore};
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
    runtime: Option<OffscreenGpuiRuntime>,
    shm_writer: Option<ShmRingWriter>,
    shm_fd_dirty: bool,
    scheduler: DirtyScheduler,
    toast_was_active: bool,
    started_at: Instant,
    pub surface_scale: f32,
    surface_width: u32,
    surface_height: u32,
    software_cursor: bool,

    // Isolated Wayland compositor overlay (zwlr_layer_shell_v1 / xdg_toplevel fallback)
    overlay_portal: Option<WaylandOverlayPortal>,
    overlay_runtime: Option<OffscreenGpuiRuntime>,
    overlay_dirty: DirtyScheduler,
    overlay_input: InputManager,
    overlay_rgba_scratch: Vec<u8>,
    overlay_size: (u32, u32),
    external_overlay_reported: bool,
}

impl DaemonState {
    fn new() -> Self {
        Self {
            config: HudConfig::default(),
            telemetry: TelemetrySnapshot::default(),
            runtime: None,
            shm_writer: None,
            shm_fd_dirty: true,
            scheduler: DirtyScheduler::new(false),
            toast_was_active: true,
            started_at: Instant::now(),
            surface_scale: 1.0,
            surface_width: 1280,
            surface_height: 720,
            software_cursor: false,
            overlay_portal: None,
            overlay_runtime: None,
            overlay_dirty: DirtyScheduler::new(false),
            overlay_input: InputManager::new_headless(),
            overlay_rgba_scratch: Vec::new(),
            overlay_size: (1280, 720),
            external_overlay_reported: false,
        }
    }

    fn sync_in_game_runtime(&mut self) {
        if let Some(rt) = &mut self.runtime {
            let cfg = self.config.clone();
            let snap = self.telemetry.clone();
            let mode = if self.overlay_portal.is_some() {
                UiRenderMode::HudOnly
            } else {
                UiRenderMode::Full
            };
            rt.update_app(|cx| {
                FpshudRootView::set_render_mode(cx, mode);
                FpshudRootView::sync_all(cx, cfg, snap);
            });
        }
    }

    fn sync_overlay_runtime(&mut self) {
        if let Some(ort) = &mut self.overlay_runtime {
            let cfg = self.config.clone();
            let snap = self.telemetry.clone();
            ort.update_app(|cx| {
                FpshudRootView::set_render_mode(cx, UiRenderMode::DashboardOnly);
                FpshudRootView::set_software_cursor(cx, false);
                FpshudRootView::sync_all(cx, cfg, snap);
            });
        }
    }

    pub fn apply_outgoing_msg(&mut self, msg: &UiToLayerMsg) {
        match msg {
            UiToLayerMsg::ConfigUpdated(cfg) => {
                self.config = cfg.clone();
                if (cfg.gui_scale - self.surface_scale).abs() > 1e-4 {
                    self.surface_scale = cfg.gui_scale.clamp(0.5, 3.0);
                    if let Some(rt) = &mut self.runtime {
                        rt.resize_with_scale(
                            self.surface_width,
                            self.surface_height,
                            self.surface_scale,
                        );
                    }
                    if let Some(ort) = &mut self.overlay_runtime {
                        ort.resize_with_scale(
                            self.overlay_size.0,
                            self.overlay_size.1,
                            self.surface_scale,
                        );
                    }
                }
                // Reflect live settings edits immediately into the in-game HUD behind the overlay.
                self.sync_in_game_runtime();
                self.scheduler.mark_dirty();
                if self.overlay_portal.is_some() {
                    self.sync_overlay_runtime();
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

        let mode = if self.overlay_portal.is_some() {
            UiRenderMode::HudOnly
        } else {
            UiRenderMode::Full
        };

        if let (Some(rt), Some(writer)) = (&mut self.runtime, &mut self.shm_writer) {
            rt.resize_with_scale(w, h, s);
            writer.resize(w, h)?;
            self.shm_fd_dirty = true;
            let cfg = self.config.clone();
            let snap = self.telemetry.clone();
            let sw = self.software_cursor;
            rt.update_app(|cx| {
                FpshudRootView::set_render_mode(cx, mode);
                FpshudRootView::sync_all(cx, cfg, snap);
                FpshudRootView::set_software_cursor(cx, sw);
            });
        } else {
            let cfg = self.config.clone();
            let snap = self.telemetry.clone();
            let sw = self.software_cursor;
            let mut rt = OffscreenGpuiRuntime::new_with_view(w, h, s, |_, _| {
                FpshudRootView::new(UiStore::new(cfg, snap))
            })?;
            rt.update_app(|cx| {
                FpshudRootView::set_render_mode(cx, mode);
                FpshudRootView::set_software_cursor(cx, sw);
            });
            let writer = ShmRingWriter::new(w, h)?;
            self.runtime = Some(rt);
            self.shm_writer = Some(writer);
            self.shm_fd_dirty = true;
        }
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
                if let Some(rt) = &mut self.runtime {
                    rt.resize_with_scale(w, h, s);
                }
                if let Some(writer) = &mut self.shm_writer {
                    writer.resize(w, h)?;
                    self.shm_fd_dirty = true;
                }
                self.scheduler.mark_dirty();
            }
            LayerToUiMsg::TelemetryUpdate(snap) => {
                self.telemetry = *snap;
                self.sync_in_game_runtime();
                let toast_active = !self.config.gui_open
                    && ToastState::should_show(self.started_at.elapsed().as_secs_f32());
                let in_game_needs_draw = self.config.hud_visible
                    || toast_active
                    || (self.config.gui_open && self.overlay_portal.is_none());
                if in_game_needs_draw {
                    self.scheduler.mark_dirty();
                }
                if self.overlay_portal.is_some() {
                    self.sync_overlay_runtime();
                    self.overlay_dirty.mark_dirty();
                }
            }
            LayerToUiMsg::ConfigSync(cfg) => {
                self.config = cfg;
                if (self.config.gui_scale - self.surface_scale).abs() > 1e-4 {
                    self.surface_scale = self.config.gui_scale.clamp(0.5, 3.0);
                    if let Some(rt) = &mut self.runtime {
                        rt.resize_with_scale(
                            self.surface_width,
                            self.surface_height,
                            self.surface_scale,
                        );
                    }
                    if let Some(ort) = &mut self.overlay_runtime {
                        ort.resize_with_scale(
                            self.overlay_size.0,
                            self.overlay_size.1,
                            self.surface_scale,
                        );
                    }
                }
                self.sync_in_game_runtime();
                self.scheduler.mark_dirty();
                if self.overlay_portal.is_some() {
                    self.sync_overlay_runtime();
                    self.overlay_dirty.mark_dirty();
                }
            }
            LayerToUiMsg::InputEvents(events) => {
                // While the isolated Wayland overlay owns compositor focus, ignore stale
                // XWayland/in-process layer input so events never double-fire.
                if self.overlay_portal.is_none()
                    && let Some(rt) = &mut self.runtime
                {
                    rt.dispatch_input(&events);
                    self.scheduler.mark_dirty();
                }
            }
            LayerToUiMsg::SoftwareCursor(enabled) => {
                self.software_cursor = enabled;
                if let Some(rt) = &mut self.runtime {
                    rt.update_app(|cx| FpshudRootView::set_software_cursor(cx, enabled));
                    self.scheduler.mark_dirty();
                }
            }
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
        let scale = self.config.gui_scale.clamp(0.5, 3.0);
        self.overlay_size = (ow, oh);
        self.overlay_input = InputManager::new_headless();

        let cfg = self.config.clone();
        let snap = self.telemetry.clone();
        if let Some(ort) = &mut self.overlay_runtime {
            ort.resize_with_scale(ow, oh, scale);
            ort.update_app(|cx| {
                FpshudRootView::set_render_mode(cx, UiRenderMode::DashboardOnly);
                FpshudRootView::set_software_cursor(cx, false);
                FpshudRootView::clear_keybind_capture(cx);
                FpshudRootView::clear_open_dropdown(cx);
                FpshudRootView::sync_all(cx, cfg, snap);
            });
        } else {
            match OffscreenGpuiRuntime::new_with_view(ow, oh, scale, |_, _| {
                FpshudRootView::new(UiStore::new(cfg, snap))
            }) {
                Ok(mut ort) => {
                    ort.update_app(|cx| {
                        FpshudRootView::set_render_mode(cx, UiRenderMode::DashboardOnly);
                        FpshudRootView::set_software_cursor(cx, false);
                    });
                    self.overlay_runtime = Some(ort);
                }
                Err(_) => {
                    // Clean fallback to in-swapchain dashboard if secondary wgpu init fails
                    drop(portal);
                    return Ok(());
                }
            }
        }

        self.overlay_portal = Some(portal);
        self.overlay_dirty.mark_dirty();
        self.sync_in_game_runtime();
        self.scheduler.mark_dirty();

        send_ui_msg(conn, &UiToLayerMsg::ExternalOverlayActive(true), None, false)?;
        self.external_overlay_reported = true;
        Ok(())
    }

    fn close_external_overlay(&mut self, conn: &mut IpcConnection) -> Result<()> {
        if self.overlay_portal.take().is_some() {
            self.overlay_input = InputManager::new_headless();
            self.overlay_dirty = DirtyScheduler::new(false);
            self.sync_in_game_runtime();
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
        let mut gpui_keys = Vec::new();

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
                            if let Some(ort) = &mut self.overlay_runtime {
                                ort.update_app(FpshudRootView::clear_keybind_capture);
                            }
                            if self.config != prev_cfg {
                                outgoing.push(UiToLayerMsg::ConfigUpdated(self.config.clone()));
                            }
                            self.sync_in_game_runtime();
                            self.scheduler.mark_dirty();
                            self.sync_overlay_runtime();
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
                            InputAction::ToggleGui | InputAction::ToggleHud => {}
                        }
                    }

                    if self.config != prev_cfg {
                        outgoing.push(UiToLayerMsg::ConfigUpdated(self.config.clone()));
                        self.sync_in_game_runtime();
                        self.scheduler.mark_dirty();
                        if self.config.gui_open {
                            self.sync_overlay_runtime();
                            self.overlay_dirty.mark_dirty();
                        }
                    }

                    if self.config.gui_open {
                        gpui_keys.push(if *down {
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

        (outgoing, gpui_keys)
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
            if let Some(ort) = &mut self.overlay_runtime {
                ort.resize_with_scale(rw, rh, self.config.gui_scale.clamp(0.5, 3.0));
            }
            self.overlay_dirty.mark_dirty();
        }

        let mut all_ui_events = poll.ui_events;
        if !poll.key_events.is_empty() {
            let (key_outgoing, gpui_keys) = self.handle_overlay_key_events(&poll.key_events);
            all_ui_events.extend(gpui_keys);
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
            && let Some(ort) = &mut self.overlay_runtime
        {
            ort.dispatch_input(&all_ui_events);
            self.overlay_dirty.mark_dirty();
            let outgoing = ort.update_app(FpshudRootView::take_outgoing);
            self.forward_overlay_outgoing(conn, outgoing)?;
            if self.overlay_portal.is_none() {
                return Ok(true);
            }
        }

        if self.overlay_dirty.take_should_render()
            && let (Some(portal), Some(ort)) =
                (&mut self.overlay_portal, &mut self.overlay_runtime)
        {
            let (rw, rh) = ort.render_scene_to_rgba(&mut self.overlay_rgba_scratch)?;
            if !portal.present_rgba(rw, rh, &self.overlay_rgba_scratch)? {
                self.overlay_dirty.mark_dirty();
            }
            had_activity = true;
            let outgoing = ort.update_app(FpshudRootView::take_outgoing);
            self.forward_overlay_outgoing(conn, outgoing)?;
        }

        Ok(had_activity)
    }

    fn check_toast_expiry(&mut self) {
        let now_active = !self.config.gui_open
            && ToastState::should_show(self.started_at.elapsed().as_secs_f32());
        if self.toast_was_active && !now_active {
            self.toast_was_active = false;
            self.scheduler.mark_dirty();
        }
    }

    fn render_if_dirty(&mut self, conn: &mut IpcConnection) -> Result<()> {
        if !self.scheduler.take_should_render() {
            return Ok(());
        }
        let (Some(rt), Some(writer)) = (&mut self.runtime, &mut self.shm_writer) else {
            return Ok(());
        };
        let meta = rt.render_scene_to_slot(writer)?;
        let send_fd = self.shm_fd_dirty;
        send_ui_msg(conn, &UiToLayerMsg::FrameReady(meta), Some(writer), send_fd)?;
        self.shm_fd_dirty = false;

        let outgoing = rt.update_app(FpshudRootView::take_outgoing);
        for msg in outgoing {
            self.apply_outgoing_msg(&msg);
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

    let mut state = DaemonState::new();
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
        state.check_toast_expiry();
        state.render_if_dirty(&mut conn)?;

        if !received_any && !overlay_active {
            let sleep_ms = if state.overlay_portal.is_some() { 4 } else { 12 };
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
}
