use crate::snapshot::{GpuBackendType, TelemetrySnapshot};
use nvml_wrapper::Nvml;
use nvml_wrapper::enum_wrappers::device::{Clock, TemperatureSensor};
use std::ffi::OsStr;
use std::fs;
use std::path::Path;

/// Checks sysfs `vendor` & `power/runtime_status` to avoid waking a D3cold-suspended NVIDIA GPU.
#[must_use]
pub fn is_nvidia_d3cold_suspended(root: &Path) -> bool {
    let mut has_nvidia = false;
    let mut all_suspended = true;

    for rel in ["sys/bus/pci/devices", "sys/class/drm"] {
        let Ok(entries) = fs::read_dir(root.join(rel)) else {
            continue;
        };
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            let dev_dir = if rel == "sys/class/drm" {
                if !name.starts_with("card") || name.contains('-') {
                    continue;
                }
                entry.path().join("device")
            } else {
                entry.path()
            };
            let Ok(vendor_raw) = fs::read_to_string(dev_dir.join("vendor")) else {
                continue;
            };
            let stripped = vendor_raw.trim().strip_prefix("0x").unwrap_or(vendor_raw.trim());
            let Ok(vendor_id) = u32::from_str_radix(stripped, 16) else {
                continue;
            };
            if vendor_id != 0x10de {
                continue;
            }
            has_nvidia = true;
            let status = fs::read_to_string(dev_dir.join("power/runtime_status"))
                .unwrap_or_default()
                .trim()
                .to_ascii_lowercase();
            if !status.contains("suspended") {
                all_suspended = false;
            }
        }
    }

    has_nvidia && all_suspended
}

#[must_use]
pub fn compute_nvml_bandwidth_gbs(mem_clock_mhz: u32, bus_width_bits: u32, mem_util_pct: u32) -> f32 {
    let max_bw_gbs =
        (mem_clock_mhz as f32) * 2.0 * ((bus_width_bits as f32) / 8.0) / 1000.0;
    max_bw_gbs * ((mem_util_pct as f32) / 100.0)
}

pub struct NvidiaNvmlProvider {
    nvml: Nvml,
    device_index: u32,
    gpu_name: String,
    bus_width_bits: u32,
}

impl NvidiaNvmlProvider {
    pub fn init_for_root(root: &Path, vendor_id: u32, device_id: u32) -> Option<Self> {
        if root != Path::new("/") {
            return None;
        }
        if vendor_id != 0 && vendor_id != 0x10de {
            return None;
        }
        if is_nvidia_d3cold_suspended(root) && vendor_id != 0x10de {
            return None;
        }

        const CANDIDATES: [&str; 4] = [
            "libnvidia-ml.so.1",
            "libnvidia-ml.so",
            "/run/host/usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1",
            "/run/host/usr/lib64/libnvidia-ml.so.1",
        ];
        let nvml = CANDIDATES
            .iter()
            .find_map(|path| Nvml::builder().lib_path(OsStr::new(path)).init().ok())?;

        let dev_count = nvml.device_count().unwrap_or(1).clamp(1, 8);

        let mut best_index: Option<u32> = None;
        let mut best_vram = 0_u64;
        for idx in 0..dev_count {
            let Ok(dev) = nvml.device_by_index(idx) else {
                continue;
            };
            if device_id != 0
                && let Ok(pci) = dev.pci_info()
            {
                let pci_dev_id = pci.pci_device_id;
                if (pci_dev_id >> 16) == device_id || (pci_dev_id & 0xffff) == device_id {
                    best_index = Some(idx);
                    break;
                }
            }
            let total_vram = dev.memory_info().map_or(0, |m| m.total);
            if best_index.is_none() || total_vram > best_vram {
                best_index = Some(idx);
                best_vram = total_vram;
            }
        }

        let device_index = best_index?;
        let dev = nvml.device_by_index(device_index).ok()?;
        let gpu_name = dev
            .name()
            .ok()
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| "NVIDIA GPU".to_string());
        let bus_width_bits = dev.memory_bus_width().unwrap_or(0);

        Some(Self {
            nvml,
            device_index,
            gpu_name,
            bus_width_bits,
        })
    }

    pub fn poll(&mut self, snap: &mut TelemetrySnapshot) -> bool {
        let Ok(dev) = self.nvml.device_by_index(self.device_index) else {
            return false;
        };

        snap.gpu_available = true;
        snap.gpu_backend = GpuBackendType::Nvml;
        snap.gpu_model = self.gpu_name.clone();

        let mut mem_util_pct = 0_u32;
        let mut ok_util = false;
        if let Ok(util) = dev.utilization_rates() {
            snap.gpu_load = (util.gpu as f32).clamp(0.0, 100.0);
            mem_util_pct = util.memory.min(100);
            ok_util = true;
        }

        if let Ok(mem) = dev.memory_info() {
            snap.gpu_vram_total_gb = (mem.total as f32) / (1024.0 * 1024.0 * 1024.0);
            snap.gpu_vram_used_gb = (mem.used as f32) / (1024.0 * 1024.0 * 1024.0);
        }

        if let Ok(gr_mhz) = dev.clock_info(Clock::Graphics) {
            snap.gpu_clock_mhz = gr_mhz;
        }
        if let Ok(mem_mhz) = dev.clock_info(Clock::Memory) {
            snap.gpu_vram_clock_mhz = mem_mhz;
            if self.bus_width_bits > 0 && ok_util {
                snap.gpu_bandwidth_gbs =
                    compute_nvml_bandwidth_gbs(mem_mhz, self.bus_width_bits, mem_util_pct);
            }
        }

        if let Ok(temp) = dev.temperature(TemperatureSensor::Gpu) {
            snap.gpu_temp_c = temp as f32;
        }

        if let Ok(fan) = dev.fan_speed(0) {
            snap.gpu_fan_pct = fan.min(100);
        }

        if let Ok(mw) = dev.power_usage() {
            snap.gpu_power_w = (mw as f32) / 1000.0;
        }

        true
    }
}
