#pragma once

#include "config.hpp"

#include <chrono>

namespace fpshud::experimental {

class AdaptiveVfrController {
public:
  static bool is_vfr_active(const HudConfig& cfg) noexcept;

  static bool should_capture_frame(std::chrono::steady_clock::time_point now,
                                   std::chrono::steady_clock::time_point last_capture,
                                   int max_fps) noexcept;
};

void draw_experimental_menu(HudConfig& cfg);

}  // namespace fpshud::experimental
