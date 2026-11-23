use std::collections::HashMap;
use std::ffi::OsStr;
use std::os::fd::RawFd;
use std::path::{Path, PathBuf};
use std::process::Command;

fn is_executable_file(path: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(path).is_ok_and(|m| m.is_file() && (m.permissions().mode() & 0o111) != 0)
}

#[must_use]
fn parse_maps_path_for_addr(maps: &str, addr: usize) -> Option<PathBuf> {
    for line in maps.lines() {
        let mut rest = line.trim_start();
        let (range, after_range) = rest.split_once(char::is_whitespace)?;
        let (start_hex, end_hex) = range.split_once('-')?;
        let start = usize::from_str_radix(start_hex, 16).ok()?;
        let end = usize::from_str_radix(end_hex, 16).ok()?;
        if !(start..end).contains(&addr) {
            continue;
        }
        rest = after_range;
        for _ in 0..4 {
            rest = rest.trim_start();
            let (_, next) = rest.split_once(char::is_whitespace)?;
            rest = next;
        }
        let raw_path = rest.trim_start();
        if !raw_path.starts_with('/') {
            return None;
        }
        let clean = raw_path.strip_suffix(" (deleted)").unwrap_or(raw_path);
        return (!clean.is_empty()).then(|| PathBuf::from(clean));
    }
    None
}

#[must_use]
fn resolve_self_module_path() -> Option<PathBuf> {
    let maps = std::fs::read_to_string("/proc/self/maps").ok()?;
    parse_maps_path_for_addr(&maps, resolve_self_module_path as *const () as usize)
}

/// Deterministic bundled `fpshud-ffmpeg` lookup with explicit inputs (for unit testing).
/// Never falls back to system `PATH` `"ffmpeg"`.
#[must_use]
pub fn locate_bundled_ffmpeg_with(
    env_override: Option<&OsStr>,
    self_module_path: Option<PathBuf>,
    current_exe_path: Option<PathBuf>,
    home_dir: Option<PathBuf>,
) -> Option<PathBuf> {
    if let Some(raw) = env_override
        && !raw.is_empty()
    {
        let p = PathBuf::from(raw);
        if is_executable_file(&p) {
            return Some(p);
        }
    }

    for anchor in [self_module_path, current_exe_path].into_iter().flatten() {
        if let Some(parent) = anchor.parent()
            && !parent.as_os_str().is_empty()
        {
            let sibling = parent.join("fpshud-ffmpeg");
            if is_executable_file(&sibling) {
                return Some(sibling);
            }
        }
    }

    if let Some(home) = home_dir
        && !home.as_os_str().is_empty()
    {
        let installed = home.join(".local/lib/fpshud/fpshud-ffmpeg");
        if is_executable_file(&installed) {
            return Some(installed);
        }
    }

    None
}

/// Locates the bundled `fpshud-ffmpeg` binary without ever falling back to system host `ffmpeg`.
#[must_use]
pub fn locate_bundled_ffmpeg() -> Option<PathBuf> {
    let env_override = std::env::var_os("FPSHUD_FFMPEG_PATH");
    let home = std::env::var_os("HOME").map(PathBuf::from);
    locate_bundled_ffmpeg_with(
        env_override.as_deref(),
        resolve_self_module_path(),
        std::env::current_exe().ok(),
        home,
    )
}

#[must_use]
pub fn is_running_in_container() -> bool {
    Path::new("/run/host").exists() || Path::new("/var/run/host").exists()
}

fn is_steam_scout_path(entry: &str) -> bool {
    entry.contains("ubuntu12_32")
        || entry.contains("steam-runtime")
        || entry.contains("SteamLinuxRuntime")
}

/// Sanitizes child environment variables for `fpshud-ffmpeg` and helper tools.
/// Never prepends `/run/host/usr/lib` to `LD_LIBRARY_PATH` inside containers to avoid glibc mismatches.
pub fn sanitize_child_env_map(env_map: &mut HashMap<String, String>, in_container: bool) {
    env_map.remove("LD_PRELOAD");
    env_map.remove("FPS_HUD");
    env_map.insert("DISABLE_FPS_HUD".to_string(), "1".to_string());

    if let Some(layers) = env_map.remove("VK_INSTANCE_LAYERS") {
        let filtered: Vec<&str> = layers
            .split(':')
            .filter(|s| !s.is_empty() && *s != "VK_LAYER_FPSHUD_overlay")
            .collect();
        if !filtered.is_empty() {
            env_map.insert("VK_INSTANCE_LAYERS".to_string(), filtered.join(":"));
        }
    }

    if let Some(ld) = env_map.remove("LD_LIBRARY_PATH") {
        let filtered: Vec<&str> = ld
            .split(':')
            .filter(|p| {
                !p.is_empty()
                    && !is_steam_scout_path(p)
                    && (!in_container || !p.starts_with("/run/host/usr/lib"))
                    && (!in_container || !p.starts_with("/var/run/host/usr/lib"))
            })
            .collect();
        if !filtered.is_empty() {
            env_map.insert("LD_LIBRARY_PATH".to_string(), filtered.join(":"));
        }
    }

    if in_container
        && env_map
            .get("LIBVA_DRIVERS_PATH")
            .is_none_or(String::is_empty)
    {
        for cand in [
            "/usr/lib/x86_64-linux-gnu/dri",
            "/usr/lib64/dri",
            "/usr/lib/dri",
            "/run/host/usr/lib/x86_64-linux-gnu/dri",
            "/run/host/usr/lib64/dri",
            "/run/host/usr/lib/dri",
        ] {
            if Path::new(cand).is_dir() {
                env_map.insert("LIBVA_DRIVERS_PATH".to_string(), cand.to_string());
                break;
            }
        }
    }
}

/// Constructs a sanitized `Command` for the bundled `fpshud-ffmpeg` binary.
#[must_use]
pub fn bundled_ffmpeg_command(bin: &Path) -> Command {
    let mut env_map: HashMap<String, String> = std::env::vars().collect();
    sanitize_child_env_map(&mut env_map, is_running_in_container());
    let mut cmd = Command::new(bin);
    cmd.env_clear().envs(&env_map);
    cmd
}

/// Best-effort increases Linux pipe buffer capacity to 1 MiB to cut kernel context switches.
pub fn tune_pipe_capacity(fd: RawFd) {
    const TARGET_PIPE_BYTES: libc::c_int = 1024 * 1024;
    // SAFETY: `fcntl(F_SETPIPE_SZ)` on an open pipe fd is safe and returns -1 on failure.
    let _ = unsafe { libc::fcntl(fd, libc::F_SETPIPE_SZ, TARGET_PIPE_BYTES) };
}
