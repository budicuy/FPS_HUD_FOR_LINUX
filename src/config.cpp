#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace fpshud {

namespace {

constexpr MetricType kCanonicalMetricsOrder[] = {
    MetricType::FPS,
    MetricType::FpsAvg,
    MetricType::Fps1Low,
    MetricType::Fps01Low,
    MetricType::FrameTimes,
    MetricType::GpuLoad,
    MetricType::GpuClock,
    MetricType::GpuVram,
    MetricType::GpuBandwidth,
    MetricType::GpuTemp,
    MetricType::GpuFan,
    MetricType::GpuWatt,
    MetricType::CpuLoad,
    MetricType::CpuClock,
    MetricType::CpuWatt,
    MetricType::CpuTemp,
    MetricType::Ram,
    MetricType::EngineApi,
    MetricType::SystemClock
};

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

void add_unique_metric(std::vector<MetricType>& vec, MetricType m) {
    if (std::find(vec.begin(), vec.end(), m) == vec.end()) {
        vec.push_back(m);
    }
}

void sort_canonical_metrics(std::vector<MetricType>& vec) {
    std::vector<MetricType> sorted;
    sorted.reserve(vec.size());
    for (MetricType m : kCanonicalMetricsOrder) {
        if (std::find(vec.begin(), vec.end(), m) != vec.end()) {
            sorted.push_back(m);
        }
    }
    vec = std::move(sorted);
}

const char* metric_id_key(MetricType m) {
    switch (m) {
        case MetricType::FPS:          return "fps";
        case MetricType::FpsAvg:       return "fpsavg";
        case MetricType::Fps1Low:      return "fps1low";
        case MetricType::Fps01Low:     return "fps01low";
        case MetricType::FrameTimes:   return "frametimes";
        case MetricType::GpuLoad:      return "gpuload";
        case MetricType::GpuClock:     return "gpuclock";
        case MetricType::GpuVram:      return "gpuvram";
        case MetricType::GpuBandwidth: return "gpubw";
        case MetricType::GpuTemp:      return "gputemp";
        case MetricType::GpuFan:       return "gpufan";
        case MetricType::GpuWatt:      return "gpuwatt";
        case MetricType::CpuLoad:      return "cpuload";
        case MetricType::CpuClock:     return "cpuclock";
        case MetricType::CpuWatt:      return "cpuwatt";
        case MetricType::CpuTemp:      return "cputemp";
        case MetricType::Ram:          return "ram";
        case MetricType::EngineApi:    return "api";
        case MetricType::SystemClock:  return "clock";
    }
    return "fps";
}

bool parse_metric_id_key(const std::string& key, MetricType& out) {
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

std::string get_config_file_path() {
    return get_config_dir() + "/config.ini";
}

std::string get_custom_preset_file_path() {
    return get_config_dir() + "/custom_preset.ini";
}

bool save_ini_file(const std::string& path, const HudConfig& cfg, const char* header_comment) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "# " << header_comment << "\n";
    f << "preset=" << cfg.active_preset << "\n";
    f << "hud_visible=" << (cfg.hud_visible ? 1 : 0) << "\n";
    f << "layout=" << static_cast<int>(cfg.layout) << "\n";
    f << "arrangement=" << static_cast<int>(cfg.arrangement) << "\n";
    f << "background=" << static_cast<int>(cfg.background) << "\n";
    f << "font_family=" << static_cast<int>(cfg.font_family) << "\n";
    f << "font_size_px=" << cfg.font_size_px << "\n";
    f << "text_color="
      << static_cast<int>(cfg.text_color.r) << ","
      << static_cast<int>(cfg.text_color.g) << ","
      << static_cast<int>(cfg.text_color.b) << "\n";
    f << "text_opacity=" << cfg.text_opacity << "\n";
    f << "bg_alpha=" << cfg.bg_alpha << "\n";
    f << "outline_enabled=" << (cfg.outline_enabled ? 1 : 0) << "\n";
    f << "outline_thickness=" << cfg.outline_thickness << "\n";
    f << "shadow_enabled=" << (cfg.shadow_enabled ? 1 : 0) << "\n";
    f << "shadow_offset=" << cfg.shadow_offset << "\n";
    f << "shadow_opacity=" << cfg.shadow_opacity << "\n";
    f << "category_colors_enabled=" << (cfg.category_colors_enabled ? 1 : 0) << "\n";
    f << "thermal_warnings_enabled=" << (cfg.thermal_warnings_enabled ? 1 : 0) << "\n";
    f << "hud_graph_enabled=" << (cfg.hud_graph_enabled ? 1 : 0) << "\n";
    f << "key_toggle_gui=" << cfg.key_toggle_gui << "\n";
    f << "key_toggle_hud=" << cfg.key_toggle_hud << "\n";
    f << "key_benchmark=" << cfg.key_benchmark << "\n";

    f << "metrics=";
    for (size_t i = 0; i < cfg.metrics.size(); ++i) {
        if (i > 0) f << ",";
        f << metric_id_key(cfg.metrics[i]);
    }
    f << "\n";
    return true;
}

bool load_ini_file(const std::string& path, HudConfig& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    bool keep_enabled = cfg.enabled;
    bool keep_open = cfg.gui_open;
    bool parsed_any = false;

    std::string line;
    while (std::getline(f, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim_copy(line.substr(0, eq));
        std::string val = trim_copy(line.substr(eq + 1));

        if (key == "preset") {
            cfg.active_preset = val;
            parsed_any = true;
        } else if (key == "hud_visible") {
            cfg.hud_visible = (std::atoi(val.c_str()) != 0);
            parsed_any = true;
        } else if (key == "layout") {
            int v = std::clamp(std::atoi(val.c_str()), 0, 5);
            cfg.layout = static_cast<LayoutPosition>(v);
            parsed_any = true;
        } else if (key == "arrangement") {
            int v = std::clamp(std::atoi(val.c_str()), 0, 2);
            cfg.arrangement = static_cast<ArrangementMode>(v);
            parsed_any = true;
        } else if (key == "background") {
            int v = std::clamp(std::atoi(val.c_str()), 0, 2);
            cfg.background = static_cast<BackgroundStyle>(v);
            parsed_any = true;
        } else if (key == "font_family") {
            int v = std::clamp(std::atoi(val.c_str()), 0, 3);
            cfg.font_family = static_cast<FontFamily>(v);
            parsed_any = true;
        } else if (key == "font_size_px") {
            cfg.font_size_px = std::clamp(std::strtof(val.c_str(), nullptr), 10.0f, 64.0f);
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
            cfg.text_opacity = std::clamp(std::strtof(val.c_str(), nullptr), 0.05f, 1.0f);
            parsed_any = true;
        } else if (key == "bg_alpha") {
            cfg.bg_alpha = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 1.0f);
            parsed_any = true;
        } else if (key == "outline_enabled") {
            cfg.outline_enabled = (std::atoi(val.c_str()) != 0);
            parsed_any = true;
        } else if (key == "outline_thickness") {
            cfg.outline_thickness = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 6.0f);
            parsed_any = true;
        } else if (key == "shadow_enabled") {
            cfg.shadow_enabled = (std::atoi(val.c_str()) != 0);
            parsed_any = true;
        } else if (key == "shadow_offset") {
            cfg.shadow_offset = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 10.0f);
            parsed_any = true;
        } else if (key == "shadow_opacity") {
            cfg.shadow_opacity = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 1.0f);
            parsed_any = true;
        } else if (key == "category_colors_enabled") {
            cfg.category_colors_enabled = (std::atoi(val.c_str()) != 0);
            parsed_any = true;
        } else if (key == "thermal_warnings_enabled") {
            cfg.thermal_warnings_enabled = (std::atoi(val.c_str()) != 0);
            parsed_any = true;
        } else if (key == "hud_graph_enabled") {
            cfg.hud_graph_enabled = (std::atoi(val.c_str()) != 0);
            parsed_any = true;
        } else if (key == "key_toggle_gui") {
            cfg.key_toggle_gui = std::clamp(std::atoi(val.c_str()), 0, 4);
            parsed_any = true;
        } else if (key == "key_toggle_hud") {
            cfg.key_toggle_hud = std::clamp(std::atoi(val.c_str()), 0, 2);
            parsed_any = true;
        } else if (key == "key_benchmark") {
            cfg.key_benchmark = std::clamp(std::atoi(val.c_str()), 0, 2);
            parsed_any = true;
        } else if (key == "metrics") {
            cfg.metrics.clear();
            std::stringstream ss(val);
            std::string item;
            while (std::getline(ss, item, ',')) {
                item = trim_copy(item);
                MetricType m;
                if (parse_metric_id_key(item, m)) {
                    add_unique_metric(cfg.metrics, m);
                }
            }
            sort_canonical_metrics(cfg.metrics);
            parsed_any = true;
        }
    }

    if (!parsed_any) return false;

    if (cfg.metrics.empty()) {
        cfg.metrics.push_back(MetricType::FPS);
    }
    cfg.enabled = keep_enabled;
    cfg.gui_open = keep_open;
    return true;
}

} // namespace

bool HudConfig::has_metric(MetricType m) const {
    return std::find(metrics.begin(), metrics.end(), m) != metrics.end();
}

void HudConfig::set_metric(MetricType m, bool active) {
    if (active) {
        add_unique_metric(metrics, m);
        sort_canonical_metrics(metrics);
    } else {
        metrics.erase(std::remove(metrics.begin(), metrics.end(), m), metrics.end());
    }
}

void apply_preset_minimal(HudConfig& cfg) {
    cfg.active_preset = "minimal";
    cfg.metrics = {
        MetricType::FPS,
        MetricType::FrameTimes
    };
    cfg.layout = LayoutPosition::TopLeft;
    cfg.arrangement = ArrangementMode::Linear;
    cfg.background = BackgroundStyle::Translucent;
    cfg.bg_alpha = 0.35f;
}

void apply_preset_standard(HudConfig& cfg) {
    cfg.active_preset = "standard";
    cfg.metrics = {
        MetricType::FPS,
        MetricType::FrameTimes,
        MetricType::GpuLoad,
        MetricType::GpuClock,
        MetricType::GpuTemp,
        MetricType::CpuLoad,
        MetricType::CpuClock,
        MetricType::CpuTemp,
        MetricType::Ram
    };
    cfg.layout = LayoutPosition::TopLeft;
    cfg.arrangement = ArrangementMode::Stack;
    cfg.background = BackgroundStyle::Translucent;
    cfg.bg_alpha = 0.38f;
}

void apply_preset_advance(HudConfig& cfg) {
    cfg.active_preset = "advance";
    cfg.metrics.assign(std::begin(kCanonicalMetricsOrder), std::end(kCanonicalMetricsOrder));
    cfg.layout = LayoutPosition::CenterTop;
    cfg.arrangement = ArrangementMode::Linear;
    cfg.background = BackgroundStyle::Translucent;
    cfg.bg_alpha = 0.42f;
}

void apply_preset_custom(HudConfig& cfg) {
    if (!load_custom_preset_from_disk(cfg)) {
        if (!load_config_from_disk(cfg)) {
            apply_preset_standard(cfg);
            cfg.text_color = {0, 229, 255, 255};
            cfg.font_size_px = 21.0f;
            cfg.font_scale = cfg.font_size_px / kAtlasBaseFontSizePx;
        }
    }
    cfg.active_preset = "custom";
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

bool save_custom_preset_to_disk(const HudConfig& cfg, std::string* out_path) {
    std::string path = get_custom_preset_file_path();
    if (out_path) *out_path = path;
    return save_ini_file(path, cfg, "FPSHUD User Custom Preset");
}

bool load_custom_preset_from_disk(HudConfig& cfg, std::string* out_path) {
    std::string path = get_custom_preset_file_path();
    if (out_path) *out_path = path;
    bool ok = load_ini_file(path, cfg);
    if (ok) cfg.active_preset = "custom";
    return ok;
}

std::mutex& get_config_mutex() {
    static std::mutex m;
    return m;
}

HudConfig& get_mutable_config() {
    static HudConfig instance = init_config_from_env(std::getenv("FPS_HUD"));
    return instance;
}

const HudConfig& get_global_config() {
    return get_mutable_config();
}

const char* metric_name(MetricType m) {
    switch (m) {
        case MetricType::FPS:          return "FPS";
        case MetricType::FpsAvg:       return "FPS.AVG";
        case MetricType::Fps1Low:      return "FPS.1%";
        case MetricType::Fps01Low:     return "FPS.0.1%";
        case MetricType::FrameTimes:   return "FT";
        case MetricType::GpuLoad:      return "GPU";
        case MetricType::GpuClock:     return "G.CLK";
        case MetricType::GpuVram:      return "VRAM";
        case MetricType::GpuBandwidth: return "G.BW";
        case MetricType::GpuTemp:      return "G.TMP";
        case MetricType::GpuFan:       return "G.FAN";
        case MetricType::GpuWatt:      return "G.PWR";
        case MetricType::CpuLoad:      return "CPU";
        case MetricType::CpuClock:     return "C.CLK";
        case MetricType::CpuWatt:      return "C.PWR";
        case MetricType::CpuTemp:      return "C.TMP";
        case MetricType::Ram:          return "RAM";
        case MetricType::EngineApi:    return "API";
        case MetricType::SystemClock:  return "CLOCK";
    }
    return "UNK";
}

const char* layout_name(LayoutPosition p) {
    switch (p) {
        case LayoutPosition::TopLeft:      return "TopLeft";
        case LayoutPosition::TopRight:     return "TopRight";
        case LayoutPosition::CenterTop:    return "CenterTop";
        case LayoutPosition::BottomLeft:   return "BottomLeft";
        case LayoutPosition::BottomRight:  return "BottomRight";
        case LayoutPosition::CenterBottom: return "CenterBottom";
    }
    return "TopLeft";
}

const char* arrangement_name(ArrangementMode a) {
    switch (a) {
        case ArrangementMode::Stack:  return "Stack (Vertical Grouped)";
        case ArrangementMode::Double: return "Double (2-Col Grouped)";
        case ArrangementMode::Linear: return "Linear (Horizontal Grouped)";
    }
    return "Stack";
}

const char* background_name(BackgroundStyle b) {
    switch (b) {
        case BackgroundStyle::None:        return "None (Transparent)";
        case BackgroundStyle::Translucent: return "Translucent";
        case BackgroundStyle::Solid:       return "Solid";
    }
    return "None";
}

const char* font_family_name(FontFamily f) {
    switch (f) {
        case FontFamily::SansBold:    return "Sans Bold (NVIDIA Style)";
        case FontFamily::MonoBold:    return "DejaVu Mono Bold";
        case FontFamily::VeraMono:    return "Vera Mono Bold";
        case FontFamily::AdwaitaMono: return "Adwaita Mono Bold";
    }
    return "Sans Bold";
}

const char* gui_keybind_name(int idx) {
    switch (idx) {
        case 0: return "Insert / F12 / F10";
        case 1: return "Insert Only";
        case 2: return "F10 Only";
        case 3: return "Home Only";
        case 4: return "F12 Only";
    }
    return "Insert / F12";
}

const char* hud_keybind_name(int idx) {
    switch (idx) {
        case 0: return "F11";
        case 1: return "F8";
        case 2: return "End";
    }
    return "F11";
}

const char* bench_keybind_name(int idx) {
    switch (idx) {
        case 0: return "F9";
        case 1: return "F7";
        case 2: return "PageUp";
    }
    return "F9";
}

} // namespace fpshud
