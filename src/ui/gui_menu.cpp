#include "gui_menu.hpp"
#if FPSHUD_ENABLE_EXPERIMENTAL
#include "adaptive_vfr.hpp"
#endif
#include "input_manager.hpp"
#include "overlay_ui.hpp"
#include "recorder_ipc.hpp"

#include "imgui.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace fpshud {

namespace {

std::string g_status_msg;
std::chrono::steady_clock::time_point g_status_time{};
std::chrono::steady_clock::time_point g_startup_time{};
bool g_startup_initialized = false;

void set_status_message(const std::string& msg) {
  g_status_msg = msg;
  g_status_time = std::chrono::steady_clock::now();
}

void open_system_folder(const std::string& path) {
  std::vector<std::string> env_strings;
  for (char** env = environ; env && *env; ++env) {
    std::string e = *env;
    if (e.rfind("LD_PRELOAD=", 0) == 0) continue;
    env_strings.push_back(std::move(e));
  }
  std::vector<char*> envp;
  envp.reserve(env_strings.size() + 1);
  for (auto& s : env_strings) {
    envp.push_back(s.data());
  }
  envp.push_back(nullptr);

  char* argv[] = {const_cast<char*>("xdg-open"), const_cast<char*>(path.c_str()), nullptr};
  pid_t pid = 0;
  int res = posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, envp.data());
  if (res == 0 && pid > 0) {
    std::thread([pid]() {
      int status = 0;
      ::waitpid(pid, &status, 0);
    }).detach();
  }
}

void draw_section_header(const char* title,
                         const ImVec4& color = ImVec4(0.38f, 0.84f, 1.0f, 1.0f)) {
  ImGui::Spacing();
  ImGui::TextColored(color, "%s", title);
  ImGui::Separator();
}

// Helper to draw a metric checkbox with right-aligned live value inside the right panel
void draw_metric_checkbox_row(HudConfig& cfg, MetricType type, const char* label,
                              const char* live_val, bool is_available = true,
                              const char* unavailable_hint = nullptr) {
  if (!is_available) {
    ImGui::BeginDisabled();
  }
  bool active = cfg.has_metric(type);
  ImGui::PushID(static_cast<int>(type));
  if (ImGui::Checkbox(label, &active)) {
    if (is_available) {
      cfg.set_metric(type, active);
      cfg.active_preset = "custom";
    }
  }
  if (live_val && *live_val) {
    ImGui::SameLine();
    float avail = ImGui::GetContentRegionAvail().x;
    float txt_w = ImGui::CalcTextSize(live_val).x;
    if (avail > txt_w + 4.0f) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - txt_w - 4.0f);
    }
    ImGui::TextColored(
        active ? ImVec4(0.50f, 0.95f, 0.62f, 1.0f) : ImVec4(0.50f, 0.55f, 0.62f, 1.0f), "%s",
        live_val);
  }
  ImGui::PopID();
  if (!is_available) {
    ImGui::EndDisabled();
    if (unavailable_hint && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("%s", unavailable_hint);
    }
  }
}

void draw_keybind_row(const char* label, KeybindTarget target, const Keybind& kb,
                      const char* default_hint, float scale) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::Text("%s", label);
  ImGui::TableSetColumnIndex(1);

  auto& im = get_input_manager();
  bool is_cap = im.is_capturing_keybind() && (im.capturing_target() == target);
  ImGui::PushID(static_cast<int>(target));

  float btn_w = 58.0f * scale;
  float avail_w = ImGui::GetContentRegionAvail().x;
  float box_w = std::max(40.0f, avail_w - btn_w - (6.0f * scale));

  if (is_cap) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.42f, 0.08f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.50f, 0.12f, 1.0f));
    ImGui::Button("Tekan 1/2 Tombol...", ImVec2(box_w, 0.0f));
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0.0f, 6.0f * scale);
    if (ImGui::Button("Batal", ImVec2(btn_w, 0.0f))) {
      im.cancel_keybind_capture();
    }
  } else {
    std::string kb_str =
        (default_hint && *default_hint) ? std::string(default_hint) : format_keybind(kb);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.15f, 0.20f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.20f, 0.28f, 1.0f));
    if (ImGui::Button(kb_str.c_str(), ImVec2(box_w, 0.0f))) {
      im.start_keybind_capture(target);
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0.0f, 6.0f * scale);
    if (ImGui::Button("Ubah", ImVec2(btn_w, 0.0f))) {
      im.start_keybind_capture(target);
    }
  }
  ImGui::PopID();
}

}  // namespace

void render_gui_menu(HudConfig& cfg, const TelemetryData& data, float display_width,
                     float display_height) {
  if (!cfg.enabled) return;

  float scale = std::clamp(cfg.gui_scale, 0.50f, 2.00f);
  float base_gui_font_px = 16.0f;
  ImFont* gui_font = get_gui_font(scale, base_gui_font_px);

  auto now = std::chrono::steady_clock::now();
  if (!g_startup_initialized) {
    g_startup_initialized = true;
    g_startup_time = now;
  }

  // Show a brief 5-second toast hint on launch so user knows how to toggle the GUI
  double elapsed_since_start = std::chrono::duration<double>(now - g_startup_time).count();
  if (!cfg.gui_open && elapsed_since_start < 5.5) {
    ImGui::SetNextWindowPos(ImVec2(display_width * 0.5f, display_height - 28.0f), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.82f);
    ImGuiWindowFlags toast_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    if (gui_font) ImGui::PushFont(gui_font);
    if (ImGui::Begin("##FPSHUD_ToastHint", nullptr, toast_flags)) {
      ImGui::SetWindowFontScale(16.0f / base_gui_font_px);
      if (get_input_manager().has_evdev_access()) {
        std::string g_kb = format_keybind(cfg.bind_toggle_gui);
        std::string h_kb = format_keybind(cfg.bind_toggle_hud);
        std::string b_kb = format_keybind(cfg.bind_benchmark);
        ImGui::TextColored(ImVec4(0.4f, 0.92f, 1.0f, 1.0f),
                           "FPSHUD Active  |  GUI: %s  |  Hide HUD: %s  |  Benchmark: %s",
                           g_kb.c_str(), h_kb.c_str(), b_kb.c_str());
      } else {
        ImGui::TextColored(
            ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
            "[FPSHUD GUI] Hotkeys need input access: re-run the installer and allow sudo");
      }
    }
    ImGui::End();
    if (gui_font) ImGui::PopFont();
  }

  if (!cfg.gui_open) return;

  constexpr float kBaseGuiWidth = 1280.0f;
  constexpr float kBaseGuiHeight = 720.0f;

  float max_w = std::max(display_width - 24.0f, 320.0f);
  float max_h = std::max(display_height - 24.0f, 240.0f);
  float win_w = std::min(kBaseGuiWidth * scale, max_w);
  float win_h = std::min(kBaseGuiHeight * scale, max_h);

  static float s_applied_scale = -1.0f;
  bool scale_changed = (s_applied_scale > 0.0f && std::fabs(s_applied_scale - scale) > 0.005f);
  s_applied_scale = scale;

  // Wide side-by-side dashboard window (1280x720 base landscape, scalable live)
  ImGui::SetNextWindowPos(ImVec2(display_width * 0.5f, display_height * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  if (scale_changed) {
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
  } else {
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_FirstUseEver);
  }

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 4.0f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, 10.0f * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f * scale, 4.0f * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 5.0f * scale));

  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.10f, 0.97f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.11f, 0.14f, 0.90f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.10f, 0.14f, 0.20f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.14f, 0.22f, 0.34f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.14f, 0.17f, 0.23f, 0.92f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.19f, 0.25f, 0.35f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.26f, 0.40f, 0.95f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.36f, 0.55f, 1.00f));

  if (gui_font) ImGui::PushFont(gui_font);

  if (ImGui::Begin("FPSHUD Configuration Tool", &cfg.gui_open, ImGuiWindowFlags_MenuBar)) {
    ImGui::SetWindowFontScale((15.5f * scale) / base_gui_font_px);

    // ====================================================================
    // 1. Top Menu Bar
    // ====================================================================
    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save Configuration (config.ini)")) {
          if (save_config_to_disk(cfg)) {
            set_status_message("Tersimpan ke config.ini");
          }
        }
        if (ImGui::MenuItem("Save as Custom Preset (custom_preset.ini)")) {
          cfg.active_preset = "custom";
          if (save_custom_preset_to_disk(cfg)) {
            set_status_message("Tersimpan ke custom_preset.ini");
          }
        }
        if (ImGui::MenuItem("Reload Config from Disk")) {
          if (load_config_from_disk(cfg)) {
            set_status_message("Konfigurasi dimuat ulang.");
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset to Standard Default")) {
          apply_preset_standard(cfg);
          cfg.gui_scale = 1.0f;
          cfg.text_color = {255, 255, 255, 255};
          cfg.text_opacity = 1.0f;
          cfg.font_size_px = 20.0f;
          cfg.font_scale = 20.0f / kAtlasBaseFontSizePx;
          cfg.outline_enabled = true;
          cfg.outline_thickness = 1.4f;
          cfg.shadow_enabled = true;
          cfg.shadow_offset = 2.0f;
          cfg.category_colors_enabled = true;
          cfg.thermal_warnings_enabled = true;
          cfg.dim_labels_enabled = true;
          set_status_message("Direset ke Preset Standard.");
        }
        if (ImGui::MenuItem("Close GUI (ESC)")) {
          cfg.gui_open = false;
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Show Overlay HUD", nullptr, &cfg.hud_visible);
        ImGui::MenuItem("Show Mini Frametime Graph on HUD", nullptr, &cfg.hud_graph_enabled);
        ImGui::MenuItem("Category Color Coding", nullptr, &cfg.category_colors_enabled);
        ImGui::MenuItem("Thermal & Load Warnings", nullptr, &cfg.thermal_warnings_enabled);
        ImGui::MenuItem("Dim Unit & Prefix Labels", nullptr, &cfg.dim_labels_enabled);
        ImGui::EndMenu();
      }
#if FPSHUD_ENABLE_EXPERIMENTAL
      if (ImGui::BeginMenu("Experimental")) {
        fpshud::experimental::draw_experimental_menu(cfg);
        ImGui::EndMenu();
      }
#endif
      ImGui::EndMenuBar();
    }

    // ====================================================================
    // 2. Top Header Bar: Presets on Left + Live FPS Waveform on Right
    // ====================================================================
    float presets_col_w = 395.0f * scale;
    if (ImGui::BeginTable("##TopHeaderBar", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("PresetsCol", ImGuiTableColumnFlags_WidthFixed, presets_col_w);
      ImGui::TableSetupColumn("GraphCol", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ImVec4(0.75f, 0.84f, 0.95f, 1.0f), "Presets:");
      ImGui::SameLine();

      auto draw_preset_btn = [&](const char* label, const char* preset_id, auto apply_fn) {
        bool is_active = (cfg.active_preset == preset_id);
        if (is_active) {
          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.54f, 0.34f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.64f, 0.40f, 1.0f));
        }
        if (ImGui::Button(label, ImVec2(74.0f * scale, 28.0f * scale))) {
          apply_fn(cfg);
          set_status_message(std::string("Preset ") + label + " aktif.");
        }
        if (is_active) {
          ImGui::PopStyleColor(2);
        }
      };

      draw_preset_btn("Minimal", "minimal", apply_preset_minimal);
      ImGui::SameLine(0.0f, 5.0f * scale);
      draw_preset_btn("Standard", "standard", apply_preset_standard);
      ImGui::SameLine(0.0f, 5.0f * scale);
      draw_preset_btn("Advance", "advance", apply_preset_advance);
      ImGui::SameLine(0.0f, 5.0f * scale);
      draw_preset_btn("Custom", "custom", apply_preset_custom);

      ImGui::TableSetColumnIndex(1);
      const auto& telemetry = get_telemetry();
      char graph_overlay[96];
      std::snprintf(graph_overlay, sizeof(graph_overlay),
                    "%.0f FPS  |  AVG %.0f  |  1%% %.0f  |  0.1%% %.0f (%.1fms)", data.fps,
                    data.fps_avg, data.fps_1pct_low, data.fps_01pct_low, data.frametime_ms);
      ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.25f, 0.88f, 1.0f, 1.0f));
      ImGui::PlotLines("##HeaderFPSWave", telemetry.get_fps_history(),
                       TelemetryCollector::kGraphHistorySize, telemetry.get_graph_offset(),
                       graph_overlay, 0.0f, 240.0f, ImVec2(-1.0f, 28.0f * scale));
      ImGui::PopStyleColor();

      ImGui::EndTable();
    }

    // ====================================================================
    // 3. Three-Column Scrollable Dashboard
    //    Col 1 (Left ~32%):   HUD Style, Typography, Colors & All 6 Hotkeys
    //    Col 2 (Center ~36%): Sticky Header + Full-Width Scrollable 31 Sensors
    //    Col 3 (Right ~32%):  Sticky Quick Actions + Scrollable Capture Settings
    // ====================================================================
    float footer_height = 42.0f * scale;
    float panel_height = ImGui::GetContentRegionAvail().y - footer_height;
    float total_avail_w = ImGui::GetContentRegionAvail().x;
    float col_gap = 8.0f * scale;
    float col1_w = std::floor((total_avail_w - (2.0f * col_gap)) * 0.32f);
    float col2_w = std::floor((total_avail_w - (2.0f * col_gap)) * 0.36f);
    float col3_w = total_avail_w - col1_w - col2_w - (2.0f * col_gap);

    // --------------------------------------------------------------------
    // COLUMN 1: HUD Style, Appearance, Typography & All 6 Hotkeys
    // --------------------------------------------------------------------
    ImGui::BeginChild("##Col1_HudStyle", ImVec2(col1_w, panel_height), true);

    if (ImGui::BeginTable("##Col1FormTable", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f * scale);
      ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 1.0f);

      // Section 1: Layout & Grouping
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      draw_section_header("Layout & Grouping");
      ImGui::TableSetColumnIndex(1);
      draw_section_header("");

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Screen Position");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      const char* layout_items[] = {"Top-Left",    "Top-Center",    "Top-Right",
                                    "Bottom-Left", "Bottom-Center", "Bottom-Right"};
      int current_layout = static_cast<int>(cfg.layout);
      if (ImGui::Combo("##Position", &current_layout, layout_items, IM_ARRAYSIZE(layout_items))) {
        cfg.layout = static_cast<LayoutPosition>(current_layout);
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Arrangement");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      const char* arr_items[] = {"Stack (Vertikal per Grup)", "Double (Grid 2 Kolom Grup)",
                                 "Linear (1 Baris Horizontal)"};
      int current_arr = static_cast<int>(cfg.arrangement);
      if (ImGui::Combo("##Arrangement", &current_arr, arr_items, IM_ARRAYSIZE(arr_items))) {
        cfg.arrangement = static_cast<ArrangementMode>(current_arr);
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("HUD Display");
      ImGui::TableSetColumnIndex(1);
      ImGui::Checkbox("Show HUD", &cfg.hud_visible);
      ImGui::SameLine(0.0f, 12.0f * scale);
      ImGui::Checkbox("Mini FT Graph", &cfg.hud_graph_enabled);

      // Section 2: Typography & Scaling
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      draw_section_header("Typography & Scaling");
      ImGui::TableSetColumnIndex(1);
      draw_section_header("");

      // 1. Render Scale Dropdown (0.50x to 2.00x)
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Render Scale");
      ImGui::TableSetColumnIndex(1);

      const char* scale_options[] = {"0.50x (50% - Compact)",    "0.75x (75% - Small)",
                                     "1.00x (100% - Default)",   "1.25x (125% - Medium)",
                                     "1.50x (150% - Large)",     "1.75x (175% - Extra Large)",
                                     "2.00x (200% - Ultra / 4K)"};
      const float scale_values[] = {0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 1.75f, 2.00f};

      int current_scale_idx = -1;
      for (int i = 0; i < 7; ++i) {
        if (std::fabs(cfg.gui_scale - scale_values[i]) < 0.015f) {
          current_scale_idx = i;
          break;
        }
      }

      char combo_preview[64];
      if (current_scale_idx >= 0) {
        std::snprintf(combo_preview, sizeof(combo_preview), "%s", scale_options[current_scale_idx]);
      } else {
        std::snprintf(combo_preview, sizeof(combo_preview), "Custom (%.0f%%)",
                      cfg.gui_scale * 100.0f);
      }

      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##RenderScaleCombo", combo_preview)) {
        for (int i = 0; i < 7; ++i) {
          bool is_selected = (current_scale_idx == i);
          if (ImGui::Selectable(scale_options[i], is_selected)) {
            cfg.gui_scale = scale_values[i];
          }
          if (is_selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }

      // 2. Scale Fine-Tune Slider (50% - 200%) + 100% Reset Button
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Scale Fine-Tune");
      ImGui::TableSetColumnIndex(1);
      float reset_btn_w = 54.0f * scale;
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reset_btn_w - (6.0f * scale));
      int scale_pct = static_cast<int>(std::round(cfg.gui_scale * 100.0f));
      if (ImGui::SliderInt("##MenuScaleSlider", &scale_pct, 50, 200, "%d%%")) {
        cfg.gui_scale = std::clamp(scale_pct / 100.0f, 0.50f, 2.00f);
      }
      ImGui::SameLine(0.0f, 6.0f * scale);
      if (ImGui::Button("100%", ImVec2(reset_btn_w, 0.0f))) {
        cfg.gui_scale = 1.0f;
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Font Family");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      const char* font_items[] = {"Sans Bold (NVIDIA Style)", "DejaVu Sans Mono Bold",
                                  "Vera Sans Mono Bold", "Adwaita Mono Bold"};
      int current_font = static_cast<int>(cfg.font_family);
      if (ImGui::Combo("##FontStyle", &current_font, font_items, IM_ARRAYSIZE(font_items))) {
        cfg.font_family = static_cast<FontFamily>(current_font);
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("HUD Font Size");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::SliderFloat("##FontSize", &cfg.font_size_px, 12.0f, 42.0f, "%.0f px")) {
        cfg.font_scale = cfg.font_size_px / kAtlasBaseFontSizePx;
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Text Outline");
      ImGui::TableSetColumnIndex(1);
      ImGui::Checkbox("##OutlineToggle", &cfg.outline_enabled);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::BeginDisabled(!cfg.outline_enabled);
      ImGui::SliderFloat("##OutlineThickness", &cfg.outline_thickness, 0.5f, 3.5f,
                         "Thickness: %.1f px");
      ImGui::EndDisabled();

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Drop Shadow");
      ImGui::TableSetColumnIndex(1);
      ImGui::Checkbox("##ShadowToggle", &cfg.shadow_enabled);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::BeginDisabled(!cfg.shadow_enabled);
      ImGui::SliderFloat("##ShadowOffset", &cfg.shadow_offset, 0.5f, 5.0f, "Offset: %.1f px");
      ImGui::EndDisabled();

      // Section 3: Colors & Opacity
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      draw_section_header("Colors & Opacity");
      ImGui::TableSetColumnIndex(1);
      draw_section_header("");

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Base Text Color");
      ImGui::TableSetColumnIndex(1);
      float col3[3] = {cfg.text_color.r / 255.0f, cfg.text_color.g / 255.0f,
                       cfg.text_color.b / 255.0f};
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::ColorEdit3("##BaseTextColor", col3, ImGuiColorEditFlags_Uint8)) {
        cfg.text_color.r = static_cast<uint8_t>(std::clamp(col3[0] * 255.0f, 0.0f, 255.0f));
        cfg.text_color.g = static_cast<uint8_t>(std::clamp(col3[1] * 255.0f, 0.0f, 255.0f));
        cfg.text_color.b = static_cast<uint8_t>(std::clamp(col3[2] * 255.0f, 0.0f, 255.0f));
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Text Opacity");
      ImGui::TableSetColumnIndex(1);
      float txt_pct = cfg.text_opacity * 100.0f;
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::SliderFloat("##TextOpacity", &txt_pct, 5.0f, 100.0f, "%.0f%%")) {
        cfg.text_opacity = std::clamp(txt_pct / 100.0f, 0.05f, 1.0f);
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("BG Opacity");
      ImGui::TableSetColumnIndex(1);
      float bg_pct = cfg.bg_alpha * 100.0f;
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::SliderFloat("##BGOpacity", &bg_pct, 0.0f, 95.0f, "%.0f%%")) {
        cfg.bg_alpha = bg_pct / 100.0f;
        cfg.background =
            (cfg.bg_alpha <= 0.005f) ? BackgroundStyle::None : BackgroundStyle::Translucent;
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Color Coding");
      ImGui::TableSetColumnIndex(1);
      ImGui::Checkbox("Category Colors", &cfg.category_colors_enabled);
      ImGui::Checkbox("Smart Color Alerts (Yellow/Red)", &cfg.thermal_warnings_enabled);

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Label Brightness");
      ImGui::TableSetColumnIndex(1);
      ImGui::Checkbox("Dim Unit & Sub-Labels (Redup)", &cfg.dim_labels_enabled);

      // Section 4: Unified Hotkeys (All 6 Shortcuts)
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      draw_section_header("Hotkeys (Pintasan)");
      ImGui::TableSetColumnIndex(1);
      draw_section_header("");

      const char* gui_default_hint = (cfg.key_toggle_gui == 0 && cfg.bind_toggle_gui.mod_key == 0 &&
                                      cfg.bind_toggle_gui.main_key == 88)
                                         ? "Insert / F12 / F10"
                                         : nullptr;
      draw_keybind_row("GUI Menu Key", KeybindTarget::ToggleGui, cfg.bind_toggle_gui,
                       gui_default_hint, scale);
      draw_keybind_row("HUD Toggle Key", KeybindTarget::ToggleHud, cfg.bind_toggle_hud, nullptr,
                       scale);
      draw_keybind_row("Record Key", KeybindTarget::ToggleRecord, cfg.bind_record, nullptr, scale);
      draw_keybind_row("Replay Key", KeybindTarget::SaveReplay, cfg.bind_save_replay, nullptr,
                       scale);
      draw_keybind_row("Screenshot Key", KeybindTarget::TakeScreenshot, cfg.bind_screenshot,
                       nullptr, scale);
      draw_keybind_row("Bench Key", KeybindTarget::ToggleBenchmark, cfg.bind_benchmark, nullptr,
                       scale);

      ImGui::EndTable();
    }

    ImGui::EndChild();

    ImGui::SameLine(0.0f, col_gap);

    // --------------------------------------------------------------------
    // COLUMN 2: 31 Hardware Telemetry Sensors (Sticky Header + 1-Col Scroll)
    // --------------------------------------------------------------------
    ImGui::BeginChild("##Col2_HardwareSensors", ImVec2(col2_w, panel_height), true);

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.38f, 0.84f, 1.0f, 1.0f), "Hardware Sensors (%zu/%zu Active)",
                       cfg.metrics.size(), kTotalMetricCount);
    float filter_btns_w = 148.0f * scale;
    float avail_hdr_x = ImGui::GetContentRegionAvail().x;
    if (avail_hdr_x > filter_btns_w) {
      ImGui::SameLine(ImGui::GetCursorPosX() + avail_hdr_x - filter_btns_w);
    } else {
      ImGui::SameLine();
    }
    if (ImGui::SmallButton("All")) {
      apply_preset_advance(cfg);
      cfg.active_preset = "custom";
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Standard")) {
      apply_preset_standard(cfg);
      cfg.active_preset = "custom";
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("FPS")) {
      // Zero-heap assignment to std::inplace_vector
      cfg.metrics = {MetricType::FPS, MetricType::FrameTimes};
      cfg.active_preset = "custom";
    }
    ImGui::Separator();

    char b_fps[24], b_avg[24], b_1l[24], b_01l[24], b_ft[24];
    char b_gload[24], b_gclk[24], b_gvclk[24], b_gvram[24], b_gbw[24], b_gtmp[24], b_gfan[24],
        b_gpwr[24];
    char b_cload[24], b_cclk[24], b_cpwr[24], b_ctmp[24], b_ram[32], b_api[32];
    char b_gram[24], b_swap[32], b_psi[24];
    char b_all_cload[32], b_all_cclk[32];

    std::snprintf(b_fps, sizeof(b_fps), "%.0f", data.fps);
    std::snprintf(b_avg, sizeof(b_avg), "%.0f", data.fps_avg);
    std::snprintf(b_1l, sizeof(b_1l), "%.0f", data.fps_1pct_low);
    std::snprintf(b_01l, sizeof(b_01l), "%.0f", data.fps_01pct_low);
    std::snprintf(b_ft, sizeof(b_ft), "%.1f ms", data.frametime_ms);
    std::snprintf(b_gload, sizeof(b_gload), "%.0f%%", data.gpu_load_pct);
    std::snprintf(b_gclk, sizeof(b_gclk), "%d MHz", data.gpu_clock_mhz);
    std::snprintf(b_gvclk, sizeof(b_gvclk), "%d MHz", data.gpu_vram_clock_mhz);
    std::snprintf(b_gvram, sizeof(b_gvram), "%.2f GB", data.gpu_vram_used_gb);
    std::snprintf(b_gbw, sizeof(b_gbw), "%.1f GB/s", data.gpu_bandwidth_gbs);
    std::snprintf(b_gtmp, sizeof(b_gtmp), "%d °C", data.gpu_temp_c);
    std::snprintf(b_gfan, sizeof(b_gfan), "%d%%", data.gpu_fan_pct);
    std::snprintf(b_gpwr, sizeof(b_gpwr), "%.1f W", data.gpu_power_w);
    std::snprintf(b_cload, sizeof(b_cload), "%.1f%%", data.cpu_load_pct);
    std::snprintf(b_cclk, sizeof(b_cclk), "%d MHz", data.cpu_clock_mhz);
    std::snprintf(b_cpwr, sizeof(b_cpwr), "%.1f W", data.cpu_power_w);
    std::snprintf(b_ctmp, sizeof(b_ctmp), "%d °C", data.cpu_temp_c);
    std::snprintf(b_ram, sizeof(b_ram), "%.2f GB (%.0f%%)", data.ram_used_gb, data.ram_used_pct);
    std::snprintf(b_api, sizeof(b_api), "%s", data.render_api);
    std::snprintf(b_gram, sizeof(b_gram), "%.2f GB", data.game_ram_gb);
    std::snprintf(b_all_cload, sizeof(b_all_cload), "%d Cores (%.0f%%)",
                  std::max(1, data.cpu_core_count), data.cpu_load_pct);
    std::snprintf(b_all_cclk, sizeof(b_all_cclk), "%d Cores (%d MHz)",
                  std::max(1, data.cpu_core_count), data.cpu_clock_mhz);
    if (data.swap_total_gb > 0.0f) {
      std::snprintf(b_swap, sizeof(b_swap), "%.2f/%.1f GB", data.swap_used_gb, data.swap_total_gb);
    } else {
      std::snprintf(b_swap, sizeof(b_swap), "OFF");
    }
    if (data.mem_psi_some_pct >= 0.0f) {
      std::snprintf(b_psi, sizeof(b_psi), "%.1f%%", data.mem_psi_some_pct);
    } else {
      std::snprintf(b_psi, sizeof(b_psi), "N/A (kernel)");
    }

    char b_bat[48] = {0};
    if (data.battery_available) {
      if (data.battery_power_w > 0.05f) {
        std::snprintf(b_bat, sizeof(b_bat), "%.0f%% (%.1fW%s)", data.battery_pct,
                      data.battery_power_w, data.battery_charging ? "+" : "");
      } else {
        std::snprintf(b_bat, sizeof(b_bat), "%.0f%%%s", data.battery_pct,
                      data.battery_charging ? " (+)" : "");
      }
    } else {
      std::snprintf(b_bat, sizeof(b_bat), "N/A (Desktop)");
    }

    char b_dsk_io[40] = {0};
    std::snprintf(b_dsk_io, sizeof(b_dsk_io), "%.1fR / %.1fW MB/s", data.disk_read_mbs,
                  data.disk_write_mbs);

    char b_dsk_tmp[32] = {0};
    if (data.disk_temp_available) {
      std::snprintf(b_dsk_tmp, sizeof(b_dsk_tmp), "%d °C", data.disk_temp_c);
    } else {
      std::snprintf(b_dsk_tmp, sizeof(b_dsk_tmp), "N/A (hwmon)");
    }

    char b_net[40] = {0};
    if (std::max(data.net_rx_mbs, data.net_tx_mbs) < 1.0f) {
      std::snprintf(b_net, sizeof(b_net), "%.0fD / %.0fU KB/s", data.net_rx_mbs * 1024.0f,
                    data.net_tx_mbs * 1024.0f);
    } else {
      std::snprintf(b_net, sizeof(b_net), "%.1fD / %.1fU MB/s", data.net_rx_mbs, data.net_tx_mbs);
    }

    // Full-width 1-column scrollable sensor list (prevents text/value collision)
    ImGui::BeginChild("##SensorsScrollList", ImVec2(0.0f, 0.0f), false);

    ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "FPS & Frame Timing");
    draw_metric_checkbox_row(cfg, MetricType::FPS, "FPS Counter", b_fps);
    draw_metric_checkbox_row(cfg, MetricType::FpsAvg, "FPS Average", b_avg);
    draw_metric_checkbox_row(cfg, MetricType::Fps1Low, "FPS 1% Low", b_1l);
    draw_metric_checkbox_row(cfg, MetricType::Fps01Low, "FPS 0.1% Low", b_01l);
    draw_metric_checkbox_row(cfg, MetricType::FrameTimes, "Frame Time", b_ft);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.46f, 0.88f, 0.0f, 1.0f), "GPU (%s)", data.gpu_name);
    draw_metric_checkbox_row(cfg, MetricType::GpuLoad, "GPU Load", b_gload);
    draw_metric_checkbox_row(cfg, MetricType::GpuClock, "GPU Clock", b_gclk);
    draw_metric_checkbox_row(cfg, MetricType::GpuVramClock, "GPU VRAM Clock", b_gvclk);
    draw_metric_checkbox_row(cfg, MetricType::GpuVram, "GPU VRAM", b_gvram);
    draw_metric_checkbox_row(cfg, MetricType::GpuBandwidth, "GPU Bandwidth", b_gbw);
    draw_metric_checkbox_row(cfg, MetricType::GpuTemp, "GPU Temp", b_gtmp);
    draw_metric_checkbox_row(cfg, MetricType::GpuFan, "GPU Fan", b_gfan);
    draw_metric_checkbox_row(cfg, MetricType::GpuWatt, "GPU Power", b_gpwr);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.0f, 0.90f, 1.0f, 1.0f), "CPU (%s)", data.cpu_name);
    draw_metric_checkbox_row(cfg, MetricType::CpuLoad, "CPU Load", b_cload);
    draw_metric_checkbox_row(cfg, MetricType::CpuAllCoreLoad, "All Core Load", b_all_cload);
    draw_metric_checkbox_row(cfg, MetricType::CpuClock, "CPU Clock", b_cclk);
    draw_metric_checkbox_row(cfg, MetricType::CpuAllCoreClock, "All Core Clock", b_all_cclk);
    draw_metric_checkbox_row(cfg, MetricType::CpuTemp, "CPU Temp", b_ctmp);
    draw_metric_checkbox_row(cfg, MetricType::CpuWatt, "CPU Power", b_cpwr);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.86f, 0.53f, 1.0f, 1.0f), "Memory (RAM)");
    draw_metric_checkbox_row(cfg, MetricType::Ram, "System RAM", b_ram);
    draw_metric_checkbox_row(cfg, MetricType::GameRam, "Game RAM", b_gram);
    draw_metric_checkbox_row(cfg, MetricType::SwapRam, "Swap / ZRAM", b_swap);
    draw_metric_checkbox_row(cfg, MetricType::MemPressure, "Mem Pressure", b_psi);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.90f, 1.0f), "System, Storage & Network");
    draw_metric_checkbox_row(cfg, MetricType::EngineApi, "Render API", b_api);
    draw_metric_checkbox_row(cfg, MetricType::GpuModel, "GPU Model", data.gpu_name);
    draw_metric_checkbox_row(cfg, MetricType::CpuModel, "CPU Model", data.cpu_name);
    draw_metric_checkbox_row(cfg, MetricType::SystemClock, "Local Clock", data.local_time_str);
    draw_metric_checkbox_row(cfg, MetricType::DiskIo, "Disk Read/Write", b_dsk_io);
    draw_metric_checkbox_row(cfg, MetricType::DiskTemp, "Disk Temp (NVMe)", b_dsk_tmp);
    draw_metric_checkbox_row(cfg, MetricType::NetSpeed, "Network Speed", b_net);
    draw_metric_checkbox_row(cfg, MetricType::Battery, "Battery & Power", b_bat,
                             data.battery_available,
                             "(Disabled - No Battery Detected / Desktop PC)");

    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, col_gap);

    // --------------------------------------------------------------------
    // COLUMN 3: Quick Actions & Capture / Recorder Settings
    // --------------------------------------------------------------------
    ImGui::BeginChild("##Col3_RecorderHub", ImVec2(col3_w, panel_height), true);

    draw_section_header("Quick Actions & Capture Hub", ImVec4(0.38f, 0.84f, 1.0f, 1.0f));

    // Action Buttons Row 1: Primary Record Trigger (Full Width)
    float btn_row_w = ImGui::GetContentRegionAvail().x;
    float btn_half_w = (btn_row_w - (6.0f * scale)) * 0.50f;

    bool is_rec = RecorderController::instance().is_recording();
    std::string rec_kb_str = format_keybind(cfg.bind_record);
    std::string bench_kb_str = format_keybind(cfg.bind_benchmark);
    std::string shot_kb = format_keybind(cfg.bind_screenshot);
    std::string rep_kb = format_keybind(cfg.bind_save_replay);

    if (!is_rec) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.58f, 0.32f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.70f, 0.38f, 1.0f));
      char btn_lbl[64];
      std::snprintf(btn_lbl, sizeof(btn_lbl), "[REC] Start Record (%s)", rec_kb_str.c_str());
      if (ImGui::Button(btn_lbl, ImVec2(btn_row_w, 30.0f * scale))) {
        RecorderController::instance().start_recording(cfg, display_width, display_height);
        set_status_message("Perekaman gameplay dimulai!");
      }
      ImGui::PopStyleColor(2);
    } else {
      float dur = RecorderController::instance().get_recording_duration();
      int m = static_cast<int>(dur) / 60;
      int s = static_cast<int>(dur) % 60;
      char stop_lbl[64];
      std::snprintf(stop_lbl, sizeof(stop_lbl), "[STOP] REC %02d:%02d (%s)", m, s,
                    rec_kb_str.c_str());
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.18f, 0.18f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.25f, 0.25f, 1.0f));
      if (ImGui::Button(stop_lbl, ImVec2(btn_row_w, 30.0f * scale))) {
        RecorderController::instance().stop_recording();
        set_status_message("Menyimpan video di latar belakang...");
      }
      ImGui::PopStyleColor(2);
    }

    // Action Buttons Row 2: Screenshot & Save Replay
    char take_shot_lbl[64];
    std::snprintf(take_shot_lbl, sizeof(take_shot_lbl), "[IMG] Take Screenshot (%s)",
                  shot_kb.c_str());
    if (ImGui::Button(take_shot_lbl, ImVec2(btn_half_w, 26.0f * scale))) {
      RecorderController::instance().request_screenshot();
      set_status_message("Screenshot dijadwalkan pada frame berikutnya!");
    }

    ImGui::SameLine(0.0f, 6.0f * scale);
    ImGui::BeginDisabled(!cfg.replay_enabled || is_rec);
    char save_rep_lbl[64];
    std::snprintf(save_rep_lbl, sizeof(save_rep_lbl), "[SAVE REPLAY] (%s)", rep_kb.c_str());
    if (ImGui::Button(save_rep_lbl, ImVec2(btn_half_w, 26.0f * scale))) {
      RecorderController::instance().trigger_save_replay();
      set_status_message("Menyimpan klip Instant Replay...");
    }
    ImGui::EndDisabled();

    // Action Buttons Row 3: Benchmark & Open Folder
    if (!data.benchmark_active) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.45f, 0.12f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.55f, 0.16f, 1.0f));
      char b_lbl[64];
      std::snprintf(b_lbl, sizeof(b_lbl), "[CSV] Start Bench (%s)", bench_kb_str.c_str());
      if (ImGui::Button(b_lbl, ImVec2(btn_half_w, 26.0f * scale))) {
        get_telemetry().start_benchmark();
        set_status_message("Benchmark dimulai...");
      }
      ImGui::PopStyleColor(2);
    } else {
      char stop_label[64];
      std::snprintf(stop_label, sizeof(stop_label), "[STOP] Bench (%.1fs)",
                    data.benchmark_elapsed_sec);
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.18f, 0.18f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.25f, 0.25f, 1.0f));
      if (ImGui::Button(stop_label, ImVec2(btn_half_w, 26.0f * scale))) {
        get_telemetry().stop_benchmark();
        set_status_message("Benchmark disimpan ke CSV!");
      }
      ImGui::PopStyleColor(2);
    }

    ImGui::SameLine(0.0f, 6.0f * scale);
    std::string cur_dir = get_effective_record_dir(cfg);
    if (ImGui::Button("[DIR] Open Save Folder", ImVec2(btn_half_w, 26.0f * scale))) {
      open_system_folder(cur_dir);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Scrollable settings area inside Column 3
    ImGui::BeginChild("##Col3ScrollSettings", ImVec2(0.0f, 0.0f), false);

    // Screenshot settings remain unlocked even during video recording
    if (ImGui::BeginTable("##Col3ScreenshotTable", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 110.0f * scale);
      ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 1.0f);

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      draw_section_header("Screenshot");
      ImGui::TableSetColumnIndex(1);
      draw_section_header("");

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Capture Mode");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      const char* shot_modes[] = {"Clean Game (Tanpa HUD)", "With HUD (Dengan HUD)"};
      int shot_mode_idx = cfg.screenshot_with_hud ? 1 : 0;
      if (ImGui::Combo("##ScreenshotMode", &shot_mode_idx, shot_modes, IM_ARRAYSIZE(shot_modes))) {
        cfg.screenshot_with_hud = (shot_mode_idx == 1);
      }

      ImGui::EndTable();
    }

    if (is_rec) {
      ImGui::TextColored(ImVec4(1.0f, 0.50f, 0.30f, 1.0f),
                         "[Pengaturan video dikunci saat perekaman berjalan]");
    }

    // Video Recorder & Instant Replay Settings (locked while recording is active)
    ImGui::BeginDisabled(is_rec);
    if (ImGui::BeginTable("##Col3RecorderTable", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 110.0f * scale);
      ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 1.0f);

      // Instant Replay Section
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      draw_section_header("Instant Replay");
      ImGui::TableSetColumnIndex(1);
      draw_section_header("");

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Replay Buffer");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      const char* replay_items[] = {"OFF (Disabled - 0 MB RAM)", "15 Seconds", "30 Seconds",
                                    "60 Seconds", "120 Seconds"};
      int replay_idx = 0;
      if (cfg.replay_enabled) {
        if (cfg.replay_duration_sec <= 15)
          replay_idx = 1;
        else if (cfg.replay_duration_sec <= 30)
          replay_idx = 2;
        else if (cfg.replay_duration_sec <= 60)
          replay_idx = 3;
        else
          replay_idx = 4;
      }
      if (ImGui::Combo("##ReplayModeCombo", &replay_idx, replay_items,
                       IM_ARRAYSIZE(replay_items))) {
        if (replay_idx == 0) {
          cfg.replay_enabled = false;
        } else {
          cfg.replay_enabled = true;
          const int durs[] = {0, 15, 30, 60, 120};
          cfg.replay_duration_sec = durs[replay_idx];
        }
      }

      // Screen Recorder (Video & Audio) Section
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      draw_section_header("Recorder Settings");
      ImGui::TableSetColumnIndex(1);
      draw_section_header("");

      // Resolution
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Resolution");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      const char* res_items[] = {"Native (Match Game)", "1440p (2560x1440 QHD)",
                                 "1080p (1920x1080 FHD)", "720p (1280x720 HD)"};
      ImGui::Combo("##RecordResolution", &cfg.record_resolution, res_items,
                   IM_ARRAYSIZE(res_items));

      // Framerate
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Framerate");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      const char* fps_items[] = {"60 FPS (Rekomendasi)", "30 FPS", "120 FPS", "Match Display"};
      int current_fps_idx = (cfg.record_fps == 30)
                                ? 1
                                : ((cfg.record_fps == 120) ? 2 : ((cfg.record_fps == 0) ? 3 : 0));
      ImGui::BeginDisabled(cfg.record_adaptive_vfr);
      if (ImGui::Combo("##RecordFps", &current_fps_idx, fps_items, IM_ARRAYSIZE(fps_items))) {
        cfg.record_fps = (current_fps_idx == 1)
                             ? 30
                             : ((current_fps_idx == 2) ? 120 : ((current_fps_idx == 3) ? 0 : 60));
      }
      ImGui::EndDisabled();
      if (cfg.record_adaptive_vfr) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.20f, 1.0f), "[VFR Mode Active: 1:1 Adaptive]");
      }

      // Video Codec
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Video Codec");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      const char* codec_items[] = {"H.264 (AVC Universal)", "HEVC (H.265 Hemat 50%)"};
      ImGui::Combo("##RecordCodec", &cfg.record_codec, codec_items, IM_ARRAYSIZE(codec_items));

      // Container
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Container");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      const char* container_items[] = {".mp4 (FastStart)", ".mkv (Matroska)", ".webm (WebM)"};
      ImGui::Combo("##RecordContainer", &cfg.record_container, container_items,
                   IM_ARRAYSIZE(container_items));

      // Rate Control
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Rate Control");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      const char* qmode_items[] = {"CQP Adaptif (AQ ON)", "Fixed Bitrate (VBR)"};
      ImGui::Combo("##RecordQMode", &cfg.record_quality_mode, qmode_items,
                   IM_ARRAYSIZE(qmode_items));

      // Quality Level / Bitrate
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      if (cfg.record_quality_mode == 0) {
        ImGui::Text("Target CQ");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("##RecordCQ", &cfg.record_cq_level, 16, 28, "CQ %d (18=Ultra, 22=Bal)");

        // Max Bitrate Cap for CQP mode to prevent bloated file sizes
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Max Bitrate");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1.0f);
        int br_mbps = std::clamp(cfg.record_bitrate_kbps / 1000, 4, 80);
        if (ImGui::SliderInt("##RecordBitrateCQP", &br_mbps, 4, 80, "%d Mbps (Cap)")) {
          cfg.record_bitrate_kbps = br_mbps * 1000;
        }
      } else {
        ImGui::Text("Target Bitrate");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1.0f);
        int br_mbps = std::clamp(cfg.record_bitrate_kbps / 1000, 4, 80);
        if (ImGui::SliderInt("##RecordBitrateVBR", &br_mbps, 4, 80, "%d Mbps")) {
          cfg.record_bitrate_kbps = br_mbps * 1000;
        }
      }

      // Audio Source
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Audio Source");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1.0f);
      const char* audio_items[] = {"Game Audio Saja", "Mikrofon Saja", "Game + Mikrofon",
                                   "Mute (Tanpa Audio)"};
      ImGui::Combo("##RecordAudio", &cfg.record_audio_source, audio_items,
                   IM_ARRAYSIZE(audio_items));

      // Save Folder Path Display
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Folder Path");
      ImGui::TableSetColumnIndex(1);
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ImVec4(0.45f, 0.88f, 0.95f, 1.0f), "%s", cur_dir.c_str());

      ImGui::EndTable();
    }
    ImGui::EndDisabled();

    if (data.last_benchmark.has_result) {
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f),
                         "Last Bench: AVG %.0f | 1%% %.0f | 0.1%% %.0f (%.1fs)",
                         data.last_benchmark.avg_fps, data.last_benchmark.fps_1pct_low,
                         data.last_benchmark.fps_01pct_low, data.last_benchmark.duration_sec);
    }

    ImGui::EndChild();
    ImGui::EndChild();

    // ====================================================================
    // 4. Bottom Action Bar (Unified Save Configuration + Custom Preset)
    // ====================================================================
    ImGui::Separator();
    if (ImGui::Button("Save Configuration", ImVec2(195.0f * scale, 28.0f * scale))) {
      bool ok = save_config_to_disk(cfg);
      if (cfg.active_preset == "custom") {
        ok = save_custom_preset_to_disk(cfg) && ok;
      }
      if (ok) {
        set_status_message(cfg.active_preset == "custom"
                               ? "Tersimpan ke config.ini & custom_preset.ini"
                               : "Tersimpan ke config.ini");
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close & Return to Game", ImVec2(210.0f * scale, 28.0f * scale))) {
      cfg.gui_open = false;
    }

    if (!g_status_msg.empty()) {
      double since_msg = std::chrono::duration<double>(now - g_status_time).count();
      if (since_msg < 4.0) {
        ImGui::SameLine(0.0f, 14.0f * scale);
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.50f, 1.0f), "%s", g_status_msg.c_str());
      }
    }
  }
  ImGui::End();

  ImGui::PopStyleColor(8);
  ImGui::PopStyleVar(7);
  if (gui_font) {
    ImGui::PopFont();
  }
}

}  // namespace fpshud
