use fpshud_core::paths::{get_default_record_dir, sanitize_filename_prefix};
use std::fs::{self, OpenOptions};
use std::io::{self, BufWriter};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{self, SyncSender};
use std::thread::{self, JoinHandle};
use std::time::{SystemTime, UNIX_EPOCH};

static SHOT_SEQ: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PixelFormat {
    Rgba8,
    Bgra8,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawFrameCapture {
    pub width: u32,
    pub height: u32,
    pub format: PixelFormat,
    pub flip_y: bool,
    pub pixels: Vec<u8>,
}

/// Generates a traversal-safe `.png` destination path inside `dir` (falls back to `~/Videos/FPSHUD`).
#[must_use]
pub fn generate_screenshot_path(dir: &Path, prefix: &str) -> PathBuf {
    let target_dir = if dir.as_os_str().is_empty() {
        get_default_record_dir()
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
        }
    }

    let file = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .mode(0o600)
        .open(path)?;

    let writer = BufWriter::new(file);
    let mut encoder = png::Encoder::new(writer, frame.width, frame.height);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    let mut png_writer = encoder.write_header().map_err(io::Error::other)?;
    png_writer
        .write_image_data(&normalized)
        .map_err(io::Error::other)?;
    png_writer.finish().map_err(io::Error::other)?;
    Ok(())
}

/// Bounded asynchronous background encoder keeping the game render thread lock-free.
pub struct ScreenshotWorker {
    tx: Option<SyncSender<RawFrameCapture>>,
    handle: Option<JoinHandle<()>>,
}

impl ScreenshotWorker {
    #[must_use]
    pub fn new(save_dir: PathBuf) -> Self {
        let (tx, rx) = mpsc::sync_channel::<RawFrameCapture>(2);
        let dir = if save_dir.as_os_str().is_empty() {
            get_default_record_dir()
        } else {
            save_dir
        };
        let handle = thread::Builder::new()
            .name("fpshud_shot".to_string())
            .spawn(move || {
                while let Ok(frame) = rx.recv() {
                    let target = generate_screenshot_path(&dir, "FPSHUD");
                    let _ = write_png_to_path(&target, &frame);
                }
            })
            .ok();

        Self {
            tx: Some(tx),
            handle,
        }
    }

    pub fn submit(&self, frame: RawFrameCapture) {
        if let Some(tx) = &self.tx {
            let _ = tx.try_send(frame);
        }
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
