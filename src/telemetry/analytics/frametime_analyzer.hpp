#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <vector>

namespace fpshud {

struct SimdStatsResult {
  double total_ms = 0.0;
  float min_val = 0.0f;
  float max_val = 0.0f;
};

constexpr bool has_simd_frametime_backend() noexcept {
#if __has_include(<experimental/simd>)
  return true;
#else
  return false;
#endif
}

// Accelerated frametime analytics (sum, min, max) using C++26 SIMD reduction
[[nodiscard]] SimdStatsResult compute_frametime_stats_simd(const std::vector<float>& data);

class FrametimeAnalyzer {
public:
  static constexpr size_t kFrameWindowSize = 512;
  static constexpr size_t kGraphHistorySize = 120;
  static constexpr float kRollingWindowMaxMs = 3500.0f;

  FrametimeAnalyzer() = default;

  void on_frame(float dt_ms, bool force_update = false);
  void reset();

  [[nodiscard]] float get_fps() const noexcept {
    return displayed_fps_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] float get_frametime_ms() const noexcept {
    return displayed_frametime_ms_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] float get_1pct_low() const noexcept {
    return displayed_fps_1low_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] float get_01pct_low() const noexcept {
    return displayed_fps_01low_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] float get_fps_avg() const noexcept {
    return displayed_fps_avg_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] const float* get_ft_history() const noexcept { return graph_ft_ms_.data(); }
  [[nodiscard]] const float* get_fps_history() const noexcept { return graph_fps_.data(); }
  [[nodiscard]] int get_graph_offset() const noexcept { return graph_offset_; }

private:
  std::chrono::steady_clock::time_point last_fps_display_update_{};
  float ema_frametime_ms_ = 0.0f;
  std::array<float, kFrameWindowSize> ft_ring_{};
  size_t ft_ring_pos_ = 0;
  size_t ft_ring_count_ = 0;

  std::array<float, kGraphHistorySize> graph_ft_ms_{};
  std::array<float, kGraphHistorySize> graph_fps_{};
  int graph_offset_ = 0;

  std::atomic<float> displayed_fps_{0.0f};
  std::atomic<float> displayed_fps_avg_{0.0f};
  std::atomic<float> displayed_fps_1low_{0.0f};
  std::atomic<float> displayed_fps_01low_{0.0f};
  std::atomic<float> displayed_frametime_ms_{0.0f};
};

}  // namespace fpshud
