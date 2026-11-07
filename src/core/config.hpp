#pragma once

#include <cstddef>
#include <cstdint>
#include <inplace_vector>
#include <mutex>
#include <string>

namespace fpshud {

enum class MetricType {
  FPS = 0,
  FpsAvg,
  Fps1Low,
  Fps01Low,
  FrameTimes,
  GpuLoad,
  GpuClock,
  GpuVramClock,
  GpuVram,
  GpuBandwidth,
  GpuTemp,
  GpuFan,
  GpuWatt,
  CpuLoad,
  CpuAllCoreLoad,
  CpuClock,
  CpuAllCoreClock,
  CpuWatt,
  CpuTemp,
  Ram,
  GameRam,      // Resident memory used by the game process itself
  SwapRam,      // Swap / ZRAM usage
  MemPressure,  // Linux PSI memory pressure (stutter indicator)
  EngineApi,
  GpuModel,
  CpuModel,
  SystemClock,
  Battery,   // Laptop / Handheld battery % & discharge power (W)
  DiskIo,    // Physical disk Read/Write throughput (MB/s)
  DiskTemp,  // NVMe / SSD temperature (°C)
  NetSpeed   // Active network interface RX/TX throughput (MB/s or KB/s)
};

enum class LayoutPosition {
  TopLeft = 0,
  TopCenter = 1,
  TopRight = 2,
  BottomLeft = 3,
  BottomCenter = 4,
  BottomRight = 5,
  // Backward-compatible aliases
  CenterTop = TopCenter,
  CenterBottom = BottomCenter
};

struct Keybind {
  uint32_t mod_key = 0;   // 0 if single key, or Linux KEY_* modifier/first key
  uint32_t main_key = 0;  // Linux KEY_* primary key (e.g. KEY_F7=65, KEY_F12=88)

  constexpr bool operator==(const Keybind& o) const noexcept {
    return mod_key == o.mod_key && main_key == o.main_key;
  }
  constexpr bool operator!=(const Keybind& o) const noexcept { return !(*this == o); }
};

enum class ArrangementMode {
  Stack = 0,  // Vertical grouped rows
  Double,     // 2-column grouped grid
  Linear      // Single horizontal grouped line
};

enum class BackgroundStyle {
  None = 0,     // Transparent no background
  Translucent,  // Semi-transparent dark rounded box
  Solid         // Solid dark box
};

enum class FontFamily {
  SansBold = 0,    // OpenSans-Bold (Embedded TTF, NVIDIA clean style)
  MonoBold = 1,    // DejaVuSansMono-Bold / VeraMono-Bold
  VeraMono = 2,    // Bitstream Vera Sans Mono Bold (Embedded TTF)
  AdwaitaMono = 3  // AdwaitaMono-Bold
};

constexpr float kAtlasBaseFontSizePx = 20.0f;

// Total number of selectable HUD sensors (MetricType entries)
constexpr std::size_t kTotalMetricCount = 31;
constexpr std::size_t kMaxMetricsCapacity = 32;
static_assert(kTotalMetricCount <= kMaxMetricsCapacity,
              "kMaxMetricsCapacity must accommodate all MetricType sensors");

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
  std::inplace_vector<MetricType, kMaxMetricsCapacity> metrics;
  LayoutPosition layout = LayoutPosition::TopLeft;
  ArrangementMode arrangement = ArrangementMode::Stack;
  BackgroundStyle background = BackgroundStyle::Translucent;
  FontFamily font_family = FontFamily::SansBold;
  ColorRGBA text_color = {255, 255, 255, 255};

  // Dynamic Color Coding & HUD Graph
  bool category_colors_enabled = true;
  bool thermal_warnings_enabled = true;
  bool dim_labels_enabled = true;
  bool hud_graph_enabled = false;

  // Customizable Keybindings (Legacy index + interactive 1-or-2-key bindings)
  int key_toggle_gui = 0;
  int key_toggle_hud = 0;
  int key_benchmark = 0;
  int key_record = 0;

  // Linux evdev keycodes: KEY_F12=88, KEY_F11=87, KEY_F9=67, KEY_F7=65, KEY_F8=66, KEY_F10=68
  Keybind bind_toggle_gui{0, 88};
  Keybind bind_toggle_hud{0, 87};
  Keybind bind_benchmark{0, 67};
  Keybind bind_record{0, 65};
  Keybind bind_save_replay{0, 66};
  Keybind bind_screenshot{0, 68};

  // Instant Replay & Screenshot Settings
  bool replay_enabled = false;       // Default OFF (0 MB RAM, 0% CPU/GPU overhead)
  int replay_duration_sec = 30;      // 15, 30, 60, 120 seconds
  bool screenshot_with_hud = false;  // false = Clean Game (Tanpa HUD), true = With HUD (Dengan HUD)

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

  // Experimental Features (Isolated in src/experimental-feature/)
  bool record_adaptive_vfr = false;
  int record_vfr_max_fps = 120;

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

  bool has_metric(MetricType m) const noexcept;
  void set_metric(MetricType m, bool active) noexcept;
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
bool save_ini_file(const std::string& path, const HudConfig& cfg,
                   const char* header_comment = nullptr);
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
const char* linux_keycode_name(uint32_t code) noexcept;
std::string format_keybind(const Keybind& kb);
std::string get_default_record_dir();
std::string get_effective_record_dir(const HudConfig& cfg);

}  // namespace fpshud
