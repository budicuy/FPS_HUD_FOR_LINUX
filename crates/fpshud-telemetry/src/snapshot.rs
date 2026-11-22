use fpshud_analytics::BenchmarkSummary;
use fpshud_core::MetricType;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum CpuPowerSource {
    #[default]
    Unavailable,
    DirectHwmon,
    DirectRapl,
    EstimatedDvfs,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum GpuBackendType {
    #[default]
    Unknown,
    DrmSysfs,
    Nvml,
    LevelZero,
}

pub const MAX_CPU_CORES: usize = 64;

/// Consolidated read-only telemetry snapshot covering all 31 FPSHUD metrics cleanly.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TelemetrySnapshot {
    pub fps: f32,
    pub fps_avg: f32,
    pub fps_1pct_low: f32,
    pub fps_01pct_low: f32,
    pub frametime_ms: f32,
    pub frametime_history: Vec<f32>,

    pub gpu_available: bool,
    pub gpu_backend: GpuBackendType,
    pub gpu_load: f32,
    pub gpu_clock_mhz: u32,
    pub gpu_vram_clock_mhz: u32,
    pub gpu_vram_used_gb: f32,
    pub gpu_vram_total_gb: f32,
    pub gpu_bandwidth_gbs: f32,
    pub gpu_temp_c: f32,
    pub gpu_fan_pct: u32,
    pub gpu_power_w: f32,

    pub cpu_load: f32,
    pub cpu_clock_mhz: u32,
    pub cpu_temp_c: f32,
    pub cpu_power_w: f32,
    pub cpu_power_source: CpuPowerSource,
    pub cpu_core_loads: Vec<f32>,
    pub cpu_core_clocks_mhz: Vec<u16>,

    pub ram_used_gb: f32,
    pub ram_total_gb: f32,
    pub game_ram_gb: f32,
    pub swap_used_gb: f32,
    pub swap_total_gb: f32,
    pub mem_psi_some_pct: f32,

    pub battery_available: bool,
    pub battery_pct: f32,
    pub battery_power_w: f32,
    pub battery_charging: bool,

    pub disk_read_mbs: f32,
    pub disk_write_mbs: f32,
    pub nvme_temp_c: f32,
    pub nvme_temp_available: bool,
    pub net_rx_mbs: f32,
    pub net_tx_mbs: f32,

    pub render_api: String,
    pub cpu_model: String,
    pub gpu_model: String,
    pub system_time: String,

    pub benchmark_active: bool,
    pub benchmark_elapsed_sec: f32,
    pub last_benchmark: Option<BenchmarkSummary>,
}

impl Default for TelemetrySnapshot {
    fn default() -> Self {
        Self {
            fps: 0.0,
            fps_avg: 0.0,
            fps_1pct_low: 0.0,
            fps_01pct_low: 0.0,
            frametime_ms: 0.0,
            frametime_history: Vec::new(),
            gpu_available: false,
            gpu_backend: GpuBackendType::Unknown,
            gpu_load: 0.0,
            gpu_clock_mhz: 0,
            gpu_vram_clock_mhz: 0,
            gpu_vram_used_gb: 0.0,
            gpu_vram_total_gb: 0.0,
            gpu_bandwidth_gbs: 0.0,
            gpu_temp_c: 0.0,
            gpu_fan_pct: 0,
            gpu_power_w: 0.0,
            cpu_load: 0.0,
            cpu_clock_mhz: 0,
            cpu_temp_c: 0.0,
            cpu_power_w: 0.0,
            cpu_power_source: CpuPowerSource::Unavailable,
            cpu_core_loads: Vec::new(),
            cpu_core_clocks_mhz: Vec::new(),
            ram_used_gb: 0.0,
            ram_total_gb: 0.0,
            game_ram_gb: 0.0,
            swap_used_gb: 0.0,
            swap_total_gb: 0.0,
            mem_psi_some_pct: -1.0,
            battery_available: false,
            battery_pct: 0.0,
            battery_power_w: 0.0,
            battery_charging: false,
            disk_read_mbs: 0.0,
            disk_write_mbs: 0.0,
            nvme_temp_c: 0.0,
            nvme_temp_available: false,
            net_rx_mbs: 0.0,
            net_tx_mbs: 0.0,
            render_api: "VULKAN".to_string(),
            cpu_model: "CPU".to_string(),
            gpu_model: "GPU".to_string(),
            system_time: "00:00".to_string(),
            benchmark_active: false,
            benchmark_elapsed_sec: 0.0,
            last_benchmark: None,
        }
    }
}

impl TelemetrySnapshot {
    /// Formats a metric value concisely; marks DVFS-estimated CPU wattage with a leading `~`.
    #[must_use]
    pub fn format_metric_value(&self, metric: MetricType) -> String {
        match metric {
            MetricType::Fps => format!("{:.0}", self.fps),
            MetricType::FpsAvg => format!("{:.0}", self.fps_avg),
            MetricType::Fps1Low => format!("{:.0}", self.fps_1pct_low),
            MetricType::Fps01Low => format!("{:.0}", self.fps_01pct_low),
            MetricType::FrameTimes => format!("{:.1} ms", self.frametime_ms),
            MetricType::GpuLoad => format!("{:.0}%", self.gpu_load),
            MetricType::GpuClock => format!("{} MHz", self.gpu_clock_mhz),
            MetricType::GpuVramClock => format!("{} MHz", self.gpu_vram_clock_mhz),
            MetricType::GpuVram => {
                format!("{:.1}/{:.1} GB", self.gpu_vram_used_gb, self.gpu_vram_total_gb)
            }
            MetricType::GpuBandwidth => format!("{:.1} GB/s", self.gpu_bandwidth_gbs),
            MetricType::GpuTemp => format!("{:.0}°C", self.gpu_temp_c),
            MetricType::GpuFan => format!("{}%", self.gpu_fan_pct),
            MetricType::GpuPower => format!("{:.1} W", self.gpu_power_w),
            MetricType::CpuLoad => format!("{:.0}%", self.cpu_load),
            MetricType::CpuAllCoreLoad => format!(
                "{} cores ({:.0}%)",
                self.cpu_core_loads.len(),
                self.cpu_load
            ),
            MetricType::CpuClock => format!("{} MHz", self.cpu_clock_mhz),
            MetricType::CpuAllCoreClock => format!("{} MHz avg", self.cpu_clock_mhz),
            MetricType::CpuPower => match self.cpu_power_source {
                CpuPowerSource::Unavailable => "N/A".to_string(),
                CpuPowerSource::EstimatedDvfs
                | CpuPowerSource::DirectHwmon
                | CpuPowerSource::DirectRapl => {
                    format!("{:.1} W", self.cpu_power_w.max(0.0))
                }
            },
            MetricType::CpuTemp => format!("{:.0}°C", self.cpu_temp_c),
            MetricType::Ram => format!("{:.1}/{:.1} GB", self.ram_used_gb, self.ram_total_gb),
            MetricType::GameRam => format!("{:.1} GB", self.game_ram_gb),
            MetricType::SwapRam => format!("{:.1}/{:.1} GB", self.swap_used_gb, self.swap_total_gb),
            MetricType::MemPressure => {
                if self.mem_psi_some_pct < 0.0 {
                    "N/A".to_string()
                } else {
                    format!("{:.1}%", self.mem_psi_some_pct)
                }
            }
            MetricType::EngineApi => self.render_api.clone(),
            MetricType::GpuModel => self.gpu_model.clone(),
            MetricType::CpuModel => self.cpu_model.clone(),
            MetricType::SystemClock => self.system_time.clone(),
            MetricType::Battery => {
                if !self.battery_available {
                    "N/A".to_string()
                } else {
                    format!("{:.0}% ({:.1}W)", self.battery_pct, self.battery_power_w)
                }
            }
            MetricType::DiskIo => format!("{:.1}/{:.1} MB/s", self.disk_read_mbs, self.disk_write_mbs),
            MetricType::DiskTemp => {
                if self.nvme_temp_available {
                    format!("{:.0}°C", self.nvme_temp_c)
                } else {
                    "N/A".to_string()
                }
            }
            MetricType::NetSpeed => format!("{:.1}/{:.1} MB/s", self.net_rx_mbs, self.net_tx_mbs),
        }
    }
}
