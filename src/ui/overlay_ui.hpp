#pragma once

#include "config.hpp"
#include "telemetry.hpp"

struct ImGuiIO;
struct ImFont;

namespace fpshud {

// Loads high-visibility TrueType fonts (multi-tier crisp glyphs for HUD and GUI)
void setup_overlay_font(ImGuiIO& io);

// Multi-tier crisp font getters for GUI and HUD (prevents bilinear blur across all scales)
ImFont* get_gui_font(float gui_scale, float& out_base_size);
ImFont* get_hud_font(FontFamily family, float font_size_px, float& out_base_size);

// Renders the ImGui overlay window (and interactive GUI menu if enabled) for the current frame.
// Must be called between ImGui::NewFrame() and ImGui::Render().
void render_overlay_ui(HudConfig& cfg, const TelemetryData& data, float display_width,
                       float display_height);

}  // namespace fpshud
