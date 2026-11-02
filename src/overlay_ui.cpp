#include "overlay_ui.hpp"
#include "embedded_fonts.hpp"
#include "gui_menu.hpp"
#include "input_manager.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <chrono>
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
            return GroupCategory::SYS;
    }
    return GroupCategory::FPS;
}

const char* get_category_label(GroupCategory cat) noexcept {
    switch (cat) {
        case GroupCategory::FPS: return "FPS";
        case GroupCategory::GPU: return "GPU";
        case GroupCategory::CPU: return "CPU";
        case GroupCategory::RAM: return "RAM";
        case GroupCategory::SYS: return "SYS";
    }
    return "";
}

ImU32 get_category_color(GroupCategory cat, const HudConfig& cfg, uint8_t alpha) noexcept {
    if (!cfg.category_colors_enabled) {
        return IM_COL32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
    }
    switch (cat) {
        case GroupCategory::FPS: return IM_COL32(255, 215, 0, alpha);   // Gold/Yellow
        case GroupCategory::GPU: return IM_COL32(118, 225, 0, alpha);   // NVIDIA Green
        case GroupCategory::CPU: return IM_COL32(0, 229, 255, alpha);   // Cyan
        case GroupCategory::RAM: return IM_COL32(220, 135, 255, alpha); // Soft Purple
        case GroupCategory::SYS: return IM_COL32(190, 210, 230, alpha); // Cool Silver
    }
    return IM_COL32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
}

ImU32 get_metric_value_color(MetricType m, const TelemetryData& d, const HudConfig& cfg, uint8_t alpha) noexcept {
    ImU32 base_col = IM_COL32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
    if (!cfg.thermal_warnings_enabled) {
        return base_col;
    }

    if (m == MetricType::GpuTemp || m == MetricType::CpuTemp) {
        int temp = (m == MetricType::GpuTemp) ? d.gpu_temp_c : d.cpu_temp_c;
        if (temp >= 82) {
            return IM_COL32(255, 70, 70, alpha);  // Critical Red
        }
        if (temp >= 72) {
            return IM_COL32(255, 185, 0, alpha);  // Warning Amber/Orange
        }
    } else if (m == MetricType::GpuLoad || m == MetricType::CpuLoad) {
        float load = (m == MetricType::GpuLoad) ? d.gpu_load_pct : d.cpu_load_pct;
        if (load >= 97.0f) {
            return IM_COL32(255, 185, 0, alpha);  // High Load Amber
        }
    } else if (m == MetricType::Ram || m == MetricType::GameRam) {
        // System RAM nearly full -> stutter / swapping risk
        if (d.ram_used_pct >= 90.0f) return IM_COL32(255, 70, 70, alpha);
        if (d.ram_used_pct >= 80.0f) return IM_COL32(255, 185, 0, alpha);
    } else if (m == MetricType::SwapRam) {
        if (d.swap_total_gb > 0.0f) {
            float swap_pct = 100.0f * d.swap_used_gb / d.swap_total_gb;
            if (swap_pct >= 80.0f) return IM_COL32(255, 70, 70, alpha);
            if (swap_pct >= 50.0f) return IM_COL32(255, 185, 0, alpha);
        }
    } else if (m == MetricType::MemPressure) {
        // PSI "some" avg10: >= 5% noticeable hitching, >= 20% heavy memory stalls
        if (d.mem_psi_some_pct >= 20.0f) return IM_COL32(255, 70, 70, alpha);
        if (d.mem_psi_some_pct >= 5.0f) return IM_COL32(255, 185, 0, alpha);
    }
    return base_col;
}

void format_styled_value(MetricType m, const TelemetryData& d, StyledValue& sv) noexcept {
    sv.prefix[0] = '\0';
    sv.number[0] = '\0';
    sv.unit[0]   = '\0';

    switch (m) {
        case MetricType::FPS:
            std::snprintf(sv.number, sizeof(sv.number), "%.0f", d.fps);
            return;
        case MetricType::FpsAvg:
            std::snprintf(sv.prefix, sizeof(sv.prefix), "AVG:");
            std::snprintf(sv.number, sizeof(sv.number), "%.0f", d.fps_avg);
            return;
        case MetricType::Fps1Low:
            std::snprintf(sv.prefix, sizeof(sv.prefix), "1%%:");
            std::snprintf(sv.number, sizeof(sv.number), "%.0f", d.fps_1pct_low);
            return;
        case MetricType::Fps01Low:
            std::snprintf(sv.prefix, sizeof(sv.prefix), "0.1%%:");
            std::snprintf(sv.number, sizeof(sv.number), "%.0f", d.fps_01pct_low);
            return;
        case MetricType::FrameTimes:
            std::snprintf(sv.number, sizeof(sv.number), "%.1f", d.frametime_ms);
            std::snprintf(sv.unit, sizeof(sv.unit), "ms");
            return;
        case MetricType::GpuLoad:
            std::snprintf(sv.number, sizeof(sv.number), "%.0f", d.gpu_load_pct);
            std::snprintf(sv.unit, sizeof(sv.unit), "%%");
            return;
        case MetricType::GpuClock:
            std::snprintf(sv.number, sizeof(sv.number), "%d", d.gpu_clock_mhz);
            std::snprintf(sv.unit, sizeof(sv.unit), "MHz");
            return;
        case MetricType::GpuVram:
            std::snprintf(sv.number, sizeof(sv.number), "%.1f", d.gpu_vram_used_gb);
            std::snprintf(sv.unit, sizeof(sv.unit), "GB");
            return;
        case MetricType::GpuBandwidth:
            std::snprintf(sv.number, sizeof(sv.number), "%.1f", d.gpu_bandwidth_gbs);
            std::snprintf(sv.unit, sizeof(sv.unit), "GB/s");
            return;
        case MetricType::GpuTemp:
            std::snprintf(sv.number, sizeof(sv.number), "%d", d.gpu_temp_c);
            std::snprintf(sv.unit, sizeof(sv.unit), "\xC2\xB0" "C"); // UTF-8 °C
            return;
        case MetricType::GpuFan:
            std::snprintf(sv.prefix, sizeof(sv.prefix), "FAN:");
            std::snprintf(sv.number, sizeof(sv.number), "%d", d.gpu_fan_pct);
            std::snprintf(sv.unit, sizeof(sv.unit), "%%");
            return;
        case MetricType::GpuWatt:
            std::snprintf(sv.number, sizeof(sv.number), "%.0f", d.gpu_power_w);
            std::snprintf(sv.unit, sizeof(sv.unit), "W");
            return;
        case MetricType::CpuLoad:
            std::snprintf(sv.number, sizeof(sv.number), "%.0f", d.cpu_load_pct);
            std::snprintf(sv.unit, sizeof(sv.unit), "%%");
            return;
        case MetricType::CpuClock:
            std::snprintf(sv.number, sizeof(sv.number), "%d", d.cpu_clock_mhz);
            std::snprintf(sv.unit, sizeof(sv.unit), "MHz");
            return;
        case MetricType::CpuWatt:
            std::snprintf(sv.number, sizeof(sv.number), "%.0f", d.cpu_power_w);
            std::snprintf(sv.unit, sizeof(sv.unit), "W");
            return;
        case MetricType::CpuTemp:
            std::snprintf(sv.number, sizeof(sv.number), "%d", d.cpu_temp_c);
            std::snprintf(sv.unit, sizeof(sv.unit), "\xC2\xB0" "C"); // UTF-8 °C
            return;
        case MetricType::Ram:
            std::snprintf(sv.number, sizeof(sv.number), "%.1f", d.ram_used_gb);
            std::snprintf(sv.unit, sizeof(sv.unit), "GB %.0f%%", d.ram_used_pct);
            return;
        case MetricType::GameRam:
            std::snprintf(sv.prefix, sizeof(sv.prefix), "GAME:");
            std::snprintf(sv.number, sizeof(sv.number), "%.1f", d.game_ram_gb);
            std::snprintf(sv.unit, sizeof(sv.unit), "GB");
            return;
        case MetricType::SwapRam:
            std::snprintf(sv.prefix, sizeof(sv.prefix), "SWAP:");
            if (d.swap_total_gb <= 0.0f) {
                std::snprintf(sv.number, sizeof(sv.number), "OFF");
            } else {
                std::snprintf(sv.number, sizeof(sv.number), "%.1f", d.swap_used_gb);
                std::snprintf(sv.unit, sizeof(sv.unit), "GB");
            }
            return;
        case MetricType::MemPressure:
            std::snprintf(sv.prefix, sizeof(sv.prefix), "PSI:");
            if (d.mem_psi_some_pct < 0.0f) {
                std::snprintf(sv.number, sizeof(sv.number), "N/A");
            } else {
                std::snprintf(sv.number, sizeof(sv.number), "%.1f", d.mem_psi_some_pct);
                std::snprintf(sv.unit, sizeof(sv.unit), "%%");
            }
            return;
        case MetricType::RamSpeed:
            std::snprintf(sv.prefix, sizeof(sv.prefix), "%s:", d.ram_type);
            if (d.ram_speed_mts <= 0) {
                std::snprintf(sv.number, sizeof(sv.number), "N/A");
            } else {
                std::snprintf(sv.number, sizeof(sv.number), "%d", d.ram_speed_mts);
                std::snprintf(sv.unit, sizeof(sv.unit), "MT/s");
            }
            return;
        case MetricType::EngineApi:
            std::snprintf(sv.number, sizeof(sv.number), "%s", d.render_api ? d.render_api : "VULKAN");
            return;
        case MetricType::SystemClock:
            std::snprintf(sv.number, sizeof(sv.number), "%s", d.local_time_str);
            return;
    }
}

MetricGroupList build_metric_groups(const HudConfig& cfg, const TelemetryData& data, uint8_t alpha) noexcept {
    MetricGroupList list{};
    for (MetricType m : cfg.metrics) {
        GroupCategory cat = get_metric_category(m);
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
            target->label = get_category_label(cat);
            target->label_color = get_category_color(cat, cfg, alpha);
            target->value_count = 0;
        }
        if (target && target->value_count < target->values.size()) {
            StyledValue& sv = target->values[target->value_count++];
            format_styled_value(m, data, sv);
            sv.color = get_metric_value_color(m, data, cfg, alpha);
        }
    }
    return list;
}

// Draws text with adaptive 4/8-direction outline stroke and drop shadow
void draw_crisp_text(const char* text, ImU32 color, const HudConfig& cfg) {
    if (!text || !*text) return;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize();

    // 1. Drop shadow (drawn first, furthest behind)
    if (cfg.shadow_enabled && cfg.shadow_offset > 0.0f) {
        uint8_t sh_alpha = static_cast<uint8_t>(std::clamp(cfg.shadow_opacity * cfg.text_opacity * 255.0f, 0.0f, 255.0f));
        ImU32 drop_col = IM_COL32(0, 0, 0, sh_alpha);
        float d = cfg.shadow_offset;
        draw_list->AddText(font, font_size, ImVec2(pos.x + d, pos.y + d), drop_col, text);
    }

    // 2. Adaptive stroke outline
    if (cfg.outline_enabled && cfg.outline_thickness > 0.0f) {
        uint8_t out_alpha = static_cast<uint8_t>(std::clamp(0.95f * cfg.text_opacity * 255.0f, 0.0f, 255.0f));
        ImU32 outline_col = IM_COL32(0, 0, 0, out_alpha);
        const float o = cfg.outline_thickness;

        draw_list->AddText(font, font_size, ImVec2(pos.x - o, pos.y - o), outline_col, text);
        draw_list->AddText(font, font_size, ImVec2(pos.x + o, pos.y - o), outline_col, text);
        draw_list->AddText(font, font_size, ImVec2(pos.x - o, pos.y + o), outline_col, text);
        draw_list->AddText(font, font_size, ImVec2(pos.x + o, pos.y + o), outline_col, text);

        if (o > 1.5f) {
            draw_list->AddText(font, font_size, ImVec2(pos.x,     pos.y - o), outline_col, text);
            draw_list->AddText(font, font_size, ImVec2(pos.x - o, pos.y),     outline_col, text);
            draw_list->AddText(font, font_size, ImVec2(pos.x + o, pos.y),     outline_col, text);
            draw_list->AddText(font, font_size, ImVec2(pos.x,     pos.y + o), outline_col, text);
        }
    }

    // 3. Main foreground text
    draw_list->AddText(font, font_size, pos, color, text);
    ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::Dummy(sz);
}

// Draws a single metric item with dimmed prefix/unit and bright number so values never blend together
void draw_styled_metric_item(const StyledValue& sv, const HudConfig& cfg, ImU32 muted_col) {
    float tight_gap = 2.0f * (cfg.font_size_px / 20.0f);
    bool has_prev = false;

    if (sv.prefix[0] != '\0') {
        draw_crisp_text(sv.prefix, muted_col, cfg);
        has_prev = true;
    }
    if (sv.number[0] != '\0') {
        if (has_prev) ImGui::SameLine(0.0f, tight_gap);
        draw_crisp_text(sv.number, sv.color, cfg);
        has_prev = true;
    }
    if (sv.unit[0] != '\0') {
        if (has_prev) ImGui::SameLine(0.0f, 1.5f);
        draw_crisp_text(sv.unit, muted_col, cfg);
    }
}

void draw_styled_group_values(const MetricGroup& g, const HudConfig& cfg, ImU32 muted_col, ImU32 dot_col, float ui_ratio) {
    float dot_gap = 6.0f * ui_ratio;
    for (size_t i = 0; i < g.value_count; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0f, dot_gap);
            draw_crisp_text("\xC2\xB7", dot_col, cfg); // UTF-8 middle dot ·
            ImGui::SameLine(0.0f, dot_gap);
        }
        draw_styled_metric_item(g.values[i], cfg, muted_col);
    }
}

void draw_mini_frametime_graph(const HudConfig& cfg, uint8_t alpha) {
    const auto& telemetry = get_telemetry();
    float ui_ratio = cfg.font_size_px / 20.0f;
    float graph_w = std::clamp(220.0f * ui_ratio, 140.0f, 380.0f);
    float graph_h = std::clamp(28.0f * ui_ratio, 18.0f, 52.0f);

    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.04f, 0.05f, 0.07f, std::max(0.45f, cfg.bg_alpha)));
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.0f, 0.90f, 1.0f, alpha / 255.0f));
    ImGui::PlotLines("##HUD_MiniFTGraph",
                     telemetry.get_ft_history(),
                     TelemetryCollector::kGraphHistorySize,
                     telemetry.get_graph_offset(),
                     "frametime (ms)", 0.0f, 50.0f, ImVec2(graph_w, graph_h));
    ImGui::PopStyleColor(2);
}

ImFont* load_ttf_with_fallback(ImGuiIO& io,
                               const std::vector<std::string>& candidate_paths,
                               const unsigned char* fallback_mem,
                               size_t fallback_size) {
    // Compact ASCII + Latin-1 Supplement (includes ° degree and · middle dot)
    static const ImWchar kCompactAsciiRanges[] = {0x0020, 0x00FF, 0};

    ImFontConfig font_cfg;
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 1;
    font_cfg.PixelSnapH = true;
    font_cfg.GlyphRanges = kCompactAsciiRanges;

    std::error_code ec;
    for (const auto& path : candidate_paths) {
        if (!path.empty() && std::filesystem::exists(path, ec)) {
            ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), kAtlasBaseFontSizePx, &font_cfg);
            if (f) return f;
        }
    }

    if (fallback_mem && fallback_size > 16) {
        ImFontConfig mem_cfg = font_cfg;
        mem_cfg.FontDataOwnedByAtlas = false;
        ImFont* f = io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char*>(fallback_mem),
            static_cast<int>(fallback_size),
            kAtlasBaseFontSizePx,
            &mem_cfg);
        if (f) return f;
    }
    return io.Fonts->AddFontDefault();
}

} // namespace

void setup_overlay_font(ImGuiIO& io) {
    std::string home_fonts;
    if (const char* home = std::getenv("HOME"); home && *home) {
        home_fonts = std::string(home) + "/.local/share/fpshud/fonts/";
    }

    // Slot 0: FontFamily::SansBold (OpenSans-Bold embedded in memory)
    load_ttf_with_fallback(
        io,
        {home_fonts + "OpenSans-Bold.ttf",
         "/run/host/usr/share/fonts/TTF/OpenSans-Bold.ttf",
         "/usr/share/fonts/TTF/OpenSans-Bold.ttf"},
        kEmbeddedOpenSansBoldTTF,
        sizeof(kEmbeddedOpenSansBoldTTF));

    // Slot 1: FontFamily::MonoBold (DejaVuSansMono-Bold, fallback to embedded VeraMonoBold)
    load_ttf_with_fallback(
        io,
        {home_fonts + "DejaVuSansMono-Bold.ttf",
         "/run/host/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
         "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf"},
        kEmbeddedVeraMonoBoldTTF,
        sizeof(kEmbeddedVeraMonoBoldTTF));

    // Slot 2: FontFamily::VeraMono (Embedded VeraMono-Bold in memory)
    load_ttf_with_fallback(
        io,
        {},
        kEmbeddedVeraMonoBoldTTF,
        sizeof(kEmbeddedVeraMonoBoldTTF));

    // Slot 3: FontFamily::AdwaitaMono (AdwaitaMono-Bold, fallback to embedded VeraMonoBold)
    load_ttf_with_fallback(
        io,
        {home_fonts + "AdwaitaMono-Bold.ttf",
         "/run/host/usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf",
         "/usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf"},
        kEmbeddedVeraMonoBoldTTF,
        sizeof(kEmbeddedVeraMonoBoldTTF));
}

void render_overlay_ui(HudConfig& cfg,
                       const TelemetryData& data,
                       float display_width,
                       float display_height) {
    if (!cfg.enabled || display_width <= 0.0f || display_height <= 0.0f) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    int font_idx = std::clamp(static_cast<int>(cfg.font_family), 0, io.Fonts->Fonts.Size - 1);
    ImFont* chosen_font = (io.Fonts->Fonts.Size > 0) ? io.Fonts->Fonts[font_idx] : nullptr;
    if (chosen_font) {
        ImGui::PushFont(chosen_font);
    }

    // Render Interactive GUI Menu & synchronize mouse grab state
    render_gui_menu(cfg, data, display_width, display_height);
    get_input_manager().sync_grab_state(io, cfg);

    if (!cfg.hud_visible || cfg.metrics.empty()) {
        if (chosen_font) ImGui::PopFont();
        return;
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

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        cfg.background == BackgroundStyle::None ? ImVec2(4.0f, 4.0f) : ImVec2(12.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.06f, cfg.bg_alpha));

    if (ImGui::Begin("##FPSHUD_Overlay", nullptr, flags)) {
        cfg.font_scale = std::clamp(cfg.font_size_px / kAtlasBaseFontSizePx, 0.25f, 2.5f);
        ImGui::SetWindowFontScale(cfg.font_scale);

        uint8_t fg_alpha = static_cast<uint8_t>(std::clamp(cfg.text_opacity * 255.0f, 10.0f, 255.0f));
        uint8_t muted_alpha = static_cast<uint8_t>(std::clamp(cfg.text_opacity * 215.0f, 10.0f, 255.0f));
        uint8_t sep_alpha = static_cast<uint8_t>(std::clamp(cfg.text_opacity * 170.0f, 10.0f, 255.0f));

        // Soft cool-gray for units & sub-labels (AVG:, 1%:, MHz, GB, W, °C) so numbers pop out cleanly
        ImU32 muted_col = IM_COL32(175, 190, 208, muted_alpha);
        ImU32 dot_col   = IM_COL32(130, 145, 165, sep_alpha);
        ImU32 sep_color = IM_COL32(160, 175, 195, sep_alpha);

        MetricGroupList groups = build_metric_groups(cfg, data, fg_alpha);
        float ui_ratio = cfg.font_size_px / 20.0f;

        if (cfg.arrangement == ArrangementMode::Linear) {
            for (size_t i = 0; i < groups.count; ++i) {
                const MetricGroup& g = groups.items[i];
                if (i > 0) {
                    ImGui::SameLine(0.0f, 10.0f * ui_ratio);
                    draw_crisp_text("|", sep_color, cfg);
                    ImGui::SameLine(0.0f, 10.0f * ui_ratio);
                }
                draw_crisp_text(g.label, g.label_color, cfg);
                ImGui::SameLine(0.0f, 7.0f * ui_ratio);
                draw_styled_group_values(g, cfg, muted_col, dot_col, ui_ratio);
            }
            if (data.benchmark_active) {
                ImGui::SameLine(0.0f, 10.0f * ui_ratio);
                char rec_buf[32];
                std::snprintf(rec_buf, sizeof(rec_buf), "[REC %.1fs]", data.benchmark_elapsed_sec);
                draw_crisp_text(rec_buf, IM_COL32(255, 60, 60, fg_alpha), cfg);
            }
        } else if (cfg.arrangement == ArrangementMode::Double) {
            ImGuiTableFlags tflags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(7.0f * ui_ratio, 3.0f * ui_ratio));
            if (ImGui::BeginTable("##FPSHUD_DoubleGroupTable", 4, tflags)) {
                ImGui::TableSetupColumn("G1", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("V1", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("G2", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("V2", ImGuiTableColumnFlags_WidthFixed);

                for (size_t i = 0; i < groups.count; i += 2) {
                    const MetricGroup& g1 = groups.items[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    draw_crisp_text(g1.label, g1.label_color, cfg);
                    ImGui::TableSetColumnIndex(1);
                    draw_styled_group_values(g1, cfg, muted_col, dot_col, ui_ratio);

                    if (i + 1 < groups.count) {
                        const MetricGroup& g2 = groups.items[i + 1];
                        ImGui::TableSetColumnIndex(2);
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f * ui_ratio);
                        draw_crisp_text(g2.label, g2.label_color, cfg);
                        ImGui::TableSetColumnIndex(3);
                        draw_styled_group_values(g2, cfg, muted_col, dot_col, ui_ratio);
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
        } else {
            size_t max_vals = 1;
            for (size_t i = 0; i < groups.count; ++i) {
                if (groups.items[i].value_count > max_vals) max_vals = groups.items[i].value_count;
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
