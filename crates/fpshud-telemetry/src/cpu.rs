use crate::snapshot::{CpuPowerSource, MAX_CPU_CORES, TelemetrySnapshot};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Instant;

#[derive(Debug, Clone, Copy, Default)]
struct CpuJiffies {
    busy: u64,
    total: u64,
}

/// 100% sudo-free CPU provider: per-core load, clock, temp, hwmon power, & DVFS PL1/PL2 curve.
pub struct CpuProvider {
    root: PathBuf,
    prev_total: Option<CpuJiffies>,
    prev_cores: Vec<CpuJiffies>,
    prev_rapl: Option<(u64, Instant)>,
    cached_model: Option<String>,
}

impl CpuProvider {
    #[must_use]
    pub fn new(root: &Path) -> Self {
        Self {
            root: root.to_path_buf(),
            prev_total: None,
            prev_cores: Vec::new(),
            prev_rapl: None,
            cached_model: None,
        }
    }

    pub fn poll(&mut self, snap: &mut TelemetrySnapshot) {
        if self.cached_model.is_none() {
            self.cached_model = read_cpu_model(&self.root);
        }
        if let Some(model) = &self.cached_model {
            snap.cpu_model = model.clone();
        }

        let core_ratios = self.poll_proc_stat(snap);
        let (core_freqs_khz, max_freqs_khz) = self.poll_frequencies(snap, core_ratios.len());
        self.poll_temperature(snap);

        // Tier 1: Direct world-readable hwmon power / VRM sensors
        if let Some(watts) = self.poll_hwmon_power() {
            snap.cpu_power_w = watts;
            snap.cpu_power_source = CpuPowerSource::DirectHwmon;
            return;
        }

        // Passive Tier 1b: readable energy_uj without requiring root setup
        if let Some(watts) = self.poll_passive_rapl() {
            snap.cpu_power_w = watts;
            snap.cpu_power_source = CpuPowerSource::DirectRapl;
            return;
        }

        // Tier 2: Multi-Core DVFS + PL1/PL2 world-readable physics model
        if let Some(watts) = self.estimate_dvfs_power(&core_ratios, &core_freqs_khz, &max_freqs_khz)
        {
            snap.cpu_power_w = watts;
            snap.cpu_power_source = CpuPowerSource::EstimatedDvfs;
        } else {
            snap.cpu_power_source = CpuPowerSource::Unavailable;
        }
    }

    fn poll_proc_stat(&mut self, snap: &mut TelemetrySnapshot) -> Vec<f32> {
        let Ok(content) = fs::read_to_string(self.root.join("proc/stat")) else {
            return Vec::new();
        };

        let mut total_load = snap.cpu_load;
        let mut core_ratios = Vec::new();

        for line in content.lines() {
            let mut cols = line.split_whitespace();
            let Some(label) = cols.next() else {
                continue;
            };
            if label == "cpu" {
                if let Some(cur) = parse_jiffies(cols) {
                    if let Some(prev) = self.prev_total {
                        total_load = delta_busy_pct(prev, cur);
                    }
                    self.prev_total = Some(cur);
                }
            } else if let Some(idx_str) = label.strip_prefix("cpu")
                && let Ok(idx) = idx_str.parse::<usize>()
                && idx < MAX_CPU_CORES
                && let Some(cur) = parse_jiffies(cols)
            {
                if self.prev_cores.len() <= idx {
                    self.prev_cores.resize(idx + 1, CpuJiffies::default());
                }
                let prev = self.prev_cores[idx];
                let ratio = if prev.total > 0 {
                    delta_busy_pct(prev, cur) / 100.0
                } else {
                    0.0
                };
                self.prev_cores[idx] = cur;
                if core_ratios.len() <= idx {
                    core_ratios.resize(idx + 1, 0.0);
                }
                core_ratios[idx] = ratio;
                snap.cpu_core_loads[idx] = ratio * 100.0;
            }
        }

        snap.cpu_load = total_load;
        snap.cpu_core_count = core_ratios.len().min(MAX_CPU_CORES) as u8;
        core_ratios
    }

    fn poll_frequencies(
        &self,
        snap: &mut TelemetrySnapshot,
        core_hint: usize,
    ) -> (Vec<u64>, Vec<u64>) {
        let count = core_hint.clamp(1, MAX_CPU_CORES);
        let mut cur_khz = Vec::with_capacity(count);
        let mut max_khz = Vec::with_capacity(count);
        let mut sum_mhz = 0_u64;
        let mut valid = 0_u64;

        for (idx, clock_slot) in snap.cpu_core_clocks_mhz.iter_mut().enumerate() {
            let base = self
                .root
                .join(format!("sys/devices/system/cpu/cpu{idx}/cpufreq"));
            let cur = read_u64(&base.join("scaling_cur_freq"));
            let max_f = read_u64(&base.join("cpuinfo_max_freq"));
            if cur.is_none() && max_f.is_none() && idx >= core_hint {
                break;
            }
            if let Some(khz) = cur {
                let mhz = (khz / 1000).min(u16::MAX as u64) as u16;
                *clock_slot = mhz;
                sum_mhz += u64::from(mhz);
                valid += 1;
            }
            cur_khz.push(cur.unwrap_or(0));
            max_khz.push(max_f.unwrap_or(0));
        }

        if let Some(avg_mhz) = sum_mhz.checked_div(valid) {
            snap.cpu_clock_mhz = avg_mhz as u32;
        }
        (cur_khz, max_khz)
    }

    fn poll_temperature(&self, snap: &mut TelemetrySnapshot) {
        let hwmon_root = self.root.join("sys/class/hwmon");
        let Ok(entries) = fs::read_dir(hwmon_root) else {
            return;
        };

        let mut fallback: Option<f32> = None;
        for entry in entries.flatten() {
            let dir = entry.path();
            let name = fs::read_to_string(dir.join("name"))
                .unwrap_or_default()
                .trim()
                .to_ascii_lowercase();
            let is_preferred = matches!(
                name.as_str(),
                "k10temp" | "zenpower" | "coretemp" | "fam15h_power"
            );
            let is_fallback = matches!(name.as_str(), "acpitz" | "thinkpad");
            if !is_preferred && !is_fallback {
                continue;
            }
            if let Some(milli) = read_u64(&dir.join("temp1_input")) {
                let deg = (milli as f32) / 1000.0;
                if deg > 0.0 && deg < 130.0 {
                    if is_preferred {
                        snap.cpu_temp_c = deg;
                        return;
                    }
                    fallback = Some(deg);
                }
            }
        }
        if let Some(deg) = fallback {
            snap.cpu_temp_c = deg;
        }
    }

    fn poll_hwmon_power(&self) -> Option<f32> {
        let hwmon_root = self.root.join("sys/class/hwmon");
        let entries = fs::read_dir(hwmon_root).ok()?;

        for entry in entries.flatten() {
            let dir = entry.path();
            let name = fs::read_to_string(dir.join("name"))
                .unwrap_or_default()
                .trim()
                .to_ascii_lowercase();
            if name == "amdgpu" || name == "nvme" || name.starts_with("bat") {
                continue;
            }
            let is_cpu_power = matches!(
                name.as_str(),
                "zenergy" | "zenpower" | "amd_energy" | "k10temp" | "coretemp" | "fam15h_power"
            );
            if !is_cpu_power {
                continue;
            }
            if let Some(uw) = read_u64(&dir.join("power1_average"))
                .or_else(|| read_u64(&dir.join("power1_input")))
            {
                let watts = (uw as f32) / 1_000_000.0;
                if watts > 0.1 && watts < 500.0 {
                    return Some(watts);
                }
            }
            if let (Some(mv), Some(ma)) = (
                read_u64(&dir.join("in0_input")),
                read_u64(&dir.join("curr1_input")),
            ) {
                let watts = ((mv as f64) * (ma as f64) / 1_000_000.0) as f32;
                if watts > 0.1 && watts < 500.0 {
                    return Some(watts);
                }
            }
        }
        None
    }

    fn poll_passive_rapl(&mut self) -> Option<f32> {
        let rapl_path = self.root.join("sys/class/powercap/intel-rapl:0/energy_uj");
        let energy = read_u64(&rapl_path)?;
        let now = Instant::now();
        if let Some((prev_energy, prev_time)) = self.prev_rapl {
            let dt = now.duration_since(prev_time).as_secs_f64();
            self.prev_rapl = Some((energy, now));
            if dt > 0.01 && energy >= prev_energy {
                let watts = ((energy - prev_energy) as f64 / 1_000_000.0 / dt) as f32;
                if watts > 0.1 && watts < 500.0 {
                    return Some(watts);
                }
            }
        } else {
            self.prev_rapl = Some((energy, now));
        }
        None
    }

    fn estimate_dvfs_power(
        &self,
        core_ratios: &[f32],
        cur_khz: &[u64],
        max_khz: &[u64],
    ) -> Option<f32> {
        if core_ratios.is_empty() {
            return None;
        }
        let (pl1_uw, pl2_uw) = self.read_powercap_limits();
        if cur_khz.iter().all(|&f| f == 0) && pl1_uw.is_none() && pl2_uw.is_none() {
            return None;
        }

        let pl1_w = pl1_uw.map_or(45.0_f32, |u| (u as f32) / 1_000_000.0).clamp(5.0, 350.0);
        let pl2_w = pl2_uw
            .map_or(pl1_w * 1.45, |u| (u as f32) / 1_000_000.0)
            .max(pl1_w)
            .clamp(5.0, 450.0);

        let idle_w = (pl1_w * 0.08).clamp(2.5, 25.0);
        let dynamic_budget = (pl1_w - idle_w).max(1.0);
        let per_core_budget = dynamic_budget / (core_ratios.len() as f32);

        let mut dyn_sum = 0.0_f32;
        for (idx, &util) in core_ratios.iter().enumerate() {
            let f_cur = cur_khz.get(idx).copied().unwrap_or(0) as f32;
            let f_max = max_khz.get(idx).copied().unwrap_or(0) as f32;
            let freq_factor = if f_cur > 0.0 && f_max > 0.0 {
                let f_base = (f_max * 0.80).max(1.0);
                (f_cur / f_base).clamp(0.25, 1.50).powf(2.3)
            } else {
                1.0
            };
            dyn_sum += util.clamp(0.0, 1.0) * per_core_budget * freq_factor;
        }

        Some((idle_w + dyn_sum).clamp(idle_w, pl2_w))
    }

    fn read_powercap_limits(&self) -> (Option<u64>, Option<u64>) {
        let dir = self.root.join("sys/class/powercap");
        let Ok(entries) = fs::read_dir(dir) else {
            return (None, None);
        };

        for entry in entries.flatten() {
            let p = entry.path();
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.starts_with("intel-rapl:") && !name.matches(':').nth(1).is_some() {
                let pl1 = read_u64(&p.join("constraint_0_max_power_uw"))
                    .or_else(|| read_u64(&p.join("constraint_0_power_limit_uw")));
                let pl2 = read_u64(&p.join("constraint_1_power_limit_uw"));
                if pl1.is_some() || pl2.is_some() {
                    return (pl1, pl2);
                }
            }
        }
        (None, None)
    }
}

fn parse_jiffies<'a>(mut cols: impl Iterator<Item = &'a str>) -> Option<CpuJiffies> {
    let user = cols.next()?.parse::<u64>().ok()?;
    let nice = cols.next()?.parse::<u64>().ok()?;
    let system = cols.next()?.parse::<u64>().ok()?;
    let idle = cols.next()?.parse::<u64>().ok()?;
    let iowait = cols.next().and_then(|s| s.parse::<u64>().ok()).unwrap_or(0);
    let irq = cols.next().and_then(|s| s.parse::<u64>().ok()).unwrap_or(0);
    let softirq = cols.next().and_then(|s| s.parse::<u64>().ok()).unwrap_or(0);
    let steal = cols.next().and_then(|s| s.parse::<u64>().ok()).unwrap_or(0);

    let busy = user + nice + system + irq + softirq + steal;
    let total = busy + idle + iowait;
    Some(CpuJiffies { busy, total })
}

fn delta_busy_pct(prev: CpuJiffies, cur: CpuJiffies) -> f32 {
    let dt_total = cur.total.saturating_sub(prev.total);
    let dt_busy = cur.busy.saturating_sub(prev.busy);
    if dt_total == 0 {
        return 0.0;
    }
    ((dt_busy as f64) * 100.0 / (dt_total as f64)).clamp(0.0, 100.0) as f32
}

fn read_u64(path: &Path) -> Option<u64> {
    fs::read_to_string(path).ok()?.trim().parse::<u64>().ok()
}

fn read_cpu_model(root: &Path) -> Option<String> {
    let content = fs::read_to_string(root.join("proc/cpuinfo")).ok()?;
    for line in content.lines() {
        if let Some((_, val)) = line.split_once(':')
            && line.trim_start().starts_with("model name")
        {
            return Some(val.trim().to_string());
        }
    }
    None
}
