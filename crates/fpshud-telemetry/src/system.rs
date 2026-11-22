use crate::snapshot::TelemetrySnapshot;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Instant;

/// Reads battery, NVMe temp, `/proc/diskstats` I/O, `/proc/net/dev` RX/TX, & local time.
/// Caches static sysfs device paths (`power_supply`, `hwmon`) once on first poll.
pub struct SystemProvider {
    root: PathBuf,
    probed: bool,
    battery_dir: Option<PathBuf>,
    nvme_temp_path: Option<PathBuf>,
    prev_disk: Option<(u64, u64, Instant)>,
    prev_net: Option<(u64, u64, Instant)>,
}

impl SystemProvider {
    #[must_use]
    pub fn new(root: &Path) -> Self {
        Self {
            root: root.to_path_buf(),
            probed: false,
            battery_dir: None,
            nvme_temp_path: None,
            prev_disk: None,
            prev_net: None,
        }
    }

    fn ensure_probed(&mut self) {
        if self.probed {
            return;
        }
        self.probed = true;
        self.battery_dir = discover_battery_dir(&self.root);
        self.nvme_temp_path = discover_nvme_temp_path(&self.root);
    }

    pub fn poll(&mut self, snap: &mut TelemetrySnapshot) {
        self.ensure_probed();
        self.poll_battery(snap);
        self.poll_nvme_temp(snap);
        self.poll_disk_io(snap);
        self.poll_net_io(snap);
        snap.system_time = current_hh_mm();
    }

    fn poll_battery(&self, snap: &mut TelemetrySnapshot) {
        let Some(p) = &self.battery_dir else {
            return;
        };
        if let Some(cap) = read_f32(&p.join("capacity")) {
            snap.battery_available = true;
            snap.battery_pct = cap.clamp(0.0, 100.0);
            let status = fs::read_to_string(p.join("status"))
                .unwrap_or_default()
                .trim()
                .to_ascii_lowercase();
            snap.battery_charging = status == "charging" || status == "full";
            if let Some(uw) = read_f32(&p.join("power_now")) {
                snap.battery_power_w = (uw / 1_000_000.0).max(0.0);
            } else if let (Some(ua), Some(uv)) = (
                read_f32(&p.join("current_now")),
                read_f32(&p.join("voltage_now")),
            ) {
                snap.battery_power_w = ((ua * uv) / 1e12).max(0.0);
            }
        }
    }

    fn poll_nvme_temp(&self, snap: &mut TelemetrySnapshot) {
        let Some(p) = &self.nvme_temp_path else {
            return;
        };
        if let Some(milli) = read_f32(p) {
            snap.nvme_temp_available = true;
            snap.nvme_temp_c = milli / 1000.0;
        }
    }

    fn poll_disk_io(&mut self, snap: &mut TelemetrySnapshot) {
        let Ok(content) = fs::read_to_string(self.root.join("proc/diskstats")) else {
            return;
        };
        let mut read_sectors = 0_u64;
        let mut write_sectors = 0_u64;
        for line in content.lines() {
            let mut cols = line.split_whitespace();
            let Some(dev) = cols.nth(2) else {
                continue;
            };
            if dev.starts_with("loop") || dev.starts_with("ram") || dev.starts_with("zram") {
                continue;
            }
            let Some(r_str) = cols.nth(2) else {
                continue;
            };
            let Some(w_str) = cols.nth(3) else {
                continue;
            };
            read_sectors += r_str.parse::<u64>().unwrap_or(0);
            write_sectors += w_str.parse::<u64>().unwrap_or(0);
        }

        let now = Instant::now();
        if let Some((pr, pw, pt)) = self.prev_disk {
            let dt = now.duration_since(pt).as_secs_f64();
            if dt > 0.01 {
                snap.disk_read_mbs = ((read_sectors.saturating_sub(pr) as f64) * 512.0
                    / (1024.0 * 1024.0)
                    / dt) as f32;
                snap.disk_write_mbs = ((write_sectors.saturating_sub(pw) as f64) * 512.0
                    / (1024.0 * 1024.0)
                    / dt) as f32;
            }
        }
        self.prev_disk = Some((read_sectors, write_sectors, now));
    }

    fn poll_net_io(&mut self, snap: &mut TelemetrySnapshot) {
        let Ok(content) = fs::read_to_string(self.root.join("proc/net/dev")) else {
            return;
        };
        let mut rx_bytes = 0_u64;
        let mut tx_bytes = 0_u64;
        for line in content.lines() {
            let Some((iface, rest)) = line.split_once(':') else {
                continue;
            };
            if iface.trim() == "lo" {
                continue;
            }
            let mut cols = rest.split_whitespace();
            let Some(rx_str) = cols.next() else {
                continue;
            };
            let Some(tx_str) = cols.nth(7) else {
                continue;
            };
            rx_bytes += rx_str.parse::<u64>().unwrap_or(0);
            tx_bytes += tx_str.parse::<u64>().unwrap_or(0);
        }

        let now = Instant::now();
        if let Some((prx, ptx, pt)) = self.prev_net {
            let dt = now.duration_since(pt).as_secs_f64();
            if dt > 0.01 {
                snap.net_rx_mbs =
                    ((rx_bytes.saturating_sub(prx) as f64) / (1024.0 * 1024.0) / dt) as f32;
                snap.net_tx_mbs =
                    ((tx_bytes.saturating_sub(ptx) as f64) / (1024.0 * 1024.0) / dt) as f32;
            }
        }
        self.prev_net = Some((rx_bytes, tx_bytes, now));
    }
}

fn discover_battery_dir(root: &Path) -> Option<PathBuf> {
    let entries = fs::read_dir(root.join("sys/class/power_supply")).ok()?;
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        if !name.starts_with("BAT") {
            continue;
        }
        let p = entry.path();
        if read_f32(&p.join("capacity")).is_some() {
            return Some(p);
        }
    }
    None
}

fn discover_nvme_temp_path(root: &Path) -> Option<PathBuf> {
    let entries = fs::read_dir(root.join("sys/class/hwmon")).ok()?;
    for entry in entries.flatten() {
        let p = entry.path();
        let name = fs::read_to_string(p.join("name"))
            .unwrap_or_default()
            .trim()
            .to_ascii_lowercase();
        if name != "nvme" && name != "drivetemp" {
            continue;
        }
        let temp = p.join("temp1_input");
        if read_f32(&temp).is_some() {
            return Some(temp);
        }
    }
    None
}

fn read_f32(path: &Path) -> Option<f32> {
    fs::read_to_string(path).ok()?.trim().parse::<f32>().ok()
}

fn current_hh_mm() -> String {
    chrono::Local::now().format("%H:%M").to_string()
}
