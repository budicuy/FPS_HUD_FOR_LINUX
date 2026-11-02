#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fpshud {

namespace {

HudConfig g_global_config;
std::mutex g_config_mutex;

void trim_inplace(std::string& s) {
    auto start = std::find_if_not(s.begin(), s.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    auto end = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();

    if (start < end) {
        s = std::string(start, end);
    } else {
        s.clear();
    }
}

std::filesystem::path get_config_dir() {
    if (const char* xdg_config = std::getenv("XDG_CONFIG_HOME"); xdg_config && *xdg_config) {
        return std::filesystem::path(xdg_config) / "fpshud";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "fpshud";
    }
    return std::filesystem::path("/tmp");
}

} // namespace

bool HudConfig::has_metric(MetricType m) const {
    return std::find(metrics.begin(), metrics.end(), m) != metrics.end();
}

void HudConfig::set_metric(MetricType m, bool active) {
    auto it = std::find(metrics.begin(), metrics.end(), m);
    if (active && it == metrics.end()) {
        metrics.push_back(m);
    } else if (!active && it != metrics.end()) {
        metrics.erase(it);
    }
}

void apply_preset_minimal(HudConfig& cfg) {
    cfg.active_preset = "minimal";
    cfg.metrics = {
        MetricType::FPS,
        MetricType::FrameTimes
    };
}

void apply_preset_standard(HudConfig& cfg) {
    cfg.active_preset = "standard";
    cfg.metrics = {
        MetricType::FPS,
        MetricType::FpsAvg,
        MetricType::Fps1Low,
        MetricType::FrameTimes,
        MetricType::GpuLoad,
        MetricType::GpuTemp,
        MetricType::CpuLoad,
        MetricType::CpuTemp,
        MetricType::Ram
    };
}

void apply_preset_advance(HudConfig& cfg) {
    cfg.active_preset = "advance";
    cfg.metrics = {
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
        MetricType::GameRam,
        MetricType::SwapRam,
        MetricType::MemPressure,
        MetricType::RamSpeed,
        MetricType::EngineApi,
        MetricType::SystemClock
    };
}

void apply_preset_custom(HudConfig& cfg) {
    cfg.active_preset = "custom";
}

HudConfig init_config_from_env(const char* env_value) {
    HudConfig cfg;
    apply_preset_standard(cfg);

    if (!env_value) {
        cfg.enabled = false;
        return cfg;
    }

    if (std::strcmp(env_value, "1") == 0 ||
        std::strcmp(env_value, "true") == 0 ||
        std::strcmp(env_value, "TRUE") == 0) {
        cfg.enabled = true;
    } else {
        cfg.enabled = false;
    }

    if (cfg.enabled) {
        load_config_from_disk(cfg);
    }

    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_global_config = cfg;
    return cfg;
}

bool save_ini_file(const std::string& path, const HudConfig& cfg, const char* header_comment) {
    std::error_code ec;
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }

    if (header_comment) {
        ofs << header_comment << "\n";
    }

    ofs << "[fpshud]\n";
    ofs << "preset = " << cfg.active_preset << "\n";
    ofs << "layout = " << static_cast<int>(cfg.layout) << "\n";
    ofs << "arrangement = " << static_cast<int>(cfg.arrangement) << "\n";
    ofs << "background = " << static_cast<int>(cfg.background) << "\n";
    ofs << "font_family = " << static_cast<int>(cfg.font_family) << "\n";
    ofs << "font_size = " << cfg.font_size_px << "\n";
    ofs << "gui_scale = " << cfg.gui_scale << "\n";
    ofs << "hud_visible = " << (cfg.hud_visible ? 1 : 0) << "\n";
    ofs << "hud_graph_enabled = " << (cfg.hud_graph_enabled ? 1 : 0) << "\n";
    ofs << "category_colors = " << (cfg.category_colors_enabled ? 1 : 0) << "\n";
    ofs << "thermal_warnings = " << (cfg.thermal_warnings_enabled ? 1 : 0) << "\n";
    ofs << "key_toggle_gui = " << cfg.key_toggle_gui << "\n";
    ofs << "key_toggle_hud = " << cfg.key_toggle_hud << "\n";
    ofs << "key_benchmark = " << cfg.key_benchmark << "\n";
    ofs << "text_opacity = " << cfg.text_opacity << "\n";
    ofs << "bg_alpha = " << cfg.bg_alpha << "\n";
    ofs << "outline_enabled = " << (cfg.outline_enabled ? 1 : 0) << "\n";
    ofs << "outline_thickness = " << cfg.outline_thickness << "\n";
    ofs << "shadow_enabled = " << (cfg.shadow_enabled ? 1 : 0) << "\n";
    ofs << "shadow_offset = " << cfg.shadow_offset << "\n";
    ofs << "shadow_opacity = " << cfg.shadow_opacity << "\n";
    ofs << "margin_x = " << cfg.margin_x << "\n";
    ofs << "margin_y = " << cfg.margin_y << "\n";

    // Text Color
    ofs << "color_r = " << static_cast<int>(cfg.text_color.r) << "\n";
    ofs << "color_g = " << static_cast<int>(cfg.text_color.g) << "\n";
    ofs << "color_b = " << static_cast<int>(cfg.text_color.b) << "\n";
    ofs << "color_a = " << static_cast<int>(cfg.text_color.a) << "\n";

    // Metrics comma-separated integers
    ofs << "metrics = ";
    for (size_t i = 0; i < cfg.metrics.size(); ++i) {
        ofs << static_cast<int>(cfg.metrics[i]);
        if (i + 1 < cfg.metrics.size()) ofs << ",";
    }
    ofs << "\n";

    return true;
}

bool load_ini_file(const std::string& path, HudConfig& cfg) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return false;
    }

    std::string line;
    bool in_section = false;
    bool parsed_any = false;

    while (std::getline(ifs, line)) {
        trim_inplace(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            std::string sec = line.substr(1, line.size() - 2);
            trim_inplace(sec);
            in_section = (sec == "fpshud");
            continue;
        }

        if (!in_section) {
            continue;
        }

        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq_pos);
        std::string val = line.substr(eq_pos + 1);
        trim_inplace(key);
        trim_inplace(val);

        if (key == "preset") {
            cfg.active_preset = val;
            parsed_any = true;
        } else if (key == "gui_scale") {
            cfg.gui_scale = std::clamp(std::strtof(val.c_str(), nullptr), 0.50f, 2.00f);
            parsed_any = true;
        } else if (key == "hud_visible") {
            cfg.hud_visible = (std::atoi(val.c_str()) != 0);
            parsed_any = true;
        } else if (key == "hud_graph_enabled") {
            cfg.hud_graph_enabled = (std::atoi(val.c_str()) != 0);
            parsed_any = true;
        } else if (key == "category_colors") {
            cfg.category_colors_enabled = (std::atoi(val.c_str()) != 0);
            parsed_any = true;
        } else if (key == "thermal_warnings") {
            cfg.thermal_warnings_enabled = (std::atoi(val.c_str()) != 0);
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
        } else if (key == "layout") {
            int v = std::atoi(val.c_str());
            cfg.layout = static_cast<LayoutPosition>(std::clamp(v, 0, 5));
            parsed_any = true;
        } else if (key == "arrangement") {
            int v = std::atoi(val.c_str());
            cfg.arrangement = static_cast<ArrangementMode>(std::clamp(v, 0, 2));
            parsed_any = true;
        } else if (key == "background") {
            int v = std::atoi(val.c_str());
            cfg.background = static_cast<BackgroundStyle>(std::clamp(v, 0, 2));
            parsed_any = true;
        } else if (key == "font_family") {
            int v = std::atoi(val.c_str());
            cfg.font_family = static_cast<FontFamily>(std::clamp(v, 0, 3));
            parsed_any = true;
        } else if (key == "font_size") {
            float sz = std::strtof(val.c_str(), nullptr);
            cfg.font_size_px = std::clamp(sz, 10.0f, 60.0f);
            cfg.font_scale = cfg.font_size_px / kAtlasBaseFontSizePx;
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
            cfg.outline_thickness = std::clamp(std::strtof(val.c_str(), nullptr), 0.5f, 5.0f);
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
        } else if (key == "margin_x") {
            cfg.margin_x = std::strtof(val.c_str(), nullptr);
            parsed_any = true;
        } else if (key == "margin_y") {
            cfg.margin_y = std::strtof(val.c_str(), nullptr);
            parsed_any = true;
        } else if (key == "color_r") {
            cfg.text_color.r = static_cast<uint8_t>(std::clamp(std::atoi(val.c_str()), 0, 255));
            parsed_any = true;
        } else if (key == "color_g") {
            cfg.text_color.g = static_cast<uint8_t>(std::clamp(std::atoi(val.c_str()), 0, 255));
            parsed_any = true;
        } else if (key == "color_b") {
            cfg.text_color.b = static_cast<uint8_t>(std::clamp(std::atoi(val.c_str()), 0, 255));
            parsed_any = true;
        } else if (key == "color_a") {
            cfg.text_color.a = static_cast<uint8_t>(std::clamp(std::atoi(val.c_str()), 0, 255));
            parsed_any = true;
        } else if (key == "metrics") {
            cfg.metrics.clear();
            std::stringstream ss(val);
            std::string item;
            while (std::getline(ss, item, ',')) {
                trim_inplace(item);
                if (!item.empty()) {
                    int m_val = std::atoi(item.c_str());
                    if (m_val >= 0 && m_val < static_cast<int>(kTotalMetricCount)) {
                        cfg.metrics.push_back(static_cast<MetricType>(m_val));
                    }
                }
            }
            parsed_any = true;
        }
    }

    return parsed_any;
}

bool save_config_to_disk(const HudConfig& cfg, std::string* out_path) {
    auto conf_path = get_config_dir() / "config.ini";
    if (out_path) *out_path = conf_path.string();
    return save_ini_file(conf_path.string(), cfg, "; FPSHUD Active Runtime Configuration");
}

bool load_config_from_disk(HudConfig& cfg, std::string* out_path) {
    auto conf_path = get_config_dir() / "config.ini";
    if (out_path) *out_path = conf_path.string();
    return load_ini_file(conf_path.string(), cfg);
}

bool save_custom_preset_to_disk(const HudConfig& cfg, std::string* out_path) {
    auto conf_path = get_config_dir() / "custom_preset.ini";
    if (out_path) *out_path = conf_path.string();
    return save_ini_file(conf_path.string(), cfg, "; FPSHUD User Custom Preset");
}

bool load_custom_preset_from_disk(HudConfig& cfg, std::string* out_path) {
    auto conf_path = get_config_dir() / "custom_preset.ini";
    if (out_path) *out_path = conf_path.string();
    return load_ini_file(conf_path.string(), cfg);
}

HudConfig& get_mutable_config() {
    return g_global_config;
}

const HudConfig& get_global_config() {
    return g_global_config;
}

std::mutex& get_config_mutex() {
    return g_config_mutex;
}

const char* metric_name(MetricType m) {
    switch (m) {
        case MetricType::FPS:          return "FPS (Current)";
        case MetricType::FpsAvg:       return "FPS (Average)";
        case MetricType::Fps1Low:      return "1% Low FPS";
        case MetricType::Fps01Low:     return "0.1% Low FPS";
        case MetricType::FrameTimes:   return "Frametime (ms)";
        case MetricType::GpuLoad:      return "GPU Utilization (%)";
        case MetricType::GpuClock:     return "GPU Core Clock (MHz)";
        case MetricType::GpuVram:      return "VRAM Used / Total (GB)";
        case MetricType::GpuBandwidth: return "PCIe / Memory BW (GB/s)";
        case MetricType::GpuTemp:      return "GPU Temperature (°C)";
        case MetricType::GpuFan:       return "GPU Fan Speed (%)";
        case MetricType::GpuWatt:      return "GPU Power (Watts)";
        case MetricType::CpuLoad:      return "CPU Total Utilization (%)";
        case MetricType::CpuClock:     return "CPU Core Frequency (MHz)";
        case MetricType::CpuWatt:      return "CPU Package Power (Watts)";
        case MetricType::CpuTemp:      return "CPU Package Temp (°C)";
        case MetricType::Ram:          return "System RAM Used / Total";
        case MetricType::GameRam:      return "Game Process RAM (GB)";
        case MetricType::SwapRam:      return "Swap Memory Usage (GB)";
        case MetricType::MemPressure:  return "Memory PSI Pressure (%)";
        case MetricType::RamSpeed:     return "RAM Speed (MT/s) & Type";
        case MetricType::EngineApi:    return "Graphics API (Vulkan/GL)";
        case MetricType::SystemClock:  return "System Time (HH:MM:SS)";
        default:                       return "Unknown Sensor";
    }
}

const char* layout_name(LayoutPosition p) {
    switch (p) {
        case LayoutPosition::TopLeft:      return "Top-Left (Default)";
        case LayoutPosition::TopRight:     return "Top-Right";
        case LayoutPosition::CenterTop:    return "Center-Top";
        case LayoutPosition::BottomLeft:   return "Bottom-Left";
        case LayoutPosition::BottomRight:  return "Bottom-Right";
        case LayoutPosition::CenterBottom: return "Center-Bottom";
        default:                           return "Custom";
    }
}

const char* arrangement_name(ArrangementMode a) {
    switch (a) {
        case ArrangementMode::Stack:  return "Vertical Stack (Grouped Rows)";
        case ArrangementMode::Double: return "Double Column (Side-by-side)";
        case ArrangementMode::Linear: return "Single Line (Horizontal Bar)";
        default:                      return "Default";
    }
}

const char* background_name(BackgroundStyle b) {
    switch (b) {
        case BackgroundStyle::None:        return "Transparent (Clean Text)";
        case BackgroundStyle::Translucent: return "Translucent Box";
        case BackgroundStyle::Solid:       return "Solid Box";
        default:                           return "Default";
    }
}

const char* font_family_name(FontFamily f) {
    switch (f) {
        case FontFamily::SansBold:    return "Sans-Serif Bold (OpenSans / Clean)";
        case FontFamily::MonoBold:    return "Monospace Bold (DejaVu / Cyberpunk)";
        case FontFamily::VeraMono:    return "Vera Sans Mono Bold";
        case FontFamily::AdwaitaMono: return "Adwaita Mono Bold";
        default:                      return "Default Font";
    }
}

const char* gui_keybind_name(int idx) {
    switch (idx) {
        case 0:  return "[Insert] or [F12] (Default)";
        case 1:  return "[Insert] Only";
        case 2:  return "[F10] Key";
        case 3:  return "[Home] Key";
        case 4:  return "[F12] Only";
        default: return "[Insert] Key";
    }
}

const char* hud_keybind_name(int idx) {
    switch (idx) {
        case 0:  return "[F11] Key (Default)";
        case 1:  return "[F8] Key";
        case 2:  return "[End] Key";
        default: return "[F11] Key";
    }
}

const char* bench_keybind_name(int idx) {
    switch (idx) {
        case 0:  return "[F9] Key (Default)";
        case 1:  return "[F7] Key";
        case 2:  return "[PageUp] Key";
        default: return "[F9] Key";
    }
}

} // namespace fpshud
