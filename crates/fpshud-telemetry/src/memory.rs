use crate::snapshot::TelemetrySnapshot;
use std::fs;
use std::path::{Path, PathBuf};

/// Reads `/proc/meminfo`, `/proc/self/statm`, and Linux PSI `/proc/pressure/memory`.
pub struct MemoryProvider {
    root: PathBuf,
    page_bytes: u64,
}

impl MemoryProvider {
    #[must_use]
    pub fn new(root: &Path) -> Self {
        Self {
            root: root.to_path_buf(),
            page_bytes: (rustix::param::page_size() as u64).max(4096),
        }
    }

    pub fn poll(&mut self, snap: &mut TelemetrySnapshot) {
        self.poll_meminfo(snap);
        self.poll_statm(snap);
        self.poll_psi(snap);
    }

    fn poll_meminfo(&self, snap: &mut TelemetrySnapshot) {
        let Ok(content) = fs::read_to_string(self.root.join("proc/meminfo")) else {
            return;
        };

        let mut mem_total_kb = 0_u64;
        let mut mem_avail_kb = 0_u64;
        let mut swap_total_kb = 0_u64;
        let mut swap_free_kb = 0_u64;

        for line in content.lines() {
            let Some((key, rest)) = line.split_once(':') else {
                continue;
            };
            let val = rest
                .split_whitespace()
                .next()
                .and_then(|s| s.parse::<u64>().ok())
                .unwrap_or(0);
            match key.trim() {
                "MemTotal" => mem_total_kb = val,
                "MemAvailable" => mem_avail_kb = val,
                "SwapTotal" => swap_total_kb = val,
                "SwapFree" => swap_free_kb = val,
                _ => {}
            }
        }

        if mem_total_kb > 0 {
            let used_kb = mem_total_kb.saturating_sub(mem_avail_kb);
            snap.ram_total_gb = (mem_total_kb as f32) / (1024.0 * 1024.0);
            snap.ram_used_gb = (used_kb as f32) / (1024.0 * 1024.0);
        }
        if swap_total_kb > 0 {
            let used_swap_kb = swap_total_kb.saturating_sub(swap_free_kb);
            snap.swap_total_gb = (swap_total_kb as f32) / (1024.0 * 1024.0);
            snap.swap_used_gb = (used_swap_kb as f32) / (1024.0 * 1024.0);
        }
    }

    fn poll_statm(&self, snap: &mut TelemetrySnapshot) {
        let Ok(content) = fs::read_to_string(self.root.join("proc/self/statm")) else {
            return;
        };
        let mut cols = content.split_whitespace();
        let _total_pages = cols.next();
        if let Some(rss_pages_str) = cols.next()
            && let Ok(rss_pages) = rss_pages_str.parse::<u64>()
        {
            let bytes = rss_pages.saturating_mul(self.page_bytes);
            snap.game_ram_gb = (bytes as f32) / (1024.0 * 1024.0 * 1024.0);
        }
    }

    fn poll_psi(&self, snap: &mut TelemetrySnapshot) {
        let Ok(content) = fs::read_to_string(self.root.join("proc/pressure/memory")) else {
            return;
        };
        for line in content.lines() {
            if !line.starts_with("some ") {
                continue;
            }
            for token in line.split_whitespace() {
                if let Some(raw) = token.strip_prefix("avg10=")
                    && let Ok(v) = raw.parse::<f32>()
                    && v.is_finite()
                {
                    snap.mem_psi_some_pct = v.max(0.0);
                    return;
                }
            }
        }
    }
}
