use std::sync::{Arc, Mutex, OnceLock};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecorderPixelFormat {
    Rgba8,
    Bgra8,
    A2R10G10B10,
    A2B10G10R10,
    R16G16B16A16Sfloat,
}

impl RecorderPixelFormat {
    #[must_use]
    pub const fn bytes_per_pixel(self) -> usize {
        match self {
            Self::R16G16B16A16Sfloat => 8,
            _ => 4,
        }
    }

    #[must_use]
    pub const fn is_direct_8bit(self) -> bool {
        matches!(self, Self::Rgba8 | Self::Bgra8)
    }

    #[must_use]
    pub const fn ffmpeg_pix_fmt(self) -> &'static str {
        match self {
            Self::Bgra8 => "bgra",
            _ => "rgba",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ColorTransfer {
    Sdr,
    PqHdr10,
    LinearHdr,
}

fn linear_to_srgb_u8(v: f32) -> u8 {
    let c = v.clamp(0.0, 1.0);
    let s = if c <= 0.003_130_8 {
        c * 12.92
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    };
    (s.clamp(0.0, 1.0) * 255.0).round() as u8
}

fn pq_10bit_lut() -> &'static [u8; 1024] {
    static LUT: OnceLock<[u8; 1024]> = OnceLock::new();
    LUT.get_or_init(|| {
        let mut table = [0_u8; 1024];
        const M1: f64 = 0.159_301_757_812_5;
        const M2: f64 = 78.843_75;
        const C1: f64 = 0.835_937_5;
        const C2: f64 = 18.851_562_5;
        const C3: f64 = 18.687_5;
        for (i, slot) in table.iter_mut().enumerate() {
            let n = (i as f64) / 1023.0;
            let p = n.powf(1.0 / M2);
            let num = (p - C1).max(0.0);
            let den = (C2 - C3 * p).max(1e-9);
            let nits_norm = (num / den).powf(1.0 / M1);
            // Simple Reinhard tonemap targeting 203-nit SDR reference white
            let l = (nits_norm * 10_000.0 / 203.0) as f32;
            let mapped = l / (1.0 + l);
            *slot = linear_to_srgb_u8(mapped);
        }
        table
    })
}

fn fp16_to_f32(bits: u16) -> f32 {
    let sign = (bits >> 15) & 1;
    let exp = (bits >> 10) & 0x1F;
    let mant = bits & 0x03FF;
    if exp == 31 {
        if mant != 0 {
            return f32::NAN;
        }
        return if sign == 0 {
            f32::INFINITY
        } else {
            f32::NEG_INFINITY
        };
    }
    let val = if exp == 0 {
        (f32::from(mant) / 1024.0) * 2.0_f32.powi(-14)
    } else {
        (1.0 + f32::from(mant) / 1024.0) * 2.0_f32.powi(i32::from(exp) - 15)
    };
    if sign == 1 { -val } else { val }
}

fn fp16_sdr_lut() -> &'static [u8; 65_536] {
    static LUT: OnceLock<Box<[u8; 65_536]>> = OnceLock::new();
    LUT.get_or_init(|| {
        let mut table = vec![0_u8; 65_536].into_boxed_slice();
        for bits in 0..=u16::MAX {
            let f = fp16_to_f32(bits);
            let out = if f.is_nan() || f <= 0.0 {
                0
            } else if f.is_infinite() {
                255
            } else {
                let mapped = f / (1.0 + f);
                linear_to_srgb_u8(mapped)
            };
            table[usize::from(bits)] = out;
        }
        table.try_into().expect("65536 entries")
    })
}

/// Converts HDR 10-bit or FP16 frames into 8-bit RGBA (`dst` is resized and overwritten).
pub fn convert_frame_to_rgba8(
    src: &[u8],
    width: u32,
    height: u32,
    fmt: RecorderPixelFormat,
    transfer: ColorTransfer,
    dst: &mut Vec<u8>,
) {
    let pixel_count = (width as usize).saturating_mul(height as usize);
    dst.resize(pixel_count.saturating_mul(4), 0);

    match fmt {
        RecorderPixelFormat::Rgba8 | RecorderPixelFormat::Bgra8 => {
            let len = dst.len().min(src.len());
            dst[..len].copy_from_slice(&src[..len]);
        }
        RecorderPixelFormat::A2B10G10R10 | RecorderPixelFormat::A2R10G10B10 => {
            let pq = (transfer == ColorTransfer::PqHdr10).then(pq_10bit_lut);
            let swap_rb = fmt == RecorderPixelFormat::A2R10G10B10;
            for (i, chunk) in src.as_chunks::<4>().0.iter().take(pixel_count).enumerate() {
                let v = u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
                let c0 = (v & 0x3FF) as usize;
                let c1 = ((v >> 10) & 0x3FF) as usize;
                let c2 = ((v >> 20) & 0x3FF) as usize;
                let (r10, g10, b10) = if swap_rb { (c2, c1, c0) } else { (c0, c1, c2) };
                let (r, g, b) = if let Some(lut) = pq {
                    (lut[r10], lut[g10], lut[b10])
                } else {
                    ((r10 >> 2) as u8, (g10 >> 2) as u8, (b10 >> 2) as u8)
                };
                let o = i * 4;
                dst[o] = r;
                dst[o + 1] = g;
                dst[o + 2] = b;
                dst[o + 3] = 255;
            }
        }
        RecorderPixelFormat::R16G16B16A16Sfloat => {
            let lut = fp16_sdr_lut();
            for (i, chunk) in src.as_chunks::<8>().0.iter().take(pixel_count).enumerate() {
                let r = lut[usize::from(u16::from_le_bytes([chunk[0], chunk[1]]))];
                let g = lut[usize::from(u16::from_le_bytes([chunk[2], chunk[3]]))];
                let b = lut[usize::from(u16::from_le_bytes([chunk[4], chunk[5]]))];
                let o = i * 4;
                dst[o] = r;
                dst[o + 1] = g;
                dst[o + 2] = b;
                dst[o + 3] = 255;
            }
        }
    }
}

/// Bounded buffer pool to recycle staging pixel buffers without repeated heap allocations.
#[derive(Debug, Clone)]
pub struct PixelBufferPool {
    inner: Arc<Mutex<Vec<Vec<u8>>>>,
    max_buffers: usize,
}

impl PixelBufferPool {
    #[must_use]
    pub fn new(max_buffers: usize) -> Self {
        Self {
            inner: Arc::new(Mutex::new(Vec::with_capacity(max_buffers))),
            max_buffers: max_buffers.max(1),
        }
    }

    #[must_use]
    pub fn take(&self, byte_len: usize) -> Vec<u8> {
        if let Ok(mut guard) = self.inner.lock()
            && let Some(mut buf) = guard.pop()
        {
            buf.clear();
            if buf.capacity() < byte_len {
                buf.reserve(byte_len - buf.capacity());
            }
            return buf;
        }
        Vec::with_capacity(byte_len)
    }

    pub fn put(&self, mut buf: Vec<u8>) {
        if let Ok(mut guard) = self.inner.lock()
            && guard.len() < self.max_buffers
        {
            buf.clear();
            guard.push(buf);
        }
    }

    pub fn clear(&self) {
        if let Ok(mut guard) = self.inner.lock() {
            guard.clear();
            guard.shrink_to_fit();
        }
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.inner.lock().map_or(0, |g| g.len())
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}
