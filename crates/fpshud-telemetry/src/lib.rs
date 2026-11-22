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
use fpshud_core::MetricType;
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

/// Selective category bitmask controlling which OS/vendor providers are polled.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SensorPollMask {
    pub cpu: bool,
    pub gpu: bool,
    pub ram: bool,
    pub system: bool,
}

impl SensorPollMask {
    #[must_use]
    pub const fn all() -> Self {
        Self {
            cpu: true,
            gpu: true,
            ram: true,
            system: true,
        }
    }

    #[must_use]
    pub fn from_metrics(metrics: &[MetricType], gui_open: bool) -> Self {
        if gui_open {
            return Self::all();
        }
        let mut mask = Self {
            cpu: false,
            gpu: false,
            ram: false,
            system: false,
        };
        for &m in metrics {
            match m {
                MetricType::Fps
                | MetricType::FpsAvg
                | MetricType::Fps1Low
                | MetricType::Fps01Low
                | MetricType::FrameTimes
                | MetricType::EngineApi => {}
                MetricType::GpuLoad
                | MetricType::GpuClock
                | MetricType::GpuVramClock
                | MetricType::GpuVram
                | MetricType::GpuBandwidth
                | MetricType::GpuTemp
                | MetricType::GpuFan
                | MetricType::GpuPower
                | MetricType::GpuModel => mask.gpu = true,
                MetricType::CpuLoad
                | MetricType::CpuAllCoreLoad
                | MetricType::CpuClock
                | MetricType::CpuAllCoreClock
                | MetricType::CpuPower
                | MetricType::CpuTemp
                | MetricType::CpuModel => mask.cpu = true,
                MetricType::Ram
                | MetricType::GameRam
                | MetricType::SwapRam
                | MetricType::MemPressure => mask.ram = true,
                MetricType::SystemClock
                | MetricType::Battery
                | MetricType::DiskIo
                | MetricType::DiskTemp
                | MetricType::NetSpeed => mask.system = true,
            }
        }
        mask
    }
}

struct Providers {
    #[cfg(feature = "vendor-gpu-apis")]
    root: PathBuf,
    mask: SensorPollMask,
    cpu: CpuProvider,
    mem: MemoryProvider,
    gpu: GpuDrmProvider,
    #[cfg(feature = "vendor-gpu-apis")]
    nvml: Option<vendor::NvidiaNvmlProvider>,
    #[cfg(feature = "vendor-gpu-apis")]
    nvml_attempted: bool,
    #[cfg(feature = "vendor-gpu-apis")]
    preferred_gpu: (u32, u32),
    sys: SystemProvider,
}

impl Providers {
    fn new(root: &Path, target_pid: Option<u32>) -> Self {
        Self {
            #[cfg(feature = "vendor-gpu-apis")]
            root: root.to_path_buf(),
            mask: SensorPollMask::all(),
            cpu: CpuProvider::new(root),
            mem: MemoryProvider::new_for_pid(root, target_pid),
            gpu: GpuDrmProvider::new(root),
            #[cfg(feature = "vendor-gpu-apis")]
            nvml: None,
            #[cfg(feature = "vendor-gpu-apis")]
            nvml_attempted: false,
            #[cfg(feature = "vendor-gpu-apis")]
            preferred_gpu: (0, 0),
            sys: SystemProvider::new(root),
        }
    }

    fn set_active_gpu(&mut self, vendor_id: u32, device_id: u32) {
        self.gpu.set_preferred_gpu(vendor_id, device_id);
        #[cfg(feature = "vendor-gpu-apis")]
        {
            self.preferred_gpu = (vendor_id, device_id);
            self.nvml = None;
            self.nvml_attempted = false;
        }
    }

    fn poll_into(&mut self, snap: &mut TelemetrySnapshot) {
        if self.mask.cpu {
            self.cpu.poll(snap);
        }
        if self.mask.ram {
            self.mem.poll(snap);
        }
        if self.mask.gpu {
            #[cfg(feature = "vendor-gpu-apis")]
            let nvml_ok = {
                if !self.nvml_attempted {
                    self.nvml_attempted = true;
                    let (vid, did) = self.preferred_gpu;
                    self.nvml = vendor::NvidiaNvmlProvider::init_for_root(&self.root, vid, did);
                }
                self.nvml.as_mut().is_some_and(|n| n.poll(snap))
            };
            #[cfg(not(feature = "vendor-gpu-apis"))]
            let nvml_ok = false;
            if !nvml_ok {
                self.gpu.poll(snap);
            }
        }
        if self.mask.system {
            self.sys.poll(snap);
        }
    }
}

/// Thread-safe background telemetry collector with synchronous mock-root support for tests.
pub struct TelemetryCollector {
    snapshot: Arc<RwLock<TelemetrySnapshot>>,
    providers: Arc<Mutex<Providers>>,
    frametime: Mutex<(Option<Instant>, FrametimeAnalyzer)>,
    benchmark: Mutex<BenchmarkRecorder>,
    benchmark_active: AtomicBool,
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
        Self::build(PathBuf::from("/"), None, false)
    }

    #[must_use]
    pub fn new_for_pid(target_pid: Option<u32>) -> Self {
        Self::build(PathBuf::from("/"), target_pid, false)
    }

    #[must_use]
    pub fn with_mock_root(root: PathBuf) -> Self {
        Self::build(root, None, true)
    }

    pub fn set_poll_mask(&self, mask: SensorPollMask) {
        if let Ok(mut p) = self.providers.lock() {
            p.mask = mask;
        }
    }

    fn build(root: PathBuf, target_pid: Option<u32>, mock_mode: bool) -> Self {
        let providers = Arc::new(Mutex::new(Providers::new(&root, target_pid)));
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
            let worker_snap = TelemetrySnapshot::default();
            thread::Builder::new()
                .name("fpshud_telemetry".to_string())
                .spawn(move || {
                    let mut next = worker_snap;
                    while !stop_clone.load(Ordering::Relaxed) {
                        if let Ok(mut p) = p_clone.lock() {
                            p.poll_into(&mut next);
                        }
                        if let Ok(mut guard) = s_clone.write() {
                            next.render_api.clone_from(&guard.render_api);
                            next.benchmark_active = guard.benchmark_active;
                            next.benchmark_elapsed_sec = guard.benchmark_elapsed_sec;
                            next.last_benchmark = guard.last_benchmark.clone();
                            *guard = next.clone();
                        }
                        thread::sleep(Duration::from_millis(500));
                    }
                })
                .ok()
        };

        Self {
            snapshot,
            providers,
            frametime: Mutex::new((None, FrametimeAnalyzer::new())),
            benchmark: Mutex::new(BenchmarkRecorder::default()),
            benchmark_active: AtomicBool::new(false),
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
        if let Ok(guard) = self.snapshot.read()
            && guard.render_api == api
        {
            return;
        }
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

    /// Ultra-lightweight per-frame hot path called from Vulkan/OpenGL Present.
    /// Does zero sorting, zero heap allocation, and zero RwLock writes during normal gaming.
    pub fn on_frame(&self) {
        let now = Instant::now();
        let dt_sec = if let Ok(mut ft_guard) = self.frametime.lock() {
            let (last, ft) = &mut *ft_guard;
            let dt = last
                .map_or(1.0 / 60.0, |prev| now.duration_since(prev).as_secs_f32())
                .clamp(0.000_05, 0.25);
            *last = Some(now);
            ft.record_frame(dt);
            dt
        } else {
            return;
        };

        if self.benchmark_active.load(Ordering::Relaxed)
            && let Ok(mut bench) = self.benchmark.lock()
            && bench.is_active()
        {
            let (cpu_load, gpu_load) = self
                .snapshot
                .read()
                .map(|s| (s.cpu_load, s.gpu_load))
                .unwrap_or((0.0, 0.0));
            let fps_inst = if dt_sec > 0.0 { 1.0 / dt_sec } else { 0.0 };
            bench.push_sample(fps_inst, dt_sec * 1000.0, cpu_load, gpu_load);
        }
    }

    pub fn toggle_benchmark(&self) {
        let dir = &self.benchmark_dir;
        if let Ok(mut bench) = self.benchmark.lock() {
            if bench.is_active() {
                self.benchmark_active.store(false, Ordering::Relaxed);
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
                self.benchmark_active.store(true, Ordering::Relaxed);
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
        let mut snap = self
            .snapshot
            .read()
            .map(|g| g.clone())
            .unwrap_or_default();

        if let Ok(ft_guard) = self.frametime.lock() {
            let (_, ft) = &*ft_guard;
            let metrics = ft.compute_metrics();
            snap.fps = metrics.fps;
            snap.fps_avg = metrics.fps_avg;
            snap.fps_1pct_low = metrics.fps_1pct_low;
            snap.fps_01pct_low = metrics.fps_01pct_low;
            snap.frametime_ms = metrics.frametime_ms;
            snap.frametime_history = ft
                .history_slice()
                .iter()
                .copied()
                .take(GRAPH_HISTORY_SIZE)
                .collect();
        }
        if snap.benchmark_active
            && let Ok(bs) = self.bench_started_at.lock()
        {
            snap.benchmark_elapsed_sec = bs.map_or(0.0, |t| t.elapsed().as_secs_f32());
        }
        snap
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
