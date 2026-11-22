use fpshud_core::ScreenshotQuality;
use fpshud_core::paths::{get_default_screenshot_dir, sanitize_filename_prefix};
use rustix::fs::{Mode, OFlags, open as rustix_open};
use std::fs::{self, File};
use std::io::{self, BufWriter};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{self, Receiver, SyncSender};
use std::thread::{self, JoinHandle};
use std::time::{SystemTime, UNIX_EPOCH};

static SHOT_SEQ: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PixelFormat {
    Rgba8,
    Bgra8,
    A2R10G10B10,
    A2B10G10R10,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawFrameCapture {
    pub width: u32,
    pub height: u32,
    pub format: PixelFormat,
    pub flip_y: bool,
    pub pixels: Vec<u8>,
}

/// Generates a traversal-safe `.png` destination path inside `dir` (falls back to `<XDG_PICTURES_DIR>/FPSHUD`).
#[must_use]
pub fn generate_screenshot_path(dir: &Path, prefix: &str) -> PathBuf {
    let target_dir = if dir.as_os_str().is_empty() {
        get_default_screenshot_dir()
    } else {
        dir.to_path_buf()
    };
    let clean_prefix = sanitize_filename_prefix(prefix);
    let ts = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs());
    let seq = SHOT_SEQ.fetch_add(1, Ordering::Relaxed);
    target_dir.join(format!("{clean_prefix}_{ts}_{seq}.png"))
}

/// Converts BGRA/flipped rows into tightly packed RGBA8 and writes PNG with `O_NOFOLLOW`.
pub fn write_png_to_path(path: &Path, frame: &RawFrameCapture) -> io::Result<()> {
    write_png_with_quality(path, frame, ScreenshotQuality::High)
}

/// Writes a normalized 100% native-resolution PNG to `path` using the requested quality tier.
pub fn write_png_with_quality(
    path: &Path,
    frame: &RawFrameCapture,
    quality: ScreenshotQuality,
) -> io::Result<()> {
    if frame.width == 0 || frame.height == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "screenshot dimensions must be non-zero",
        ));
    }

    let w = frame.width as usize;
    let h = frame.height as usize;
    let row_bytes = w
        .checked_mul(4)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "width overflow"))?;
    let expected_len = row_bytes
        .checked_mul(h)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "image size overflow"))?;
    if frame.pixels.len() != expected_len {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "pixel buffer mismatch: got {} bytes, expected {}",
                frame.pixels.len(),
                expected_len
            ),
        ));
    }

    if let Some(parent) = path.parent()
        && !parent.as_os_str().is_empty()
    {
        fs::create_dir_all(parent)?;
    }

    let mut normalized = vec![0_u8; expected_len];
    for dst_row in 0..h {
        let src_row = if frame.flip_y { h - 1 - dst_row } else { dst_row };
        let src_slice = &frame.pixels[src_row * row_bytes..(src_row + 1) * row_bytes];
        let dst_slice = &mut normalized[dst_row * row_bytes..(dst_row + 1) * row_bytes];
        match frame.format {
            PixelFormat::Rgba8 => dst_slice.copy_from_slice(src_slice),
            PixelFormat::Bgra8 => {
                for (src_px, dst_px) in src_slice
                    .as_chunks::<4>()
                    .0
                    .iter()
                    .zip(dst_slice.as_chunks_mut::<4>().0.iter_mut())
                {
                    dst_px[0] = src_px[2];
                    dst_px[1] = src_px[1];
                    dst_px[2] = src_px[0];
                    dst_px[3] = src_px[3];
                }
            }
            PixelFormat::A2R10G10B10 => {
                for (src_px, dst_px) in src_slice
                    .as_chunks::<4>()
                    .0
                    .iter()
                    .zip(dst_slice.as_chunks_mut::<4>().0.iter_mut())
                {
                    let v = u32::from_le_bytes(*src_px);
                    dst_px[0] = (((v >> 20) & 0x3FF) >> 2) as u8;
                    dst_px[1] = (((v >> 10) & 0x3FF) >> 2) as u8;
                    dst_px[2] = ((v & 0x3FF) >> 2) as u8;
                    dst_px[3] = 255;
                }
            }
            PixelFormat::A2B10G10R10 => {
                for (src_px, dst_px) in src_slice
                    .as_chunks::<4>()
                    .0
                    .iter()
                    .zip(dst_slice.as_chunks_mut::<4>().0.iter_mut())
                {
                    let v = u32::from_le_bytes(*src_px);
                    dst_px[0] = ((v & 0x3FF) >> 2) as u8;
                    dst_px[1] = (((v >> 10) & 0x3FF) >> 2) as u8;
                    dst_px[2] = (((v >> 20) & 0x3FF) >> 2) as u8;
                    dst_px[3] = 255;
                }
            }
        }
    }

    let pixel_count = (frame.width as usize).saturating_mul(frame.height as usize);
    let mut rgb = Vec::with_capacity(pixel_count.saturating_mul(3));

    for px in normalized.as_chunks::<4>().0 {
        let (r, g, b) = match quality {
            ScreenshotQuality::ExtraHigh | ScreenshotQuality::High => (px[0], px[1], px[2]),
            ScreenshotQuality::Medium => (
                (px[0] & 0xFC) | (px[0] >> 6),
                (px[1] & 0xFC) | (px[1] >> 6),
                (px[2] & 0xFC) | (px[2] >> 6),
            ),
            ScreenshotQuality::Low => (
                (px[0] & 0xF8) | (px[0] >> 5),
                (px[1] & 0xF8) | (px[1] >> 5),
                (px[2] & 0xF8) | (px[2] >> 5),
            ),
            ScreenshotQuality::ExtraLow => (
                (px[0] & 0xF0) | (px[0] >> 4),
                (px[1] & 0xF0) | (px[1] >> 4),
                (px[2] & 0xF0) | (px[2] >> 4),
            ),
        };
        rgb.push(r);
        rgb.push(g);
        rgb.push(b);
    }

    let fd = rustix_open(
        path,
        OFlags::WRONLY | OFlags::CREATE | OFlags::TRUNC | OFlags::NOFOLLOW | OFlags::CLOEXEC,
        Mode::from_bits_truncate(0o600),
    )
    .map_err(io::Error::from)?;
    let file = File::from(fd);

    let writer = BufWriter::new(file);
    let mut encoder = png::Encoder::new(writer, frame.width, frame.height);
    encoder.set_color(png::ColorType::Rgb);
    encoder.set_depth(png::BitDepth::Eight);
    if quality == ScreenshotQuality::ExtraHigh {
        encoder.set_compression(png::Compression::NoCompression);
    }
    let mut png_writer = encoder.write_header().map_err(io::Error::other)?;
    png_writer
        .write_image_data(&rgb)
        .map_err(io::Error::other)?;
    png_writer.finish().map_err(io::Error::other)?;
    Ok(())
}

/// Bounded asynchronous background encoder keeping the game render thread lock-free.
pub struct ScreenshotWorker {
    tx: Option<SyncSender<(RawFrameCapture, Option<PathBuf>, ScreenshotQuality)>>,
    saved_rx: Mutex<Receiver<String>>,
    handle: Option<JoinHandle<()>>,
}

impl ScreenshotWorker {
    #[must_use]
    pub fn new(save_dir: PathBuf) -> Self {
        let (tx, rx) = mpsc::sync_channel::<(RawFrameCapture, Option<PathBuf>, ScreenshotQuality)>(2);
        let (saved_tx, saved_rx) = mpsc::channel::<String>();
        let default_dir = if save_dir.as_os_str().is_empty() {
            get_default_screenshot_dir()
        } else {
            save_dir
        };
        let handle = thread::Builder::new()
            .name("fpshud_shot".to_string())
            .spawn(move || {
                while let Ok((frame, override_dir, quality)) = rx.recv() {
                    let dir = override_dir
                        .filter(|p| !p.as_os_str().is_empty())
                        .unwrap_or_else(|| default_dir.clone());
                    let target = generate_screenshot_path(&dir, "FPSHUD");
                    if write_png_with_quality(&target, &frame, quality).is_ok() {
                        let label = target
                            .file_name()
                            .and_then(|s| s.to_str())
                            .unwrap_or("screenshot.png")
                            .to_string();
                        let _ = saved_tx.send(label);
                    }
                }
            })
            .ok();

        Self {
            tx: Some(tx),
            saved_rx: Mutex::new(saved_rx),
            handle,
        }
    }

    pub fn submit(&self, frame: RawFrameCapture) {
        self.submit_to(frame, None, ScreenshotQuality::High);
    }

    pub fn submit_to(
        &self,
        frame: RawFrameCapture,
        save_dir: Option<PathBuf>,
        quality: ScreenshotQuality,
    ) {
        if let Some(tx) = &self.tx {
            let _ = tx.try_send((frame, save_dir, quality));
        }
    }

    /// Drains any completed screenshot filenames for in-HUD toast notifications.
    #[must_use]
    pub fn drain_saved_notifications(&self) -> Vec<String> {
        let mut out = Vec::new();
        if let Ok(rx) = self.saved_rx.lock() {
            while let Ok(name) = rx.try_recv() {
                out.push(name);
            }
        }
        out
    }
}

impl Drop for ScreenshotWorker {
    fn drop(&mut self) {
        drop(self.tx.take());
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}
