use fpshud_core::{RecordCodec, RecordResolution};
use fpshud_recorder::capture::{
    ColorTransfer, PixelBufferPool, RecorderPixelFormat, convert_frame_to_rgba8,
};
use fpshud_recorder::locate::{locate_bundled_ffmpeg_with, sanitize_child_env_map};
use fpshud_recorder::pacer::{
    FramePacer, MAX_CAPTURE_RING_BYTES, adaptive_ring_slot_count, compute_target_dimensions,
};
use fpshud_recorder::probe::{SelectedEncoder, encoder_priority_for_gpu};
use std::collections::HashMap;

#[test]
fn test_locate_never_falls_back_to_system_path() {
    let dir = tempfile::tempdir().unwrap();
    let missing_env = dir.path().join("does_not_exist");
    let found = locate_bundled_ffmpeg_with(
        Some(missing_env.as_os_str()),
        Some(dir.path().join("libfpshud.so")),
        Some(dir.path().join("fpshud-ui")),
        Some(dir.path().join("home")),
    );
    assert!(
        found.is_none(),
        "must never fall back to system PATH /usr/bin/ffmpeg"
    );
}

#[test]
fn test_sanitize_env_strips_preload_scout_and_never_prepends_run_host() {
    let mut env = HashMap::new();
    env.insert("LD_PRELOAD".into(), "/home/u/.local/lib/fpshud/libfpshud.so".into());
    env.insert("FPS_HUD".into(), "1".into());
    env.insert(
        "VK_INSTANCE_LAYERS".into(),
        "VK_LAYER_MESA_overlay:VK_LAYER_FPSHUD_overlay".into(),
    );
    env.insert(
        "LD_LIBRARY_PATH".into(),
        "/home/u/.steam/ubuntu12_32/steam-runtime/lib:/usr/lib".into(),
    );

    sanitize_child_env_map(&mut env, true);
    assert!(!env.contains_key("LD_PRELOAD"));
    assert!(!env.contains_key("FPS_HUD"));
    assert_eq!(env.get("DISABLE_FPS_HUD").map(String::as_str), Some("1"));
    assert_eq!(
        env.get("VK_INSTANCE_LAYERS").map(String::as_str),
        Some("VK_LAYER_MESA_overlay")
    );
    let ld = env.get("LD_LIBRARY_PATH").cloned().unwrap_or_default();
    assert!(!ld.contains("ubuntu12_32"));
    assert!(!ld.contains("steam-runtime"));
    assert!(
        !ld.contains("/run/host/usr/lib"),
        "must never prepend /run/host/usr/lib to LD_LIBRARY_PATH inside containers"
    );
}

#[test]
fn test_encoder_priority_by_gpu_vendor() {
    assert_eq!(
        encoder_priority_for_gpu(Some((0x10DE, 0x21C4)), RecordCodec::H264),
        vec![
            SelectedEncoder::H264Nvenc,
            SelectedEncoder::H264Vulkan,
            SelectedEncoder::LibX264
        ]
    );
    assert_eq!(
        encoder_priority_for_gpu(Some((0x10DE, 0x21C4)), RecordCodec::H265),
        vec![SelectedEncoder::HevcNvenc, SelectedEncoder::HevcVulkan]
    );
    assert_eq!(
        encoder_priority_for_gpu(Some((0x1002, 0x744C)), RecordCodec::H264),
        vec![
            SelectedEncoder::H264Vaapi,
            SelectedEncoder::H264Vulkan,
            SelectedEncoder::LibX264
        ]
    );
    assert_eq!(
        encoder_priority_for_gpu(None, RecordCodec::H264),
        vec![
            SelectedEncoder::H264Nvenc,
            SelectedEncoder::H264Vaapi,
            SelectedEncoder::H264Vulkan,
            SelectedEncoder::LibX264
        ]
    );
}

#[test]
fn test_compute_target_dimensions_never_upscales_and_even_aligns() {
    assert_eq!(
        compute_target_dimensions(3840, 2160, RecordResolution::P1080),
        (1920, 1080)
    );
    assert_eq!(
        compute_target_dimensions(1280, 720, RecordResolution::P1080),
        (1280, 720)
    );
    assert_eq!(
        compute_target_dimensions(2560, 1080, RecordResolution::P720),
        (1706, 720)
    );
    assert_eq!(
        compute_target_dimensions(1366, 768, RecordResolution::Native),
        (1366, 768)
    );
    let (w, h) = compute_target_dimensions(1365, 767, RecordResolution::Native);
    assert_eq!(w % 2, 0);
    assert_eq!(h % 2, 0);
}

#[test]
fn test_adaptive_ring_slot_count_caps_at_100_mib() {
    let f1080 = 1920 * 1080 * 4;
    let f1440 = 2560 * 1440 * 4;
    let f4k = 3840 * 2160 * 4;
    assert_eq!(adaptive_ring_slot_count(f1080, MAX_CAPTURE_RING_BYTES), 4);
    assert_eq!(adaptive_ring_slot_count(f1440, MAX_CAPTURE_RING_BYTES), 4);
    assert_eq!(adaptive_ring_slot_count(f4k, MAX_CAPTURE_RING_BYTES), 3);
    assert!(adaptive_ring_slot_count(f4k, MAX_CAPTURE_RING_BYTES) * f4k <= MAX_CAPTURE_RING_BYTES);
}

#[test]
fn test_frame_pacer_vsync_jitter_and_240fps_cap() {
    let mut pacer = FramePacer::new(60);
    let mut captured_vsync = 0_u32;
    let mut t_ns = 0_u64;
    // 60 frames with 16.5ms..17.5ms VSync jitter (~5% jitter < 12.5% tolerance)
    for i in 0..60 {
        if pacer.should_capture(t_ns) {
            captured_vsync += 1;
        }
        t_ns += if i % 2 == 0 { 16_500_000 } else { 16_833_333 };
    }
    assert!(
        captured_vsync >= 58,
        "VSync jitter must not drop 60Hz to 30fps (got {captured_vsync})"
    );

    // 240 FPS over 1 second should capture ~60 frames
    let mut pacer240 = FramePacer::new(60);
    let mut captured_240 = 0_u32;
    for i in 0..240_u64 {
        if pacer240.should_capture(i * 4_166_667) {
            captured_240 += 1;
        }
    }
    assert!((58..=62).contains(&captured_240), "got {captured_240}");
}

#[test]
fn test_hdr_lut_and_fp16_sanitization() {
    // 10-bit A2B10G10R10: R=1023, G=512, B=0, A=3
    let packed: u32 = 1023 | (512 << 10) | (3 << 30);
    let mut dst = Vec::new();
    convert_frame_to_rgba8(
        &packed.to_le_bytes(),
        1,
        1,
        RecorderPixelFormat::A2B10G10R10,
        ColorTransfer::Sdr,
        &mut dst,
    );
    assert_eq!(dst.len(), 4);
    assert_eq!(dst[0], 255);
    assert!(dst[1] > 100 && dst[1] < 160);
    assert_eq!(dst[2], 0);
    assert_eq!(dst[3], 255);

    // FP16 with NaN (0x7e00), +Inf (0x7c00), -0.5 (0xb800), 1.0 (0x3c00)
    let mut fp16 = Vec::with_capacity(8);
    for bits in [0x7e00_u16, 0x7c00_u16, 0xb800_u16, 0x3c00_u16] {
        fp16.extend_from_slice(&bits.to_le_bytes());
    }
    convert_frame_to_rgba8(
        &fp16,
        1,
        1,
        RecorderPixelFormat::R16G16B16A16Sfloat,
        ColorTransfer::LinearHdr,
        &mut dst,
    );
    assert_eq!(dst.len(), 4);
    assert_eq!(dst[0], 0); // NaN sanitized to 0
    assert_eq!(dst[1], 255); // +Inf clamped
    assert_eq!(dst[2], 0); // negative clamped to 0
    assert_eq!(dst[3], 255); // opaque alpha
}

#[test]
fn test_pixel_buffer_pool_reuses_and_bounds_capacity() {
    let pool = PixelBufferPool::new(2);
    let mut b1 = pool.take(1024);
    b1.resize(1024, 7);
    let ptr1 = b1.as_ptr();
    pool.put(b1);

    let b2 = pool.take(1024);
    assert_eq!(b2.as_ptr(), ptr1, "pool must reuse allocation");
    pool.put(b2);

    pool.put(vec![0_u8; 1024]);
    pool.put(vec![0_u8; 1024]);
    assert_eq!(pool.len(), 2, "pool must respect max capacity");
    pool.clear();
    assert_eq!(pool.len(), 0);
}
