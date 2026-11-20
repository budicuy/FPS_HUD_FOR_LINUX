pub const FRAME_WINDOW_SIZE: usize = 2048;
pub const GRAPH_HISTORY_SIZE: usize = 120;
const ROLLING_WINDOW_MAX_MS: f64 = 3500.0;

/// Instantaneous and rolling percentile frametime statistics.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct FrametimeMetrics {
    pub fps: f32,
    pub fps_avg: f32,
    pub fps_1pct_low: f32,
    pub fps_01pct_low: f32,
    pub frametime_ms: f32,
    pub min_fps: f32,
    pub max_fps: f32,
}

/// Computes time-weighted `1%` and `0.1%` low FPS from frametimes sorted descending (worst first).
#[must_use]
pub(crate) fn compute_time_weighted_lows(sorted_desc_ms: &[f32], total_ms: f64) -> (f32, f32) {
    if sorted_desc_ms.is_empty() || total_ms <= 0.0001 {
        return (0.0, 0.0);
    }

    let calc_tail = |tail_ratio: f64| -> f32 {
        let target_ms = total_ms * tail_ratio;
        let mut accum_ms = 0.0_f64;
        let mut weighted_sum = 0.0_f64;

        for &sample in sorted_desc_ms {
            let ft = f64::from(sample);
            if accum_ms + ft >= target_ms {
                let rem = target_ms - accum_ms;
                weighted_sum += rem * ft;
                accum_ms = target_ms;
                break;
            }
            weighted_sum += ft * ft;
            accum_ms += ft;
        }

        if accum_ms <= 0.0001 {
            return if sorted_desc_ms[0] > 0.0001 {
                1000.0 / sorted_desc_ms[0]
            } else {
                0.0
            };
        }
        let denom = weighted_sum / accum_ms;
        if denom > 0.0001 {
            (1000.0 / denom) as f32
        } else {
            0.0
        }
    };

    let low_1 = calc_tail(0.01);
    let low_01 = if sorted_desc_ms.len() >= 4 {
        let worst_blend_ms = sorted_desc_ms[0] * 0.65 + sorted_desc_ms[1] * 0.35;
        let blended_fps = if worst_blend_ms > 0.0001 {
            1000.0 / worst_blend_ms
        } else {
            0.0
        };
        let tw_01 = calc_tail(0.002);
        low_1.min(blended_fps * 0.5 + tw_01 * 0.5)
    } else {
        low_1.min(calc_tail(0.001))
    };

    (low_1, low_01.min(low_1))
}

/// Fixed-capacity ring buffer computing EMA FPS, percentiles, and graph sparkline samples.
#[derive(Debug, Clone)]
pub struct FrametimeAnalyzer {
    ring: Vec<f32>,
    ring_pos: usize,
    ring_count: usize,
    ema_ms: f32,
    history: Vec<f32>,
}

impl Default for FrametimeAnalyzer {
    fn default() -> Self {
        Self::new()
    }
}

impl FrametimeAnalyzer {
    #[must_use]
    pub fn new() -> Self {
        Self {
            ring: vec![0.0; FRAME_WINDOW_SIZE],
            ring_pos: 0,
            ring_count: 0,
            ema_ms: 0.0,
            history: Vec::with_capacity(GRAPH_HISTORY_SIZE),
        }
    }

    pub fn record_frame(&mut self, dt_sec: f32) {
        if !dt_sec.is_finite() || dt_sec <= 0.000_01 || dt_sec > 5.0 {
            return;
        }
        let dt_ms = dt_sec * 1000.0;
        if dt_ms > 250.0 {
            self.ring_pos = 0;
            self.ring_count = 0;
            self.ema_ms = 0.0;
            return;
        }

        self.ema_ms = if self.ema_ms <= 0.0 {
            dt_ms
        } else {
            self.ema_ms * 0.90 + dt_ms * 0.10
        };

        self.ring[self.ring_pos] = dt_ms;
        self.ring_pos = (self.ring_pos + 1) % FRAME_WINDOW_SIZE;
        if self.ring_count < FRAME_WINDOW_SIZE {
            self.ring_count += 1;
        }

        if self.history.len() == GRAPH_HISTORY_SIZE {
            self.history.remove(0);
        }
        self.history.push(self.ema_ms);
    }

    #[must_use]
    pub fn compute_metrics(&self) -> FrametimeMetrics {
        if self.ring_count == 0 {
            return FrametimeMetrics::default();
        }

        let mut recent = Vec::with_capacity(self.ring_count);
        let mut total_ms = 0.0_f64;
        let mut min_ft = f32::MAX;
        let mut max_ft = 0.0_f32;

        for step in 0..self.ring_count {
            let idx = (self.ring_pos + FRAME_WINDOW_SIZE - 1 - step) % FRAME_WINDOW_SIZE;
            let ft = self.ring[idx];
            recent.push(ft);
            total_ms += f64::from(ft);
            min_ft = min_ft.min(ft);
            max_ft = max_ft.max(ft);
            if total_ms >= ROLLING_WINDOW_MAX_MS && recent.len() >= 30 {
                break;
            }
        }

        let fps = if self.ema_ms > 0.001 {
            1000.0 / self.ema_ms
        } else {
            0.0
        };
        let fps_avg = if total_ms > 0.001 {
            ((recent.len() as f64) * 1000.0 / total_ms) as f32
        } else {
            0.0
        };

        recent.sort_by(|a, b| b.total_cmp(a));
        let (fps_1pct_low, fps_01pct_low) = compute_time_weighted_lows(&recent, total_ms);

        FrametimeMetrics {
            fps,
            fps_avg,
            fps_1pct_low,
            fps_01pct_low,
            frametime_ms: self.ema_ms,
            min_fps: if max_ft > 0.001 { 1000.0 / max_ft } else { 0.0 },
            max_fps: if min_ft > 0.001 && min_ft < f32::MAX {
                1000.0 / min_ft
            } else {
                0.0
            },
        }
    }

    #[must_use]
    pub fn history_slice(&self) -> &[f32] {
        &self.history
    }
}
