#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fpshud {

enum class MetricType {
    FPS = 0,
    FpsAvg,
    Fps1Low,
    Fps01Low,
    FrameTimes,
    GpuLoad,
    GpuClock,
    GpuVram,
    GpuBandwidth,
    GpuTemp,
    GpuFan,
    GpuWatt,
    CpuLoad,
    CpuClock,
    CpuWatt,
    CpuTemp,
    Ram,
    GameRam,      // Resident memory used by the game process itself
    SwapRam,      // Swap / ZRAM usage
    MemPressure,  // Linux PSI memory pressure (stutter indicator)
    RamSpeed,     // RAM type + speed (MT/s)
    EngineApi,
    SystemClock
};

enum class LayoutPosition {
    TopLeft = 0,
    TopRight,
    CenterTop,
    BottomLeft,
    BottomRight,
    CenterBottom
};

enum class ArrangementMode {
    Stack = 0, // Vertical grouped rows
    Double,    // 2-column grouped grid
    Linear     // Single horizontal grouped line
};

enum class BackgroundStyle {
    None = 0,    // Transparent no background
    Translucent, // Semi-transparent dark rounded box
    Solid        // Solid dark box
};

enum class FontFamily {
    SansBold = 0,    // OpenSans-Bold (Embedded TTF, NVIDIA clean style)
    MonoBold = 1,    // DejaVuSansMono-Bold / VeraMono-Bold
    VeraMono = 2,    // Bitstream Vera Sans Mono Bold (Embedded TTF)
    AdwaitaMono = 3  // AdwaitaMono-Bold
};

constexpr float kAtlasBaseFontSizePx = 20.0f;

// Total number of selectable HUD sensors (MetricType entries)
constexpr std::size_t kTotalMetricCount = 23;

struct ColorRGBA {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    uint8_t a = 255;
};

struct HudConfig {
    bool enabled = false;
    bool gui_open = false;
    bool hud_visible = true;

    std::string active_preset = "standard";
    std::vector<MetricType> metrics;
    LayoutPosition layout = LayoutPosition::TopLeft;
    ArrangementMode arrangement = ArrangementMode::Stack;
    BackgroundStyle background = BackgroundStyle::Translucent;
    FontFamily font_family = FontFamily::SansBold;
    ColorRGBA text_color = {255, 255, 255, 255};

    // Dynamic Color Coding & HUD Graph
    bool category_colors_enabled = true;
    bool thermal_warnings_enabled = true;
    bool hud_graph_enabled = false;

    // Customizable Keybindings
    // key_toggle_gui: 0=Insert/F12, 1=Insert, 2=F10, 3=Home, 4=F12
    // key_toggle_hud: 0=F11, 1=End
    // key_benchmark : 0=F9, 1=PageUp
    // key_record    : 0=F7, 1=Alt+F9, 2=ScrollLock
    int key_toggle_gui = 0;
    int key_toggle_hud = 0;
    int key_benchmark = 0;
    int key_record = 0;

    // Screen Recorder Settings
    // record_resolution: 0=Native (100%), 1=1440p, 2=1080p, 3=720p
    int record_resolution = 0;
    // record_fps: 30, 60 (default), 0=Match Display
    int record_fps = 60;
    // record_bitrate_kbps: 8000, 18000 (default), 32000, 50000
    int record_bitrate_kbps = 18000;
    // record_audio_source: 0=Game Audio Only (@DEFAULT_MONITOR@), 1=Mic Only, 2=Game+Mic, 3=Mute
    int record_audio_source = 0;
    // record_container: 0=.mp4 (FastStart), 1=.mkv, 2=.webm
    int record_container = 0;
    // record_codec: 0=H.264 (AVC), 1=HEVC (H.265 - 50% Hemat Disk, Rekomendasi)
    int record_codec = 1;
    // record_quality_mode: 0=CQP (Kualitas Adaptif - Rekomendasi), 1=Fixed VBR
    int record_quality_mode = 0;
    // record_cq_level: 16-30 (18=Ultra, 22=Balanced/Default, 26=Performance)
    int record_cq_level = 22;
    // record_directory: default ~/Videos/FPSHUD
    std::string record_directory;

    // GUI Menu & Render Scaling (0.50f .. 2.00f, default 1.0f = 100%)
    float gui_scale = 1.0f;

    // Font sizing (atlas rasterized at 32.0px for crisp vector quality)
    float font_size_px = 20.0f;
    float font_scale = 20.0f / kAtlasBaseFontSizePx;

    // Opacity controls (0.0f .. 1.0f)
    float text_opacity = 1.0f;
    float bg_alpha = 0.38f;

    // Outline controls
    bool outline_enabled = true;
    float outline_thickness = 1.4f;

    // Drop shadow controls
    bool shadow_enabled = true;
    float shadow_offset = 2.0f;
    float shadow_opacity = 0.85f;

    // Screen edge margins
    float margin_x = 16.0f;
    float margin_y = 14.0f;

    bool has_metric(MetricType m) const;
    void set_metric(MetricType m, bool active);
};

// Preset applicators (Minimal, Standard, Advance, Custom)
void apply_preset_minimal(HudConfig& cfg);
void apply_preset_standard(HudConfig& cfg);
void apply_preset_advance(HudConfig& cfg);
void apply_preset_custom(HudConfig& cfg);

// Initialize config purely from FPS_HUD=1 (enabled) or FPS_HUD=0 (disabled)
HudConfig init_config_from_env(const char* env_value);

// Save / Load main config file (~/.config/fpshud/config.ini)
bool save_config_to_disk(const HudConfig& cfg, std::string* out_path = nullptr);
bool load_config_from_disk(HudConfig& cfg, std::string* out_path = nullptr);

// Generic INI file serializer / parser
bool save_ini_file(const std::string& path, const HudConfig& cfg, const char* header_comment = nullptr);
bool load_ini_file(const std::string& path, HudConfig& cfg);

// Save / Load user-customizable preset (~/.config/fpshud/custom_preset.ini)
bool save_custom_preset_to_disk(const HudConfig& cfg, std::string* out_path = nullptr);
bool load_custom_preset_from_disk(HudConfig& cfg, std::string* out_path = nullptr);

// Global mutable config & mutex for live GUI editing
HudConfig& get_mutable_config();
const HudConfig& get_global_config();
std::mutex& get_config_mutex();

// Helper names for GUI & CLI diagnostics
const char* metric_name(MetricType m);
const char* layout_name(LayoutPosition p);
const char* arrangement_name(ArrangementMode a);
const char* background_name(BackgroundStyle b);
const char* font_family_name(FontFamily f);
const char* gui_keybind_name(int idx);
const char* hud_keybind_name(int idx);
const char* bench_keybind_name(int idx);
const char* record_keybind_name(int idx);
std::string get_default_record_dir();
std::string get_effective_record_dir(const HudConfig& cfg);

} // namespace fpshud
