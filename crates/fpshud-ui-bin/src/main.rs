use anyhow::{Context, Result};
use fpshud_core::HudConfig;
use fpshud_gpui_platform::OffscreenGpuiRuntime;
use fpshud_ipc::protocol::{LayerToUiMsg, MAX_IPC_PAYLOAD, UiToLayerMsg};
use fpshud_ipc::{IpcClient, IpcConnection, ShmRingWriter};
use fpshud_telemetry::TelemetrySnapshot;
use fpshud_ui::toast::ToastState;
use fpshud_ui::{FpshudRootView, UiStore};
use std::io;
use std::path::PathBuf;
use std::time::{Duration, Instant};

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
    let rc = unsafe { libc::kill(pid as libc::pid_t, 0) };
    if rc == 0 {
        return true;
    }
    let errno = io::Error::last_os_error().raw_os_error().unwrap_or(0);
    errno == libc::EPERM
}

fn send_ui_msg(conn: &mut IpcConnection, msg: &UiToLayerMsg, shm: Option<&ShmRingWriter>, send_fd: bool) -> Result<()> {
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
        }
    }

    pub fn apply_outgoing_msg(&mut self, msg: &UiToLayerMsg) {
        if let UiToLayerMsg::ConfigUpdated(cfg) = msg {
            self.config = cfg.clone();
            if (cfg.gui_scale - self.surface_scale).abs() > 1e-4 {
                self.surface_scale = cfg.gui_scale.clamp(0.5, 3.0);
                if let Some(rt) = &mut self.runtime {
                    rt.resize_with_scale(self.surface_width, self.surface_height, self.surface_scale);
                }
            }
        }
    }

    fn init_surface(&mut self, width: u32, height: u32, scale: f32, config: HudConfig) -> Result<()> {
        let w = width.max(1);
        let h = height.max(1);
        let s = scale.clamp(0.5, 3.0);
        self.config = config;
        self.surface_width = w;
        self.surface_height = h;
        self.surface_scale = s;

        if let (Some(rt), Some(writer)) = (&mut self.runtime, &mut self.shm_writer) {
            rt.resize_with_scale(w, h, s);
            writer.resize(w, h)?;
            self.shm_fd_dirty = true;
            let cfg = self.config.clone();
            let snap = self.telemetry.clone();
            rt.update_app(|cx| FpshudRootView::sync_all(cx, cfg, snap));
        } else {
            let cfg = self.config.clone();
            let snap = self.telemetry.clone();
            let rt = OffscreenGpuiRuntime::new_with_view(w, h, s, |_, _| {
                FpshudRootView::new(UiStore::new(cfg, snap))
            })?;
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
                if let Some(rt) = &mut self.runtime {
                    let cfg = self.config.clone();
                    let t = self.telemetry.clone();
                    rt.update_app(|cx| FpshudRootView::sync_all(cx, cfg, t));
                }
                let toast_active = !self.config.gui_open
                    && ToastState::should_show(self.started_at.elapsed().as_secs_f32());
                if self.config.hud_visible || self.config.gui_open || toast_active {
                    self.scheduler.mark_dirty();
                }
            }
            LayerToUiMsg::ConfigSync(cfg) => {
                self.config = cfg;
                if let Some(rt) = &mut self.runtime {
                    let c = self.config.clone();
                    let t = self.telemetry.clone();
                    rt.update_app(|cx| FpshudRootView::sync_all(cx, c, t));
                }
                self.scheduler.mark_dirty();
            }
            LayerToUiMsg::InputEvents(events) => {
                if let Some(rt) = &mut self.runtime {
                    rt.dispatch_input(&events);
                    self.scheduler.mark_dirty();
                }
            }
            LayerToUiMsg::Shutdown => return Ok(false),
        }
        Ok(true)
    }

    fn check_toast_expiry(&mut self) {
        let now_active =
            !self.config.gui_open && ToastState::should_show(self.started_at.elapsed().as_secs_f32());
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
            return Err(err).with_context(|| format!("connect IPC socket {}", args.socket.display()));
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

        state.check_toast_expiry();
        state.render_if_dirty(&mut conn)?;

        if !received_any {
            std::thread::sleep(Duration::from_millis(12));
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
    use super::{DirtyScheduler, is_parent_alive, parse_cli_args};
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
        assert!(parse_cli_args(["fpshud-ui", "--socket", "/tmp/a.sock", "--parent-pid", "abc"]).is_err());
        assert!(parse_cli_args(["fpshud-ui", "--socket", "/tmp/a.sock", "--parent-pid", "-5"]).is_err());
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
    fn daemon_state_retains_ui_config_across_telemetry_update() {
        let mut st = super::DaemonState::new();
        let edited = fpshud_core::HudConfig {
            hud_visible: false,
            preset: fpshud_core::PresetType::Minimal,
            ..fpshud_core::HudConfig::default()
        };

        st.apply_outgoing_msg(&fpshud_ipc::protocol::UiToLayerMsg::ConfigUpdated(edited.clone()));
        assert!(!st.config.hud_visible);
        assert_eq!(st.config.preset, fpshud_core::PresetType::Minimal);

        st.handle_layer_msg(fpshud_ipc::protocol::LayerToUiMsg::TelemetryUpdate(Box::default()))
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
}
