//! Pure-Rust PNG screenshot capture encoder and non-blocking worker thread.

pub mod writer;

pub use writer::{
    PixelFormat, RawFrameCapture, ScreenshotWorker, generate_screenshot_path, write_png_to_path,
    write_png_with_quality,
};
