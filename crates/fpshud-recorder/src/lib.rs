//! Pure-Rust bundled FFmpeg (`n9.1`) orchestrator for FPSHUD screen recording,
//! media library scanning, and 1-process built-in video playback.

pub mod capture;
pub mod library;
pub mod locate;
pub mod pacer;
pub mod player;
pub mod probe;
pub mod session;
pub mod yuv;
