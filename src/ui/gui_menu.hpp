#pragma once

#include "config.hpp"
#include "telemetry.hpp"

namespace fpshud {

// Renders the interactive Dear ImGui configuration window when cfg.gui_open is true,
// and a brief startup notification hint when cfg.gui_enabled is true.
void render_gui_menu(HudConfig& cfg,
                     const TelemetryData& data,
                     float display_width,
                     float display_height);

} // namespace fpshud
