//! Pure-Rust frametime analyzer (1% / 0.1% lows) and symlink-safe CSV benchmark recorder.

pub mod benchmark;
pub mod frametime;

pub use benchmark::{BenchmarkRecorder, BenchmarkSummary};
pub use frametime::{FrametimeAnalyzer, FrametimeMetrics};
