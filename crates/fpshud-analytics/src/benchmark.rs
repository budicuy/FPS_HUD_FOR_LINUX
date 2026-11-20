use crate::frametime::compute_time_weighted_lows;
use fpshud_core::paths::get_default_record_dir;
use std::fs::{self, File, OpenOptions};
use std::io::{self, BufWriter, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

const MAX_BENCHMARK_SAMPLES: usize = 360_000;

#[derive(Debug, Clone, PartialEq)]
pub struct BenchmarkSummary {
    pub avg_fps: f32,
    pub fps_1pct_low: f32,
    pub fps_01pct_low: f32,
    pub min_fps: f32,
    pub max_fps: f32,
    pub total_frames: u64,
    pub duration_sec: f32,
    pub csv_path: PathBuf,
}

#[derive(Debug, Clone, Copy)]
struct SampleRow {
    fps: f32,
    dt_ms: f32,
    cpu_load: f32,
    gpu_load: f32,
}

/// Records per-frame telemetry samples and exports a symlink-safe CSV summary report.
#[derive(Debug, Default)]
pub struct BenchmarkRecorder {
    active: bool,
    started_at: Option<Instant>,
    target_dir: PathBuf,
    samples: Vec<SampleRow>,
}

impl BenchmarkRecorder {
    #[must_use]
    pub fn is_active(&self) -> bool {
        self.active
    }

    /// Opens a CSV file strictly with `O_NOFOLLOW | O_CLOEXEC` (`0600`) to prevent symlink attacks.
    pub fn open_safe_csv(path: &Path) -> io::Result<File> {
        if let Some(parent) = path.parent()
            && !parent.as_os_str().is_empty()
        {
            fs::create_dir_all(parent)?;
        }
        OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .mode(0o600)
            .open(path)
    }

    pub fn start(&mut self, dir: &Path) {
        self.target_dir = if dir.as_os_str().is_empty() {
            get_default_record_dir()
        } else {
            dir.to_path_buf()
        };
        self.samples.clear();
        self.started_at = Some(Instant::now());
        self.active = true;
    }

    pub fn push_sample(&mut self, fps: f32, dt_ms: f32, cpu_load: f32, gpu_load: f32) {
        if !self.active || !dt_ms.is_finite() || dt_ms <= 0.0001 {
            return;
        }
        if self.samples.len() < MAX_BENCHMARK_SAMPLES {
            self.samples.push(SampleRow {
                fps: if fps.is_finite() { fps } else { 1000.0 / dt_ms },
                dt_ms,
                cpu_load: if cpu_load.is_finite() { cpu_load } else { 0.0 },
                gpu_load: if gpu_load.is_finite() { gpu_load } else { 0.0 },
            });
        }
    }

    pub fn stop(&mut self) -> Option<BenchmarkSummary> {
        if !self.active {
            return None;
        }
        self.active = false;
        let samples = std::mem::take(&mut self.samples);
        let duration_sec = self
            .started_at
            .take()
            .map_or(0.0, |t| t.elapsed().as_secs_f32());

        if samples.is_empty() {
            return None;
        }

        let mut total_ms = 0.0_f64;
        let mut min_ft = f32::MAX;
        let mut max_ft = 0.0_f32;
        let mut sorted_ft: Vec<f32> = Vec::with_capacity(samples.len());
        for s in &samples {
            total_ms += f64::from(s.dt_ms);
            min_ft = min_ft.min(s.dt_ms);
            max_ft = max_ft.max(s.dt_ms);
            sorted_ft.push(s.dt_ms);
        }

        let mean_ft = (total_ms / samples.len() as f64) as f32;
        let avg_fps = if mean_ft > 0.001 { 1000.0 / mean_ft } else { 0.0 };
        let max_fps = if min_ft > 0.001 && min_ft < f32::MAX {
            1000.0 / min_ft
        } else {
            0.0
        };
        let min_fps = if max_ft > 0.001 { 1000.0 / max_ft } else { 0.0 };

        sorted_ft.sort_by(|a, b| b.total_cmp(a));
        let (fps_1pct_low, fps_01pct_low) = compute_time_weighted_lows(&sorted_ft, total_ms);

        let stamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |d| d.as_secs());
        let csv_path = self
            .target_dir
            .join(format!("FPSHUD_Benchmark_{stamp}.csv"));

        if let Ok(file) = Self::open_safe_csv(&csv_path) {
            let mut writer = BufWriter::new(file);
            let _ = writeln!(writer, "Frame,FPS,Frametime_MS,CPU_Load_Pct,GPU_Load_Pct");
            for (idx, row) in samples.iter().enumerate() {
                let _ = writeln!(
                    writer,
                    "{},{:.2},{:.3},{:.1},{:.1}",
                    idx + 1,
                    row.fps,
                    row.dt_ms,
                    row.cpu_load,
                    row.gpu_load
                );
            }
            let _ = writeln!(
                writer,
                "# Summary,Avg_FPS={avg_fps:.2},1Pct_Low={fps_1pct_low:.2},0.1Pct_Low={fps_01pct_low:.2},Duration_Sec={duration_sec:.2}"
            );
            let _ = writer.flush();
        }

        Some(BenchmarkSummary {
            avg_fps,
            fps_1pct_low,
            fps_01pct_low,
            min_fps,
            max_fps,
            total_frames: samples.len() as u64,
            duration_sec,
            csv_path,
        })
    }
}
