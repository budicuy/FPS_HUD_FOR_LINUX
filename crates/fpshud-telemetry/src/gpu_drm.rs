use crate::snapshot::{GpuBackendType, TelemetrySnapshot};
use std::fs;
use std::path::{Path, PathBuf};

/// Reads pure-sysfs DRM GPU metrics (AMD amdgpu, Intel i915/xe) matched by PCI ID.
pub struct GpuDrmProvider {
    root: PathBuf,
    preferred_vendor: Option<u32>,
    preferred_device: Option<u32>,
}

impl GpuDrmProvider {
    #[must_use]
    pub fn new(root: &Path) -> Self {
        Self {
            root: root.to_path_buf(),
            preferred_vendor: None,
            preferred_device: None,
        }
    }

    pub fn set_preferred_gpu(&mut self, vendor_id: u32, device_id: u32) {
        self.preferred_vendor = if vendor_id != 0 { Some(vendor_id) } else { None };
        self.preferred_device = if device_id != 0 { Some(device_id) } else { None };
    }

    pub fn poll(&self, snap: &mut TelemetrySnapshot) {
        let drm_dir = self.root.join("sys/class/drm");
        let Ok(entries) = fs::read_dir(drm_dir) else {
            return;
        };

        let mut candidates = Vec::new();
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            if !name.starts_with("card") || name.contains('-') {
                continue;
            }
            let dev_dir = entry.path().join("device");
            if !dev_dir.exists() {
                continue;
            }
            let vendor = read_hex_id(&dev_dir.join("vendor"));
            let device = read_hex_id(&dev_dir.join("device"));
            let exact_match = self.preferred_vendor.is_some()
                && vendor == self.preferred_vendor
                && (self.preferred_device.is_none() || device == self.preferred_device);
            candidates.push((exact_match, vendor.unwrap_or(0), entry.path(), dev_dir));
        }

        candidates.sort_by_key(|c| (!c.0, c.2.clone()));

        for (_matched, vendor, card_dir, dev_dir) in candidates {
            let busy = read_f32(&dev_dir.join("gpu_busy_percent"));
            let vram_used = read_u64(&dev_dir.join("mem_info_vram_used"));
            let vram_total = read_u64(&dev_dir.join("mem_info_vram_total"));
            let gt_cur_mhz = read_u64(&card_dir.join("gt_cur_freq_mhz"));

            if busy.is_none() && vram_total.is_none() && gt_cur_mhz.is_none() && self.preferred_vendor.is_none() {
                continue;
            }

            snap.gpu_available = true;
            snap.gpu_backend = GpuBackendType::DrmSysfs;
            snap.gpu_model = match vendor {
                0x1002 => "AMD Radeon".to_string(),
                0x8086 => "Intel Arc/UHD".to_string(),
                0x10de => "NVIDIA GPU".to_string(),
                _ => "DRM GPU".to_string(),
            };

            if let Some(load) = busy {
                snap.gpu_load = load.clamp(0.0, 100.0);
            }
            if let Some(used) = vram_used {
                snap.gpu_vram_used_gb = (used as f32) / (1024.0 * 1024.0 * 1024.0);
            }
            if let Some(total) = vram_total {
                snap.gpu_vram_total_gb = (total as f32) / (1024.0 * 1024.0 * 1024.0);
            }
            if let Some(mhz) = gt_cur_mhz.or_else(|| read_active_dpm_mhz(&dev_dir.join("pp_dpm_sclk"))) {
                snap.gpu_clock_mhz = mhz as u32;
            }
            if let Some(mclk) = read_active_dpm_mhz(&dev_dir.join("pp_dpm_mclk")) {
                snap.gpu_vram_clock_mhz = mclk as u32;
            }

            self.poll_drm_hwmon(&dev_dir.join("hwmon"), snap);
            return;
        }
    }

    fn poll_drm_hwmon(&self, hwmon_dir: &Path, snap: &mut TelemetrySnapshot) {
        let Ok(entries) = fs::read_dir(hwmon_dir) else {
            return;
        };
        for entry in entries.flatten() {
            let p = entry.path();
            if let Some(milli) = read_u64(&p.join("temp1_input")) {
                snap.gpu_temp_c = (milli as f32) / 1000.0;
            }
            if let Some(uw) =
                read_u64(&p.join("power1_average")).or_else(|| read_u64(&p.join("power1_input")))
            {
                snap.gpu_power_w = (uw as f32) / 1_000_000.0;
            }
            if let Some(fan) = read_u64(&p.join("fan1_input")) {
                let pct = if let Some(max_fan) = read_u64(&p.join("fan1_max"))
                    && max_fan > 0
                {
                    ((fan * 100) / max_fan).min(100)
                } else {
                    (fan / 40).min(100)
                };
                snap.gpu_fan_pct = pct as u32;
            }
            if snap.gpu_clock_mhz == 0
                && let Some(hz) = read_u64(&p.join("freq1_input"))
            {
                snap.gpu_clock_mhz = (hz / 1_000_000) as u32;
            }
        }
    }
}

fn read_u64(path: &Path) -> Option<u64> {
    fs::read_to_string(path).ok()?.trim().parse::<u64>().ok()
}

fn read_f32(path: &Path) -> Option<f32> {
    fs::read_to_string(path).ok()?.trim().parse::<f32>().ok()
}

fn read_hex_id(path: &Path) -> Option<u32> {
    let raw = fs::read_to_string(path).ok()?;
    let stripped = raw.trim().strip_prefix("0x").unwrap_or(raw.trim());
    u32::from_str_radix(stripped, 16).ok()
}

fn read_active_dpm_mhz(path: &Path) -> Option<u64> {
    let content = fs::read_to_string(path).ok()?;
    for line in content.lines() {
        if !line.contains('*') {
            continue;
        }
        for tok in line.split_whitespace() {
            if let Some(mhz_str) = tok.strip_suffix("Mhz").or_else(|| tok.strip_suffix("MHz"))
                && let Ok(mhz) = mhz_str.parse::<u64>()
            {
                return Some(mhz);
            }
        }
    }
    None
}
