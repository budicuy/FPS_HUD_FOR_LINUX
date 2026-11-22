#![forbid(unsafe_code)]
//! 100% sudo-free Linux telemetry collector covering all 31 FPSHUD metrics.

pub mod cpu;
pub mod gpu_drm;
pub mod memory;
pub mod snapshot;
pub mod system;
#[cfg(feature = "vendor-gpu-apis")]
pub mod vendor;

pub use snapshot::{CpuPowerSource, GpuBackendType, MAX_CPU_CORES, TelemetrySnapshot};

use cpu::CpuProvider;
use fpshud_analytics::{BenchmarkRecorder, FrametimeAnalyzer, frametime::GRAPH_HISTORY_SIZE};
use fpshud_core::paths::get_default_record_dir;
use gpu_drm::GpuDrmProvider;
use memory::MemoryProvider;
use std::path::{Path, PathBuf};
use std::sync::{
    Arc, Mutex, RwLock,
    atomic::{AtomicBool, Ordering},
};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};
use system::SystemProvider;

struct Providers {
    #[cfg(feature = "vendor-gpu-apis")]
    root: PathBuf,
    cpu: CpuProvider,
    mem: MemoryProvider,
    gpu: GpuDrmProvider,
    #[cfg(feature = "vendor-gpu-apis")]
    nvml: Option<vendor::NvidiaNvmlProvider>,
    sys: SystemProvider,
}

impl Providers {
    fn new(root: &Path) -> Self {
        Self {
            #[cfg(feature = "vendor-gpu-apis")]
            root: root.to_path_buf(),
            cpu: CpuProvider::new(root),
            mem: MemoryProvider::new(root),
            gpu: GpuDrmProvider::new(root),
            #[cfg(feature = "vendor-gpu-apis")]
            nvml: vendor::NvidiaNvmlProvider::init_for_root(root, 0, 0),
            sys: SystemProvider::new(root),
        }
    }

    fn set_active_gpu(&mut self, vendor_id: u32, device_id: u32) {
        self.gpu.set_preferred_gpu(vendor_id, device_id);
        #[cfg(feature = "vendor-gpu-apis")]
        {
            self.nvml = vendor::NvidiaNvmlProvider::init_for_root(&self.root, vendor_id, device_id);
        }
    }

    fn poll_into(&mut self, snap: &mut TelemetrySnapshot) {
        self.cpu.poll(snap);
        self.mem.poll(snap);
        #[cfg(feature = "vendor-gpu-apis")]
        let nvml_ok = self.nvml.as_mut().is_some_and(|n| n.poll(snap));
        #[cfg(not(feature = "vendor-gpu-apis"))]
        let nvml_ok = false;
        if !nvml_ok {
            self.gpu.poll(snap);
        }
        self.sys.poll(snap);
    }
}

/// Thread-safe background telemetry collector with synchronous mock-root support for tests.
pub struct TelemetryCollector {
    snapshot: Arc<RwLock<TelemetrySnapshot>>,
    providers: Arc<Mutex<Providers>>,
    frametime: Mutex<FrametimeAnalyzer>,
    benchmark: Mutex<BenchmarkRecorder>,
    last_frame_at: Mutex<Option<Instant>>,
    bench_started_at: Mutex<Option<Instant>>,
    benchmark_dir: PathBuf,
    mock_mode: bool,
    stop_flag: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

impl Default for TelemetryCollector {
    fn default() -> Self {
        Self::new()
    }
}

impl TelemetryCollector {
    #[must_use]
    pub fn new() -> Self {
        Self::build(PathBuf::from("/"), false)
    }

    #[must_use]
    pub fn with_mock_root(root: PathBuf) -> Self {
        Self::build(root, true)
    }

    fn build(root: PathBuf, mock_mode: bool) -> Self {
        let providers = Arc::new(Mutex::new(Providers::new(&root)));
        let snapshot = Arc::new(RwLock::new(TelemetrySnapshot::default()));
        let stop_flag = Arc::new(AtomicBool::new(false));

        if mock_mode
            && let (Ok(mut p), Ok(mut s)) = (providers.lock(), snapshot.write())
        {
            p.poll_into(&mut s);
        }

        let worker = if mock_mode {
            None
        } else {
            let p_clone = Arc::clone(&providers);
            let s_clone = Arc::clone(&snapshot);
            let stop_clone = Arc::clone(&stop_flag);
            thread::Builder::new()
                .name("fpshud_telemetry".to_string())
                .spawn(move || {
                    while !stop_clone.load(Ordering::Relaxed) {
                        let mut next = {
                            s_clone
                                .read()
                                .map(|g| g.clone())
                                .unwrap_or_default()
                        };
                        if let Ok(mut p) = p_clone.lock() {
                            p.poll_into(&mut next);
                        }
                        if let Ok(mut guard) = s_clone.write() {
                            next.fps = guard.fps;
                            next.fps_avg = guard.fps_avg;
                            next.fps_1pct_low = guard.fps_1pct_low;
                            next.fps_01pct_low = guard.fps_01pct_low;
                            next.frametime_ms = guard.frametime_ms;
                            next.frametime_history = guard.frametime_history.clone();
                            next.render_api = guard.render_api.clone();
                            next.benchmark_active = guard.benchmark_active;
                            next.benchmark_elapsed_sec = guard.benchmark_elapsed_sec;
                            next.last_benchmark = guard.last_benchmark.clone();
                            *guard = next;
                        }
                        thread::sleep(Duration::from_millis(250));
                    }
                })
                .ok()
        };

        Self {
            snapshot,
            providers,
            frametime: Mutex::new(FrametimeAnalyzer::new()),
            benchmark: Mutex::new(BenchmarkRecorder::default()),
            last_frame_at: Mutex::new(None),
            bench_started_at: Mutex::new(None),
            benchmark_dir: if mock_mode {
                root.join("bench")
            } else {
                get_default_record_dir()
            },
            mock_mode,
            stop_flag,
            worker,
        }
    }

    pub fn set_render_api(&self, api: &str) {
        if let Ok(mut s) = self.snapshot.write() {
            s.render_api = api.to_string();
        }
    }

    pub fn notify_active_gpu(&self, vendor_id: u32, device_id: u32) {
        if let Ok(mut p) = self.providers.lock() {
            p.set_active_gpu(vendor_id, device_id);
            if self.mock_mode
                && let Ok(mut s) = self.snapshot.write()
            {
                p.poll_into(&mut s);
            }
        }
    }

    pub fn on_frame(&self) {
        let now = Instant::now();
        let dt_sec = if let Ok(mut last) = self.last_frame_at.lock() {
            let dt = last.map_or(1.0 / 60.0, |prev| now.duration_since(prev).as_secs_f32());
            *last = Some(now);
            dt.clamp(0.000_05, 0.25)
        } else {
            1.0 / 60.0
        };

        let (metrics, hist) = if let Ok(mut ft) = self.frametime.lock() {
            ft.record_frame(dt_sec);
            let m = ft.compute_metrics();
            let hist: Vec<f32> = ft
                .history_slice()
                .iter()
                .copied()
                .take(GRAPH_HISTORY_SIZE)
                .collect();
            (m, hist)
        } else {
            return;
        };

        let (cpu_load, gpu_load) = self
            .snapshot
            .read()
            .map(|s| (s.cpu_load, s.gpu_load))
            .unwrap_or((0.0, 0.0));

        if let Ok(mut bench) = self.benchmark.lock()
            && bench.is_active()
        {
            bench.push_sample(metrics.fps, dt_sec * 1000.0, cpu_load, gpu_load);
        }

        if let Ok(mut s) = self.snapshot.write() {
            s.fps = metrics.fps;
            s.fps_avg = metrics.fps_avg;
            s.fps_1pct_low = metrics.fps_1pct_low;
            s.fps_01pct_low = metrics.fps_01pct_low;
            s.frametime_ms = metrics.frametime_ms;
            s.frametime_history = hist;
            if let Ok(bs) = self.bench_started_at.lock() {
                s.benchmark_elapsed_sec = bs.map_or(0.0, |t| t.elapsed().as_secs_f32());
            }
        }
    }

    pub fn toggle_benchmark(&self) {
        let dir = &self.benchmark_dir;
        if let Ok(mut bench) = self.benchmark.lock() {
            if bench.is_active() {
                let summary = bench.stop();
                if let Ok(mut bs) = self.bench_started_at.lock() {
                    *bs = None;
                }
                if let Ok(mut s) = self.snapshot.write() {
                    s.benchmark_active = false;
                    s.benchmark_elapsed_sec = 0.0;
                    if summary.is_some() {
                        s.last_benchmark = summary;
                    }
                }
            } else {
                bench.start(dir);
                if let Ok(mut bs) = self.bench_started_at.lock() {
                    *bs = Some(Instant::now());
                }
                if let Ok(mut s) = self.snapshot.write() {
                    s.benchmark_active = true;
                }
            }
        }
    }

    #[must_use]
    pub fn get_snapshot(&self) -> TelemetrySnapshot {
        self.snapshot
            .read()
            .map(|g| g.clone())
            .unwrap_or_default()
    }
}

impl Drop for TelemetryCollector {
    fn drop(&mut self) {
        self.stop_flag.store(true, Ordering::Relaxed);
        if let Some(handle) = self.worker.take() {
            let _ = handle.join();
        }
    }
}
