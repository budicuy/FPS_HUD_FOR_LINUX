use fpshud_recorder::library::parse_ffmpeg_probe_output;
use fpshud_recorder::yuv::{
    PlayerColorSpace, PlayerPixelFormat, expected_frame_byte_len, yuv_frame_to_argb8888,
    yuv_frame_to_rgba,
};

#[test]
fn test_parse_ffmpeg_probe_color_space() {
    let bt709_tv = "\
  Duration: 00:00:10.00, start: 0.000000, bitrate: 12000 kb/s
  Stream #0:0[0x1](und): Video: h264 (High), yuv420p(tv, bt709, progressive), 1920x1080 [SAR 1:1 DAR 16:9], 60 fps
";
    let m1 = parse_ffmpeg_probe_output(bt709_tv);
    assert_eq!(m1.color_space, PlayerColorSpace::Bt709Limited);

    let bt709_pc = "\
  Stream #0:0: Video: h264, yuv420p(pc, bt709), 1920x1080, 60 fps
";
    assert_eq!(
        parse_ffmpeg_probe_output(bt709_pc).color_space,
        PlayerColorSpace::Bt709Full
    );

    let yuvj_hd = "\
  Stream #0:0: Video: h264, yuvj420p, 1280x720, 30 fps
";
    assert_eq!(
        parse_ffmpeg_probe_output(yuvj_hd).color_space,
        PlayerColorSpace::Bt709Full
    );

    let bt601_tv = "\
  Stream #0:0: Video: h264, yuv420p(tv, bt470bg), 720x480, 30 fps
";
    assert_eq!(
        parse_ffmpeg_probe_output(bt601_tv).color_space,
        PlayerColorSpace::Bt601Limited
    );

    let untagged_hd = "\
  Stream #0:0: Video: h264, yuv420p, 1920x1080, 60 fps
";
    assert_eq!(
        parse_ffmpeg_probe_output(untagged_hd).color_space,
        PlayerColorSpace::Bt709Limited
    );

    let untagged_sd = "\
  Stream #0:0: Video: h264, yuv420p, 640x480, 30 fps
";
    assert_eq!(
        parse_ffmpeg_probe_output(untagged_sd).color_space,
        PlayerColorSpace::Bt601Limited
    );
}

#[test]
fn test_nv12_and_yuv420p_bt709_limited_black_white_and_primary_colors() {
    let w = 2u32;
    let h = 2u32;
    assert_eq!(expected_frame_byte_len(w, h, PlayerPixelFormat::Nv12), 6);
    assert_eq!(expected_frame_byte_len(w, h, PlayerPixelFormat::Yuv420p), 6);
    assert_eq!(expected_frame_byte_len(w, h, PlayerPixelFormat::Rgba8), 16);

    // Pure black limited: Y=16, U=128, V=128
    let nv12_black = [16u8, 16, 16, 16, 128, 128];
    let mut rgba = Vec::new();
    yuv_frame_to_rgba(
        w,
        h,
        PlayerPixelFormat::Nv12,
        PlayerColorSpace::Bt709Limited,
        &nv12_black,
        &mut rgba,
    );
    assert_eq!(rgba.len(), 16);
    for px in rgba.chunks_exact(4) {
        assert!(px[0] <= 2 && px[1] <= 2 && px[2] <= 2 && px[3] == 255, "got {px:?}");
    }

    // Pure white limited: Y=235, U=128, V=128
    let yuv420_white = [235u8, 235, 235, 235, 128, 128];
    yuv_frame_to_rgba(
        w,
        h,
        PlayerPixelFormat::Yuv420p,
        PlayerColorSpace::Bt709Limited,
        &yuv420_white,
        &mut rgba,
    );
    for px in rgba.chunks_exact(4) {
        assert!(px[0] >= 253 && px[1] >= 253 && px[2] >= 253 && px[3] == 255, "got {px:?}");
    }

    // Pure red BT.709 limited: Y=63, U=102, V=240
    let nv12_red = [63u8, 63, 63, 63, 102, 240];
    yuv_frame_to_rgba(
        w,
        h,
        PlayerPixelFormat::Nv12,
        PlayerColorSpace::Bt709Limited,
        &nv12_red,
        &mut rgba,
    );
    for px in rgba.chunks_exact(4) {
        assert!(px[0] >= 245 && px[1] <= 10 && px[2] <= 10 && px[3] == 255, "got {px:?}");
    }

    // ARGB8888 little-endian [B, G, R, A] matches swizzled RGBA
    let mut argb = Vec::new();
    yuv_frame_to_argb8888(
        w,
        h,
        PlayerPixelFormat::Nv12,
        PlayerColorSpace::Bt709Limited,
        &nv12_red,
        &mut argb,
    );
    assert_eq!(argb.len(), rgba.len());
    for (r_px, a_px) in rgba.chunks_exact(4).zip(argb.chunks_exact(4)) {
        assert_eq!(a_px, &[r_px[2], r_px[1], r_px[0], r_px[3]]);
    }
}

#[test]
fn test_yuv_converter_odd_dimensions_and_short_buffer_no_panic() {
    let mut out = Vec::new();
    // Odd dimensions 5x3 with short buffer
    yuv_frame_to_rgba(
        5,
        3,
        PlayerPixelFormat::Nv12,
        PlayerColorSpace::Bt709Limited,
        &[16u8; 10],
        &mut out,
    );
    assert_eq!(out.len(), 5 * 3 * 4);

    yuv_frame_to_argb8888(
        5,
        3,
        PlayerPixelFormat::Yuv420p,
        PlayerColorSpace::Bt601Full,
        &[128u8; 5],
        &mut out,
    );
    assert_eq!(out.len(), 5 * 3 * 4);
}
