#include "adaptive_vfr.hpp"
#include "config.hpp"
#include "imgui.h"
#include "recorder_ipc.hpp"

namespace fpshud::experimental {

bool AdaptiveVfrController::is_vfr_active(const HudConfig& cfg) noexcept {
  return cfg.record_adaptive_vfr;
}

bool AdaptiveVfrController::should_capture_frame(std::chrono::steady_clock::time_point now,
                                                 std::chrono::steady_clock::time_point last_capture,
                                                 int max_fps) noexcept {
  if (last_capture.time_since_epoch().count() == 0) {
    return true;
  }
  if (max_fps <= 0) {
    return true;  // Uncapped VFR
  }
  // Allow a 10% sub-frame jitter window (90% threshold) so 120 FPS games are not decimated by 8.0ms
  // presents
  int64_t min_interval_us = (1000000LL * 90) / (static_cast<int64_t>(max_fps) * 100);
  auto elapsed_us =
      std::chrono::duration_cast<std::chrono::microseconds>(now - last_capture).count();
  return elapsed_us >= min_interval_us;
}

void draw_experimental_menu(HudConfig& cfg) {
  bool is_rec = RecorderController::instance().is_recording();
  if (is_rec) {
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "[Terkunci saat perekaman berjalan]");
  }
  ImGui::BeginDisabled(is_rec);
  ImGui::MenuItem("Adaptive VFR Recording (Pure 1:1)", nullptr, &cfg.record_adaptive_vfr);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(
        "Merekam setiap frame yang dirender oleh game (misal 40-59 FPS)\n"
        "secara murni tanpa frame drop atau duplikasi statis 60 FPS.");
  }
  if (cfg.record_adaptive_vfr) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.20f, 1.0f), "[Experimental: Pure 1:1 VFR Active]");
    ImGui::SliderInt("Safety Max FPS", &cfg.record_vfr_max_fps, 30, 240, "%d FPS");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(
          "Batas maksimum FPS pengaman untuk menghindari overload GPU\n"
          "pada menu / loading screen 1000+ FPS (Default: 120 FPS).");
    }
  }
  ImGui::EndDisabled();
}

}  // namespace fpshud::experimental
