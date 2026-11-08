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

// Computes the screen-clamped top-left coordinate (out_x, out_y) for Custom HUD placement
// so the overlay window never overflows [0, display_w - hud_w] x [0, display_h - hud_h].
void compute_clamped_hud_pos(const HudConfig& cfg, float display_w, float display_h, float hud_w,
                             float hud_h, float& out_x, float& out_y) noexcept;

// Renders the ImGui overlay window (and interactive GUI menu if enabled) for the current frame.
// Must be called between ImGui::NewFrame() and ImGui::Render().
void render_overlay_ui(HudConfig& cfg, const TelemetryData& data, float display_width,
                       float display_height);

}  // namespace fpshud
