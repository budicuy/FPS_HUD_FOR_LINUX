use fpshud_core::RecordResolution;

/// Maximum memory budget for the Vulkan/OpenGL capture staging ring (100 MiB).
pub const MAX_CAPTURE_RING_BYTES: usize = 100 * 1024 * 1024;

/// Computes even-aligned, aspect-preserving target dimensions without ever upscaling.
#[must_use]
pub fn compute_target_dimensions(src_w: u32, src_h: u32, preset: RecordResolution) -> (u32, u32) {
    let w = src_w.max(2);
    let h = src_h.max(2);
    let (raw_w, raw_h) = match preset.max_height() {
        Some(max_h) if h > max_h => {
            let scaled_w = ((u64::from(w) * u64::from(max_h)) / u64::from(h)) as u32;
            (scaled_w.max(2), max_h)
        }
        _ => (w, h),
    };
    ((raw_w & !1).max(2), (raw_h & !1).max(2))
}

/// Computes adaptive ring slot count (2..=4) bounded by `max_ring_bytes`.
#[must_use]
pub fn adaptive_ring_slot_count(frame_bytes: usize, max_ring_bytes: usize) -> usize {
    if frame_bytes == 0 {
        return 4;
    }
    (max_ring_bytes / frame_bytes).clamp(2, 4)
}

/// Deadline-grid frame pacer tolerant to 12.5% VSync jitter.
#[derive(Debug, Clone)]
pub struct FramePacer {
    interval_ns: u64,
    tolerance_ns: u64,
    next_deadline_ns: Option<u64>,
}

impl FramePacer {
    #[must_use]
    pub fn new(target_fps: u32) -> Self {
        let fps = u64::from(target_fps.clamp(1, 240));
        let interval_ns = 1_000_000_000 / fps;
        Self {
            interval_ns,
            tolerance_ns: interval_ns / 8, // 12.5% VSync jitter tolerance
            next_deadline_ns: None,
        }
    }

    pub fn reset(&mut self, target_fps: u32) {
        *self = Self::new(target_fps);
    }

    #[must_use]
    pub fn should_capture(&mut self, now_ns: u64) -> bool {
        let Some(deadline) = self.next_deadline_ns else {
            self.next_deadline_ns = Some(now_ns.saturating_add(self.interval_ns));
            return true;
        };

        if now_ns.saturating_add(self.tolerance_ns) < deadline {
            return false;
        }

        let mut next = deadline.saturating_add(self.interval_ns);
        if next.saturating_add(self.interval_ns) < now_ns {
            next = now_ns.saturating_add(self.interval_ns);
        }
        self.next_deadline_ns = Some(next);
        true
    }
}
