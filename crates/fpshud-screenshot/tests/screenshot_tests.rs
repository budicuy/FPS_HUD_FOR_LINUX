use fpshud_screenshot::{generate_screenshot_path, write_png_to_path, PixelFormat, RawFrameCapture, ScreenshotWorker};
use std::io::BufReader;
use std::os::unix::fs::symlink;

#[test]
fn pure_rust_png_encodes_bgra_and_flips_y() {
    let dir = tempfile::tempdir().unwrap();
    let out = dir.path().join("shot.png");
    let frame = RawFrameCapture {
        width: 2,
        height: 2,
        format: PixelFormat::Bgra8,
        flip_y: true,
        pixels: vec![
            255, 0, 0, 255, 255, 0, 0, 255,
            0, 0, 255, 255, 0, 0, 255, 255,
        ],
    };
    write_png_to_path(&out, &frame).unwrap();

    let decoder = png::Decoder::new(BufReader::new(std::fs::File::open(&out).unwrap()));
    let mut reader = decoder.read_info().unwrap();
    let mut buf = vec![0; reader.output_buffer_size().unwrap()];
    reader.next_frame(&mut buf).unwrap();
    assert_eq!(&buf[0..4], &[255, 0, 0, 255]);
    assert_eq!(&buf[8..12], &[0, 0, 255, 255]);
}

#[test]
fn rgba_passthrough_odd_height_error_guard_and_worker_async_capture() {
    let dir = tempfile::tempdir().unwrap();

    // 1. Rgba8 tanpa flip mempertahankan piksel identik
    let direct_out = dir.path().join("direct.png");
    let direct = RawFrameCapture {
        width: 1,
        height: 2,
        format: PixelFormat::Rgba8,
        flip_y: false,
        pixels: vec![11, 22, 33, 255, 44, 55, 66, 255],
    };
    write_png_to_path(&direct_out, &direct).unwrap();
    let dec0 = png::Decoder::new(BufReader::new(std::fs::File::open(&direct_out).unwrap()));
    let mut r0 = dec0.read_info().unwrap();
    let mut px0 = vec![0; r0.output_buffer_size().unwrap()];
    r0.next_frame(&mut px0).unwrap();
    assert_eq!(px0, vec![11, 22, 33, 255, 44, 55, 66, 255]);

    // 2. Rgba8 + flip_y=true pada odd height=3
    let rgba_out = dir.path().join("rgba_flip.png");
    let frame = RawFrameCapture {
        width: 1,
        height: 3,
        format: PixelFormat::Rgba8,
        flip_y: true,
        pixels: vec![10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255],
    };
    write_png_to_path(&rgba_out, &frame).unwrap();
    let dec = png::Decoder::new(BufReader::new(std::fs::File::open(&rgba_out).unwrap()));
    let mut r = dec.read_info().unwrap();
    let mut px = vec![0; r.output_buffer_size().unwrap()];
    r.next_frame(&mut px).unwrap();
    assert_eq!(&px[0..4], &[70, 80, 90, 255]);
    assert_eq!(&px[4..8], &[40, 50, 60, 255]);

    // 3. Panjang buffer salah & target symlink ditolak tanpa panic
    let bad_len = RawFrameCapture {
        width: 4,
        height: 4,
        format: PixelFormat::Rgba8,
        flip_y: false,
        pixels: vec![0; 4],
    };
    assert!(write_png_to_path(&dir.path().join("bad.png"), &bad_len).is_err());

    let victim = dir.path().join("victim.png");
    std::fs::write(&victim, b"PROTECTED").unwrap();
    let link = dir.path().join("shot_link.png");
    symlink(&victim, &link).unwrap();
    assert!(write_png_to_path(&link, &frame).is_err());
    assert_eq!(std::fs::read(&victim).unwrap(), b"PROTECTED");

    // 4. Sanitasi path + ScreenshotWorker backpressure sync_channel(2) + clean Drop flush
    let safe_path = generate_screenshot_path(dir.path(), "../../bad/prefix");
    assert_eq!(safe_path.parent().unwrap(), dir.path());
    assert!(safe_path.extension().and_then(|e| e.to_str()) == Some("png"));

    let worker = ScreenshotWorker::new(dir.path().to_path_buf());
    for _ in 0..5 {
        worker.submit(frame.clone());
    }
    drop(worker);
    let png_count = std::fs::read_dir(dir.path())
        .unwrap()
        .filter(|e| e.as_ref().unwrap().path().extension() == Some("png".as_ref()))
        .count();
    assert!(png_count >= 1);
}
