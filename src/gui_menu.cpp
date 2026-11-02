#include "gui_menu.hpp"
#include "input_manager.hpp"
#include "overlay_ui.hpp"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fpshud {

namespace {

bool g_startup_initialized = false;
auto g_startup_time = std::chrono::steady_clock::now();
bool g_show_saved_toast = false;
auto g_toast_timestamp = std::chrono::steady_clock::now();
char g_toast_message[128] = {0};

void draw_section_header(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.78f, 1.0f, 1.0f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void trigger_toast(const char* msg) {
    std::snprintf(g_toast_message, sizeof(g_toast_message), "%s", msg);
    g_show_saved_toast = true;
    g_toast_timestamp = std::chrono::steady_clock::now();
}

} // namespace

void render_gui_menu(HudConfig& cfg,
                     const TelemetryData& data,
                     float display_width,
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

    // Startup Toast Hint (shown for 5 seconds on game launch)
    double elapsed_start = std::chrono::duration<double>(now - g_startup_time).count();
    if (elapsed_start < 5.0 && !cfg.gui_open) {
        ImGui::SetNextWindowPos(ImVec2(display_width * 0.5f, 24.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGuiWindowFlags toast_flags = ImGuiWindowFlags_NoDecoration |
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoInputs;
        if (gui_font) ImGui::PushFont(gui_font);
        if (ImGui::Begin("##FPSHUD_ToastHint", nullptr, toast_flags)) {
            ImGui::SetWindowFontScale(16.0f / base_gui_font_px);
            if (get_input_manager().has_evdev_access()) {
                ImGui::TextColored(ImVec4(0.4f, 0.92f, 1.0f, 1.0f),
                                   "FPSHUD Active  |  GUI: %s  |  Hide HUD: %s  |  Benchmark: %s",
                                   gui_keybind_name(cfg.key_toggle_gui),
                                   hud_keybind_name(cfg.key_toggle_hud),
                                   bench_keybind_name(cfg.key_benchmark));
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                                   "FPSHUD Active  |  Run with 'sudo usermod -aG input $USER' to enable hotkeys");
            }
        }
        ImGui::End();
        if (gui_font) ImGui::PopFont();
    }

    if (!cfg.gui_open) return;

    // 1280x720 base landscape resolution scaled dynamically with gui_scale (0.50x to 2.00x)
    constexpr float kBaseGuiWidth = 1280.0f;
    constexpr float kBaseGuiHeight = 720.0f;

    float max_w = std::max(display_width - 24.0f, 320.0f);
    float max_h = std::max(display_height - 24.0f, 240.0f);
    float win_w = std::min(kBaseGuiWidth * scale, max_w);
    float win_h = std::min(kBaseGuiHeight * scale, max_h);

    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(display_width * 0.5f, display_height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * scale, 6.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * scale, 12.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 14.0f * scale);

    // Deep OLED/Dark Carbon Styling
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.10f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.24f, 0.32f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.22f, 0.30f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.35f, 0.50f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.12f, 0.14f, 0.18f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.20f, 0.32f, 0.48f, 1.00f));
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
                if (ImGui::MenuItem("Save to ~/.config/fpshud/config.ini")) {
                    save_config_to_disk(cfg);
                    trigger_toast("Configuration saved to config.ini");
                }
                if (ImGui::MenuItem("Reload from Disk")) {
                    load_config_from_disk(cfg);
                    trigger_toast("Configuration reloaded from disk");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Close GUI", "[Insert] / [F12]")) {
                    cfg.gui_open = false;
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Presets")) {
                if (ImGui::MenuItem("Minimal Preset (FPS & Frametime)")) {
                    apply_preset_minimal(cfg);
                    trigger_toast("Switched to Minimal Preset");
                }
                if (ImGui::MenuItem("Standard Preset (9 Core Sensors)")) {
                    apply_preset_standard(cfg);
                    trigger_toast("Switched to Standard Preset");
                }
                if (ImGui::MenuItem("Advance Preset (All 23 Sensors)")) {
                    apply_preset_advance(cfg);
                    trigger_toast("Switched to Advance Preset");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save as Custom Preset")) {
                    save_custom_preset_to_disk(cfg);
                    trigger_toast("Saved current setup as Custom Preset");
                }
                if (ImGui::MenuItem("Load Custom Preset")) {
                    load_custom_preset_from_disk(cfg);
                    trigger_toast("Loaded Custom Preset");
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help")) {
                ImGui::MenuItem("Toggle GUI: [Insert], [F10], or [F12]", nullptr, false, false);
                ImGui::MenuItem("Toggle HUD: [F11] or [F8]", nullptr, false, false);
                ImGui::MenuItem("Trigger Benchmark: [F9]", nullptr, false, false);
                ImGui::Separator();
                ImGui::MenuItem("Zero Game Overhead | Pure C++20 Hook", nullptr, false, false);
                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }

        // ====================================================================
        // 2. Hardware Live Banner (Top Info Bar)
        // ====================================================================
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.12f, 0.16f, 0.90f));
        ImGui::BeginChild("##HwBanner", ImVec2(0.0f, 38.0f * scale), true, ImGuiWindowFlags_NoScrollbar);
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.4f, 1.0f), "LIVE TELEMETRY:");
            ImGui::SameLine(0.0f, 10.0f * scale);

            char live_txt[256];
            std::snprintf(live_txt, sizeof(live_txt),
                          "GPU: %s (%u%%, %u°C)  |  CPU: %s (%.0f%%, %u°C)  |  API: %s  |  FPS: %.0f",
                          data.gpu_model[0] ? data.gpu_model : "Generic GPU",
                          data.gpu_load_percent,
                          data.gpu_temp_c,
                          data.cpu_model[0] ? data.cpu_model : "Generic CPU",
                          data.cpu_load_percent,
                          data.cpu_temp_c,
                          (data.engine_api == GraphicsApi::Vulkan) ? "Vulkan" :
                          (data.engine_api == GraphicsApi::OpenGL) ? "OpenGL" : "Graphics API",
                          data.fps);
            ImGui::TextUnformatted(live_txt);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // ====================================================================
        // 3. Tabbed Sections (Presets, Layout, Appearance, Sensors, Benchmark)
        // ====================================================================
        if (ImGui::BeginTabBar("##FPSHUD_Tabs", ImGuiTabBarFlags_None)) {

            // ----------------------------------------------------------------
            // TAB 1: PRESETS & QUICK SETUP
            // ----------------------------------------------------------------
            if (ImGui::BeginTabItem("Presets & Quick Setup")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "Choose a preconfigured telemetry profile or load your custom setup:");
                ImGui::Spacing();

                ImVec2 btn_sz(240.0f * scale, 42.0f * scale);

                bool is_min = (cfg.active_preset == "minimal");
                if (is_min) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.78f, 1.0f));
                if (ImGui::Button("Minimal Preset\n(FPS + Frametime)", btn_sz)) {
                    apply_preset_minimal(cfg);
                    trigger_toast("Switched to Minimal Preset");
                }
                if (is_min) ImGui::PopStyleColor();

                ImGui::SameLine(0.0f, 14.0f * scale);
                bool is_std = (cfg.active_preset == "standard");
                if (is_std) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.78f, 1.0f));
                if (ImGui::Button("Standard Preset\n(9 Core Sensors)", btn_sz)) {
                    apply_preset_standard(cfg);
                    trigger_toast("Switched to Standard Preset");
                }
                if (is_std) ImGui::PopStyleColor();

                ImGui::SameLine(0.0f, 14.0f * scale);
                bool is_adv = (cfg.active_preset == "advance");
                if (is_adv) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.78f, 1.0f));
                if (ImGui::Button("Advance Preset\n(All 23 Sensors)", btn_sz)) {
                    apply_preset_advance(cfg);
                    trigger_toast("Switched to Advance Preset");
                }
                if (is_adv) ImGui::PopStyleColor();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                draw_section_header("User Custom Preset");
                ImGui::Text("Save your current selection of sensors, positions, and styling as your personal default:");
                if (ImGui::Button("Save Current Setup as Custom Preset", ImVec2(320.0f * scale, 34.0f * scale))) {
                    save_custom_preset_to_disk(cfg);
                    trigger_toast("Current setup saved to custom_preset.ini");
                }
                ImGui::SameLine(0.0f, 14.0f * scale);
                if (ImGui::Button("Load Custom Preset", ImVec2(220.0f * scale, 34.0f * scale))) {
                    if (load_custom_preset_from_disk(cfg)) {
                        trigger_toast("Custom Preset loaded successfully");
                    } else {
                        trigger_toast("No custom preset found on disk");
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                draw_section_header("Quick Configuration Controls");
                if (ImGui::Button("Save to ~/.config/fpshud/config.ini", ImVec2(320.0f * scale, 34.0f * scale))) {
                    save_config_to_disk(cfg);
                    trigger_toast("Configuration saved to config.ini");
                }
                ImGui::SameLine(0.0f, 14.0f * scale);
                if (ImGui::Button("Reset All to Factory Defaults", ImVec2(260.0f * scale, 34.0f * scale))) {
                    cfg = HudConfig();
                    apply_preset_standard(cfg);
                    cfg.enabled = true;
                    cfg.gui_open = true;
                    trigger_toast("Reset to Factory Defaults");
                }

                ImGui::EndTabItem();
            }

            // ----------------------------------------------------------------
            // TAB 2: POSITION & ARRANGEMENT
            // ----------------------------------------------------------------
            if (ImGui::BeginTabItem("Position & Layout")) {
                ImGui::Spacing();

                if (ImGui::BeginTable("##LayoutTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 180.0f * scale);
                    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch);

                    // Row 1: Screen Position
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Screen Position");
                    ImGui::TableSetColumnIndex(1);
                    const char* positions[] = {
                        "Top-Left (Default)",
                        "Top-Right",
                        "Center-Top",
                        "Bottom-Left",
                        "Bottom-Right",
                        "Center-Bottom"
                    };
                    int current_pos = static_cast<int>(cfg.layout);
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::Combo("##ScreenPos", &current_pos, positions, IM_ARRAYSIZE(positions))) {
                        cfg.layout = static_cast<LayoutPosition>(current_pos);
                    }

                    // Row 2: Arrangement Mode
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Arrangement Mode");
                    ImGui::TableSetColumnIndex(1);
                    const char* arrangements[] = {
                        "Vertical Stack (Grouped Categories)",
                        "Double Column (Compact 2-Column Grid)",
                        "Single Line (Horizontal Top/Bottom Bar)"
                    };
                    int current_arr = static_cast<int>(cfg.arrangement);
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::Combo("##Arrangement", &current_arr, arrangements, IM_ARRAYSIZE(arrangements))) {
                        cfg.arrangement = static_cast<ArrangementMode>(current_arr);
                    }

                    // Row 3: Screen Margins
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Horizontal Margin (X)");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::SliderFloat("##MarginX", &cfg.margin_x, 0.0f, 300.0f, "%.0f px");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Vertical Margin (Y)");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::SliderFloat("##MarginY", &cfg.margin_y, 0.0f, 300.0f, "%.0f px");

                    ImGui::EndTable();
                }

                ImGui::EndTabItem();
            }

            // ----------------------------------------------------------------
            // TAB 3: APPEARANCE & FONT
            // ----------------------------------------------------------------
            if (ImGui::BeginTabItem("Appearance & Font")) {
                ImGui::Spacing();

                if (ImGui::BeginTable("##AppearanceTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 180.0f * scale);
                    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch);

                    // Section 1: Visibility & Graph
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    draw_section_header("Display & Graphs");
                    ImGui::TableSetColumnIndex(1);
                    draw_section_header("");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("HUD Display");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Checkbox("Show HUD", &cfg.hud_visible);
                    ImGui::SameLine(0.0f, 14.0f * scale);
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

                    const char* scale_options[] = {
                        "0.50x (50% - Compact)",
                        "0.75x (75% - Small)",
                        "1.00x (100% - Default)",
                        "1.25x (125% - Medium)",
                        "1.50x (150% - Large)",
                        "1.75x (175% - Extra Large)",
                        "2.00x (200% - Ultra / 4K)"
                    };
                    const float scale_values[] = { 0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 1.75f, 2.00f };

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
                        std::snprintf(combo_preview, sizeof(combo_preview), "Custom (%.0f%%)", cfg.gui_scale * 100.0f);
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
                    const char* font_names[] = {
                        "Sans-Serif Bold (OpenSans / Clean NVIDIA Style)",
                        "Monospace Bold (DejaVuSansMono-Bold)",
                        "Bitstream Vera Sans Mono Bold",
                        "GNOME Adwaita Mono Bold"
                    };
                    int cur_font = static_cast<int>(cfg.font_family);
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::Combo("##FontFamily", &cur_font, font_names, IM_ARRAYSIZE(font_names))) {
                        cfg.font_family = static_cast<FontFamily>(cur_font);
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
                    ImGui::Text("Text Opacity");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::SliderFloat("##TextOpacity", &cfg.text_opacity, 0.20f, 1.0f, "%.2f");

                    // Section 3: Colors & Warnings
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    draw_section_header("Colors & Warnings");
                    ImGui::TableSetColumnIndex(1);
                    draw_section_header("");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Smart Category Colors");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Checkbox("Enable Smart Group Coloring (Green GPU, Blue CPU, Orange RAM)", &cfg.category_colors_enabled);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Thermal Warnings");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Checkbox("Highlight Hot Components (>82°C Orange, >90°C Flashing Red)", &cfg.thermal_warnings_enabled);

                    // Section 4: Outline & Drop Shadow
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    draw_section_header("Outline & Drop Shadow");
                    ImGui::TableSetColumnIndex(1);
                    draw_section_header("");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Text Outline");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Checkbox("Enable Outline", &cfg.outline_enabled);
                    if (cfg.outline_enabled) {
                        ImGui::SameLine(0.0f, 14.0f * scale);
                        ImGui::SetNextItemWidth(140.0f * scale);
                        ImGui::SliderFloat("Thickness##OutThick", &cfg.outline_thickness, 0.5f, 3.5f, "%.1f px");
                    }

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Drop Shadow");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Checkbox("Enable Shadow", &cfg.shadow_enabled);
                    if (cfg.shadow_enabled) {
                        ImGui::SameLine(0.0f, 14.0f * scale);
                        ImGui::SetNextItemWidth(120.0f * scale);
                        ImGui::SliderFloat("Offset##ShOff", &cfg.shadow_offset, 1.0f, 6.0f, "%.1f px");
                        ImGui::SameLine(0.0f, 10.0f * scale);
                        ImGui::SetNextItemWidth(120.0f * scale);
                        ImGui::SliderFloat("Opacity##ShOp", &cfg.shadow_opacity, 0.1f, 1.0f, "%.2f");
                    }

                    // Section 5: Box Background
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    draw_section_header("Background Box");
                    ImGui::TableSetColumnIndex(1);
                    draw_section_header("");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Background Style");
                    ImGui::TableSetColumnIndex(1);
                    const char* bg_styles[] = {
                        "Transparent (No Box Background)",
                        "Translucent (Semi-Transparent Box)",
                        "Solid Dark Box"
                    };
                    int cur_bg = static_cast<int>(cfg.background);
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::Combo("##BgStyle", &cur_bg, bg_styles, IM_ARRAYSIZE(bg_styles))) {
                        cfg.background = static_cast<BackgroundStyle>(cur_bg);
                    }

                    if (cfg.background != BackgroundStyle::None) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("Background Opacity");
                        ImGui::TableSetColumnIndex(1);
                        ImGui::SetNextItemWidth(-1.0f);
                        ImGui::SliderFloat("##BgAlpha", &cfg.bg_alpha, 0.0f, 1.0f, "%.2f");
                    }

                    ImGui::EndTable();
                }

                ImGui::EndTabItem();
            }

            // ----------------------------------------------------------------
            // TAB 4: METRICS & SENSORS (CHECKBOX MATRIX)
            // ----------------------------------------------------------------
            if (ImGui::BeginTabItem("Telemetry Sensors (23)")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "Toggle individual hardware sensors on the overlay:");
                ImGui::Spacing();

                // Select All / Deselect All quick buttons
                if (ImGui::Button("Select All (23 Sensors)", ImVec2(220.0f * scale, 30.0f * scale))) {
                    cfg.metrics.clear();
                    for (size_t i = 0; i < kTotalMetricCount; ++i) {
                        cfg.metrics.push_back(static_cast<MetricType>(i));
                    }
                    cfg.active_preset = "custom";
                }
                ImGui::SameLine(0.0f, 10.0f * scale);
                if (ImGui::Button("Clear All Sensors", ImVec2(180.0f * scale, 30.0f * scale))) {
                    cfg.metrics.clear();
                    cfg.active_preset = "custom";
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                auto draw_sensor_check = [&](MetricType m, const char* label) {
                    bool active = cfg.has_metric(m);
                    if (ImGui::Checkbox(label, &active)) {
                        cfg.set_metric(m, active);
                        cfg.active_preset = "custom";
                    }
                };

                if (ImGui::BeginTable("##SensorsMatrix", 3, ImGuiTableFlags_SizingStretchSame)) {
                    ImGui::TableNextColumn();
                    draw_section_header("Framerate & Timing");
                    draw_sensor_check(MetricType::FPS, "FPS (Current)");
                    draw_sensor_check(MetricType::FpsAvg, "FPS Average");
                    draw_sensor_check(MetricType::Fps1Low, "1% Low FPS");
                    draw_sensor_check(MetricType::Fps01Low, "0.1% Low FPS");
                    draw_sensor_check(MetricType::FrameTimes, "Frametime (ms)");

                    ImGui::Spacing();
                    draw_section_header("System & Engine");
                    draw_sensor_check(MetricType::EngineApi, "Graphics API (Vulkan/GL)");
                    draw_sensor_check(MetricType::SystemClock, "System Clock (HH:MM)");

                    ImGui::TableNextColumn();
                    draw_section_header("GPU Telemetry");
                    draw_sensor_check(MetricType::GpuLoad, "GPU Core Utilization (%)");
                    draw_sensor_check(MetricType::GpuClock, "GPU Core Frequency (MHz)");
                    draw_sensor_check(MetricType::GpuVram, "VRAM Usage (GB)");
                    draw_sensor_check(MetricType::GpuBandwidth, "PCIe / VRAM Bandwidth");
                    draw_sensor_check(MetricType::GpuTemp, "GPU Core Temperature (°C)");
                    draw_sensor_check(MetricType::GpuFan, "GPU Fan Speed (%)");
                    draw_sensor_check(MetricType::GpuWatt, "GPU Power Consumption (W)");

                    ImGui::TableNextColumn();
                    draw_section_header("CPU & RAM Telemetry");
                    draw_sensor_check(MetricType::CpuLoad, "CPU Total Utilization (%)");
                    draw_sensor_check(MetricType::CpuClock, "CPU Core Frequency (MHz)");
                    draw_sensor_check(MetricType::CpuWatt, "CPU Package Power (W)");
                    draw_sensor_check(MetricType::CpuTemp, "CPU Core Temperature (°C)");
                    draw_sensor_check(MetricType::Ram, "System RAM Usage (GB)");
                    draw_sensor_check(MetricType::GameRam, "Game Process RAM (GB)");
                    draw_sensor_check(MetricType::SwapRam, "Swap / ZRAM Usage (GB)");
                    draw_sensor_check(MetricType::MemPressure, "Memory PSI Pressure (%)");
                    draw_sensor_check(MetricType::RamSpeed, "RAM Speed (MT/s) & Gen");

                    ImGui::EndTable();
                }

                ImGui::EndTabItem();
            }

            // ----------------------------------------------------------------
            // TAB 5: KEYBINDINGS & BENCHMARK
            // ----------------------------------------------------------------
            if (ImGui::BeginTabItem("Keybindings & Benchmark")) {
                ImGui::Spacing();

                draw_section_header("Global Game Overlay Hotkeys (Linux evdev / Wayland compatible)");
                ImGui::Spacing();

                if (ImGui::BeginTable("##KeybindsTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 220.0f * scale);
                    ImGui::TableSetupColumn("Key Assignment", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Toggle Configuration GUI");
                    ImGui::TableSetColumnIndex(1);
                    const char* gui_keys[] = {
                        "[Insert] or [F12] (Default)",
                        "[Insert] Only",
                        "[F10] Key",
                        "[Home] Key",
                        "[F12] Only"
                    };
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::Combo("##GuiKey", &cfg.key_toggle_gui, gui_keys, IM_ARRAYSIZE(gui_keys));

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Show / Hide HUD Overlay");
                    ImGui::TableSetColumnIndex(1);
                    const char* hud_keys[] = {
                        "[F11] Key (Default)",
                        "[F8] Key",
                        "[End] Key"
                    };
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::Combo("##HudKey", &cfg.key_toggle_hud, hud_keys, IM_ARRAYSIZE(hud_keys));

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Start / Stop Benchmark");
                    ImGui::TableSetColumnIndex(1);
                    const char* bench_keys[] = {
                        "[F9] Key (Default)",
                        "[F7] Key",
                        "[PageUp] Key"
                    };
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::Combo("##BenchKey", &cfg.key_benchmark, bench_keys, IM_ARRAYSIZE(bench_keys));

                    ImGui::EndTable();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                draw_section_header("Real-Time Benchmark & CSV Performance Logging");
                ImGui::Text("Capture precise per-frame frametimes and sensor telemetry to CSV:");

                bool is_benchmarking = data.benchmark_active;
                if (!is_benchmarking) {
                    if (ImGui::Button("Start Benchmark Recording [F9]", ImVec2(300.0f * scale, 36.0f * scale))) {
                        get_telemetry_collector().start_benchmark();
                        trigger_toast("Benchmark recording started!");
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
                    char rec_lbl[64];
                    std::snprintf(rec_lbl, sizeof(rec_lbl), "Stop Benchmark (Elapsed: %.1fs)", data.benchmark_elapsed_sec);
                    if (ImGui::Button(rec_lbl, ImVec2(340.0f * scale, 36.0f * scale))) {
                        get_telemetry_collector().stop_benchmark();
                        trigger_toast("Benchmark stopped and saved to CSV!");
                    }
                    ImGui::PopStyleColor();
                }

                if (data.last_benchmark.has_result) {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "Latest Benchmark Summary:");
                    ImGui::BulletText("Duration   : %.2f seconds", data.last_benchmark.duration_sec);
                    ImGui::BulletText("Average FPS: %.1f", data.last_benchmark.avg_fps);
                    ImGui::BulletText("1%% Low FPS : %.1f", data.last_benchmark.fps_1pct_low);
                    ImGui::BulletText("0.1%% Low   : %.1f", data.last_benchmark.fps_01pct_low);
                    ImGui::BulletText("Peak Temp  : GPU %u°C  |  CPU %u°C", data.last_benchmark.peak_gpu_temp, data.last_benchmark.peak_cpu_temp);
                    ImGui::BulletText("Peak Power : GPU %.1f W  |  CPU %.1f W", data.last_benchmark.peak_gpu_watt, data.last_benchmark.peak_cpu_watt);
                    ImGui::BulletText("CSV Report : %s", data.last_benchmark.csv_path);
                }

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        // ====================================================================
        // 4. Status Bar / Toast at Bottom of Menu
        // ====================================================================
        if (g_show_saved_toast) {
            double toast_elapsed = std::chrono::duration<double>(now - g_toast_timestamp).count();
            if (toast_elapsed < 3.0) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.2f, 0.95f, 0.4f, 1.0f), "✓ %s", g_toast_message);
            } else {
                g_show_saved_toast = false;
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

} // namespace fpshud
