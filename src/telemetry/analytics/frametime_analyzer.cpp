#include "analytics/frametime_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#if __has_include(<experimental/simd>)
#include <experimental/simd>
namespace stdx = std::experimental;
#define FPSHUD_HAS_SIMD 1
#endif

namespace fpshud {

namespace {

#if defined(FPSHUD_HAS_SIMD)
template <size_t Width>
SimdStatsResult reduce_frametime_simd_impl(const float* data, size_t n) noexcept {
  using Abi = stdx::simd_abi::deduce_t<float, Width>;
  using V = stdx::simd<float, Abi>;
  constexpr size_t VSize = V::size();

  const V kMaxFinite(std::numeric_limits<float>::max());
  V sum_v(0.0f);
  V min_v(std::numeric_limits<float>::max());
  V max_v(std::numeric_limits<float>::lowest());

  double total = 0.0;
  float scalar_min = std::numeric_limits<float>::max();
  float scalar_max = std::numeric_limits<float>::lowest();
  bool any_valid = false;
  size_t chunks_accumulated = 0;
  size_t i = 0;

  for (; i + VSize <= n; i += VSize) {
    V chunk(&data[i], stdx::element_aligned);
    auto finite_mask = stdx::abs(chunk) <= kMaxFinite;
    if (stdx::all_of(finite_mask)) {
      any_valid = true;
      sum_v += chunk;
      min_v = stdx::min(min_v, chunk);
      max_v = stdx::max(max_v, chunk);

      // Periodically fold into double total to eliminate floating-point drift in ultra-long runs
      if (++chunks_accumulated == 512) {
        for (size_t lane = 0; lane < VSize; ++lane) {
          total += static_cast<double>(sum_v[lane]);
        }
        sum_v = V(0.0f);
        chunks_accumulated = 0;
      }
    } else {
      for (size_t lane = 0; lane < VSize; ++lane) {
        float v = chunk[lane];
        if (std::isfinite(v)) {
          any_valid = true;
          total += static_cast<double>(v);
          if (v < scalar_min) scalar_min = v;
          if (v > scalar_max) scalar_max = v;
        }
      }
    }
  }

  for (size_t lane = 0; lane < VSize; ++lane) {
    total += static_cast<double>(sum_v[lane]);
    if (min_v[lane] < scalar_min) scalar_min = min_v[lane];
    if (max_v[lane] > scalar_max) scalar_max = max_v[lane];
  }

  for (; i < n; ++i) {
    float v = data[i];
    if (std::isfinite(v)) {
      any_valid = true;
      total += static_cast<double>(v);
      if (v < scalar_min) scalar_min = v;
      if (v > scalar_max) scalar_max = v;
    }
  }

  if (!any_valid) {
    return SimdStatsResult{0.0, 0.0f, 0.0f};
  }
  return SimdStatsResult{total, scalar_min, scalar_max};
}

#if defined(__x86_64__) && defined(__clang__)
[[gnu::target("avx2")]]
SimdStatsResult reduce_frametime_avx2(const float* data, size_t n) noexcept {
  return reduce_frametime_simd_impl<8>(data, n);
}

SimdStatsResult reduce_frametime_sse2(const float* data, size_t n) noexcept {
  return reduce_frametime_simd_impl<4>(data, n);
}
#endif
#endif

}  // namespace

SimdStatsResult compute_frametime_stats_simd(const std::vector<float>& data) {
  if (data.empty()) return SimdStatsResult{};

#if defined(FPSHUD_HAS_SIMD)
#if defined(__x86_64__) && defined(__clang__)
  static const bool s_has_avx2 = __builtin_cpu_supports("avx2");
  if (s_has_avx2) {
    return reduce_frametime_avx2(data.data(), data.size());
  }
  return reduce_frametime_sse2(data.data(), data.size());
#else
  return reduce_frametime_simd_impl<stdx::native_simd<float>::size()>(data.data(), data.size());
#endif
#else
  double total = 0.0;
  float min_val = std::numeric_limits<float>::max();
  float max_val = std::numeric_limits<float>::lowest();
  bool any_valid = false;
  for (float v : data) {
    if (std::isfinite(v)) {
      any_valid = true;
      total += static_cast<double>(v);
      if (v < min_val) min_val = v;
      if (v > max_val) max_val = v;
    }
  }
  if (!any_valid) return SimdStatsResult{0.0, 0.0f, 0.0f};
  return SimdStatsResult{total, min_val, max_val};
#endif
}

void FrametimeAnalyzer::reset() {
  ft_ring_pos_ = 0;
  ft_ring_count_ = 0;
  ema_frametime_ms_ = 0.0f;
  graph_offset_ = 0;
  ft_ring_.fill(0.0f);
  graph_ft_ms_.fill(0.0f);
  graph_fps_.fill(0.0f);
  last_fps_display_update_ = {};
  displayed_fps_.store(0.0f, std::memory_order_relaxed);
  displayed_fps_avg_.store(0.0f, std::memory_order_relaxed);
  displayed_fps_1low_.store(0.0f, std::memory_order_relaxed);
  displayed_fps_01low_.store(0.0f, std::memory_order_relaxed);
  displayed_frametime_ms_.store(0.0f, std::memory_order_relaxed);
}

void FrametimeAnalyzer::on_frame(float dt_ms, bool force_update) {
  if (std::isnan(dt_ms) || std::isinf(dt_ms) || dt_ms <= 0.001f || dt_ms > 5000.0f) {
    return;
  }

  // Scene / Loading stall reset: if a single frame took > 250ms (< 4 FPS), reset rolling buffer
  if (dt_ms > 250.0f) {
    ft_ring_pos_ = 0;
    ft_ring_count_ = 0;
    ema_frametime_ms_ = 0.0f;
    return;
  }

  if (ema_frametime_ms_ <= 0.0f) {
    ema_frametime_ms_ = dt_ms;
  } else {
    ema_frametime_ms_ = ema_frametime_ms_ * 0.90f + dt_ms * 0.10f;
  }

  ft_ring_[ft_ring_pos_] = dt_ms;
  ft_ring_pos_ = (ft_ring_pos_ + 1) % kFrameWindowSize;
  if (ft_ring_count_ < kFrameWindowSize) {
    ++ft_ring_count_;
  }

  // Update 120-frame graph ring buffer (render thread only, zero lock!)
  graph_ft_ms_[graph_offset_] = ema_frametime_ms_;
  graph_fps_[graph_offset_] = (ema_frametime_ms_ > 0.001f) ? (1000.0f / ema_frametime_ms_) : 0.0f;
  graph_offset_ = (graph_offset_ + 1) % static_cast<int>(kGraphHistorySize);

  auto now = std::chrono::steady_clock::now();
  auto since_update_ms =
      std::chrono::duration<float, std::milli>(now - last_fps_display_update_).count();
  if (since_update_ms >= 200.0f || displayed_fps_.load(std::memory_order_relaxed) == 0.0f ||
      force_update) {
    last_fps_display_update_ = now;
    displayed_frametime_ms_.store(ema_frametime_ms_, std::memory_order_relaxed);
    displayed_fps_.store((ema_frametime_ms_ > 0.001f) ? (1000.0f / ema_frametime_ms_) : 0.0f,
                         std::memory_order_relaxed);

    if (ft_ring_count_ > 0) {
      std::array<float, kFrameWindowSize> recent_ft{};
      size_t recent_count = 0;
      double total_window_ms = 0.0;

      for (size_t step = 0; step < ft_ring_count_; ++step) {
        size_t idx = (ft_ring_pos_ + kFrameWindowSize - 1 - step) % kFrameWindowSize;
        float v = ft_ring_[idx];
        recent_ft[recent_count++] = v;
        total_window_ms += static_cast<double>(v);
        if (total_window_ms >= static_cast<double>(kRollingWindowMaxMs) && recent_count >= 30) {
          break;
        }
      }

      if (total_window_ms > 0.001) {
        float avg_fps =
            static_cast<float>((static_cast<double>(recent_count) * 1000.0) / total_window_ms);
        displayed_fps_avg_.store(avg_fps, std::memory_order_relaxed);
      }

      std::sort(recent_ft.begin(), recent_ft.begin() + recent_count, std::greater<float>());

      auto calc_time_weighted_low_fps = [&](double tail_ratio) -> float {
        double target_tail_ms = total_window_ms * tail_ratio;
        double accum_ms = 0.0;
        double weighted_ft_sum = 0.0;

        for (size_t i = 0; i < recent_count; ++i) {
          double ft = static_cast<double>(recent_ft[i]);
          if (accum_ms + ft >= target_tail_ms) {
            double rem = target_tail_ms - accum_ms;
            weighted_ft_sum += rem * ft;
            accum_ms = target_tail_ms;
            break;
          }
          weighted_ft_sum += ft * ft;
          accum_ms += ft;
        }

        if (accum_ms <= 0.0001)
          return (recent_count > 0 && recent_ft[0] > 0.0001f) ? (1000.0f / recent_ft[0]) : 0.0f;
        double effective_tail_ft_ms = weighted_ft_sum / accum_ms;
        return (effective_tail_ft_ms > 0.0001) ? static_cast<float>(1000.0 / effective_tail_ft_ms)
                                               : 0.0f;
      };

      float fps_1low = calc_time_weighted_low_fps(0.01);

      float fps_01low = fps_1low;
      if (recent_count >= 4) {
        float worst_0 = recent_ft[0];
        float worst_1 = recent_ft[1];
        float blended_01_ms = worst_0 * 0.65f + worst_1 * 0.35f;
        float tw_01 = calc_time_weighted_low_fps(0.002);
        float blended_fps = (blended_01_ms > 0.0001f) ? (1000.0f / blended_01_ms) : 0.0f;
        fps_01low = std::min(fps_1low, blended_fps * 0.5f + tw_01 * 0.5f);
      } else {
        fps_01low = calc_time_weighted_low_fps(0.001);
      }

      displayed_fps_1low_.store(fps_1low, std::memory_order_relaxed);
      displayed_fps_01low_.store(fps_01low, std::memory_order_relaxed);
    }
  }
}

}  // namespace fpshud
