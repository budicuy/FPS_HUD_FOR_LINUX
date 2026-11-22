#![forbid(unsafe_code)]
//! Pure-Rust configuration, keybinds, presets, and secure XDG path helpers for FPSHUD.

pub mod config;
pub mod keybind;
pub mod paths;

pub use config::{
    ArrangementMode, BackgroundStyle, ColorRgba, FontFamily, HudConfig, Keybind, LayoutPosition,
    MetricType, PresetType, apply_preset_advance, apply_preset_custom, apply_preset_minimal,
    apply_preset_standard, init_config_from_env, load_config_from_disk, load_ini_file,
    reset_layout_to_default, save_config_to_disk, save_ini_file,
};
