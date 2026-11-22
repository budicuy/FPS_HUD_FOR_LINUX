use rustix::process::getuid;
use std::env;
use std::fs;
use std::io;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, PermissionsExt};
use std::path::PathBuf;

/// Returns `$XDG_CONFIG_HOME/fpshud/config.ini` or `~/.config/fpshud/config.ini`.
#[must_use]
pub fn get_config_file_path() -> PathBuf {
    if let Ok(xdg) = env::var("XDG_CONFIG_HOME")
        && !xdg.is_empty()
    {
        return PathBuf::from(xdg).join("fpshud").join("config.ini");
    }
    if let Ok(home) = env::var("HOME")
        && !home.is_empty()
    {
        return PathBuf::from(home)
            .join(".config")
            .join("fpshud")
            .join("config.ini");
    }
    PathBuf::from("/tmp").join("fpshud").join("config.ini")
}

/// Returns the default directory for screenshots and benchmark CSV exports (`~/Videos/FPSHUD`).
#[must_use]
pub fn get_default_record_dir() -> PathBuf {
    if let Ok(xdg_vid) = env::var("XDG_VIDEOS_DIR")
        && !xdg_vid.is_empty()
    {
        return PathBuf::from(xdg_vid).join("FPSHUD");
    }
    if let Ok(home) = env::var("HOME")
        && !home.is_empty()
    {
        let home_path = PathBuf::from(home);
        if home_path.join("Video").exists() {
            return home_path.join("Video").join("FPSHUD");
        }
        return home_path.join("Videos").join("FPSHUD");
    }
    PathBuf::from("/tmp").join("FPSHUD")
}

/// Returns the default directory for screenshots (`<XDG_PICTURES_DIR>/FPSHUD`, e.g. `~/Gambar/FPSHUD` or `~/Pictures/FPSHUD`).
#[must_use]
pub fn get_default_screenshot_dir() -> PathBuf {
    if let Ok(xdg_pic) = env::var("XDG_PICTURES_DIR")
        && !xdg_pic.is_empty()
    {
        return PathBuf::from(xdg_pic).join("FPSHUD");
    }
    if let Ok(home) = env::var("HOME")
        && !home.is_empty()
    {
        let home_path = PathBuf::from(&home);
        let user_dirs_path = env::var("XDG_CONFIG_HOME")
            .ok()
            .filter(|v| !v.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| home_path.join(".config"))
            .join("user-dirs.dirs");
        if let Ok(content) = fs::read_to_string(user_dirs_path) {
            for line in content.lines() {
                let trimmed = line.trim();
                if let Some(rest) = trimmed.strip_prefix("XDG_PICTURES_DIR=") {
                    let unquoted = rest.trim().trim_matches('"');
                    if let Some(rel) = unquoted.strip_prefix("$HOME/") {
                        return home_path.join(rel).join("FPSHUD");
                    }
                    if !unquoted.is_empty() && unquoted.starts_with('/') {
                        return PathBuf::from(unquoted).join("FPSHUD");
                    }
                }
            }
        }
        if home_path.join("Gambar").exists() {
            return home_path.join("Gambar").join("FPSHUD");
        }
        return home_path.join("Pictures").join("FPSHUD");
    }
    PathBuf::from("/tmp").join("FPSHUD")
}

/// Ensures a private `0700` runtime directory owned by the current UID for IPC sockets.
pub fn get_secure_runtime_dir() -> io::Result<PathBuf> {
    let my_uid = getuid().as_raw();
    let base = match env::var("XDG_RUNTIME_DIR") {
        Ok(dir) if !dir.is_empty() => PathBuf::from(dir).join("fpshud"),
        _ => env::temp_dir().join(format!("fpshud-runtime-{my_uid}")),
    };

    if let Ok(meta) = fs::symlink_metadata(&base) {
        if meta.file_type().is_symlink() || !meta.is_dir() {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "runtime directory target is symlink or non-directory",
            ));
        }
        if meta.uid() != my_uid {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "runtime directory is not owned by current user",
            ));
        }
        if (meta.mode() & 0o777) != 0o700 {
            fs::set_permissions(&base, fs::Permissions::from_mode(0o700))?;
        }
        return Ok(base);
    }

    let mut builder = fs::DirBuilder::new();
    builder.recursive(true).mode(0o700);
    builder.create(&base)?;
    fs::set_permissions(&base, fs::Permissions::from_mode(0o700))?;
    Ok(base)
}

/// Strips traversal/directory components so prefixes stay single-segment safe filenames.
#[must_use]
pub fn sanitize_filename_prefix(raw: &str) -> String {
    let leaf = raw
        .rsplit(['/', '\\'])
        .next()
        .unwrap_or("")
        .trim();

    let cleaned: String = leaf
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '.'))
        .collect();
    let no_leading_dots = cleaned.trim_start_matches('.');
    if no_leading_dots.is_empty() || no_leading_dots.contains("..") {
        "FPSHUD".to_string()
    } else {
        no_leading_dots.to_string()
    }
}
