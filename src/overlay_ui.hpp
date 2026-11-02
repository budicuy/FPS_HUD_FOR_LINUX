#pragma once

#include "config.hpp"
#include "telemetry.hpp"

struct ImGuiIO;

namespace fpshud {

// Loads a high-visibility Bold TrueType font (DejaVuSansMono-Bold / AdwaitaMono-Bold)
// into Dear ImGui so the overlay text is thick, anti-aliased, and easy to read.
void setup_overlay_font(ImGuiIO& io);

// Renders the ImGui overlay window (and interactive GUI menu if enabled) for the current frame.
// Must be called between ImGui::NewFrame() and ImGui::Render().
void render_overlay_ui(HudConfig& cfg,
                       const TelemetryData& data,
                       float display_width,
                       float display_height);

} // namespace fpshud
