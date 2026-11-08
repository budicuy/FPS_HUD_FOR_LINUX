#include "config.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <system_error>

namespace fs = std::filesystem;

namespace fpshud {

namespace {

constexpr MetricType kCanonicalMetricsOrder[] = {MetricType::FPS,
                                                 MetricType::FpsAvg,
                                                 MetricType::Fps1Low,
                                                 MetricType::Fps01Low,
                                                 MetricType::FrameTimes,
                                                 MetricType::GpuLoad,
                                                 MetricType::GpuClock,
                                                 MetricType::GpuVramClock,
                                                 MetricType::GpuVram,
                                                 MetricType::GpuBandwidth,
                                                 MetricType::GpuTemp,
                                                 MetricType::GpuFan,
                                                 MetricType::GpuWatt,
                                                 MetricType::CpuLoad,
                                                 MetricType::CpuAllCoreLoad,
                                                 MetricType::CpuClock,
                                                 MetricType::CpuAllCoreClock,
                                                 MetricType::CpuWatt,
                                                 MetricType::CpuTemp,
                                                 MetricType::Ram,
                                                 MetricType::GameRam,
                                                 MetricType::SwapRam,
                                                 MetricType::MemPressure,
                                                 MetricType::EngineApi,
                                                 MetricType::GpuModel,
                                                 MetricType::CpuModel,
                                                 MetricType::SystemClock,
                                                 MetricType::Battery,
                                                 MetricType::DiskIo,
                                                 MetricType::DiskTemp,
                                                 MetricType::NetSpeed};
static_assert(sizeof(kCanonicalMetricsOrder) / sizeof(kCanonicalMetricsOrder[0]) ==
                  kTotalMetricCount,
              "kTotalMetricCount must match the number of MetricType entries");

std::string trim_copy(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

constexpr std::size_t canonical_metric_index(MetricType m) {
  for (std::size_t i = 0; i < std::size(kCanonicalMetricsOrder); ++i) {
    if (kCanonicalMetricsOrder[i] == m) {
      return i;
    }
  }
  return std::size(kCanonicalMetricsOrder);
}

void add_unique_metric(std::inplace_vector<MetricType, kMaxMetricsCapacity>& vec,
                       MetricType m) noexcept {
  if (std::find(vec.begin(), vec.end(), m) == vec.end()) {
    vec.try_push_back(m);
  }
}

void sort_canonical_metrics(std::inplace_vector<MetricType, kMaxMetricsCapacity>& vec) noexcept {
  std::sort(vec.begin(), vec.end(), [](MetricType a, MetricType b) noexcept {
    return canonical_metric_index(a) < canonical_metric_index(b);
  });
}

const char* metric_id_key(MetricType m) {
  switch (m) {
    case MetricType::FPS:
      return "fps";
    case MetricType::FpsAvg:
      return "fpsavg";
    case MetricType::Fps1Low:
      return "fps1low";
    case MetricType::Fps01Low:
      return "fps01low";
    case MetricType::FrameTimes:
      return "frametimes";
    case MetricType::GpuLoad:
      return "gpuload";
    case MetricType::GpuClock:
      return "gpuclock";
    case MetricType::GpuVramClock:
      return "gpuvramclock";
    case MetricType::GpuVram:
      return "gpuvram";
    case MetricType::GpuBandwidth:
      return "gpubw";
    case MetricType::GpuTemp:
      return "gputemp";
    case MetricType::GpuFan:
      return "gpufan";
    case MetricType::GpuWatt:
      return "gpuwatt";
    case MetricType::CpuLoad:
      return "cpuload";
    case MetricType::CpuAllCoreLoad:
      return "cpuallload";
    case MetricType::CpuClock:
      return "cpuclock";
    case MetricType::CpuAllCoreClock:
      return "cpuallclock";
    case MetricType::CpuWatt:
      return "cpuwatt";
    case MetricType::CpuTemp:
      return "cputemp";
    case MetricType::Ram:
      return "ram";
    case MetricType::GameRam:
      return "gameram";
    case MetricType::SwapRam:
      return "swap";
    case MetricType::MemPressure:
      return "mempsi";
    case MetricType::EngineApi:
      return "api";
    case MetricType::GpuModel:
      return "gpumodel";
    case MetricType::CpuModel:
      return "cpumodel";
    case MetricType::SystemClock:
      return "clock";
    case MetricType::Battery:
      return "battery";
    case MetricType::DiskIo:
      return "disk_io";
    case MetricType::DiskTemp:
      return "disk_temp";
    case MetricType::NetSpeed:
      return "net_speed";
  }
  return "fps";
}

bool parse_metric_id_key(const std::string& key, MetricType& out) {
  if (key == "cpuhybrid") {
    out = MetricType::CpuAllCoreLoad;
    return true;
  }
  for (MetricType m : kCanonicalMetricsOrder) {
    if (key == metric_id_key(m)) {
      out = m;
      return true;
    }
  }
  return false;
}

std::string get_config_dir() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
    return std::string(xdg) + "/fpshud";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::string(home) + "/.config/fpshud";
  }
  return "/tmp/fpshud";
}

std::string get_config_file_path() { return get_config_dir() + "/config.ini"; }

void ensure_parent_dir(const std::string& file_path) {
  if (file_path.empty()) return;
  std::error_code ec;
  fs::path p(file_path);
  if (p.has_parent_path()) {
    fs::create_directories(p.parent_path(), ec);
  }
}

static inline int safe_strtoi(const std::string& str, int fallback = 0) {
  if (str.empty()) return fallback;
  char* end = nullptr;
  errno = 0;
  long v = std::strtol(str.c_str(), &end, 10);
  if (errno != 0 || end == str.c_str()) return fallback;
  if (v < std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
  if (v > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
  return static_cast<int>(v);
}

}  // namespace

float safe_strtof(std::string_view str, float fallback) noexcept {
  if (str.empty()) return fallback;
  // Locale-independent parsing: reliably parses '.' decimal separator across all user locales
  const char* start = str.data();
  const char* end = str.data() + str.size();
  while (start < end && (*start == ' ' || *start == '\t')) ++start;
  while (end > start &&
         (*(end - 1) == ' ' || *(end - 1) == '\t' || *(end - 1) == '\r' || *(end - 1) == '\n')) {
    --end;
  }
  if (start >= end) return fallback;

  float v = fallback;
  auto [ptr, ec] = std::from_chars(start, end, v);
  if (ec == std::errc() && !std::isnan(v) && !std::isinf(v)) {
    return v;
  }
  return fallback;
}

std::string sanitize_ini_value(const std::string& val) {
  std::string out;
  out.reserve(val.size());
  for (char c : val) {
    if (c != '\r' && c != '\n') {
      out += c;
    }
  }
  return out;
}

bool save_ini_file(const std::string& path, const HudConfig& cfg, const char* header_comment) {
  if (path.empty()) return false;
  ensure_parent_dir(path);

  std::string tmp_path = path + ".tmp";
  FILE* fp = std::fopen(tmp_path.c_str(), "w");
  if (!fp) {
    fp = std::fopen(path.c_str(), "w");
    if (!fp) return false;
    tmp_path.clear();
  }

  std::fprintf(fp, "# %s\n",
               sanitize_ini_value(header_comment ? header_comment : "FPSHUD Config").c_str());
  std::fprintf(fp, "preset=%s\n", sanitize_ini_value(cfg.active_preset).c_str());
  std::fprintf(fp, "gui_scale=%.2f\n", cfg.gui_scale);
  std::fprintf(fp, "hud_visible=%d\n", cfg.hud_visible ? 1 : 0);
  std::fprintf(fp, "layout=%d\n", static_cast<int>(cfg.layout));
  std::fprintf(fp, "custom_pos_x=%.4f\n", std::clamp(cfg.custom_pos_x, 0.0f, 1.0f));
  std::fprintf(fp, "custom_pos_y=%.4f\n", std::clamp(cfg.custom_pos_y, 0.0f, 1.0f));
  std::fprintf(fp, "arrangement=%d\n", static_cast<int>(cfg.arrangement));
  std::fprintf(fp, "background=%d\n", static_cast<int>(cfg.background));
  std::fprintf(fp, "font_family=%d\n", static_cast<int>(cfg.font_family));
  std::fprintf(fp, "font_size_px=%.2f\n", cfg.font_size_px);
  std::fprintf(fp, "text_color=%d,%d,%d\n", static_cast<int>(cfg.text_color.r),
               static_cast<int>(cfg.text_color.g), static_cast<int>(cfg.text_color.b));
  std::fprintf(fp, "text_opacity=%.3f\n", cfg.text_opacity);
  std::fprintf(fp, "bg_alpha=%.3f\n", cfg.bg_alpha);
  std::fprintf(fp, "outline_enabled=%d\n", cfg.outline_enabled ? 1 : 0);
  std::fprintf(fp, "outline_thickness=%.2f\n", cfg.outline_thickness);
  std::fprintf(fp, "shadow_enabled=%d\n", cfg.shadow_enabled ? 1 : 0);
  std::fprintf(fp, "shadow_offset=%.2f\n", cfg.shadow_offset);
  std::fprintf(fp, "shadow_opacity=%.2f\n", cfg.shadow_opacity);
  std::fprintf(fp, "category_colors_enabled=%d\n", cfg.category_colors_enabled ? 1 : 0);
  std::fprintf(fp, "thermal_warnings_enabled=%d\n", cfg.thermal_warnings_enabled ? 1 : 0);
  std::fprintf(fp, "dim_labels_enabled=%d\n", cfg.dim_labels_enabled ? 1 : 0);
  std::fprintf(fp, "hud_graph_enabled=%d\n", cfg.hud_graph_enabled ? 1 : 0);
  std::fprintf(fp, "key_toggle_gui=%d\n", cfg.key_toggle_gui);
  std::fprintf(fp, "key_toggle_hud=%d\n", cfg.key_toggle_hud);
  std::fprintf(fp, "key_benchmark=%d\n", cfg.key_benchmark);
  std::fprintf(fp, "key_record=%d\n", cfg.key_record);
  std::fprintf(fp, "bind_toggle_gui=%u,%u\n", cfg.bind_toggle_gui.mod_key,
               cfg.bind_toggle_gui.main_key);
  std::fprintf(fp, "bind_toggle_hud=%u,%u\n", cfg.bind_toggle_hud.mod_key,
               cfg.bind_toggle_hud.main_key);
  std::fprintf(fp, "bind_benchmark=%u,%u\n", cfg.bind_benchmark.mod_key,
               cfg.bind_benchmark.main_key);
  std::fprintf(fp, "bind_record=%u,%u\n", cfg.bind_record.mod_key, cfg.bind_record.main_key);
  std::fprintf(fp, "bind_save_replay=%u,%u\n", cfg.bind_save_replay.mod_key,
               cfg.bind_save_replay.main_key);
  std::fprintf(fp, "bind_screenshot=%u,%u\n", cfg.bind_screenshot.mod_key,
               cfg.bind_screenshot.main_key);
  std::fprintf(fp, "replay_enabled=%d\n", cfg.replay_enabled ? 1 : 0);
  std::fprintf(fp, "replay_duration_sec=%d\n", cfg.replay_duration_sec);
  std::fprintf(fp, "screenshot_with_hud=%d\n", cfg.screenshot_with_hud ? 1 : 0);
  std::fprintf(fp, "record_resolution=%d\n", cfg.record_resolution);
  std::fprintf(fp, "record_fps=%d\n", cfg.record_fps);
  std::fprintf(fp, "record_bitrate_kbps=%d\n", cfg.record_bitrate_kbps);
  std::fprintf(fp, "record_audio_source=%d\n", cfg.record_audio_source);
  std::fprintf(fp, "record_container=%d\n", cfg.record_container);
  std::fprintf(fp, "record_codec=%d\n", cfg.record_codec);
  std::fprintf(fp, "record_quality_mode=%d\n", cfg.record_quality_mode);
  std::fprintf(fp, "record_cq_level=%d\n", cfg.record_cq_level);
  std::fprintf(fp, "record_directory=%s\n", sanitize_ini_value(cfg.record_directory).c_str());

  std::fprintf(fp, "metrics=");
  for (size_t i = 0; i < cfg.metrics.size(); ++i) {
    if (i > 0) std::fputc(',', fp);
    std::fputs(metric_id_key(cfg.metrics[i]), fp);
  }
  std::fputc('\n', fp);

  std::fprintf(fp, "\n[Experimental]\n");
  std::fprintf(fp, "record_adaptive_vfr=%d\n", cfg.record_adaptive_vfr ? 1 : 0);
  std::fprintf(fp, "record_vfr_max_fps=%d\n", cfg.record_vfr_max_fps);

  bool write_error = (std::ferror(fp) != 0 || std::fflush(fp) != 0);
  std::fclose(fp);

  if (write_error) {
    if (!tmp_path.empty()) {
      std::remove(tmp_path.c_str());
    }
    return false;
  }

  if (!tmp_path.empty()) {
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
      std::remove(tmp_path.c_str());
      return false;
    }
  }
  return true;
}

bool load_ini_file(const std::string& path, HudConfig& cfg) {
  FILE* fp = std::fopen(path.c_str(), "r");
  if (!fp) return false;

  bool keep_enabled = cfg.enabled;
  bool keep_open = cfg.gui_open;
  bool parsed_any = false;

  char buf[1024];
  while (std::fgets(buf, sizeof(buf), fp)) {
    std::string line = trim_copy(buf);
    if (line.empty() || line[0] == '#') continue;

    auto eq = line.find('=');
    if (eq == std::string::npos) continue;

    std::string key = trim_copy(line.substr(0, eq));
    std::string val = trim_copy(line.substr(eq + 1));

    if (key == "preset") {
      cfg.active_preset = val;
      parsed_any = true;
    } else if (key == "gui_scale") {
      cfg.gui_scale = std::clamp(safe_strtof(val, 1.0f), 0.50f, 2.00f);
      parsed_any = true;
    } else if (key == "hud_visible") {
      cfg.hud_visible = (safe_strtoi(val, 1) != 0);
      parsed_any = true;
    } else if (key == "layout") {
      int v = std::clamp(safe_strtoi(val, 0), 0, 6);
      cfg.layout = static_cast<LayoutPosition>(v);
      parsed_any = true;
    } else if (key == "custom_pos_x") {
      cfg.custom_pos_x = std::clamp(safe_strtof(val, 0.01f), 0.0f, 1.0f);
      parsed_any = true;
    } else if (key == "custom_pos_y") {
      cfg.custom_pos_y = std::clamp(safe_strtof(val, 0.02f), 0.0f, 1.0f);
      parsed_any = true;
    } else if (key == "arrangement") {
      int v = std::clamp(safe_strtoi(val, 0), 0, 2);
      cfg.arrangement = static_cast<ArrangementMode>(v);
      parsed_any = true;
    } else if (key == "background") {
      int v = std::clamp(safe_strtoi(val, 0), 0, 2);
      cfg.background = static_cast<BackgroundStyle>(v);
      parsed_any = true;
    } else if (key == "font_family") {
      int v = std::clamp(safe_strtoi(val, 0), 0, 3);
      cfg.font_family = static_cast<FontFamily>(v);
      parsed_any = true;
    } else if (key == "font_size_px") {
      cfg.font_size_px = std::clamp(safe_strtof(val, 24.0f), 10.0f, 64.0f);
      cfg.font_scale = cfg.font_size_px / kAtlasBaseFontSizePx;
      parsed_any = true;
    } else if (key == "text_color") {
      int r = 255, g = 255, b = 255;
      if (std::sscanf(val.c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
        cfg.text_color.r = static_cast<uint8_t>(std::clamp(r, 0, 255));
        cfg.text_color.g = static_cast<uint8_t>(std::clamp(g, 0, 255));
        cfg.text_color.b = static_cast<uint8_t>(std::clamp(b, 0, 255));
        cfg.text_color.a = 255;
      }
      parsed_any = true;
    } else if (key == "text_opacity") {
      cfg.text_opacity = std::clamp(safe_strtof(val, 1.0f), 0.05f, 1.0f);
      parsed_any = true;
    } else if (key == "bg_alpha") {
      cfg.bg_alpha = std::clamp(safe_strtof(val, 0.40f), 0.0f, 1.0f);
      parsed_any = true;
    } else if (key == "outline_enabled") {
      cfg.outline_enabled = (safe_strtoi(val, 0) != 0);
      parsed_any = true;
    } else if (key == "outline_thickness") {
      cfg.outline_thickness = std::clamp(safe_strtof(val, 1.4f), 0.0f, 6.0f);
      parsed_any = true;
    } else if (key == "shadow_enabled") {
      cfg.shadow_enabled = (safe_strtoi(val, 0) != 0);
      parsed_any = true;
    } else if (key == "shadow_offset") {
      cfg.shadow_offset = std::clamp(safe_strtof(val, 2.0f), 0.0f, 10.0f);
      parsed_any = true;
    } else if (key == "shadow_opacity") {
      cfg.shadow_opacity = std::clamp(safe_strtof(val, 0.70f), 0.0f, 1.0f);
      parsed_any = true;
    } else if (key == "category_colors_enabled") {
      cfg.category_colors_enabled = (safe_strtoi(val, 1) != 0);
      parsed_any = true;
    } else if (key == "thermal_warnings_enabled") {
      cfg.thermal_warnings_enabled = (safe_strtoi(val, 1) != 0);
      parsed_any = true;
    } else if (key == "dim_labels_enabled") {
      cfg.dim_labels_enabled = (safe_strtoi(val, 1) != 0);
      parsed_any = true;
    } else if (key == "hud_graph_enabled") {
      cfg.hud_graph_enabled = (safe_strtoi(val, 0) != 0);
      parsed_any = true;
    } else if (key == "key_toggle_gui") {
      cfg.key_toggle_gui = std::clamp(safe_strtoi(val, 0), 0, 4);
      parsed_any = true;
    } else if (key == "key_toggle_hud") {
      cfg.key_toggle_hud = std::clamp(safe_strtoi(val, 0), 0, 2);
      parsed_any = true;
    } else if (key == "key_benchmark") {
      cfg.key_benchmark = std::clamp(safe_strtoi(val, 0), 0, 1);
      parsed_any = true;
    } else if (key == "key_record") {
      cfg.key_record = std::clamp(safe_strtoi(val, 0), 0, 2);
      parsed_any = true;
    } else if (key == "bind_toggle_gui" || key == "bind_toggle_hud" || key == "bind_benchmark" ||
               key == "bind_record" || key == "bind_save_replay" || key == "bind_screenshot") {
      unsigned int mod_k = 0, main_k = 0;
      if (std::sscanf(val.c_str(), "%u,%u", &mod_k, &main_k) == 2 && main_k > 0 && main_k < 512 &&
          mod_k < 512) {
        Keybind kb{mod_k, main_k};
        if (key == "bind_toggle_gui")
          cfg.bind_toggle_gui = kb;
        else if (key == "bind_toggle_hud")
          cfg.bind_toggle_hud = kb;
        else if (key == "bind_benchmark")
          cfg.bind_benchmark = kb;
        else if (key == "bind_record")
          cfg.bind_record = kb;
        else if (key == "bind_save_replay")
          cfg.bind_save_replay = kb;
        else if (key == "bind_screenshot")
          cfg.bind_screenshot = kb;
      }
      parsed_any = true;
    } else if (key == "replay_enabled") {
      cfg.replay_enabled = (safe_strtoi(val, 0) != 0);
      parsed_any = true;
    } else if (key == "replay_duration_sec") {
      int d = safe_strtoi(val, 30);
      if (d != 15 && d != 30 && d != 60 && d != 120) d = 30;
      cfg.replay_duration_sec = d;
      parsed_any = true;
    } else if (key == "screenshot_with_hud") {
      cfg.screenshot_with_hud = (safe_strtoi(val, 0) != 0);
      parsed_any = true;
    } else if (key == "record_resolution") {
      cfg.record_resolution = std::clamp(safe_strtoi(val, 0), 0, 3);
      parsed_any = true;
    } else if (key == "record_fps") {
      cfg.record_fps = std::clamp(safe_strtoi(val, 60), 0, 240);
      parsed_any = true;
    } else if (key == "record_bitrate_kbps") {
      cfg.record_bitrate_kbps = std::clamp(safe_strtoi(val, 15000), 1000, 100000);
      parsed_any = true;
    } else if (key == "record_audio_source") {
      cfg.record_audio_source = std::clamp(safe_strtoi(val, 0), 0, 3);
      parsed_any = true;
    } else if (key == "record_container") {
      cfg.record_container = std::clamp(safe_strtoi(val, 0), 0, 2);
      parsed_any = true;
    } else if (key == "record_codec") {
      cfg.record_codec = std::clamp(safe_strtoi(val, 0), 0, 1);
      parsed_any = true;
    } else if (key == "record_quality_mode") {
      cfg.record_quality_mode = std::clamp(safe_strtoi(val, 0), 0, 1);
      parsed_any = true;
    } else if (key == "record_cq_level") {
      cfg.record_cq_level = std::clamp(safe_strtoi(val, 20), 16, 30);
      parsed_any = true;
    } else if (key == "record_directory") {
      cfg.record_directory = val;
      parsed_any = true;
    } else if (key == "record_adaptive_vfr") {
      cfg.record_adaptive_vfr = (safe_strtoi(val, 0) != 0);
      parsed_any = true;
    } else if (key == "record_vfr_max_fps") {
      cfg.record_vfr_max_fps = std::clamp(safe_strtoi(val, 120), 30, 240);
      parsed_any = true;
    } else if (key == "metrics") {
      cfg.metrics.clear();
      size_t start = 0;
      while (start <= val.size()) {
        auto comma = val.find(',', start);
        std::string item = trim_copy(
            (comma == std::string::npos) ? val.substr(start) : val.substr(start, comma - start));
        MetricType m;
        if (!item.empty() && parse_metric_id_key(item, m)) {
          add_unique_metric(cfg.metrics, m);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
      sort_canonical_metrics(cfg.metrics);
      parsed_any = true;
    }
  }
  std::fclose(fp);

  if (!parsed_any) return false;

  if (cfg.metrics.empty()) {
    cfg.metrics.try_push_back(MetricType::FPS);
  }
  cfg.enabled = keep_enabled;
  cfg.gui_open = keep_open;
  return true;
}

bool HudConfig::has_metric(MetricType m) const noexcept {
  return std::find(metrics.begin(), metrics.end(), m) != metrics.end();
}

void HudConfig::set_metric(MetricType m, bool active) noexcept {
  if (active) {
    add_unique_metric(metrics, m);
    sort_canonical_metrics(metrics);
  } else {
    std::erase(metrics, m);
  }
}

void apply_preset_minimal(HudConfig& cfg) {
  cfg.active_preset = "minimal";
  cfg.metrics = {MetricType::FPS, MetricType::FrameTimes};
  cfg.layout = LayoutPosition::TopLeft;
  cfg.arrangement = ArrangementMode::Linear;
  cfg.background = BackgroundStyle::Translucent;
  cfg.bg_alpha = 0.35f;
}

void apply_preset_standard(HudConfig& cfg) {
  cfg.active_preset = "standard";
  cfg.metrics = {MetricType::FPS,      MetricType::FrameTimes, MetricType::GpuLoad,
                 MetricType::GpuClock, MetricType::GpuTemp,    MetricType::CpuLoad,
                 MetricType::CpuClock, MetricType::CpuTemp,    MetricType::Ram};
  cfg.layout = LayoutPosition::TopLeft;
  cfg.arrangement = ArrangementMode::Stack;
  cfg.background = BackgroundStyle::Translucent;
  cfg.bg_alpha = 0.38f;
}

void apply_preset_advance(HudConfig& cfg) {
  cfg.active_preset = "advance";
  cfg.metrics.clear();
  for (MetricType m : kCanonicalMetricsOrder) {
    cfg.metrics.try_push_back(m);
  }
  cfg.layout = LayoutPosition::TopCenter;
  cfg.arrangement = ArrangementMode::Linear;
  cfg.background = BackgroundStyle::Translucent;
  cfg.bg_alpha = 0.42f;
}

void apply_preset_custom(HudConfig& cfg) {
  if (!load_config_from_disk(cfg)) {
    apply_preset_standard(cfg);
    cfg.text_color = {0, 229, 255, 255};
    cfg.font_size_px = 21.0f;
    cfg.font_scale = cfg.font_size_px / kAtlasBaseFontSizePx;
  }
  cfg.active_preset = "custom";
}

void reset_layout_to_default(HudConfig& cfg) noexcept {
  cfg.layout = LayoutPosition::TopLeft;
  cfg.arrangement = ArrangementMode::Stack;
  cfg.custom_pos_x = 0.01f;
  cfg.custom_pos_y = 0.02f;
}

HudConfig init_config_from_env(const char* env_value) {
  HudConfig cfg;
  if (!env_value) {
    cfg.enabled = false;
    return cfg;
  }

  std::string val = trim_copy(env_value);
  if (val != "1") {
    cfg.enabled = false;
    return cfg;
  }

  cfg.enabled = true;
  if (!load_config_from_disk(cfg)) {
    apply_preset_standard(cfg);
  }
  return cfg;
}

bool save_config_to_disk(const HudConfig& cfg, std::string* out_path) {
  std::string path = get_config_file_path();
  if (out_path) *out_path = path;
  return save_ini_file(path, cfg, "FPSHUD Saved Configuration");
}

bool load_config_from_disk(HudConfig& cfg, std::string* out_path) {
  std::string path = get_config_file_path();
  if (out_path) *out_path = path;
  return load_ini_file(path, cfg);
}

std::mutex& get_config_mutex() {
  static std::mutex m;
  return m;
}

HudConfig& get_mutable_config() {
  static HudConfig instance = init_config_from_env(std::getenv("FPS_HUD"));
  return instance;
}

const HudConfig& get_global_config() { return get_mutable_config(); }

const char* layout_name(LayoutPosition p) {
  switch (p) {
    case LayoutPosition::TopLeft:
      return "Top-Left";
    case LayoutPosition::TopCenter:
      return "Top-Center";
    case LayoutPosition::TopRight:
      return "Top-Right";
    case LayoutPosition::BottomLeft:
      return "Bottom-Left";
    case LayoutPosition::BottomCenter:
      return "Bottom-Center";
    case LayoutPosition::BottomRight:
      return "Bottom-Right";
    case LayoutPosition::Custom:
      return "Custom";
  }
  return "Top-Left";
}

const char* arrangement_name(ArrangementMode a) {
  switch (a) {
    case ArrangementMode::Stack:
      return "Stack (Vertical Grouped)";
    case ArrangementMode::Double:
      return "Double (2-Col Grouped)";
    case ArrangementMode::Linear:
      return "Linear (Horizontal Grouped)";
  }
  return "Stack";
}

const char* linux_keycode_name(uint32_t code) noexcept {
  switch (code) {
    case 0:
      return "None";
    case 1:
      return "ESC";
    case 2:
      return "1";
    case 3:
      return "2";
    case 4:
      return "3";
    case 5:
      return "4";
    case 6:
      return "5";
    case 7:
      return "6";
    case 8:
      return "7";
    case 9:
      return "8";
    case 10:
      return "9";
    case 11:
      return "0";
    case 12:
      return "-";
    case 13:
      return "=";
    case 14:
      return "Backspace";
    case 15:
      return "Tab";
    case 16:
      return "Q";
    case 17:
      return "W";
    case 18:
      return "E";
    case 19:
      return "R";
    case 20:
      return "T";
    case 21:
      return "Y";
    case 22:
      return "U";
    case 23:
      return "I";
    case 24:
      return "O";
    case 25:
      return "P";
    case 26:
      return "[";
    case 27:
      return "]";
    case 28:
      return "Enter";
    case 29:
    case 97:
      return "Ctrl";
    case 30:
      return "A";
    case 31:
      return "S";
    case 32:
      return "D";
    case 33:
      return "F";
    case 34:
      return "G";
    case 35:
      return "H";
    case 36:
      return "J";
    case 37:
      return "K";
    case 38:
      return "L";
    case 39:
      return ";";
    case 40:
      return "'";
    case 41:
      return "`";
    case 42:
    case 54:
      return "Shift";
    case 43:
      return "\\";
    case 44:
      return "Z";
    case 45:
      return "X";
    case 46:
      return "C";
    case 47:
      return "V";
    case 48:
      return "B";
    case 49:
      return "N";
    case 50:
      return "M";
    case 51:
      return ",";
    case 52:
      return ".";
    case 53:
      return "/";
    case 55:
      return "KP*";
    case 56:
    case 100:
      return "Alt";
    case 57:
      return "Space";
    case 58:
      return "CapsLock";
    case 59:
      return "F1";
    case 60:
      return "F2";
    case 61:
      return "F3";
    case 62:
      return "F4";
    case 63:
      return "F5";
    case 64:
      return "F6";
    case 65:
      return "F7";
    case 66:
      return "F8";
    case 67:
      return "F9";
    case 68:
      return "F10";
    case 69:
      return "NumLock";
    case 70:
      return "ScrollLock";
    case 87:
      return "F11";
    case 88:
      return "F12";
    case 99:
      return "PrintScr";
    case 102:
      return "Home";
    case 103:
      return "Up";
    case 104:
      return "PageUp";
    case 105:
      return "Left";
    case 106:
      return "Right";
    case 107:
      return "End";
    case 108:
      return "Down";
    case 109:
      return "PageDown";
    case 110:
      return "Insert";
    case 111:
      return "Delete";
    case 119:
      return "Pause";
    case 125:
    case 126:
      return "Super";
    default:
      return "Key";
  }
}

std::string format_keybind(const Keybind& kb) {
  auto fmt_single = [](uint32_t c) -> std::string {
    const char* name = linux_keycode_name(c);
    if (std::strcmp(name, "Key") == 0) {
      return "Key#" + std::to_string(c);
    }
    return name;
  };
  if (kb.main_key == 0) return "None";
  if (kb.mod_key != 0 && kb.mod_key != kb.main_key) {
    return fmt_single(kb.mod_key) + " + " + fmt_single(kb.main_key);
  }
  return fmt_single(kb.main_key);
}

std::string get_default_record_dir() {
  if (const char* xdg_vid = std::getenv("XDG_VIDEOS_DIR"); xdg_vid && *xdg_vid) {
    return std::string(xdg_vid) + "/FPSHUD";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    std::error_code ec;
    fs::path p_id = fs::path(home) / "Video" / "FPSHUD";
    if (fs::exists(fs::path(home) / "Video", ec)) {
      return p_id.string();
    }
    return (fs::path(home) / "Videos" / "FPSHUD").string();
  }
  return "/tmp/FPSHUD";
}

std::string get_effective_record_dir(const HudConfig& cfg) {
  if (!cfg.record_directory.empty()) {
    return cfg.record_directory;
  }
  return get_default_record_dir();
}

}  // namespace fpshud
