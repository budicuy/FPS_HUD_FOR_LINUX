use std::collections::HashMap;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};

const LD_HOST_CANDIDATES: &[&str] = &[
    "usr/lib/x86_64-linux-gnu",
    "usr/lib/x86_64-linux-gnu/pulseaudio",
    "usr/lib64",
    "usr/lib64/pulseaudio",
    "usr/lib",
    "usr/lib/pulseaudio",
];

const LIBVA_HOST_CANDIDATES: &[&str] = &[
    "usr/lib/x86_64-linux-gnu/dri",
    "usr/lib64/dri",
    "usr/lib/dri",
];

#[must_use]
pub fn resolve_ui_binary_path(self_so_path: &Path) -> PathBuf {
    match self_so_path.parent() {
        Some(dir) if !dir.as_os_str().is_empty() => dir.join("fpshud-ui"),
        _ => PathBuf::from("fpshud-ui"),
    }
}

#[must_use]
pub fn parse_maps_path_for_addr(maps: &str, addr: usize) -> Option<PathBuf> {
    for line in maps.lines() {
        let mut rest = line.trim_start();
        let Some((range, after_range)) = rest.split_once(char::is_whitespace) else {
            continue;
        };
        let Some((start_hex, end_hex)) = range.split_once('-') else {
            continue;
        };
        let Ok(start) = usize::from_str_radix(start_hex, 16) else {
            continue;
        };
        let Ok(end) = usize::from_str_radix(end_hex, 16) else {
            continue;
        };
        if !(start..end).contains(&addr) {
            continue;
        }
        rest = after_range;
        let mut ok = true;
        for _ in 0..4 {
            rest = rest.trim_start();
            let Some((_, next)) = rest.split_once(char::is_whitespace) else {
                ok = false;
                break;
            };
            rest = next;
        }
        if !ok {
            continue;
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
pub fn resolve_self_so_path() -> Option<PathBuf> {
    let maps = std::fs::read_to_string("/proc/self/maps").ok()?;
    parse_maps_path_for_addr(&maps, resolve_self_so_path as *const () as usize)
}

#[must_use]
pub fn locate_ui_binary() -> PathBuf {
    if let Some(so_path) = resolve_self_so_path() {
        let sibling = resolve_ui_binary_path(&so_path);
        if sibling.is_file() {
            return sibling;
        }
    }

    if let Some(home) = std::env::var_os("HOME").filter(|h| !h.is_empty()) {
        let home_path = PathBuf::from(home);
        for rel in [".local/lib/fpshud/fpshud-ui", ".local/bin/fpshud-ui"] {
            let cand = home_path.join(rel);
            if cand.is_file() {
                return cand;
            }
        }
    }

    PathBuf::from("fpshud-ui")
}

/// Strips hook preloads and disables layer recursion inside child UI processes.
pub fn sanitize_ui_child_env(env_map: &mut HashMap<String, String>) {
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
}

/// Constructs host `(LD_LIBRARY_PATH, LIBVA_DRIVERS_PATH)` when running inside `/run/host` or `/var/run/host`.
#[must_use]
pub fn build_container_host_lib_paths(root_prefix: &Path) -> Option<(String, String)> {
    let mut host_root_rel = None;
    let mut host_root_actual = String::new();
    for (rel, actual) in [("run/host", "/run/host"), ("var/run/host", "/var/run/host")] {
        let probe = root_prefix.join(rel);
        if probe.exists() {
            host_root_rel = Some(rel);
            host_root_actual = actual.to_string();
            break;
        }
    }
    let rel = host_root_rel?;

    let mut ld_parts = Vec::new();
    for cand in LD_HOST_CANDIDATES {
        if root_prefix.join(rel).join(cand).exists() {
            ld_parts.push(format!("{host_root_actual}/{cand}"));
        }
    }

    let mut libva_parts = Vec::new();
    for cand in LIBVA_HOST_CANDIDATES {
        if root_prefix.join(rel).join(cand).exists() {
            libva_parts.push(format!("{host_root_actual}/{cand}"));
        }
    }

    if ld_parts.is_empty() && libva_parts.is_empty() {
        None
    } else {
        Some((ld_parts.join(":"), libva_parts.join(":")))
    }
}

pub struct UiDaemonHandle {
    child: Child,
}

impl UiDaemonHandle {
    pub fn spawn(socket_path: &Path) -> io::Result<Self> {
        let bin = locate_ui_binary();
        let pid = std::process::id().to_string();

        let mut env_map: HashMap<String, String> = std::env::vars().collect();
        sanitize_ui_child_env(&mut env_map);

        if let Some((host_ld, host_libva)) = build_container_host_lib_paths(Path::new("/")) {
            if !host_ld.is_empty() {
                let merged = match env_map.get("LD_LIBRARY_PATH") {
                    Some(cur) if !cur.is_empty() => format!("{host_ld}:{cur}"),
                    _ => host_ld,
                };
                env_map.insert("LD_LIBRARY_PATH".to_string(), merged);
            }
            if !host_libva.is_empty()
                && env_map
                    .get("LIBVA_DRIVERS_PATH")
                    .is_none_or(String::is_empty)
            {
                env_map.insert("LIBVA_DRIVERS_PATH".to_string(), host_libva);
            }
        }

        let child = Command::new(&bin)
            .arg("--socket")
            .arg(socket_path)
            .arg("--parent-pid")
            .arg(pid)
            .env_clear()
            .envs(&env_map)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()?;

        Ok(Self { child })
    }
}

impl Drop for UiDaemonHandle {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
