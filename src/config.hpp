#pragma once

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

constexpr float kAtlasBaseFontSizePx = 32.0f;

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
    // key_toggle_hud: 0=F11, 1=F8, 2=End
    // key_benchmark : 0=F9, 1=F7, 2=PageUp
    int key_toggle_gui = 0;
    int key_toggle_hud = 0;
    int key_benchmark = 0;

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

} // namespace fpshud
