#include "overlay_ui.hpp"
#include "embedded_fonts.hpp"
#include "gui_menu.hpp"
#include "input_manager.hpp"
#include "recorder_ipc.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <inplace_vector>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fpshud {

namespace {

enum class GroupCategory { FPS = 0, GPU = 1, CPU = 2, RAM = 3, SYS = 4 };

// Fixed-capacity stack structs with separated prefix/number/unit for high readability (zero heap
// allocation!)
struct StyledValue {
  char prefix[12] = {0};  // e.g., "AVG ", "1% ", "0.1% ", "FAN "
  char number[24] = {0};  // e.g., "60", "39", "1320", "1.7", "56"
  char unit[12] = {0};    // e.g., "%", "MHz", "GB", "°C", "W", "ms"
  ImU32 color = 0;
};

constexpr size_t kMaxMetricsPerGroup = 16;
constexpr size_t kMaxMetricGroups = 8;

struct MetricGroup {
  GroupCategory category = GroupCategory::FPS;
  const char* label = "FPS";
  ImU32 label_color = 0;
  std::inplace_vector<StyledValue, kMaxMetricsPerGroup> values;
};

struct MetricGroupList {
  std::inplace_vector<MetricGroup, kMaxMetricGroups> items;
};

static_assert(sizeof(MetricGroupList) <= 16384,
              "MetricGroupList must fit within 16 KB stack safety budget");

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
    case MetricType::CpuHybrid:
      return GroupCategory::CPU;
    case MetricType::Ram:
    case MetricType::GameRam:
    case MetricType::SwapRam:
    case MetricType::MemPressure:
      return GroupCategory::RAM;
    case MetricType::EngineApi:
    case MetricType::SystemClock:
    case MetricType::Battery:
      return GroupCategory::SYS;
  }
  return GroupCategory::FPS;
}

const char* get_category_label(GroupCategory cat) noexcept {
  switch (cat) {
    case GroupCategory::FPS:
      return "FPS";
    case GroupCategory::GPU:
      return "GPU";
    case GroupCategory::CPU:
      return "CPU";
    case GroupCategory::RAM:
      return "RAM";
    case GroupCategory::SYS:
      return "SYS";
  }
  return "";
}

ImU32 get_category_color(GroupCategory cat, const HudConfig& cfg, uint8_t alpha) noexcept {
  if (!cfg.category_colors_enabled) {
    return IM_COL32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
  }
  switch (cat) {
    case GroupCategory::FPS:
      return IM_COL32(255, 215, 0, alpha);  // Gold/Yellow
    case GroupCategory::GPU:
      return IM_COL32(118, 225, 0, alpha);  // NVIDIA Green
    case GroupCategory::CPU:
      return IM_COL32(0, 229, 255, alpha);  // Cyan
    case GroupCategory::RAM:
      return IM_COL32(220, 135, 255, alpha);  // Soft Purple
    case GroupCategory::SYS:
      return IM_COL32(190, 210, 230, alpha);  // Cool Silver
  }
  return IM_COL32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
}

ImU32 get_metric_value_color(MetricType m, const TelemetryData& d, const HudConfig& cfg,
                             uint8_t alpha) noexcept {
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
  sv.unit[0] = '\0';

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
      std::snprintf(sv.unit, sizeof(sv.unit),
                    "\xC2\xB0"
                    "C");  // UTF-8 °C
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
      std::snprintf(sv.unit, sizeof(sv.unit),
                    "\xC2\xB0"
                    "C");  // UTF-8 °C
      return;
    case MetricType::CpuHybrid:
      if (!d.hybrid_cpu) return;
      std::snprintf(sv.prefix, sizeof(sv.prefix), "P/E:");
      std::snprintf(sv.number, sizeof(sv.number), "%.0f/%.0f", d.cpu_p_core_load_pct,
                    d.cpu_e_core_load_pct);
      std::snprintf(sv.unit, sizeof(sv.unit), "%%");
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
    case MetricType::EngineApi:
      std::snprintf(sv.number, sizeof(sv.number), "%s", d.render_api ? d.render_api : "VULKAN");
      return;
    case MetricType::SystemClock:
      std::snprintf(sv.number, sizeof(sv.number), "%s", d.local_time_str);
      return;
    case MetricType::Battery:
      if (!d.battery_available) return;
      std::snprintf(sv.prefix, sizeof(sv.prefix), "BAT:");
      std::snprintf(sv.number, sizeof(sv.number), "%.0f", d.battery_pct);
      if (d.battery_power_w > 0.05f) {
        std::snprintf(sv.unit, sizeof(sv.unit), "%% (%.1fW%s)", d.battery_power_w,
                      d.battery_charging ? "+" : "");
      } else {
        std::snprintf(sv.unit, sizeof(sv.unit), "%%%s", d.battery_charging ? "(+)" : "");
      }
      return;
  }
}

MetricGroupList build_metric_groups(const HudConfig& cfg, const TelemetryData& data,
                                    uint8_t alpha) noexcept {
  MetricGroupList list{};
  for (MetricType m : cfg.metrics) {
    if (m == MetricType::Battery && !data.battery_available) continue;
    if (m == MetricType::CpuHybrid && !data.hybrid_cpu) continue;
    GroupCategory cat = get_metric_category(m);
    MetricGroup* target = nullptr;
    for (auto& item : list.items) {
      if (item.category == cat) {
        target = &item;
        break;
      }
    }
    if (!target) {
      const char* label = get_category_label(cat);
      ImU32 col = get_category_color(cat, cfg, alpha);
      if (auto added = list.items.try_emplace_back(MetricGroup{cat, label, col, {}})) {
        target = &*added;
      }
    }
    if (target) {
      StyledValue sv{};
      format_styled_value(m, data, sv);
      sv.color = get_metric_value_color(m, data, cfg, alpha);
      target->values.try_push_back(sv);
    }
  }
  return list;
}

// Draws text with adaptive 4/8-direction outline stroke and drop shadow
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
    uint8_t sh_alpha = static_cast<uint8_t>(
        std::clamp(cfg.shadow_opacity * cfg.text_opacity * 255.0f, 0.0f, 255.0f));
    ImU32 drop_col = IM_COL32(0, 0, 0, sh_alpha);
    float d = std::max(1.0f, std::round(cfg.shadow_offset * scale_factor));
    draw_list->AddText(font, font_size, ImVec2(pos.x + d, pos.y + d), drop_col, text);
  }

  // 2. Adaptive stroke outline
  if (cfg.outline_enabled && cfg.outline_thickness > 0.0f) {
    uint8_t out_alpha =
        static_cast<uint8_t>(std::clamp(0.95f * cfg.text_opacity * 255.0f, 0.0f, 255.0f));
    ImU32 outline_col = IM_COL32(0, 0, 0, out_alpha);
    float o = std::max(1.0f, std::round(cfg.outline_thickness * scale_factor));

    draw_list->AddText(font, font_size, ImVec2(pos.x - o, pos.y - o), outline_col, text);
    draw_list->AddText(font, font_size, ImVec2(pos.x + o, pos.y - o), outline_col, text);
    draw_list->AddText(font, font_size, ImVec2(pos.x - o, pos.y + o), outline_col, text);
    draw_list->AddText(font, font_size, ImVec2(pos.x + o, pos.y + o), outline_col, text);

    if (o > 1.5f && cfg.font_size_px >= 18.0f) {
      draw_list->AddText(font, font_size, ImVec2(pos.x, pos.y - o), outline_col, text);
      draw_list->AddText(font, font_size, ImVec2(pos.x - o, pos.y), outline_col, text);
      draw_list->AddText(font, font_size, ImVec2(pos.x + o, pos.y), outline_col, text);
      draw_list->AddText(font, font_size, ImVec2(pos.x, pos.y + o), outline_col, text);
    }
  }

  // 3. Main foreground text
  draw_list->AddText(font, font_size, pos, color, text);
  ImVec2 sz = ImGui::CalcTextSize(text);
  ImGui::Dummy(sz);
}

// Draws a single metric item with dimmed prefix/unit and bright number so values never blend
// together
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

void draw_styled_group_values(const MetricGroup& g, const HudConfig& cfg, ImU32 muted_col,
                              ImU32 dot_col, float ui_ratio) {
  float dot_gap = 6.0f * ui_ratio;
  for (size_t i = 0; i < g.values.size(); ++i) {
    if (i > 0) {
      ImGui::SameLine(0.0f, dot_gap);
      draw_crisp_text("\xC2\xB7", dot_col, cfg);  // UTF-8 middle dot ·
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

  ImGui::PushStyleColor(ImGuiCol_FrameBg,
                        ImVec4(0.04f, 0.05f, 0.07f, std::max(0.45f, cfg.bg_alpha)));
  ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.0f, 0.90f, 1.0f, alpha / 255.0f));
  ImGui::PlotLines("##HUD_MiniFTGraph", telemetry.get_ft_history(),
                   TelemetryCollector::kGraphHistorySize, telemetry.get_graph_offset(),
                   "frametime (ms)", 0.0f, 50.0f, ImVec2(graph_w, graph_h));
  ImGui::PopStyleColor(2);
}

ImFont* g_font_gui_small = nullptr;   // 12px
ImFont* g_font_gui_normal = nullptr;  // 16px
ImFont* g_font_gui_large = nullptr;   // 24px

ImFont* g_hud_fonts_small[4] = {};   // 14px for each family
ImFont* g_hud_fonts_normal[4] = {};  // 20px for each family
ImFont* g_hud_fonts_large[4] = {};   // 32px for each family

ImFont* load_ttf_with_fallback(ImGuiIO& io, const std::vector<std::string>& candidate_paths,
                               const unsigned char* fallback_mem, size_t fallback_size,
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
    ImFont* f = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(fallback_mem),
                                               static_cast<int>(fallback_size), size_px, &mem_cfg);
    if (f) return f;
  }
  return io.Fonts->AddFontDefault();
}

}  // namespace

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

  std::vector<std::string> open_sans_paths = {home_fonts + "OpenSans-Bold.ttf",
                                              "/run/host/usr/share/fonts/TTF/OpenSans-Bold.ttf",
                                              "/usr/share/fonts/TTF/OpenSans-Bold.ttf"};

  std::vector<std::string> dejavu_paths = {home_fonts + "DejaVuSansMono-Bold.ttf",
                                           "/run/host/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
                                           "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf"};

  std::vector<std::string> adwaita_paths = {
      home_fonts + "AdwaitaMono-Bold.ttf", "/run/host/usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf",
      "/usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf"};

  // 1. Dedicated GUI Menu Fonts (Crisp OpenSans at 12px, 16px, 24px)
  g_font_gui_small = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                                            sizeof(kEmbeddedOpenSansBoldTTF), 12.0f);
  g_font_gui_normal = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                                             sizeof(kEmbeddedOpenSansBoldTTF), 16.0f);
  g_font_gui_large = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                                            sizeof(kEmbeddedOpenSansBoldTTF), 24.0f);

  // 2. Multi-tier HUD Fonts for each of the 4 FontFamily choices
  // Slot 0: SansBold
  g_hud_fonts_small[0] = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                                                sizeof(kEmbeddedOpenSansBoldTTF), 14.0f);
  g_hud_fonts_normal[0] = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                                                 sizeof(kEmbeddedOpenSansBoldTTF), 20.0f);
  g_hud_fonts_large[0] = load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                                                sizeof(kEmbeddedOpenSansBoldTTF), 32.0f);

  // Slot 1: MonoBold
  g_hud_fonts_small[1] = load_ttf_with_fallback(io, dejavu_paths, kEmbeddedVeraMonoBoldTTF,
                                                sizeof(kEmbeddedVeraMonoBoldTTF), 14.0f);
  g_hud_fonts_normal[1] = load_ttf_with_fallback(io, dejavu_paths, kEmbeddedVeraMonoBoldTTF,
                                                 sizeof(kEmbeddedVeraMonoBoldTTF), 20.0f);
  g_hud_fonts_large[1] = load_ttf_with_fallback(io, dejavu_paths, kEmbeddedVeraMonoBoldTTF,
                                                sizeof(kEmbeddedVeraMonoBoldTTF), 32.0f);

  // Slot 2: VeraMono
  g_hud_fonts_small[2] = load_ttf_with_fallback(io, {}, kEmbeddedVeraMonoBoldTTF,
                                                sizeof(kEmbeddedVeraMonoBoldTTF), 14.0f);
  g_hud_fonts_normal[2] = load_ttf_with_fallback(io, {}, kEmbeddedVeraMonoBoldTTF,
                                                 sizeof(kEmbeddedVeraMonoBoldTTF), 20.0f);
  g_hud_fonts_large[2] = load_ttf_with_fallback(io, {}, kEmbeddedVeraMonoBoldTTF,
                                                sizeof(kEmbeddedVeraMonoBoldTTF), 32.0f);

  // Slot 3: AdwaitaMono
  g_hud_fonts_small[3] = load_ttf_with_fallback(io, adwaita_paths, kEmbeddedVeraMonoBoldTTF,
                                                sizeof(kEmbeddedVeraMonoBoldTTF), 14.0f);
  g_hud_fonts_normal[3] = load_ttf_with_fallback(io, adwaita_paths, kEmbeddedVeraMonoBoldTTF,
                                                 sizeof(kEmbeddedVeraMonoBoldTTF), 20.0f);
  g_hud_fonts_large[3] = load_ttf_with_fallback(io, adwaita_paths, kEmbeddedVeraMonoBoldTTF,
                                                sizeof(kEmbeddedVeraMonoBoldTTF), 32.0f);
}

void render_overlay_ui(HudConfig& cfg, const TelemetryData& data, float display_width,
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

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
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
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, cfg.background == BackgroundStyle::None
                                                       ? ImVec2(4.0f, 4.0f)
                                                       : ImVec2(12.0f, 8.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.06f, cfg.bg_alpha));

  if (ImGui::Begin("##FPSHUD_Overlay", nullptr, flags)) {
    cfg.font_scale = std::clamp(cfg.font_size_px / base_hud_font_px, 0.40f, 2.50f);
    ImGui::SetWindowFontScale(cfg.font_scale);

    uint8_t fg_alpha = static_cast<uint8_t>(std::clamp(cfg.text_opacity * 255.0f, 10.0f, 255.0f));
    uint8_t muted_alpha =
        static_cast<uint8_t>(std::clamp(cfg.text_opacity * 215.0f, 10.0f, 255.0f));
    uint8_t sep_alpha = static_cast<uint8_t>(std::clamp(cfg.text_opacity * 170.0f, 10.0f, 255.0f));

    // Soft cool-gray for units & sub-labels (AVG:, 1%:, MHz, GB, W, °C) so numbers pop out cleanly
    ImU32 muted_col = IM_COL32(175, 190, 208, muted_alpha);
    ImU32 dot_col = IM_COL32(130, 145, 165, sep_alpha);
    ImU32 sep_color = IM_COL32(160, 175, 195, sep_alpha);

    MetricGroupList groups = build_metric_groups(cfg, data, fg_alpha);
    float ui_ratio = cfg.font_size_px / 20.0f;

    if (cfg.arrangement == ArrangementMode::Linear) {
      for (size_t i = 0; i < groups.items.size(); ++i) {
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
      if (RecorderController::instance().is_recording()) {
        ImGui::SameLine(0.0f, 10.0f * ui_ratio);
        float rec_sec = RecorderController::instance().get_recording_duration();
        int mins = static_cast<int>(rec_sec) / 60;
        int secs = static_cast<int>(rec_sec) % 60;
        char rec_tag[64];
        std::snprintf(rec_tag, sizeof(rec_tag), "[REC %02d:%02d]", mins, secs);
        draw_crisp_text(rec_tag, IM_COL32(255, 50, 50, fg_alpha), cfg);
      }
    } else if (cfg.arrangement == ArrangementMode::Double) {
      ImGuiTableFlags tflags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
      ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(7.0f * ui_ratio, 3.0f * ui_ratio));
      if (ImGui::BeginTable("##FPSHUD_DoubleGroupTable", 4, tflags)) {
        ImGui::TableSetupColumn("G1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("V1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("G2", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("V2", ImGuiTableColumnFlags_WidthFixed);

        for (size_t i = 0; i < groups.items.size(); i += 2) {
          const MetricGroup& g1 = groups.items[i];
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          draw_crisp_text(g1.label, g1.label_color, cfg);
          ImGui::TableSetColumnIndex(1);
          draw_styled_group_values(g1, cfg, muted_col, dot_col, ui_ratio);

          if (i + 1 < groups.items.size()) {
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
        std::snprintf(rec_buf, sizeof(rec_buf), "[REC BENCHMARK: %.1fs]",
                      data.benchmark_elapsed_sec);
        draw_crisp_text(rec_buf, IM_COL32(255, 60, 60, fg_alpha), cfg);
      }
      ImGui::PopStyleVar();
    } else {
      size_t max_vals = 1;
      for (const auto& g : groups.items) {
        if (g.values.size() > max_vals) max_vals = g.values.size();
      }

      int total_cols = static_cast<int>(1 + max_vals);
      ImGuiTableFlags tflags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
      ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f * ui_ratio, 3.0f * ui_ratio));
      if (ImGui::BeginTable("##FPSHUD_StackGroupTable", total_cols, tflags)) {
        for (int c = 0; c < total_cols; ++c) {
          ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed);
        }

        for (const auto& g : groups.items) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          draw_crisp_text(g.label, g.label_color, cfg);

          for (size_t v = 0; v < g.values.size(); ++v) {
            ImGui::TableSetColumnIndex(static_cast<int>(v + 1));
            draw_styled_metric_item(g.values[v], cfg, muted_col);
          }
        }
        ImGui::EndTable();
      }
      if (data.benchmark_active) {
        char rec_buf[32];
        std::snprintf(rec_buf, sizeof(rec_buf), "[REC BENCHMARK: %.1fs]",
                      data.benchmark_elapsed_sec);
        draw_crisp_text(rec_buf, IM_COL32(255, 60, 60, fg_alpha), cfg);
      }
      ImGui::PopStyleVar();
    }

    // Show recent benchmark completion summary on HUD for 8 seconds
    if (!data.benchmark_active && data.last_benchmark.has_result) {
      auto now = std::chrono::steady_clock::now();
      double since_finish =
          std::chrono::duration<double>(now - data.last_benchmark.finished_time).count();
      if (since_finish < 8.0) {
        char bsum[128];
        std::snprintf(bsum, sizeof(bsum),
                      "BENCHMARK SAVED: AVG %.0f | 1%% %.0f | 0.1%% %.0f (%.1fs)",
                      data.last_benchmark.avg_fps, data.last_benchmark.fps_1pct_low,
                      data.last_benchmark.fps_01pct_low, data.last_benchmark.duration_sec);
        draw_crisp_text(bsum, IM_COL32(80, 255, 120, fg_alpha), cfg);
      }
    }

    // Show video recording badge on HUD (stacked/double layouts only)
    if (cfg.arrangement != ArrangementMode::Linear &&
        RecorderController::instance().is_recording()) {
      float rec_sec = RecorderController::instance().get_recording_duration();
      int mins = static_cast<int>(rec_sec) / 60;
      int secs = static_cast<int>(rec_sec) % 60;
      char rec_tag[64];
      std::snprintf(rec_tag, sizeof(rec_tag), "[REC %02d:%02d]", mins, secs);
      draw_crisp_text(rec_tag, IM_COL32(255, 50, 50, fg_alpha), cfg);
    }

    // Show recent toast notification (e.g. video saved, benchmark saved) on HUD
    std::string toast_msg;
    if (RecorderController::instance().get_recent_toast(toast_msg)) {
      draw_crisp_text(toast_msg.c_str(), IM_COL32(60, 245, 120, fg_alpha), cfg);
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

}  // namespace fpshud
