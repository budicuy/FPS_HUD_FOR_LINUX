#include "overlay_ui.hpp"
#include "embedded_fonts.hpp"
#include "gui_menu.hpp"
#include "input_manager.hpp"
#include "logger.hpp"
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

constexpr ImU32 make_col32(uint32_t r, uint32_t g, uint32_t b, uint32_t a) noexcept {
  return (a << 24u) | (b << 16u) | (g << 8u) | r;
}

enum class GroupCategory { FPS = 0, GPU = 1, CPU = 2, RAM = 3, SYS = 4 };

// Fixed-capacity stack structs with separated prefix/number/unit for high readability (zero heap
// allocation!)
struct StyledValue {
  char prefix[16] = {0};  // e.g., "AVG ", "1% ", "0.1% ", "FAN "
  char number[48] = {0};  // e.g., "60", "39", "1320", "Radeon RX 7800 XT"
  char unit[12] = {0};    // e.g., "%", "MHz", "GB", "°C", "W", "ms"
  ImU32 color = 0;
};
static_assert(sizeof(StyledValue) == 80, "StyledValue size must be exactly 80 bytes");

constexpr size_t kMaxMetricsPerGroup = 16;
constexpr size_t kMaxMetricGroups = 40;

struct MetricGroup {
  GroupCategory category = GroupCategory::FPS;
  const char* label = "FPS";
  ImU32 label_color = 0;
  std::inplace_vector<StyledValue, kMaxMetricsPerGroup> values;
};

struct MetricGroupList {
  std::inplace_vector<MetricGroup, kMaxMetricGroups> items;
};

static_assert(sizeof(MetricGroupList) <= 65536,
              "MetricGroupList must fit within 64 KB stack safety budget");

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
    case MetricType::GpuVramClock:
    case MetricType::GpuVram:
    case MetricType::GpuBandwidth:
    case MetricType::GpuTemp:
    case MetricType::GpuFan:
    case MetricType::GpuWatt:
      return GroupCategory::GPU;
    case MetricType::CpuLoad:
    case MetricType::CpuAllCoreLoad:
    case MetricType::CpuClock:
    case MetricType::CpuAllCoreClock:
    case MetricType::CpuWatt:
    case MetricType::CpuTemp:
      return GroupCategory::CPU;
    case MetricType::Ram:
    case MetricType::GameRam:
    case MetricType::SwapRam:
    case MetricType::MemPressure:
      return GroupCategory::RAM;
    case MetricType::EngineApi:
    case MetricType::GpuModel:
    case MetricType::CpuModel:
    case MetricType::SystemClock:
    case MetricType::Battery:
    case MetricType::DiskIo:
    case MetricType::DiskTemp:
    case MetricType::NetSpeed:
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
    return make_col32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
  }
  switch (cat) {
    case GroupCategory::FPS:
      return make_col32(255u, 215u, 0u, alpha);  // Gold/Yellow
    case GroupCategory::GPU:
      return make_col32(118u, 225u, 0u, alpha);  // NVIDIA Green
    case GroupCategory::CPU:
      return make_col32(0u, 229u, 255u, alpha);  // Cyan
    case GroupCategory::RAM:
      return make_col32(220u, 135u, 255u, alpha);  // Soft Purple
    case GroupCategory::SYS:
      return make_col32(190u, 210u, 230u, alpha);  // Cool Silver
  }
  return make_col32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
}

ImU32 get_metric_value_color(MetricType m, const TelemetryData& d, const HudConfig& cfg,
                             uint8_t alpha) noexcept {
  ImU32 base_col = make_col32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
  if (!cfg.thermal_warnings_enabled) {
    return base_col;
  }

  const ImU32 col_red = make_col32(255u, 70u, 70u, alpha);
  const ImU32 col_yellow = make_col32(255u, 185u, 0u, alpha);

  switch (m) {
    case MetricType::FPS:
    case MetricType::FpsAvg: {
      float val = (m == MetricType::FPS) ? d.fps : d.fps_avg;
      if (val > 0.1f && val < 30.0f) return col_red;
      if (val > 0.1f && val < 45.0f) return col_yellow;
      break;
    }
    case MetricType::Fps1Low:
    case MetricType::Fps01Low: {
      float val = (m == MetricType::Fps1Low) ? d.fps_1pct_low : d.fps_01pct_low;
      if (val > 0.1f && (val < 25.0f || (d.fps_avg > 10.0f && val < 0.45f * d.fps_avg))) {
        return col_red;
      }
      if (val > 0.1f && (val < 45.0f || (d.fps_avg > 10.0f && val < 0.60f * d.fps_avg))) {
        return col_yellow;
      }
      break;
    }
    case MetricType::FrameTimes:
      if (d.frametime_ms > 33.3f) return col_red;
      if (d.frametime_ms > 22.2f) return col_yellow;
      break;
    case MetricType::GpuLoad:
    case MetricType::CpuLoad: {
      float load = (m == MetricType::GpuLoad) ? d.gpu_load_pct : d.cpu_load_pct;
      if (load >= 95.0f) return col_red;
      if (load >= 80.0f) return col_yellow;
      break;
    }
    case MetricType::GpuTemp:
    case MetricType::CpuTemp: {
      int temp = (m == MetricType::GpuTemp) ? d.gpu_temp_c : d.cpu_temp_c;
      if (temp >= 82) return col_red;
      if (temp >= 72) return col_yellow;
      break;
    }
    case MetricType::GpuVram:
      if (d.gpu_vram_total_gb > 0.1f) {
        float pct = 100.0f * d.gpu_vram_used_gb / d.gpu_vram_total_gb;
        if (pct >= 92.0f) return col_red;
        if (pct >= 80.0f) return col_yellow;
      }
      break;
    case MetricType::GpuFan:
      if (d.gpu_fan_pct >= 90) return col_red;
      if (d.gpu_fan_pct >= 75) return col_yellow;
      break;
    case MetricType::Ram:
    case MetricType::GameRam:
      if (d.ram_used_pct >= 90.0f) return col_red;
      if (d.ram_used_pct >= 80.0f) return col_yellow;
      break;
    case MetricType::SwapRam:
      if (d.swap_total_gb > 0.0f) {
        float swap_pct = 100.0f * d.swap_used_gb / d.swap_total_gb;
        if (swap_pct >= 80.0f || d.swap_used_gb >= 4.0f) return col_red;
        if (swap_pct >= 50.0f || d.swap_used_gb >= 2.0f) return col_yellow;
      }
      break;
    case MetricType::MemPressure:
      if (d.mem_psi_some_pct >= 20.0f) return col_red;
      if (d.mem_psi_some_pct >= 5.0f) return col_yellow;
      break;
    case MetricType::DiskTemp:
      if (d.disk_temp_available) {
        if (d.disk_temp_c >= 75) return col_red;
        if (d.disk_temp_c >= 65) return col_yellow;
      }
      break;
    case MetricType::DiskIo: {
      float total_io = d.disk_read_mbs + d.disk_write_mbs;
      if (total_io >= 1500.0f) return col_red;
      if (total_io >= 500.0f) return col_yellow;
      break;
    }
    case MetricType::Battery:
      if (d.battery_available && !d.battery_charging) {
        if (d.battery_pct <= 12.0f) return col_red;
        if (d.battery_pct <= 25.0f) return col_yellow;
      }
      break;
    default:
      break;
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
    case MetricType::GpuVramClock:
      std::snprintf(sv.prefix, sizeof(sv.prefix), "VCLK:");
      std::snprintf(sv.number, sizeof(sv.number), "%d", d.gpu_vram_clock_mhz);
      std::snprintf(sv.unit, sizeof(sv.unit), "MHz");
      return;
    case MetricType::GpuModel:
      std::snprintf(sv.number, sizeof(sv.number), "%s", d.gpu_name[0] ? d.gpu_name : "GPU");
      return;
    case MetricType::CpuModel:
      std::snprintf(sv.number, sizeof(sv.number), "%s", d.cpu_name[0] ? d.cpu_name : "CPU");
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
    case MetricType::DiskIo:
      std::snprintf(sv.prefix, sizeof(sv.prefix), "DSK:");
      std::snprintf(sv.number, sizeof(sv.number), "%.1fR %.1fW", d.disk_read_mbs, d.disk_write_mbs);
      std::snprintf(sv.unit, sizeof(sv.unit), "MB/s");
      return;
    case MetricType::DiskTemp:
      std::snprintf(sv.prefix, sizeof(sv.prefix), "NVME:");
      if (d.disk_temp_available) {
        std::snprintf(sv.number, sizeof(sv.number), "%d", d.disk_temp_c);
        std::snprintf(sv.unit, sizeof(sv.unit),
                      "\xC2\xB0"
                      "C");
      } else {
        std::snprintf(sv.number, sizeof(sv.number), "N/A");
      }
      return;
    case MetricType::NetSpeed:
      std::snprintf(sv.prefix, sizeof(sv.prefix), "NET:");
      if (std::max(d.net_rx_mbs, d.net_tx_mbs) < 1.0f) {
        std::snprintf(sv.number, sizeof(sv.number), "%.0fD %.0fU", d.net_rx_mbs * 1024.0f,
                      d.net_tx_mbs * 1024.0f);
        std::snprintf(sv.unit, sizeof(sv.unit), "KB/s");
      } else {
        std::snprintf(sv.number, sizeof(sv.number), "%.1fD %.1fU", d.net_rx_mbs, d.net_tx_mbs);
        std::snprintf(sv.unit, sizeof(sv.unit), "MB/s");
      }
      return;
    default:
      break;
  }
}

MetricGroupList build_metric_groups(const HudConfig& cfg, const TelemetryData& data,
                                    uint8_t alpha) noexcept {
  MetricGroupList list{};
  for (MetricType m : cfg.metrics) {
    if (m == MetricType::Battery && !data.battery_available) continue;

    if (m == MetricType::CpuAllCoreLoad || m == MetricType::CpuAllCoreClock) {
      const bool is_load = (m == MetricType::CpuAllCoreLoad);
      const char* row_label = is_load ? "C.LOAD" : "C.CLK";
      ImU32 row_col = get_category_color(GroupCategory::CPU, cfg, alpha);
      int n_cores =
          std::clamp(data.cpu_core_count, 1, static_cast<int>(TelemetryData::kMaxCpuCores));

      for (int c = 0; c < n_cores; c += 4) {
        if (auto added = list.items.try_emplace_back(
                MetricGroup{GroupCategory::CPU, row_label, row_col, {}})) {
          for (int k = c; k < std::min(c + 4, n_cores); ++k) {
            StyledValue sv{};
            std::snprintf(sv.prefix, sizeof(sv.prefix), "C%u:", static_cast<unsigned>(k));
            if (is_load) {
              int pct = static_cast<int>(data.cpu_core_loads_pct[static_cast<size_t>(k)]);
              std::snprintf(sv.number, sizeof(sv.number), "%d", pct);
              std::snprintf(sv.unit, sizeof(sv.unit), "%%");
              if (cfg.thermal_warnings_enabled && pct >= 95) {
                sv.color = make_col32(255u, 70u, 70u, alpha);
              } else if (cfg.thermal_warnings_enabled && pct >= 80) {
                sv.color = make_col32(255u, 185u, 0u, alpha);
              } else {
                sv.color = make_col32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
              }
            } else {
              int mhz = static_cast<int>(data.cpu_core_clocks_mhz[static_cast<size_t>(k)]);
              if (mhz <= 0) mhz = data.cpu_clock_mhz;
              std::snprintf(sv.number, sizeof(sv.number), "%d", mhz);
              std::snprintf(sv.unit, sizeof(sv.unit), "MHz");
              sv.color = make_col32(cfg.text_color.r, cfg.text_color.g, cfg.text_color.b, alpha);
            }
            added->values.try_push_back(sv);
          }
        }
      }
      continue;
    }

    GroupCategory cat = get_metric_category(m);
    MetricGroup* target = nullptr;
    for (auto& item : list.items) {
      if (item.category == cat && std::strcmp(item.label, get_category_label(cat)) == 0) {
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
    ImU32 drop_col = make_col32(0u, 0u, 0u, sh_alpha);
    float d = std::max(1.0f, std::round(cfg.shadow_offset * scale_factor));
    draw_list->AddText(font, font_size, ImVec2(pos.x + d, pos.y + d), drop_col, text);
  }

  // 2. Adaptive stroke outline
  if (cfg.outline_enabled && cfg.outline_thickness > 0.0f) {
    uint8_t out_alpha =
        static_cast<uint8_t>(std::clamp(0.95f * cfg.text_opacity * 255.0f, 0.0f, 255.0f));
    ImU32 outline_col = make_col32(0u, 0u, 0u, out_alpha);
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

// Draws a single metric item with dimmed (or full-brightness) prefix/unit and bright number
void draw_styled_metric_item(const StyledValue& sv, const HudConfig& cfg, ImU32 muted_col) {
  float tight_gap = 2.0f * (cfg.font_size_px / 20.0f);
  bool has_prev = false;
  ImU32 label_col = cfg.dim_labels_enabled ? muted_col : sv.color;

  if (sv.prefix[0] != '\0') {
    draw_crisp_text(sv.prefix, label_col, cfg);
    has_prev = true;
  }
  if (sv.number[0] != '\0') {
    if (has_prev) ImGui::SameLine(0.0f, tight_gap);
    draw_crisp_text(sv.number, sv.color, cfg);
    has_prev = true;
  }
  if (sv.unit[0] != '\0') {
    if (has_prev) ImGui::SameLine(0.0f, 1.5f);
    draw_crisp_text(sv.unit, label_col, cfg);
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
  ImGui::PushStyleColor(ImGuiCol_PlotLines,
                        ImVec4(0.0f, 0.90f, 1.0f, static_cast<float>(alpha) / 255.0f));
  ImGui::PlotLines("##HUD_MiniFTGraph", telemetry.get_ft_history(),
                   TelemetryCollector::kGraphHistorySize, telemetry.get_graph_offset(),
                   "frametime (ms)", 0.0f, 50.0f, ImVec2(graph_w, graph_h));
  ImGui::PopStyleColor(2);
}

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

ImFont* get_gui_font(int tier) {
  if (!ImGui::GetCurrentContext()) return nullptr;
  auto* atlas = ImGui::GetIO().Fonts;
  if (!atlas || atlas->Fonts.empty()) return nullptr;
  int idx = std::clamp(tier, 0, 2);
  if (idx < atlas->Fonts.Size) {
    return atlas->Fonts[idx];
  }
  return atlas->Fonts[0];
}

ImFont* get_gui_font(float gui_scale, float& out_base_size) {
  int tier = 1;
  if (gui_scale <= 0.75f) {
    tier = 0;
    out_base_size = 15.0f;
  } else if (gui_scale > 1.30f) {
    tier = 2;
    out_base_size = 20.0f;
  } else {
    tier = 1;
    out_base_size = 17.0f;
  }
  ImFont* font = get_gui_font(tier);
  return font ? font : ImGui::GetFont();
}

ImFont* get_hud_font(int family, int tier) {
  if (!ImGui::GetCurrentContext()) return nullptr;
  auto* atlas = ImGui::GetIO().Fonts;
  if (!atlas || atlas->Fonts.empty()) return nullptr;
  int base_idx = (family == 0) ? 3 : 6;
  int clamped_tier = std::clamp(tier, 0, 2);
  int idx = base_idx + clamped_tier;
  if (idx < atlas->Fonts.Size) {
    return atlas->Fonts[idx];
  }
  return atlas->Fonts[0];
}

ImFont* get_hud_font(FontFamily family, float font_size_px, float& out_base_size) {
  int tier = 1;
  if (font_size_px <= 15.0f) {
    tier = 0;
    out_base_size = 14.0f;
  } else if (font_size_px >= 26.0f) {
    tier = 2;
    out_base_size = 32.0f;
  } else {
    tier = 1;
    out_base_size = 20.0f;
  }
  ImFont* font = get_hud_font(static_cast<int>(family), tier);
  return font ? font : ImGui::GetFont();
}

void compute_clamped_hud_pos(const HudConfig& cfg, float display_w, float display_h, float hud_w,
                             float hud_h, float& out_x, float& out_y) noexcept {
  float safe_disp_w = std::max(1.0f, display_w);
  float safe_disp_h = std::max(1.0f, display_h);
  float safe_hud_w = std::clamp(hud_w, 0.0f, safe_disp_w);
  float safe_hud_h = std::clamp(hud_h, 0.0f, safe_disp_h);
  float max_x = std::max(0.0f, safe_disp_w - safe_hud_w);
  float max_y = std::max(0.0f, safe_disp_h - safe_hud_h);
  float nx = std::clamp(cfg.custom_pos_x, 0.0f, 1.0f);
  float ny = std::clamp(cfg.custom_pos_y, 0.0f, 1.0f);
  out_x = std::clamp(nx * max_x, 0.0f, max_x);
  out_y = std::clamp(ny * max_y, 0.0f, max_y);
}

void setup_overlay_font(ImGuiIO& io) {
  io.IniFilename = nullptr;
  std::string home_fonts;
  if (const char* home = std::getenv("HOME"); home && *home) {
    home_fonts = std::string(home) + "/.local/share/fpshud/fonts/";
  }

  std::vector<std::string> open_sans_paths = {home_fonts + "OpenSans-Bold.ttf",
                                              "/run/host/usr/share/fonts/TTF/OpenSans-Bold.ttf",
                                              "/usr/share/fonts/TTF/OpenSans-Bold.ttf"};

  std::vector<std::string> mono_paths = {home_fonts + "DejaVuSansMono-Bold.ttf",
                                         "/run/host/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
                                         "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
                                         home_fonts + "AdwaitaMono-Bold.ttf",
                                         "/run/host/usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf",
                                         "/usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf"};

  // Index 0..2: Dedicated GUI Menu Fonts (Small 15px, Normal 17px, Large 20px)
  load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                         sizeof(kEmbeddedOpenSansBoldTTF), 15.0f);
  load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                         sizeof(kEmbeddedOpenSansBoldTTF), 17.0f);
  load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                         sizeof(kEmbeddedOpenSansBoldTTF), 20.0f);

  // Index 3..5: HUD Sans-Serif Fonts (Small 14px, Normal 20px, Large 32px)
  load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                         sizeof(kEmbeddedOpenSansBoldTTF), 14.0f);
  load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                         sizeof(kEmbeddedOpenSansBoldTTF), 20.0f);
  load_ttf_with_fallback(io, open_sans_paths, kEmbeddedOpenSansBoldTTF,
                         sizeof(kEmbeddedOpenSansBoldTTF), 32.0f);

  // Index 6..8: HUD Monospace Fonts (Small 14px, Normal 20px, Large 32px)
  // Rasterized once per size tier instead of 3 duplicate times across families!
  load_ttf_with_fallback(io, mono_paths, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF),
                         14.0f);
  load_ttf_with_fallback(io, mono_paths, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF),
                         20.0f);
  load_ttf_with_fallback(io, mono_paths, kEmbeddedVeraMonoBoldTTF, sizeof(kEmbeddedVeraMonoBoldTTF),
                         32.0f);

  FPSHUD_LOG_DEBUG(fpshud::log::Subsystem::UI, "Initialized 9-font overlay atlas");
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

  ImGuiWindowFlags flags =
      static_cast<ImGuiWindowFlags>(static_cast<uint32_t>(ImGuiWindowFlags_NoDecoration) |
                                    static_cast<uint32_t>(ImGuiWindowFlags_AlwaysAutoResize) |
                                    static_cast<uint32_t>(ImGuiWindowFlags_NoSavedSettings) |
                                    static_cast<uint32_t>(ImGuiWindowFlags_NoFocusOnAppearing) |
                                    static_cast<uint32_t>(ImGuiWindowFlags_NoNav) |
                                    static_cast<uint32_t>(ImGuiWindowFlags_NoMove) |
                                    static_cast<uint32_t>(ImGuiWindowFlags_NoInputs));

  if (cfg.background == BackgroundStyle::None || cfg.bg_alpha <= 0.001f) {
    flags = static_cast<ImGuiWindowFlags>(static_cast<uint32_t>(flags) |
                                          static_cast<uint32_t>(ImGuiWindowFlags_NoBackground));
  } else {
    ImGui::SetNextWindowBgAlpha(cfg.bg_alpha);
  }

  static ImVec2 s_last_hud_size(160.0f, 48.0f);
  ImVec2 pos(cfg.margin_x, cfg.margin_y);
  ImVec2 pivot(0.0f, 0.0f);

  switch (cfg.layout) {
    case LayoutPosition::TopLeft:
      pos = ImVec2(cfg.margin_x, cfg.margin_y);
      pivot = ImVec2(0.0f, 0.0f);
      break;
    case LayoutPosition::TopCenter:
      pos = ImVec2(display_width * 0.5f, cfg.margin_y);
      pivot = ImVec2(0.5f, 0.0f);
      break;
    case LayoutPosition::TopRight:
      pos = ImVec2(display_width - cfg.margin_x, cfg.margin_y);
      pivot = ImVec2(1.0f, 0.0f);
      break;
    case LayoutPosition::BottomLeft:
      pos = ImVec2(cfg.margin_x, display_height - cfg.margin_y);
      pivot = ImVec2(0.0f, 1.0f);
      break;
    case LayoutPosition::BottomCenter:
      pos = ImVec2(display_width * 0.5f, display_height - cfg.margin_y);
      pivot = ImVec2(0.5f, 1.0f);
      break;
    case LayoutPosition::BottomRight:
      pos = ImVec2(display_width - cfg.margin_x, display_height - cfg.margin_y);
      pivot = ImVec2(1.0f, 1.0f);
      break;
    case LayoutPosition::Custom: {
      float cx = 0.0f, cy = 0.0f;
      compute_clamped_hud_pos(cfg, display_width, display_height, s_last_hud_size.x,
                              s_last_hud_size.y, cx, cy);
      pos = ImVec2(cx, cy);
      pivot = ImVec2(0.0f, 0.0f);
      break;
    }
  }

  ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, cfg.background == BackgroundStyle::None
                                                       ? ImVec2(4.0f, 4.0f)
                                                       : ImVec2(12.0f, 8.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.06f, cfg.bg_alpha));

  if (ImGui::Begin("##FPSHUD_Overlay", nullptr, flags)) {
    ImVec2 cur_win_size = ImGui::GetWindowSize();
    if (cur_win_size.x > 4.0f && cur_win_size.y > 4.0f) {
      s_last_hud_size = cur_win_size;
      if (cfg.layout == LayoutPosition::Custom) {
        float cx = 0.0f, cy = 0.0f;
        compute_clamped_hud_pos(cfg, display_width, display_height, cur_win_size.x, cur_win_size.y,
                                cx, cy);
        ImVec2 cur_pos = ImGui::GetWindowPos();
        if (std::fabs(cur_pos.x - cx) > 0.5f || std::fabs(cur_pos.y - cy) > 0.5f) {
          ImGui::SetWindowPos(ImVec2(cx, cy), ImGuiCond_Always);
        }
      }
    }
    cfg.font_scale = std::clamp(cfg.font_size_px / base_hud_font_px, 0.40f, 2.50f);
    ImGui::SetWindowFontScale(cfg.font_scale);

    uint8_t fg_alpha = static_cast<uint8_t>(std::clamp(cfg.text_opacity * 255.0f, 10.0f, 255.0f));
    uint8_t muted_alpha =
        static_cast<uint8_t>(std::clamp(cfg.text_opacity * 215.0f, 10.0f, 255.0f));
    uint8_t sep_alpha = static_cast<uint8_t>(std::clamp(cfg.text_opacity * 170.0f, 10.0f, 255.0f));

    // Soft cool-gray for units & sub-labels (AVG:, 1%:, MHz, GB, W, °C) so numbers pop out cleanly
    ImU32 muted_col = make_col32(175u, 190u, 208u, muted_alpha);
    ImU32 dot_col = make_col32(130u, 145u, 165u, sep_alpha);
    ImU32 sep_color = make_col32(160u, 175u, 195u, sep_alpha);

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
        draw_crisp_text(rec_buf, make_col32(255u, 60u, 60u, fg_alpha), cfg);
      }
      if (RecorderController::instance().is_recording()) {
        ImGui::SameLine(0.0f, 10.0f * ui_ratio);
        float rec_sec = RecorderController::instance().get_recording_duration();
        int mins = static_cast<int>(rec_sec) / 60;
        int secs = static_cast<int>(rec_sec) % 60;
        char rec_tag[64];
        std::snprintf(rec_tag, sizeof(rec_tag), "[REC %02d:%02d]", mins, secs);
        draw_crisp_text(rec_tag, make_col32(255u, 50u, 50u, fg_alpha), cfg);
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
        draw_crisp_text(rec_buf, make_col32(255u, 60u, 60u, fg_alpha), cfg);
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
        draw_crisp_text(rec_buf, make_col32(255u, 60u, 60u, fg_alpha), cfg);
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
        draw_crisp_text(bsum, make_col32(80u, 255u, 120u, fg_alpha), cfg);
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
      draw_crisp_text(rec_tag, make_col32(255u, 50u, 50u, fg_alpha), cfg);
    }

    // Show recent toast notification (e.g. video saved, benchmark saved) on HUD
    std::string toast_msg;
    if (RecorderController::instance().get_recent_toast(toast_msg)) {
      draw_crisp_text(toast_msg.c_str(), make_col32(60u, 245u, 120u, fg_alpha), cfg);
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
