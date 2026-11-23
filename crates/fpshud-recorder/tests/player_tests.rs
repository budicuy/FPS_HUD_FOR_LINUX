use fpshud_core::{RecordAudioMode, RecordBitrate, RecordCodec, RecordFps, RecordResolution};
use fpshud_recorder::capture::{ColorTransfer, RecorderPixelFormat};
use fpshud_recorder::locate::locate_bundled_ffmpeg;
use fpshud_recorder::player::{
    HwDecodeBackend, PlayerFrame, PlayerOutputMode, VideoPlayerSession, build_player_ffmpeg_args,
    build_player_video_ffmpeg_args, compute_player_target_size,
};
use fpshud_recorder::yuv::PlayerPixelFormat;
use fpshud_recorder::session::{FramePayload, RecorderConfig, RecorderSession, SubmittedFrame};
use std::path::Path;
use std::time::{Duration, Instant};

#[test]
fn test_build_player_ffmpeg_args_live_and_single_frame_and_muted() {
    let p = Path::new("/tmp/sample.mp4");
    let live_audio = build_player_ffmpeg_args(p, 1.5, 640, 360, 30.0, true, false);
    assert!(live_audio.iter().any(|a| a == "-re"));
    assert!(live_audio.iter().any(|a| a.contains("flags=bicubic")));
    assert!(!live_audio.iter().any(|a| a.contains("fast_bilinear")));
    assert!(live_audio.iter().any(|a| a == "pipe:1"));
    assert!(live_audio.iter().any(|a| a == "pulse"));
    assert!(live_audio.iter().any(|a| a == "FPSHUD Player"));

    let live_muted = build_player_ffmpeg_args(p, 1.5, 640, 360, 30.0, false, false);
    assert!(live_muted.iter().any(|a| a == "-re"));
    assert!(!live_muted.iter().any(|a| a == "pulse"));

    let single = build_player_ffmpeg_args(p, 2.0, 1280, 720, 30.0, true, true);
    assert!(!single.iter().any(|a| a == "-re"));
    assert!(single.iter().any(|a| a == "-frames:v"));
    assert!(!single.iter().any(|a| a == "pulse"));

    assert_eq!(compute_player_target_size(1920, 1080, 960, 540), (960, 540));
    assert_eq!(compute_player_target_size(1920, 1080, 800, 600), (800, 450));
}

#[test]
fn test_build_player_video_ffmpeg_args_hw_sw_and_odd_crop() {
    let p = Path::new("/tmp/sample.mp4");

    let (cuda_args, cw, ch, cfmt) = build_player_video_ffmpeg_args(
        p,
        0.0,
        1920,
        1080,
        (960, 540),
        60.0,
        PlayerOutputMode::NativeYuv,
        HwDecodeBackend::CudaNvdec,
        false,
    );
    assert_eq!((cw, ch, cfmt), (1920, 1080, PlayerPixelFormat::Nv12));
    assert!(cuda_args.iter().any(|a| a == "-hwaccel"));
    assert!(cuda_args.iter().any(|a| a == "cuda"));
    assert!(cuda_args.iter().any(|a| a == "nv12"));
    assert!(!cuda_args.iter().any(|a| a.contains("scale=")));

    let (odd_args, ow, oh, ofmt) = build_player_video_ffmpeg_args(
        p,
        0.0,
        853,
        481,
        (960, 540),
        30.0,
        PlayerOutputMode::NativeYuv,
        HwDecodeBackend::Software,
        false,
    );
    assert_eq!((ow, oh, ofmt), (852, 480, PlayerPixelFormat::Yuv420p));
    assert!(odd_args.iter().any(|a| a.contains("crop=852:480")));
    assert!(odd_args.iter().any(|a| a == "yuv420p"));

    let (scaled_args, sw, sh, sfmt) = build_player_video_ffmpeg_args(
        p,
        0.0,
        1920,
        1080,
        (960, 540),
        60.0,
        PlayerOutputMode::ViewportScaledYuv,
        HwDecodeBackend::Software,
        false,
    );
    assert_eq!((sw, sh, sfmt), (960, 540, PlayerPixelFormat::Yuv420p));
    assert!(scaled_args.iter().any(|a| a.contains("scale=960:540:flags=bicubic")));

    let (vaapi_args, _, _, vfmt) = build_player_video_ffmpeg_args(
        p,
        0.0,
        1920,
        1080,
        (1920, 1080),
        60.0,
        PlayerOutputMode::NativeYuv,
        HwDecodeBackend::Vaapi,
        false,
    );
    assert_eq!(vfmt, PlayerPixelFormat::Nv12);
    assert!(vaapi_args.iter().any(|a| a.contains("hwdownload")));
}

#[test]
fn test_player_open_play_pause_seek_eof_restart_and_clean_drop() {
    if locate_bundled_ffmpeg().is_none() {
        return;
    }
    let dir = tempfile::tempdir().unwrap();
    let cfg = RecorderConfig {
        output_dir: dir.path().to_path_buf(),
        resolution: RecordResolution::Native,
        fps: RecordFps::Fps30,
        codec: RecordCodec::H264,
        bitrate: RecordBitrate::Auto,
        audio_mode: RecordAudioMode::Off,
        target_gpu: None,
    };
    let mut rec = RecorderSession::start(cfg, |_| {}).expect("start rec");
    for _ in 0..25 {
        let _ = rec.try_submit_frame(SubmittedFrame {
            cap_w: 320,
            cap_h: 180,
            src_w: 320,
            src_h: 180,
            pix_fmt: RecorderPixelFormat::Rgba8,
            transfer: ColorTransfer::Sdr,
            flip_y: false,
            payload: FramePayload::OwnedBuffer {
                data: vec![0x66_u8; 320 * 180 * 4],
                pool: None,
            },
        });
        std::thread::sleep(Duration::from_millis(20));
    }
    rec.request_stop();
    let paths = rec.wait_finished();
    assert!(!paths.is_empty());

    let player =
        VideoPlayerSession::open_with_mode(paths[0].clone(), PlayerOutputMode::ViewportScaledYuv)
            .expect("open player");
    player.set_viewport_size(320, 180);

    // Wait for first decoded frame
    let mut frame = PlayerFrame::default();
    let deadline = Instant::now() + Duration::from_secs(4);
    let mut got_frame = false;
    while Instant::now() < deadline {
        if player.take_latest_frame(&mut frame) && !frame.data.is_empty() {
            got_frame = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(15));
    }
    assert!(got_frame, "player must deliver decoded YUV/NV12 frame");
    assert_eq!(frame.data.len(), (frame.width * frame.height * 3 / 2) as usize);
    assert!(matches!(
        frame.pixel_format,
        PlayerPixelFormat::Nv12 | PlayerPixelFormat::Yuv420p
    ));

    // Pause + resize triggers single-frame shot at new resolution
    player.pause();
    std::thread::sleep(Duration::from_millis(60));
    assert!(!player.snapshot().playing);

    player.set_viewport_size(160, 90);
    let deadline_resize = Instant::now() + Duration::from_secs(3);
    let mut got_resized = false;
    while Instant::now() < deadline_resize {
        if player.take_latest_frame(&mut frame) && frame.width == 160 && frame.height == 90 {
            got_resized = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(15));
    }
    assert!(got_resized, "paused resize must deliver sharp 1-frame shot");

    // 5 rapid seeks without bounce
    for target in [0.1, 0.4, 0.2, 0.5, 0.3] {
        player.seek(target);
    }
    std::thread::sleep(Duration::from_millis(120));
    let snap = player.snapshot();
    assert!(
        (snap.current_sec - 0.3).abs() < 0.25,
        "seek must settle near 0.3s without bounce, got {}",
        snap.current_sec
    );

    // Play to EOF and verify at_eof -> play restarts from 0.0
    player.play();
    let deadline_eof = Instant::now() + Duration::from_secs(4);
    while Instant::now() < deadline_eof {
        if player.snapshot().at_eof {
            break;
        }
        std::thread::sleep(Duration::from_millis(25));
    }
    assert!(player.snapshot().at_eof, "short clip must reach at_eof");
    player.play();
    let restarted = player.snapshot();
    assert!(!restarted.at_eof);
    assert!(restarted.playing);
    assert!(restarted.current_sec < 0.01);

    drop(player);
}

#[test]
fn test_player_headless_pulse_failure_falls_back_to_video_only() {
    let Some(bin) = locate_bundled_ffmpeg() else {
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let clip = dir.path().join("with_audio.mp4");
    // Generate a 1s clip with both H.264 video and AAC audio using bundled ffmpeg
    let status = fpshud_recorder::locate::bundled_ffmpeg_command(&bin)
        .args([
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "color=blue:s=160x90:r=30:d=1",
            "-f",
            "pulse",
            "-sample_rate",
            "48000",
            "-channels",
            "2",
            "-i",
            "@DEFAULT_MONITOR@",
            "-t",
            "1",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-pix_fmt",
            "yuv420p",
            "-c:a",
            "aac",
            "-b:a",
            "128k",
            "-shortest",
        ])
        .arg(&clip)
        .status()
        .expect("generate test clip");
    if !status.success() {
        // Headless CI without PulseAudio: skip audio-clip generation gracefully
        return;
    }

    let player = VideoPlayerSession::open_with_pulse_server(
        clip,
        Some("/nonexistent/fpshud-pulse".to_string()),
    )
    .expect("open player with broken pulse");
    player.set_viewport_size(160, 90);

    let mut frame = PlayerFrame::default();
    let deadline = Instant::now() + Duration::from_secs(4);
    let mut got = false;
    while Instant::now() < deadline {
        if player.take_latest_frame(&mut frame) && !frame.data.is_empty() {
            got = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    assert!(
        got,
        "player must fall back to video-only when PulseAudio is unavailable"
    );
    drop(player);
}
