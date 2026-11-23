use crate::locate::bundled_ffmpeg_command;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::SystemTime;

#[derive(Debug, Clone, Default, PartialEq)]
pub struct ParsedMediaMeta {
    pub duration_sec: f64,
    pub width: u32,
    pub height: u32,
    pub fps: f32,
    pub video_codec: String,
    pub audio_codec: Option<String>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct RecordingEntry {
    pub path: PathBuf,
    pub file_name: String,
    pub duration_sec: f64,
    pub size_bytes: u64,
    pub width: u32,
    pub height: u32,
    pub fps: f32,
    pub video_codec: String,
    pub audio_codec: Option<String>,
    pub in_progress: bool,
}

fn parse_hms_duration(raw: &str) -> Option<f64> {
    let trimmed = raw.trim().trim_end_matches(',');
    if trimmed.eq_ignore_ascii_case("N/A") {
        return None;
    }
    let mut parts = trimmed.split(':');
    let h: f64 = parts.next()?.parse().ok()?;
    let m: f64 = parts.next()?.parse().ok()?;
    let s: f64 = parts.next()?.parse().ok()?;
    if h.is_finite() && m.is_finite() && s.is_finite() {
        Some((h * 3600.0 + m * 60.0 + s).max(0.0))
    } else {
        None
    }
}

fn parse_dimensions_token(token: &str) -> Option<(u32, u32)> {
    let first = token.split_whitespace().next()?;
    let (w_str, h_str) = first.split_once('x')?;
    let w: u32 = w_str.parse().ok()?;
    let h: u32 = h_str.parse().ok()?;
    (w >= 16 && h >= 16).then_some((w, h))
}

/// Parses `fpshud-ffmpeg -hide_banner -i <file>` stderr output into media metadata.
#[must_use]
pub fn parse_ffmpeg_probe_output(stderr: &str) -> ParsedMediaMeta {
    let mut meta = ParsedMediaMeta::default();

    for line in stderr.lines() {
        let trimmed = line.trim();
        if let Some(rest) = trimmed.strip_prefix("Duration:")
            && let Some(dur_tok) = rest.split(',').next()
            && let Some(secs) = parse_hms_duration(dur_tok)
        {
            meta.duration_sec = secs;
        } else if trimmed.starts_with("Stream #") && trimmed.contains("Video:") {
            if let Some((_, after_vid)) = trimmed.split_once("Video:") {
                let fields: Vec<&str> = after_vid.split(',').map(str::trim).collect();
                if let Some(codec_field) = fields.first()
                    && let Some(codec) = codec_field.split_whitespace().next()
                {
                    meta.video_codec = codec.to_string();
                }
                for field in fields.iter().skip(1) {
                    if meta.width == 0
                        && let Some((w, h)) = parse_dimensions_token(field)
                    {
                        meta.width = w;
                        meta.height = h;
                    }
                    if let Some(fps_str) = field.strip_suffix(" fps")
                        && let Ok(fps) = fps_str.trim().parse::<f32>()
                        && fps.is_finite()
                        && fps > 0.0
                    {
                        meta.fps = fps;
                    }
                }
            }
        } else if trimmed.starts_with("Stream #")
            && trimmed.contains("Audio:")
            && meta.audio_codec.is_none()
            && let Some((_, after_aud)) = trimmed.split_once("Audio:")
            && let Some(codec_field) = after_aud.split(',').next()
            && let Some(codec) = codec_field.split_whitespace().next()
            && !codec.is_empty()
        {
            meta.audio_codec = Some(codec.to_string());
        }
    }

    meta
}

/// Probes a single recording file using bundled `fpshud-ffmpeg -hide_banner -i <path>`.
#[must_use]
pub fn probe_recording_entry(
    ffmpeg_bin: &Path,
    path: &Path,
    active_path: Option<&Path>,
) -> Option<RecordingEntry> {
    let meta_fs = fs::symlink_metadata(path).ok()?;
    if meta_fs.file_type().is_symlink() || !meta_fs.is_file() {
        return None;
    }
    let file_name = path.file_name()?.to_string_lossy().into_owned();
    let in_progress = active_path.is_some_and(|a| a == path);

    let output = bundled_ffmpeg_command(ffmpeg_bin)
        .args(["-hide_banner", "-i"])
        .arg(path)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .output()
        .ok()?;
    let stderr = String::from_utf8_lossy(&output.stderr);
    let parsed = parse_ffmpeg_probe_output(&stderr);

    Some(RecordingEntry {
        path: path.to_path_buf(),
        file_name,
        duration_sec: parsed.duration_sec,
        size_bytes: meta_fs.len(),
        width: parsed.width,
        height: parsed.height,
        fps: parsed.fps,
        video_codec: parsed.video_codec,
        audio_codec: parsed.audio_codec,
        in_progress,
    })
}

fn is_supported_video_ext(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .is_some_and(|ext| matches!(ext.to_ascii_lowercase().as_str(), "mp4" | "mkv" | "webm"))
}

/// Scans a recordings directory, skipping symlinks and non-regular files, newest first.
#[must_use]
pub fn scan_recordings_dir(
    ffmpeg_bin: &Path,
    dir: &Path,
    active_path: Option<&Path>,
) -> Vec<RecordingEntry> {
    let Ok(rd) = fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut candidates: Vec<(SystemTime, PathBuf)> = Vec::new();
    for entry in rd.flatten() {
        let p = entry.path();
        if !is_supported_video_ext(&p) {
            continue;
        }
        let Ok(meta) = fs::symlink_metadata(&p) else {
            continue;
        };
        if meta.file_type().is_symlink() || !meta.is_file() {
            continue;
        }
        let mtime = meta.modified().unwrap_or(SystemTime::UNIX_EPOCH);
        candidates.push((mtime, p));
    }

    candidates.sort_by_key(|a| std::cmp::Reverse(a.0));

    candidates
        .into_iter()
        .filter_map(|(_, p)| probe_recording_entry(ffmpeg_bin, &p, active_path))
        .collect()
}

/// Safely deletes a regular recording file inside `dir`, rejecting symlinks & path traversal.
pub fn delete_recording_safe(dir: &Path, file_path: &Path) -> io::Result<()> {
    let meta = fs::symlink_metadata(file_path)?;
    if meta.file_type().is_symlink() || !meta.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "refusing to delete symlink or non-regular file",
        ));
    }
    let canon_dir = fs::canonicalize(dir)?;
    let parent = file_path.parent().ok_or_else(|| {
        io::Error::new(io::ErrorKind::InvalidInput, "missing parent directory")
    })?;
    let canon_parent = fs::canonicalize(parent)?;
    if canon_parent != canon_dir {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "refusing to delete file outside recordings directory",
        ));
    }
    fs::remove_file(file_path)
}
