use fpshud_analytics::{BenchmarkRecorder, FrametimeAnalyzer};
use std::fs;
use std::os::unix::fs::symlink;

#[test]
fn frametime_zero_dt_immunity_and_percentiles() {
    let mut analyzer = FrametimeAnalyzer::new();
    for i in 0..100 {
        let dt = if i == 50 { 0.050 } else { 1.0 / 60.0 };
        analyzer.record_frame(dt);
    }
    analyzer.record_frame(0.0);
    analyzer.record_frame(-1.0);
    analyzer.record_frame(f32::NAN);
    let m = analyzer.compute_metrics();
    assert!(!m.fps.is_nan() && !m.fps.is_infinite());
    assert!(!m.fps_1pct_low.is_nan() && m.fps_1pct_low <= m.fps_avg + 0.1);
    assert!(m.fps_01pct_low <= m.fps_1pct_low + 0.1);
}

#[test]
fn benchmark_recorder_rejects_symlink_via_o_nofollow() {
    let dir = tempfile::tempdir().unwrap();
    let victim = dir.path().join("victim.txt");
    fs::write(&victim, "DO_NOT_OVERWRITE").unwrap();
    let link_path = dir.path().join("FPSHUD_Bench_link.csv");
    symlink(&victim, &link_path).unwrap();

    assert!(BenchmarkRecorder::open_safe_csv(&link_path).is_err());
    assert_eq!(fs::read_to_string(&victim).unwrap(), "DO_NOT_OVERWRITE");
}

#[test]
fn frametime_empty_history_cap_and_benchmark_csv_roundtrip() {
    let mut analyzer = FrametimeAnalyzer::new();
    let empty = analyzer.compute_metrics();
    assert_eq!(empty.fps, 0.0);
    assert!(analyzer.history_slice().is_empty());

    for i in 0..2500 {
        let dt = if i % 500 == 0 { 0.033 } else { 0.010 };
        analyzer.record_frame(dt);
    }
    assert!(analyzer.history_slice().len() <= 120);
    let m = analyzer.compute_metrics();
    assert!(m.fps_avg > 0.0 && m.fps_01pct_low <= m.fps_1pct_low + 0.1);

    let dir = tempfile::tempdir().unwrap();
    let mut rec = BenchmarkRecorder::default();
    rec.start(dir.path());
    for _ in 0..10 {
        rec.push_sample(100.0, 10.0, 35.0, 75.0);
    }
    let summary = rec.stop().expect("summary must be returned");
    assert!((summary.avg_fps - 100.0).abs() < 0.5);
    assert!(summary.csv_path.exists());
    let csv_body = fs::read_to_string(&summary.csv_path).unwrap();
    assert!(csv_body.lines().count() >= 11);
    assert!(rec.stop().is_none());
}
