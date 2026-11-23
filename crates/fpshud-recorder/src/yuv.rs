//! Fast fixed-point Pure-Rust YUV (NV12 / YUV420p) to RGBA8 / Wayland ARGB8888 conversion
//! with accurate BT.709 and BT.601 limited/full-range color matrices.

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PlayerPixelFormat {
    #[default]
    Rgba8,
    /// Semi-planar 4:2:0: Y plane (`w * h`) followed by interleaved UV plane (`w * (h / 2)`).
    Nv12,
    /// Planar 4:2:0: Y (`w * h`), U (`(w / 2) * (h / 2)`), V (`(w / 2) * (h / 2)`).
    Yuv420p,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PlayerColorSpace {
    #[default]
    Bt709Limited,
    Bt709Full,
    Bt601Limited,
    Bt601Full,
}

impl PlayerColorSpace {
    #[must_use]
    pub const fn as_shader_id(self) -> u32 {
        match self {
            Self::Bt709Limited => 0,
            Self::Bt709Full => 1,
            Self::Bt601Limited => 2,
            Self::Bt601Full => 3,
        }
    }
}

/// Returns the expected packed byte length for a frame of `(width, height, fmt)`.
#[must_use]
pub fn expected_frame_byte_len(width: u32, height: u32, fmt: PlayerPixelFormat) -> usize {
    let w = width as usize;
    let h = height as usize;
    match fmt {
        PlayerPixelFormat::Rgba8 => w.saturating_mul(h).saturating_mul(4),
        PlayerPixelFormat::Nv12 | PlayerPixelFormat::Yuv420p => {
            let y = w.saturating_mul(h);
            let cw = w.div_ceil(2);
            let ch = h.div_ceil(2);
            y.saturating_add(cw.saturating_mul(ch).saturating_mul(2))
        }
    }
}

struct YuvCoeffs {
    y_bias: i32,
    cy: i32,
    crv: i32,
    cgu: i32,
    cgv: i32,
    cbu: i32,
}

#[inline]
const fn coeffs_for(cs: PlayerColorSpace) -> YuvCoeffs {
    // Fixed-point 10-bit fractional precision (scale = 1024).
    match cs {
        PlayerColorSpace::Bt709Limited => YuvCoeffs {
            y_bias: 16,
            cy: 1192,  // 255 / 219 * 1024
            crv: 1836, // 1.5748 * 255/224 * 1024
            cgu: -218, // -0.187324 * 255/224 * 1024
            cgv: -546, // -0.468124 * 255/224 * 1024
            cbu: 2163, // 1.8556 * 255/224 * 1024
        },
        PlayerColorSpace::Bt709Full => YuvCoeffs {
            y_bias: 0,
            cy: 1024,
            crv: 1613, // 1.5748 * 1024
            cgu: -192, // -0.187324 * 1024
            cgv: -479, // -0.468124 * 1024
            cbu: 1900, // 1.8556 * 1024
        },
        PlayerColorSpace::Bt601Limited => YuvCoeffs {
            y_bias: 16,
            cy: 1192,
            crv: 1634, // 1.402 * 255/224 * 1024
            cgu: -401, // -0.344136 * 255/224 * 1024
            cgv: -833, // -0.714136 * 255/224 * 1024
            cbu: 2066, // 1.772 * 255/224 * 1024
        },
        PlayerColorSpace::Bt601Full => YuvCoeffs {
            y_bias: 0,
            cy: 1024,
            crv: 1436, // 1.402 * 1024
            cgu: -352, // -0.344136 * 1024
            cgv: -731, // -0.714136 * 1024
            cbu: 1815, // 1.772 * 1024
        },
    }
}

#[inline]
fn clamp_u8(v: i32) -> u8 {
    v.clamp(0, 255) as u8
}

fn convert_frame_inner<const BGRA: bool>(
    width: u32,
    height: u32,
    fmt: PlayerPixelFormat,
    cs: PlayerColorSpace,
    src: &[u8],
    out: &mut Vec<u8>,
) {
    let w = width as usize;
    let h = height as usize;
    let out_len = w.saturating_mul(h).saturating_mul(4);
    out.resize(out_len, 0);
    if w == 0 || h == 0 {
        return;
    }

    if fmt == PlayerPixelFormat::Rgba8 {
        if src.len() < out_len {
            out.fill(0);
            return;
        }
        if !BGRA {
            out.copy_from_slice(&src[..out_len]);
        } else {
            let (src_chunks, _) = src[..out_len].as_chunks::<4>();
            let (dst_chunks, _) = out.as_chunks_mut::<4>();
            for (s, d) in src_chunks.iter().zip(dst_chunks.iter_mut()) {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = s[3];
            }
        }
        return;
    }

    let y_len = w.saturating_mul(h);
    let cw = w.div_ceil(2);
    let ch = h.div_ceil(2);
    let needed = expected_frame_byte_len(width, height, fmt);
    if src.len() < needed {
        out.fill(0);
        return;
    }

    let c = coeffs_for(cs);
    let y_plane = &src[..y_len];

    match fmt {
        PlayerPixelFormat::Nv12 => {
            let uv_plane = &src[y_len..];
            for row in 0..h {
                let y_row = &y_plane[row * w..(row + 1) * w];
                let uv_row_off = (row / 2) * (cw * 2);
                let dst_row = &mut out[row * w * 4..(row + 1) * w * 4];
                for (col, &y_val) in y_row.iter().enumerate() {
                    let y = i32::from(y_val) - c.y_bias;
                    let uv_idx = uv_row_off + (col / 2) * 2;
                    let u = i32::from(uv_plane[uv_idx]) - 128;
                    let v = i32::from(uv_plane[uv_idx + 1]) - 128;

                    let y_term = c.cy * y + 512;
                    let r = clamp_u8((y_term + c.crv * v) >> 10);
                    let g = clamp_u8((y_term + c.cgu * u + c.cgv * v) >> 10);
                    let b = clamp_u8((y_term + c.cbu * u) >> 10);

                    let d = col * 4;
                    if BGRA {
                        dst_row[d] = b;
                        dst_row[d + 1] = g;
                        dst_row[d + 2] = r;
                        dst_row[d + 3] = 255;
                    } else {
                        dst_row[d] = r;
                        dst_row[d + 1] = g;
                        dst_row[d + 2] = b;
                        dst_row[d + 3] = 255;
                    }
                }
            }
        }
        PlayerPixelFormat::Yuv420p => {
            let u_len = cw * ch;
            let u_plane = &src[y_len..y_len + u_len];
            let v_plane = &src[y_len + u_len..];
            for row in 0..h {
                let y_row = &y_plane[row * w..(row + 1) * w];
                let c_row_off = (row / 2) * cw;
                let dst_row = &mut out[row * w * 4..(row + 1) * w * 4];
                for (col, &y_val) in y_row.iter().enumerate() {
                    let y = i32::from(y_val) - c.y_bias;
                    let c_idx = c_row_off + (col / 2);
                    let u = i32::from(u_plane[c_idx]) - 128;
                    let v = i32::from(v_plane[c_idx]) - 128;

                    let y_term = c.cy * y + 512;
                    let r = clamp_u8((y_term + c.crv * v) >> 10);
                    let g = clamp_u8((y_term + c.cgu * u + c.cgv * v) >> 10);
                    let b = clamp_u8((y_term + c.cbu * u) >> 10);

                    let d = col * 4;
                    if BGRA {
                        dst_row[d] = b;
                        dst_row[d + 1] = g;
                        dst_row[d + 2] = r;
                        dst_row[d + 3] = 255;
                    } else {
                        dst_row[d] = r;
                        dst_row[d + 1] = g;
                        dst_row[d + 2] = b;
                        dst_row[d + 3] = 255;
                    }
                }
            }
        }
        PlayerPixelFormat::Rgba8 => unreachable!(),
    }
}

/// Converts a `PlayerFrame` payload (`Nv12`, `Yuv420p`, or `Rgba8`) into packed `RGBA8888`.
pub fn yuv_frame_to_rgba(
    width: u32,
    height: u32,
    fmt: PlayerPixelFormat,
    cs: PlayerColorSpace,
    src: &[u8],
    out_rgba: &mut Vec<u8>,
) {
    convert_frame_inner::<false>(width, height, fmt, cs, src, out_rgba);
}

/// Converts a `PlayerFrame` payload (`Nv12`, `Yuv420p`, or `Rgba8`) directly into
/// little-endian Wayland `ARGB8888` (`[B, G, R, A]`).
pub fn yuv_frame_to_argb8888(
    width: u32,
    height: u32,
    fmt: PlayerPixelFormat,
    cs: PlayerColorSpace,
    src: &[u8],
    out_argb: &mut Vec<u8>,
) {
    convert_frame_inner::<true>(width, height, fmt, cs, src, out_argb);
}
