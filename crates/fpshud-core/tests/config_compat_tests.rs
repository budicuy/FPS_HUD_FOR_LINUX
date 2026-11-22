use fpshud_core::{
    apply_preset_advance, apply_preset_custom, apply_preset_minimal, apply_preset_standard,
    init_config_from_env,
    keybind::{format_keybind, linux_keycode_name},
    load_ini_file,
    paths::{
        get_config_file_path, get_default_record_dir, get_secure_runtime_dir,
        sanitize_filename_prefix,
    },
    reset_layout_to_default, save_ini_file, ArrangementMode, BackgroundStyle, ColorRgba,
    FontFamily, HudConfig, Keybind, LayoutPosition, MetricType, PresetType,
};
use std::collections::HashSet;
use std::fs;
use std::os::unix::fs::PermissionsExt;

#[test]
fn metric_count_and_env_init() {
    assert_eq!(MetricType::ALL.len(), 31);
    assert!(!init_config_from_env(None).enabled);
    assert!(!init_config_from_env(Some("0")).enabled);
    let enabled = init_config_from_env(Some("1"));
    assert!(enabled.enabled);
    assert_eq!(enabled.bind_toggle_gui, Keybind::new(0, 88));
}

#[test]
fn ini_roundtrip_and_legacy_resilience() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("config.ini");
    let legacy = "\
preset=custom
gui_scale=9.99
hud_visible=1
layout=2
arrangement=1
background=2
font_family=3
font_size_px=24.0
text_color=10,20,30
text_opacity=0.8
bg_alpha=0.5
bind_toggle_gui=0,110
bind_record=0,65
bind_save_replay=0,9999
replay_enabled=1
record_cq_level=22
screenshot_with_hud=1
metrics=fps,cpuhybrid,unknown_metric,gpuload
";
    fs::write(&path, legacy).unwrap();
    let mut cfg = HudConfig { enabled: true, ..HudConfig::default() };
    assert!(load_ini_file(&path, &mut cfg));
    assert!(cfg.enabled);
    assert!((cfg.gui_scale - 2.00).abs() < 1e-4);
    assert_eq!(cfg.position, LayoutPosition::TopRight);
    assert_eq!(cfg.arrangement, ArrangementMode::Double);
    assert_eq!(cfg.background_style, BackgroundStyle::Solid);
    assert_eq!(cfg.font_family, FontFamily::AdwaitaMono);
    assert_eq!(cfg.text_color, ColorRgba::new(10, 20, 30, 255));
    assert_eq!(cfg.bind_toggle_gui, Keybind::new(0, 110));
    assert_eq!(cfg.bind_benchmark, Keybind::new(0, 67));
    assert!(cfg.screenshot_with_hud);
    assert_eq!(
        cfg.metrics,
        vec![MetricType::Fps, MetricType::CpuAllCoreLoad, MetricType::GpuLoad]
    );

    assert!(save_ini_file(&path, &cfg));
    let saved = fs::read_to_string(&path).unwrap();
    assert!(!saved.contains("bind_record="));
    assert!(!saved.contains("replay_enabled="));
    assert!(saved.contains("bind_toggle_gui=0,110"));
    assert!(!dir.path().join("config.ini.tmp").exists());
}

#[test]
fn path_sanitization_neutralizes_traversal() {
    assert_eq!(sanitize_filename_prefix("../../../../tmp/evil"), "evil");
    assert_eq!(sanitize_filename_prefix("../../../.."), "FPSHUD");
    assert_eq!(sanitize_filename_prefix("Shot_2026-11"), "Shot_2026-11");
}

#[test]
fn every_metric_roundtrips_and_all_presets_plus_helpers_work() {
    let keys: HashSet<_> = MetricType::ALL.iter().map(|m| m.id_key()).collect();
    assert_eq!(keys.len(), 31);
    for &m in MetricType::ALL {
        assert_eq!(MetricType::from_id_key(m.id_key()), Some(m));
    }
    assert_eq!(MetricType::from_id_key("unknown_xyz"), None);

    let mut cfg = HudConfig::default();
    apply_preset_minimal(&mut cfg);
    assert_eq!(cfg.preset, PresetType::Minimal);
    assert!(!cfg.metrics.is_empty());

    apply_preset_standard(&mut cfg);
    assert_eq!(cfg.preset, PresetType::Standard);
    assert!(cfg.metrics.contains(&MetricType::CpuPower));

    apply_preset_advance(&mut cfg);
    assert_eq!(cfg.preset, PresetType::Advance);
    assert_eq!(cfg.metrics.len(), 31);

    apply_preset_custom(&mut cfg);
    assert_eq!(cfg.preset, PresetType::Custom);

    cfg.gui_scale = 1.75;
    cfg.position = LayoutPosition::BottomRight;
    reset_layout_to_default(&mut cfg);
    assert!((cfg.gui_scale - 1.0).abs() < 1e-4);
    assert_eq!(cfg.position, LayoutPosition::TopLeft);

    let missing = std::path::Path::new("/nonexistent/fpshud/config.ini");
    assert!(!load_ini_file(missing, &mut cfg));

    let dir = tempfile::tempdir().unwrap();
    let bad_path = dir.path().join("bad.ini");
    fs::write(&bad_path, "gui_scale=NaN\ntext_opacity=-5.0\nbind_screenshot=abc,def\n").unwrap();
    let prev_scale = cfg.gui_scale;
    assert!(load_ini_file(&bad_path, &mut cfg));
    assert_eq!(cfg.gui_scale, prev_scale);
    assert!((0.0..=1.0).contains(&cfg.text_opacity));
    assert_eq!(cfg.bind_screenshot, Keybind::new(0, 68));

    assert_eq!(linux_keycode_name(88), "F12");
    assert!(!linux_keycode_name(9999).is_empty());
    assert_eq!(format_keybind(Keybind::new(0, 88)), "F12");
    assert!(format_keybind(Keybind::new(1, 68)).contains("Ctrl"));
    assert_eq!(format_keybind(Keybind::new(0, 0)), "None");

    assert!(get_config_file_path().ends_with("config.ini"));
    assert!(!get_default_record_dir().as_os_str().is_empty());
    let rt_dir = get_secure_runtime_dir().unwrap();
    assert_eq!(fs::metadata(&rt_dir).unwrap().permissions().mode() & 0o777, 0o700);
}

#[test]
fn config_enums_roundtrip_as_i32_and_serde() {
    for i in 0..=6 {
        assert_eq!(LayoutPosition::from_i32(i).as_i32(), i);
    }
    for i in 0..=2 {
        assert_eq!(ArrangementMode::from_i32(i).as_i32(), i);
        assert_eq!(BackgroundStyle::from_i32(i).as_i32(), i);
    }
    for i in 0..=3 {
        assert_eq!(FontFamily::from_i32(i).as_i32(), i);
    }
}

#[test]
fn presets_default_to_stack_and_custom_text_color_bg_alpha_full_range() {
    let mut cfg = HudConfig::default();
    assert!(!cfg.custom_text_color_enabled);

    cfg.arrangement = ArrangementMode::Linear;
    apply_preset_minimal(&mut cfg);
    assert_eq!(cfg.arrangement, ArrangementMode::Stack);

    cfg.arrangement = ArrangementMode::Double;
    apply_preset_standard(&mut cfg);
    assert_eq!(cfg.arrangement, ArrangementMode::Stack);

    cfg.arrangement = ArrangementMode::Linear;
    apply_preset_advance(&mut cfg);
    assert_eq!(cfg.arrangement, ArrangementMode::Stack);

    let mut custom_cfg = HudConfig {
        arrangement: ArrangementMode::Linear,
        ..HudConfig::default()
    };
    let dir = tempfile::tempdir().unwrap();
    let missing_path = dir.path().join("no_config.ini");
    assert!(!load_ini_file(&missing_path, &mut custom_cfg));
    apply_preset_standard(&mut custom_cfg);
    assert_eq!(custom_cfg.arrangement, ArrangementMode::Stack);

    // Legacy INI dengan non-white text_color tanpa key custom_text_color_enabled -> otomatis true
    let legacy_path = dir.path().join("legacy_color.ini");
    fs::write(&legacy_path, "text_color=10,20,30\nbg_alpha=1.0\n").unwrap();
    let mut loaded = HudConfig::default();
    assert!(load_ini_file(&legacy_path, &mut loaded));
    assert!(loaded.custom_text_color_enabled);
    assert!((loaded.bg_alpha - 1.0).abs() < 1e-4);

    // Legacy INI dengan white text_color tanpa key -> tetap false
    let white_path = dir.path().join("legacy_white.ini");
    fs::write(&white_path, "text_color=255,255,255\n").unwrap();
    let mut loaded_white = HudConfig::default();
    assert!(load_ini_file(&white_path, &mut loaded_white));
    assert!(!loaded_white.custom_text_color_enabled);

    // Explicit custom_text_color_enabled=0 mempertahankan swatch text_color tetapi flag false
    loaded.custom_text_color_enabled = false;
    let rt_path = dir.path().join("explicit_flag.ini");
    assert!(save_ini_file(&rt_path, &loaded));
    let mut reloaded = HudConfig::default();
    assert!(load_ini_file(&rt_path, &mut reloaded));
    assert!(!reloaded.custom_text_color_enabled);
    assert_eq!(reloaded.text_color, ColorRgba::new(10, 20, 30, 255));
    assert!((reloaded.bg_alpha - 1.0).abs() < 1e-4);
}
