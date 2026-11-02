#include "overlay_ui.hpp"
#include "embedded_fonts.hpp"
#include "gui_menu.hpp"
#include "input_manager.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fpshud {

namespace {

enum class GroupCategory {
    FPS = 0,
    GPU = 1,
    CPU = 2,
    RAM = 3,
    SYS = 4
};

// Fixed-capacity stack structs with separated prefix/number/unit for high readability (zero heap allocation!)
struct StyledValue {
    char prefix[12] = {0}; // e.g., "AVG ", "1% ", "0.1% ", "FAN "
    char number[24] = {0}; // e.g., "60", "39", "1320", "1.7", "56"
    char unit[12]   = {0}; // e.g., "%", "MHz", "GB", "°C", "W", "ms"
    ImU32 color = 0;
};

struct MetricGroup {
    GroupCategory category = GroupCategory::FPS;
    const char* label = "FPS";
    ImU32 label_color = 0;
    std::array<StyledValue, 8> values{};
    size_t value_count = 0;
};

struct MetricGroupList {
    std::array<MetricGroup, 5> items{};
    size_t count = 0;
};

GroupCategory get_metric_category(MetricType m) noexcept {
    switch (m) {
        case MetricType::FPS:
        case MetricType::FpsAvg:
        case MetricType::Fps1Low:
        case MetricType::Fps01Low:
        case MetricType::FrameTimes:
            return GroupCategory::FPS;
        case MetricType::GpuLoad:
        case MetricType::GpuClock:
        case MetricType::GpuVram:
        case MetricType::GpuBandwidth:
        case MetricType::GpuTemp:
        case MetricType::GpuFan:
        case MetricType::GpuWatt:
            return GroupCategory::GPU;
        case MetricType::CpuLoad:
        case MetricType::CpuClock:
        case MetricType::CpuWatt:
        case MetricType::CpuTemp:
            return GroupCategory::CPU;
        case MetricType::Ram:
        case MetricType::GameRam:
        case MetricType::SwapRam:
        case MetricType::MemPressure:
        case MetricType::RamSpeed:
            return GroupCategory::RAM;
        case MetricType::EngineApi:
        case MetricType::SystemClock:
        default:
            return GroupCategory::SYS;
    }
}

const char* get_group_label(GroupCategory cat) noexcept {
    switch (cat) {
        case GroupCategory::FPS: return "FPS";
        case GroupCategory::GPU: return "GPU";
        case GroupCategory::CPU: return "CPU";
        case GroupCategory::RAM: return "RAM";
        case GroupCategory::SYS: return "SYS";
    }
    return "HUD";
}

ImU32 get_category_color(GroupCategory cat, uint8_t alpha) noexcept {
    switch (cat) {
        case GroupCategory::FPS: return IM_COL32(110, 240, 130, alpha); // Crisp Mint Green
        case GroupCategory::GPU: return IM_COL32(80, 210, 100, alpha);  // NVIDIA / Radeon Green
        case GroupCategory::CPU: return IM_COL32(80, 185, 255, alpha);  // Sky Cyan / Intel / Ryzen Blue
        case GroupCategory::RAM: return IM_COL32(255, 175, 60, alpha);  // Warm Orange Amber
        case GroupCategory::SYS: return IM_COL32(200, 205, 215, alpha); // Platinum Gray
    }
    return IM_COL32(255, 255, 255, alpha);
}

ImU32 get_thermal_color(uint32_t temp_c, uint8_t alpha) noexcept {
    if (temp_c >= 90) return IM_COL32(255, 55, 55, alpha);   // Critical Red
    if (temp_c >= 82) return IM_COL32(255, 140, 40, alpha);  // High Warm Orange
    if (temp_c >= 74) return IM_COL32(255, 215, 60, alpha);  // Medium Yellow
    return IM_COL32(255, 255, 255, alpha);                   // Normal White
}

void format_styled_value(MetricType m, const TelemetryData& data, const HudConfig& cfg, uint8_t alpha, StyledValue& out) {
    out.prefix[0] = '\0';
    out.number[0] = '\0';
    out.unit[0] = '\0';

    ImU32 default_col = IM_COL32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
    out.color = default_col;

    switch (m) {
        case MetricType::FPS:
            std::snprintf(out.number, sizeof(out.number), "%.0f", data.fps);
            break;
        case MetricType::FpsAvg:
            std::snprintf(out.prefix, sizeof(out.prefix), "AVG ");
            std::snprintf(out.number, sizeof(out.number), "%.0f", data.fps_avg);
            break;
        case MetricType::Fps1Low:
            std::snprintf(out.prefix, sizeof(out.prefix), "1%% ");
            std::snprintf(out.number, sizeof(out.number), "%.0f", data.fps_1pct_low);
            break;
        case MetricType::Fps01Low:
            std::snprintf(out.prefix, sizeof(out.prefix), "0.1%% ");
            std::snprintf(out.number, sizeof(out.number), "%.0f", data.fps_01pct_low);
            break;
        case MetricType::FrameTimes:
            std::snprintf(out.number, sizeof(out.number), "%.1f", data.frametime_ms);
            std::snprintf(out.unit, sizeof(out.unit), "ms");
            break;

        case MetricType::GpuLoad:
            std::snprintf(out.number, sizeof(out.number), "%u", data.gpu_load_percent);
            std::snprintf(out.unit, sizeof(out.unit), "%%");
            break;
        case MetricType::GpuClock:
            std::snprintf(out.number, sizeof(out.number), "%u", data.gpu_clock_mhz);
            std::snprintf(out.unit, sizeof(out.unit), "MHz");
            break;
        case MetricType::GpuVram:
            std::snprintf(out.number, sizeof(out.number), "%.1f/%.1f", data.vram_used_gb, data.vram_total_gb);
            std::snprintf(out.unit, sizeof(out.unit), "GB");
            break;
        case MetricType::GpuBandwidth:
            std::snprintf(out.number, sizeof(out.number), "%.1f", data.gpu_bandwidth_gbs);
            std::snprintf(out.unit, sizeof(out.unit), "GB/s");
            break;
        case MetricType::GpuTemp:
            std::snprintf(out.number, sizeof(out.number), "%u", data.gpu_temp_c);
            std::snprintf(out.unit, sizeof(out.unit), "°C");
            if (cfg.thermal_warnings_enabled) {
                out.color = get_thermal_color(data.gpu_temp_c, alpha);
            }
            break;
        case MetricType::GpuFan:
            std::snprintf(out.prefix, sizeof(out.prefix), "FAN ");
            std::snprintf(out.number, sizeof(out.number), "%u", data.gpu_fan_percent);
            std::snprintf(out.unit, sizeof(out.unit), "%%");
            break;
        case MetricType::GpuWatt:
            std::snprintf(out.number, sizeof(out.number), "%.0f", data.gpu_power_watts);
            std::snprintf(out.unit, sizeof(out.unit), "W");
            break;

        case MetricType::CpuLoad:
            std::snprintf(out.number, sizeof(out.number), "%.0f", data.cpu_load_percent);
            std::snprintf(out.unit, sizeof(out.unit), "%%");
            break;
        case MetricType::CpuClock:
            std::snprintf(out.number, sizeof(out.number), "%u", data.cpu_clock_mhz);
            std::snprintf(out.unit, sizeof(out.unit), "MHz");
            break;
        case MetricType::CpuWatt:
            std::snprintf(out.number, sizeof(out.number), "%.0f", data.cpu_power_watts);
            std::snprintf(out.unit, sizeof(out.unit), "W");
            break;
        case MetricType::CpuTemp:
            std::snprintf(out.number, sizeof(out.number), "%u", data.cpu_temp_c);
            std::snprintf(out.unit, sizeof(out.unit), "°C");
            if (cfg.thermal_warnings_enabled) {
                out.color = get_thermal_color(data.cpu_temp_c, alpha);
            }
            break;

        case MetricType::Ram:
            std::snprintf(out.number, sizeof(out.number), "%.1f/%.1f", data.ram_used_gb, data.ram_total_gb);
            std::snprintf(out.unit, sizeof(out.unit), "GB");
            break;
        case MetricType::GameRam:
            std::snprintf(out.prefix, sizeof(out.prefix), "GAME ");
            std::snprintf(out.number, sizeof(out.number), "%.1f", data.game_ram_used_gb);
            std::snprintf(out.unit, sizeof(out.unit), "GB");
            break;
        case MetricType::SwapRam:
            std::snprintf(out.prefix, sizeof(out.prefix), "SWAP ");
            if (data.swap_total_gb <= 0.05f) {
                std::snprintf(out.number, sizeof(out.number), "OFF");
            } else {
                std::snprintf(out.number, sizeof(out.number), "%.1f/%.1f", data.swap_used_gb, data.swap_total_gb);
                std::snprintf(out.unit, sizeof(out.unit), "GB");
            }
            break;
        case MetricType::MemPressure:
            std::snprintf(out.prefix, sizeof(out.prefix), "PSI ");
            if (data.mem_pressure_pct < 0.0f) {
                std::snprintf(out.number, sizeof(out.number), "N/A");
            } else {
                std::snprintf(out.number, sizeof(out.number), "%.1f", data.mem_pressure_pct);
                std::snprintf(out.unit, sizeof(out.unit), "%%");
                if (data.mem_pressure_pct > 10.0f) {
                    out.color = IM_COL32(255, 60, 60, alpha); // Red stutter alert
                } else if (data.mem_pressure_pct > 3.0f) {
                    out.color = IM_COL32(255, 180, 50, alpha); // Yellow warn
                }
            }
            break;
        case MetricType::RamSpeed:
            if (data.ram_speed_mts > 0) {
                if (data.ram_type[0]) {
                    std::snprintf(out.number, sizeof(out.number), "%s %u", data.ram_type, data.ram_speed_mts);
                } else {
                    std::snprintf(out.number, sizeof(out.number), "%u", data.ram_speed_mts);
                }
                std::snprintf(out.unit, sizeof(out.unit), "MT/s");
            } else {
                std::snprintf(out.number, sizeof(out.number), "%u", data.ram_percent);
                std::snprintf(out.unit, sizeof(out.unit), "%%");
            }
            break;

        case MetricType::EngineApi:
            if (data.engine_api == GraphicsApi::Vulkan) {
                std::snprintf(out.number, sizeof(out.number), "Vulkan");
            } else if (data.engine_api == GraphicsApi::OpenGL) {
                std::snprintf(out.number, sizeof(out.number), "OpenGL");
            } else {
                std::snprintf(out.number, sizeof(out.number), "API");
            }
            break;
        case MetricType::SystemClock:
            std::snprintf(out.number, sizeof(out.number), "%s", data.system_clock[0] ? data.system_clock : "00:00");
            break;
    }
}

MetricGroupList build_metric_groups(const HudConfig& cfg, const TelemetryData& data, uint8_t alpha) {
    MetricGroupList list{};

    for (MetricType m : cfg.metrics) {
        GroupCategory cat = get_metric_category(m);

        // Find or create category group
        MetricGroup* target = nullptr;
        for (size_t i = 0; i < list.count; ++i) {
            if (list.items[i].category == cat) {
                target = &list.items[i];
                break;
            }
        }

        if (!target && list.count < list.items.size()) {
            target = &list.items[list.count++];
            target->category = cat;
            target->label = get_group_label(cat);
            target->label_color = cfg.category_colors_enabled ?
                                  get_category_color(cat, alpha) :
                                  IM_COL32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
            target->value_count = 0;
        }

        if (target && target->value_count < target->values.size()) {
            format_styled_value(m, data, cfg, alpha, target->values[target->value_count++]);
        }
    }

    return list;
}

void draw_crisp_text(const char* text, ImU32 color, const HudConfig& cfg) {
    if (!text || !*text) return;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    pos.x = std::round(pos.x);
    pos.y = std::round(pos.y);
    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize();

    // Scale outline & shadow proportionally to font size to avoid black stroke bleed on small fonts
    float scale_factor = std::clamp(cfg.font_size_px / 20.0f, 0.5f, 1.5f);

    // 1. Drop shadow (drawn first, furthest behind)
    if (cfg.shadow_enabled && cfg.shadow_offset > 0.0f) {
        uint8_t sh_alpha = static_cast<uint8_t>(std::clamp(cfg.shadow_opacity * cfg.text_opacity * 255.0f, 0.0f, 255.0f));
        ImU32 drop_col = IM_COL32(0, 0, 0, sh_alpha);
        float d = std::max(1.0f, std::round(cfg.shadow_offset * scale_factor));
        draw_list->AddText(font, font_size, ImVec2(pos.x + d, pos.y + d), drop_col, text);
    }

    // 2. High-contrast Crisp Outline (4-way cross, plus diagonal corners if thick)
    if (cfg.outline_enabled && cfg.outline_thickness > 0.0f) {
        uint8_t out_alpha = static_cast<uint8_t>(std::clamp(0.95f * cfg.text_opacity * 255.0f, 0.0f, 255.0f));
        ImU32 outline_col = IM_COL32(0, 0, 0, out_alpha);
        float o = std::max(1.0f, std::round(cfg.outline_thickness * scale_factor));

        draw_list->AddText(font, font_size, ImVec2(pos.x - o, pos.y - o), outline_col, text);
        draw_list->AddText(font, font_size, ImVec2(pos.x + o, pos.y - o), outline_col, text);
        draw_list->AddText(font, font_size, ImVec2(pos.x - o, pos.y + o), outline_col, text);
        draw_list->AddText(font, font_size, ImVec2(pos.x + o, pos.y + o), outline_col, text);

        if (o > 1.5f && cfg.font_size_px >= 18.0f) {
            draw_list->AddText(font, font_size, ImVec2(pos.x,     pos.y - o), outline_col, text);
            draw_list->AddText(font, font_size, ImVec2(pos.x - o, pos.y),     outline_col, text);
            draw_list->AddText(font, font_size, ImVec2(pos.x + o, pos.y),     outline_col, text);
            draw_list->AddText(font, font_size, ImVec2(pos.x,     pos.y + o), outline_col, text);
        }
    }

    // 3. Foreground crisp text glyph
    draw_list->AddText(font, font_size, pos, color, text);

    ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text);
    ImGui::Dummy(text_size);
}

// Draws a styled value: optional prefix ("AVG "), bold number ("60"), muted unit ("ms")
void draw_styled_metric_item(const StyledValue& sv, const HudConfig& cfg, ImU32 muted_col) {
    if (sv.prefix[0] != '\0') {
        draw_crisp_text(sv.prefix, muted_col, cfg);
        ImGui::SameLine(0.0f, 1.0f);
    }

    if (sv.number[0] != '\0') {
        draw_crisp_text(sv.number, sv.color, cfg);
    }

    if (sv.unit[0] != '\0') {
        ImGui::SameLine(0.0f, 1.0f);
        draw_crisp_text(sv.unit, muted_col, cfg);
    }
}

void draw_styled_group_values(const MetricGroup& g, const HudConfig& cfg, ImU32 muted_col) {
    for (size_t v = 0; v < g.value_count; ++v) {
        if (v > 0) {
            ImGui::SameLine(0.0f, 4.0f);
            draw_crisp_text("·", muted_col, cfg);
            ImGui::SameLine(0.0f, 4.0f);
        }
        draw_styled_metric_item(g.values[v], cfg, muted_col);
    }
}

void draw_mini_frametime_graph(const HudConfig& cfg, uint8_t alpha) {
    auto& telemetry = get_telemetry_collector();
    float graph_w = 190.0f * (cfg.font_size_px / 20.0f);
    float graph_h = 32.0f * (cfg.font_size_px / 20.0f);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_PlotLines, IM_COL32(110, 240, 130, alpha));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, static_cast<uint8_t>(alpha * 0.4f)));
    ImGui::PlotLines("##HUD_MiniFTGraph",
                     telemetry.get_ft_history(),
                     TelemetryCollector::kGraphHistorySize,
                     telemetry.get_graph_offset(),
                     "frametime (ms)", 0.0f, 50.0f, ImVec2(graph_w, graph_h));
    ImGui::PopStyleColor(2);
}

ImFont* g_font_gui_small = nullptr;   // 12px
ImFont* g_font_gui_normal = nullptr;  // 16px
ImFont* g_font_gui_large = nullptr;   // 24px

ImFont* g_hud_fonts_small[4] = {};    // 14px for each family
ImFont* g_hud_fonts_normal[4] = {};   // 20px for each family
ImFont* g_hud_fonts_large[4] = {};    // 32px for each family

ImFont* load_ttf_with_fallback(ImGuiIO& io,
                               const std::vector<std::string>& candidate_paths,
                               const unsigned char* fallback_mem,
                               size_t fallback_size,
                               float size_px) {
    // Compact ASCII + Latin-1 Supplement (includes ° degree and · middle dot)
    static const ImWchar kCompactAsciiRanges[] = {0x0020, 0x00FF, 0};

    ImFontConfig font_cfg;
    font_cfg.OversampleH = (size_px < 16.0f) ? 3 : 2;
    font_cfg.OversampleV = 1;
    font_cfg.PixelSnapH = true;
    font_cfg.GlyphRanges = kCompactAsciiRanges;

    std::error_code ec;
    for (const auto& path : candidate_paths) {
        if (!path.empty() && std::filesystem::exists(path, ec)) {
            ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), size_px, &font_cfg);
            if (f) return f;
        }
    }

    if (fallback_mem && fallback_size > 16) {
        ImFontConfig mem_cfg = font_cfg;
        mem_cfg.FontDataOwnedByAtlas = false;
        ImFont* f = io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char*>(fallback_mem),
            static_cast<int>(fallback_size),
            size_px,
            &mem_cfg);
        if (f) return f;
    }
    return io.Fonts->AddFontDefault();
}

} // namespace

ImFont* get_gui_font(float gui_scale, float& out_base_size) {
    if (gui_scale <= 0.75f && g_font_gui_small) {
        out_base_size = 12.0f;
        return g_font_gui_small;
    }
    if (gui_scale > 1.30f && g_font_gui_large) {
        out_base_size = 24.0f;
        return g_font_gui_large;
    }
    out_base_size = 16.0f;
    return g_font_gui_normal ? g_font_gui_normal : ImGui::GetFont();
}

ImFont* get_hud_font(FontFamily family, float font_size_px, float& out_base_size) {
    int idx = std::clamp(static_cast<int>(family), 0, 3);
    if (font_size_px <= 15.0f && g_hud_fonts_small[idx]) {
        out_base_size = 14.0f;
        return g_hud_fonts_small[idx];
    }
    if (font_size_px >= 26.0f && g_hud_fonts_large[idx]) {
        out_base_size = 32.0f;
        return g_hud_fonts_large[idx];
    }
    out_base_size = 20.0f;
    return g_hud_fonts_normal[idx] ? g_hud_fonts_normal[idx] : ImGui::GetFont();
}

void setup_overlay_font(ImGuiIO& io) {
    std::string home_fonts;
    if (const char* home = std::getenv("HOME"); home && *home) {
        home_fonts = std::string(home) + "/.local/share/fpshud/fonts/";
    }

    std::vector<std::string> open_sans_paths = {
        home_fonts + "OpenSans-Bold.ttf",
        "/run/host/usr/share/fonts/TTF/OpenSans-Bold.ttf",
        "/usr/share/fonts/TTF/OpenSans-Bold.ttf"
    };

    std::vector<std::string> dejavu_paths = {
        home_fonts + "DejaVuSansMono-Bold.ttf",
        "/run/host/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf"
    };

    std::vector<std::string> adwaita_paths = {
        home_fonts + "AdwaitaMono-Bold.ttf",
        "/run/host/usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf",
        "/usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf"
    };

    // 1. Dedicated GUI Menu Fonts (Crisp OpenSans at 12px, 16px, 24px)
    g_font_gui_small  = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF, sizeof(kEmbeddedOpenSansBoldTTF), 12.0f);
    g_font_gui_normal = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF, sizeof(kEmbeddedOpenSansBoldTTF), 16.0f);
    g_font_gui_large  = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF, sizeof(kEmbeddedOpenSansBoldTTF), 24.0f);

    // 2. Multi-tier HUD Fonts for each of the 4 FontFamily choices
    // Slot 0: SansBold
    g_hud_fonts_small[0]  = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF, sizeof(kEmbeddedOpenSansBoldTTF), 14.0f);
    g_hud_fonts_normal[0] = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF, sizeof(kEmbeddedOpenSansBoldTTF), 20.0f);
    g_hud_fonts_large[0]  = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF, sizeof(kEmbeddedOpenSansBoldTTF), 32.0f);

    // Slot 1: MonoBold
    g_hud_fonts_small[1]  = load_ttf_with_fallback(io, dejavu_paths, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF), 14.0f);
    g_hud_fonts_normal[1] = load_ttf_with_fallback(io, dejavu_paths, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF), 20.0f);
    g_hud_fonts_large[1]  = load_ttf_with_fallback(io, dejavu_paths, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF), 32.0f);

    // Slot 2: VeraMono
    g_hud_fonts_small[2]  = load_ttf_with_fallback(io, {}, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF), 14.0f);
    g_hud_fonts_normal[2] = load_ttf_with_fallback(io, {}, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF), 20.0f);
    g_hud_fonts_large[2]  = load_ttf_with_fallback(io, {}, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF), 32.0f);

    // Slot 3: AdwaitaMono
    g_hud_fonts_small[3]  = load_ttf_with_fallback(io, adwaita_paths, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF), 14.0f);
    g_hud_fonts_normal[3] = load_ttf_with_fallback(io, adwaita_paths, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF), 20.0f);
    g_hud_fonts_large[3]  = load_ttf_with_fallback(io, adwaita_paths, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF), 32.0f);
}

void render_overlay_ui(HudConfig& cfg,
                       const TelemetryData& data,
                       float display_width,
                       float display_height) {
    if (!cfg.enabled || display_width <= 0.0f || display_height <= 0.0f) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    // 1. Render Interactive GUI Menu (uses dedicated GUI font inside render_gui_menu)
    render_gui_menu(cfg, data, display_width, display_height);
    get_input_manager().sync_grab_state(io, cfg);

    if (!cfg.hud_visible || cfg.metrics.empty()) {
        return;
    }

    // 2. Render HUD Overlay using multi-tier HUD font
    float base_hud_font_px = 20.0f;
    ImFont* chosen_font = get_hud_font(cfg.font_family, cfg.font_size_px, base_hud_font_px);
    if (chosen_font) {
        ImGui::PushFont(chosen_font);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoInputs;

    if (cfg.background == BackgroundStyle::None || cfg.bg_alpha <= 0.001f) {
        flags |= ImGuiWindowFlags_NoBackground;
    } else {
        ImGui::SetNextWindowBgAlpha(cfg.bg_alpha);
    }

    ImVec2 pos(cfg.margin_x, cfg.margin_y);
    ImVec2 pivot(0.0f, 0.0f);

    switch (cfg.layout) {
        case LayoutPosition::TopLeft:
            pos = ImVec2(cfg.margin_x, cfg.margin_y);
            pivot = ImVec2(0.0f, 0.0f);
            break;
        case LayoutPosition::TopRight:
            pos = ImVec2(display_width - cfg.margin_x, cfg.margin_y);
            pivot = ImVec2(1.0f, 0.0f);
            break;
        case LayoutPosition::CenterTop:
            pos = ImVec2(display_width * 0.5f, cfg.margin_y);
            pivot = ImVec2(0.5f, 0.0f);
            break;
        case LayoutPosition::BottomLeft:
            pos = ImVec2(cfg.margin_x, display_height - cfg.margin_y);
            pivot = ImVec2(0.0f, 1.0f);
            break;
        case LayoutPosition::BottomRight:
            pos = ImVec2(display_width - cfg.margin_x, display_height - cfg.margin_y);
            pivot = ImVec2(1.0f, 1.0f);
            break;
        case LayoutPosition::CenterBottom:
            pos = ImVec2(display_width * 0.5f, display_height - cfg.margin_y);
            pivot = ImVec2(0.5f, 1.0f);
            break;
    }

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);

    // Smooth subtle padding and rounding for the HUD box
    float ui_ratio = std::clamp(cfg.font_size_px / 20.0f, 0.6f, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * ui_ratio, 8.0f * ui_ratio));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * ui_ratio);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * ui_ratio, 3.0f * ui_ratio));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.06f, cfg.bg_alpha));

    if (ImGui::Begin("##FPSHUD_Overlay", nullptr, flags)) {
        cfg.font_scale = std::clamp(cfg.font_size_px / base_hud_font_px, 0.40f, 2.50f);
        ImGui::SetWindowFontScale(cfg.font_scale);

        uint8_t fg_alpha = static_cast<uint8_t>(std::clamp(cfg.text_opacity * 255.0f, 10.0f, 255.0f));
        ImU32 muted_col = IM_COL32(165, 175, 190, static_cast<uint8_t>(fg_alpha * 0.85f));

        MetricGroupList groups = build_metric_groups(cfg, data, fg_alpha);

        if (cfg.arrangement == ArrangementMode::Linear) {
            // Horizontal Bar Layout: category labels + values separated by dividers
            for (size_t i = 0; i < groups.count; ++i) {
                if (i > 0) {
                    ImGui::SameLine(0.0f, 8.0f * ui_ratio);
                    draw_crisp_text("|", IM_COL32(100, 110, 125, fg_alpha), cfg);
                    ImGui::SameLine(0.0f, 8.0f * ui_ratio);
                }
                const MetricGroup& g = groups.items[i];
                draw_crisp_text(g.label, g.label_color, cfg);
                ImGui::SameLine(0.0f, 5.0f * ui_ratio);
                draw_styled_group_values(g, cfg, muted_col);
            }
            if (data.benchmark_active) {
                ImGui::SameLine(0.0f, 8.0f * ui_ratio);
                char rec_buf[32];
                std::snprintf(rec_buf, sizeof(rec_buf), "[REC %.1fs]", data.benchmark_elapsed_sec);
                draw_crisp_text(rec_buf, IM_COL32(255, 60, 60, fg_alpha), cfg);
            }
        } else if (cfg.arrangement == ArrangementMode::Double) {
            // 2-Column Compact Side-by-Side Grid
            if (ImGui::BeginTable("##FPSHUD_DoubleTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Col0", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Col1", ImGuiTableColumnFlags_WidthStretch);

                for (size_t i = 0; i < groups.count; i += 2) {
                    ImGui::TableNextRow();
                    // Col 0
                    ImGui::TableSetColumnIndex(0);
                    const MetricGroup& g0 = groups.items[i];
                    draw_crisp_text(g0.label, g0.label_color, cfg);
                    ImGui::SameLine(0.0f, 5.0f * ui_ratio);
                    draw_styled_group_values(g0, cfg, muted_col);

                    // Col 1
                    if (i + 1 < groups.count) {
                        ImGui::TableSetColumnIndex(1);
                        const MetricGroup& g1 = groups.items[i + 1];
                        draw_crisp_text(g1.label, g1.label_color, cfg);
                        ImGui::SameLine(0.0f, 5.0f * ui_ratio);
                        draw_styled_group_values(g1, cfg, muted_col);
                    }
                }
                ImGui::EndTable();
            }
            if (data.benchmark_active) {
                char rec_buf[32];
                std::snprintf(rec_buf, sizeof(rec_buf), "[REC BENCHMARK: %.1fs]", data.benchmark_elapsed_sec);
                draw_crisp_text(rec_buf, IM_COL32(255, 60, 60, fg_alpha), cfg);
            }
        } else {
            // Stack (Vertical Grouped Rows): clean tabular layout with zero jitter
            size_t max_vals = 1;
            for (size_t i = 0; i < groups.count; ++i) {
                if (groups.items[i].value_count > max_vals) {
                    max_vals = groups.items[i].value_count;
                }
            }

            int total_cols = static_cast<int>(1 + max_vals);
            ImGuiTableFlags tflags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f * ui_ratio, 3.0f * ui_ratio));
            if (ImGui::BeginTable("##FPSHUD_StackGroupTable", total_cols, tflags)) {
                for (int c = 0; c < total_cols; ++c) {
                    ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed);
                }

                for (size_t i = 0; i < groups.count; ++i) {
                    const MetricGroup& g = groups.items[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    draw_crisp_text(g.label, g.label_color, cfg);

                    for (size_t v = 0; v < g.value_count; ++v) {
                        ImGui::TableSetColumnIndex(static_cast<int>(v + 1));
                        draw_styled_metric_item(g.values[v], cfg, muted_col);
                    }
                }
                ImGui::EndTable();
            }
            if (data.benchmark_active) {
                char rec_buf[32];
                std::snprintf(rec_buf, sizeof(rec_buf), "[REC BENCHMARK: %.1fs]", data.benchmark_elapsed_sec);
                draw_crisp_text(rec_buf, IM_COL32(255, 60, 60, fg_alpha), cfg);
            }
            ImGui::PopStyleVar();
        }

        // Show recent benchmark completion summary on HUD for 8 seconds
        if (!data.benchmark_active && data.last_benchmark.has_result) {
            auto now = std::chrono::steady_clock::now();
            double since_finish = std::chrono::duration<double>(now - data.last_benchmark.finished_time).count();
            if (since_finish < 8.0) {
                char bsum[128];
                std::snprintf(bsum, sizeof(bsum),
                              "BENCHMARK SAVED: AVG %.0f | 1%% %.0f | 0.1%% %.0f (%.1fs)",
                              data.last_benchmark.avg_fps,
                              data.last_benchmark.fps_1pct_low,
                              data.last_benchmark.fps_01pct_low,
                              data.last_benchmark.duration_sec);
                draw_crisp_text(bsum, IM_COL32(80, 255, 120, fg_alpha), cfg);
            }
        }

        // Optional Mini Frametime Graph directly on HUD
        if (cfg.hud_graph_enabled) {
            draw_mini_frametime_graph(cfg, fg_alpha);
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(3);
    if (chosen_font) {
        ImGui::PopFont();
    }
}

} // namespace fpshud
