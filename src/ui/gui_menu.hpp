#pragma once

#include "config.hpp"
#include "imgui.h"
#include "telemetry.hpp"

namespace fpshud {

// Renders the interactive Dear ImGui configuration window when cfg.gui_open is true,
// and a brief startup notification hint when cfg.gui_enabled is true.
void render_gui_menu(HudConfig& cfg, const TelemetryData& data, float display_width,
                     float display_height);

// In-Game Video Texture Management (implemented across Vulkan & OpenGL layers)
using TextureProviderFn = ImTextureID (*)(const uint8_t*, uint32_t, uint32_t, uint64_t);
using TextureReleaseFn = void (*)();

void register_player_texture_backend(TextureProviderFn provider, TextureReleaseFn releaser);
ImTextureID get_or_update_player_texture(const uint8_t* rgba, uint32_t w, uint32_t h, uint64_t seq);
void release_player_texture();

struct RecordedClipInfo {
  std::string filename;
  std::string filepath;
  std::string date_str;
  std::string size_str;
  uint64_t file_size_bytes{0};
  uint64_t mtime_epoch{0};
};

std::vector<RecordedClipInfo> scan_recorded_clips(const std::string& dir_path);

}  // namespace fpshud
