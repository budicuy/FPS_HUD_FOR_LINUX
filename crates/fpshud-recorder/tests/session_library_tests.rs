use fpshud_core::{RecordAudioMode, RecordBitrate, RecordCodec, RecordFps, RecordResolution};
use fpshud_recorder::capture::{ColorTransfer, PixelBufferPool, RecorderPixelFormat};
use fpshud_recorder::library::{
    delete_recording_safe, parse_ffmpeg_probe_output, probe_recording_entry, scan_recordings_dir,
};
use fpshud_recorder::locate::locate_bundled_ffmpeg;
use fpshud_recorder::probe::SelectedEncoder;
use fpshud_recorder::session::{
    FramePayload, RecorderConfig, RecorderEvent, RecorderSession, SubmittedFrame,
    build_record_ffmpeg_args,
};
use std::path::Path;
use std::sync::{Arc, Mutex};

#[test]
fn test_build_record_ffmpeg_args_all_encoders_and_audio_modes() {
    let out = Path::new("/tmp/test.mp4");
    let args_nvenc = build_record_ffmpeg_args(
        1920,
        1080,
        1920,
        1080,
        "rgba",
        false,
        60,
        SelectedEncoder::H264Nvenc,
        None,
        RecordBitrate::Auto,
        RecordAudioMode::GameAudio,
        out,
    );
    assert!(args_nvenc.iter().any(|a| a == "h264_nvenc"));
    assert!(args_nvenc.iter().any(|a| a == "@DEFAULT_MONITOR@"));
    assert!(args_nvenc.iter().any(|a| a == "aac"));
    assert!(args_nvenc.iter().any(|a| a.contains("write_colr")));
    assert!(args_nvenc.iter().any(|a| a.contains("setparams=color_primaries=bt709")));
    assert!(
        !args_nvenc.iter().any(|a| a == "-thread_queue_size"),
        "must not pass -thread_queue_size before pipe:0 on FFmpeg 9.1"
    );

    let args_mix = build_record_ffmpeg_args(
        1920,
        1080,
        1280,
        720,
        "bgra",
        true,
        30,
        SelectedEncoder::LibX264,
        None,
        RecordBitrate::Mbps25,
        RecordAudioMode::GameAndMic,
        out,
    );
    assert!(args_mix.iter().any(|a| a == "libx264"));
    assert!(args_mix.iter().any(|a| a == "25M"));
    assert!(args_mix.iter().any(|a| a == "@DEFAULT_MONITOR@"));
    assert!(args_mix.iter().any(|a| a == "@DEFAULT_SOURCE@"));
    assert!(args_mix.iter().any(|a| a.contains("amix=inputs=2")));
    assert!(args_mix.iter().any(|a| a.contains("alimiter=limit=0.95")));
    assert!(args_mix.iter().any(|a| a.contains("vflip")));
    assert!(args_mix.iter().any(|a| a.contains("scale=1280:720")));

    let args_vaapi = build_record_ffmpeg_args(
        1920,
        1080,
        1920,
        1080,
        "rgba",
        false,
        60,
        SelectedEncoder::H264Vaapi,
        Some(Path::new("/dev/dri/renderD128")),
        RecordBitrate::Auto,
        RecordAudioMode::Off,
        out,
    );
    assert!(args_vaapi.iter().any(|a| a == "-vaapi_device"));
    assert!(args_vaapi.iter().any(|a| a.contains("hwupload")));

    let args_vk = build_record_ffmpeg_args(
        1920,
        1080,
        1920,
        1080,
        "rgba",
        false,
        60,
        SelectedEncoder::H264Vulkan,
        None,
        RecordBitrate::Auto,
        RecordAudioMode::Off,
        out,
    );
    assert!(args_vk.iter().any(|a| a == "-init_hw_device"));
    assert!(args_vk.iter().any(|a| a == "h264_vulkan"));
}

#[test]
fn test_parse_ffmpeg_probe_output_real_n9_1_fixture() {
    let stderr = "\
Input #0, mov,mp4,m4a,3gp,3g2,mj2, from '/tmp/clip.mp4':
  Duration: 00:01:23.45, start: 0.000000, bitrate: 4820 kb/s
  Stream #0:0[0x1](und): Video: h264 (High) (avc1 / 0x31637661), yuv420p(tv, bt709, progressive), 1920x1080 [SAR 1:1 DAR 16:9], 4600 kb/s, 60 fps, 60 tbr, 15360 tbn (default)
  Stream #0:1[0x2](und): Audio: aac (LC) (mp4a / 0x6134706D), 48000 Hz, stereo, fltp, 192 kb/s (default)
At least one output file must be specified
";
    let meta = parse_ffmpeg_probe_output(stderr);
    assert!((meta.duration_sec - 83.45).abs() < 0.02);
    assert_eq!(meta.width, 1920);
    assert_eq!(meta.height, 1080);
    assert!((meta.fps - 60.0).abs() < 0.1);
    assert_eq!(meta.video_codec, "h264");
    assert_eq!(meta.audio_codec.as_deref(), Some("aac"));

    let video_only = "\
  Duration: 00:00:05.10, start: 0.000000
  Stream #0:0: Video: hevc (Main), yuv420p, 1280x720, 30 fps, 30 tbr
";
    let m2 = parse_ffmpeg_probe_output(video_only);
    assert_eq!(m2.video_codec, "hevc");
    assert_eq!(m2.audio_codec, None);
}

#[test]
fn test_delete_recording_safe_and_scan_reject_symlinks_and_traversal() {
    let dir = tempfile::tempdir().unwrap();
    let outside = tempfile::tempdir().unwrap();
    let outside_file = outside.path().join("secret.mp4");
    std::fs::write(&outside_file, b"outside").unwrap();

    let regular = dir.path().join("fpshud_rec_01.mp4");
    std::fs::write(&regular, b"regular").unwrap();

    let symlink_path = dir.path().join("evil_link.mp4");
    std::os::unix::fs::symlink(&outside_file, &symlink_path).unwrap();

    // delete_recording_safe must reject symlink & traversal outside dir
    assert!(delete_recording_safe(dir.path(), &symlink_path).is_err());
    assert!(delete_recording_safe(dir.path(), &outside_file).is_err());
    assert!(outside_file.exists());

    // scan_recordings_dir must skip symlinks
    if let Some(bin) = locate_bundled_ffmpeg() {
        let entries = scan_recordings_dir(&bin, dir.path(), None);
        assert!(entries.iter().all(|e| e.file_name != "evil_link.mp4"));
    }

    // Regular file inside dir can be deleted
    delete_recording_safe(dir.path(), &regular).unwrap();
    assert!(!regular.exists());
}

#[test]
fn test_session_records_valid_fmp4_and_splits_on_resize() {
    let Some(bin) = locate_bundled_ffmpeg() else {
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let events: Arc<Mutex<Vec<RecorderEvent>>> = Arc::new(Mutex::new(Vec::new()));
    let ev_clone = Arc::clone(&events);

    let cfg = RecorderConfig {
        output_dir: dir.path().to_path_buf(),
        resolution: RecordResolution::Native,
        fps: RecordFps::Fps30,
        codec: RecordCodec::H264,
        bitrate: RecordBitrate::Auto,
        audio_mode: RecordAudioMode::Off,
        target_gpu: None,
    };

    let mut handle = RecorderSession::start(cfg, move |ev| {
        if let Ok(mut g) = ev_clone.lock() {
            g.push(ev);
        }
    })
    .expect("start recorder session");

    let pool = PixelBufferPool::new(4);
    let submit_until_accepted = |w: u32, h: u32, fill: u8| {
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(3);
        while std::time::Instant::now() < deadline {
            let mut buf = pool.take((w * h * 4) as usize);
            buf.resize((w * h * 4) as usize, fill);
            if handle.try_submit_frame(SubmittedFrame {
                cap_w: w,
                cap_h: h,
                src_w: w,
                src_h: h,
                pix_fmt: RecorderPixelFormat::Rgba8,
                transfer: ColorTransfer::Sdr,
                flip_y: false,
                payload: FramePayload::OwnedBuffer {
                    data: buf,
                    pool: Some(pool.clone()),
                },
            }) {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        panic!("timed out submitting frame {w}x{h}");
    };

    for _ in 0..10 {
        submit_until_accepted(320, 180, 0x40);
        std::thread::sleep(std::time::Duration::from_millis(20));
    }

    // Mid-session resize -> triggers segment split (_part2.mp4)
    for _ in 0..10 {
        submit_until_accepted(256, 144, 0x80);
        std::thread::sleep(std::time::Duration::from_millis(20));
    }

    // Wait briefly for SegmentSplit event before stopping so slow CI runners don't race
    let split_deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while std::time::Instant::now() < split_deadline {
        let saw_split = events
            .lock()
            .is_ok_and(|g| g.iter().any(|e| matches!(e, RecorderEvent::SegmentSplit { .. })));
        if saw_split {
            break;
        }
        submit_until_accepted(256, 144, 0x80);
        std::thread::sleep(std::time::Duration::from_millis(25));
    }

    handle.request_stop();
    let paths = handle.wait_finished();
    assert_eq!(paths.len(), 2, "expected 2 segments after mid-session resize");
    assert!(paths[1].to_string_lossy().contains("_part2.mp4"));
    for p in &paths {
        let entry = probe_recording_entry(&bin, p, None).expect("probed segment");
        assert_eq!(entry.video_codec, "h264");
        assert!(entry.size_bytes > 500);
    }
}

#[test]
fn test_session_audio_failure_falls_back_to_video_only_with_warning() {
    let Some(bin) = locate_bundled_ffmpeg() else {
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let events: Arc<Mutex<Vec<RecorderEvent>>> = Arc::new(Mutex::new(Vec::new()));
    let ev_clone = Arc::clone(&events);

    let cfg = RecorderConfig {
        output_dir: dir.path().to_path_buf(),
        resolution: RecordResolution::Native,
        fps: RecordFps::Fps30,
        codec: RecordCodec::H264,
        bitrate: RecordBitrate::Auto,
        audio_mode: RecordAudioMode::GameAudio,
        target_gpu: None,
    };

    // Force invalid PulseAudio server for this session via test helper override
    let mut handle = RecorderSession::start_with_pulse_server(
        cfg,
        Some("/nonexistent/fpshud-pulse-socket".to_string()),
        move |ev| {
            if let Ok(mut g) = ev_clone.lock() {
                g.push(ev);
            }
        },
    )
    .expect("start recorder session with broken pulse");

    for _ in 0..12 {
        let _ = handle.try_submit_frame(SubmittedFrame {
            cap_w: 320,
            cap_h: 180,
            src_w: 320,
            src_h: 180,
            pix_fmt: RecorderPixelFormat::Rgba8,
            transfer: ColorTransfer::Sdr,
            flip_y: false,
            payload: FramePayload::OwnedBuffer {
                data: vec![0x55_u8; 320 * 180 * 4],
                pool: None,
            },
        });
        std::thread::sleep(std::time::Duration::from_millis(25));
    }

    handle.request_stop();
    let paths = handle.wait_finished();
    assert!(!paths.is_empty(), "must still produce video-only mp4");
    let entry = probe_recording_entry(&bin, &paths[0], None).expect("probed fallback mp4");
    assert_eq!(entry.video_codec, "h264");
    assert_eq!(entry.audio_codec, None);
    let has_warn = events
        .lock()
        .unwrap()
        .iter()
        .any(|e| matches!(e, RecorderEvent::Warning(_)));
    assert!(has_warn, "must emit RecorderEvent::Warning on audio fallback");
}
