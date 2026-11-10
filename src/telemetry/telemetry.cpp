#include "telemetry.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>

#if __has_include(<experimental/simd>)
#include <experimental/simd>
namespace stdx = std::experimental;
#define FPSHUD_HAS_SIMD 1
#endif

namespace fs = std::filesystem;

namespace fpshud {

namespace {

std::string get_benchmark_csv_path() {
  if (const char* home = std::getenv("HOME"); home && *home) {
    std::error_code ec;
    fs::path docs_en = fs::path(home) / "Documents";
    if (fs::exists(docs_en, ec)) {
      return (docs_en / "fpshud_benchmark.csv").string();
    }
    fs::path docs_id = fs::path(home) / "Dokumen";
    if (fs::exists(docs_id, ec)) {
      return (docs_id / "fpshud_benchmark.csv").string();
    }
    return (fs::path(home) / "fpshud_benchmark.csv").string();
  }
  return "/tmp/fpshud_benchmark.csv";
}

}  // namespace

TelemetryCollector::TelemetryCollector() : cpu_provider_(""), ram_provider_("") {
  init_sensors();
  poll_once_sync();
  running_.store(true, std::memory_order_relaxed);
  worker_thread_ = std::thread(&TelemetryCollector::worker_loop, this);
}

TelemetryCollector::TelemetryCollector(std::string mock_root_prefix, bool disable_nvml,
                                       bool disable_level_zero)
    : root_prefix_(std::move(mock_root_prefix)),
      disable_nvml_(disable_nvml),
      disable_level_zero_(disable_level_zero),
      cpu_provider_(root_prefix_),
      ram_provider_(root_prefix_) {
  init_sensors();
  poll_once_sync();
}

void TelemetryCollector::inject_mock_level_zero(const LevelZeroDispatch& dispatch) noexcept {
  mock_ze_dispatch_ = dispatch;
  has_mock_ze_ = true;
  std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
  shutdown_sensors_locked();
  init_gpu_sensors_locked();
}

TelemetryCollector::~TelemetryCollector() {
  running_.store(false, std::memory_order_relaxed);
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  shutdown_sensors();
}

void TelemetryCollector::init_sensors() {
  init_cpu_sensors();
  init_gpu_sensors();
  init_battery_sensors();
  init_storage_sensors();

  // Prime initial counters for delta metrics
  poll_cpu_load();
  poll_cpu_clock();
  poll_cpu_power();
  poll_battery();
  poll_disk_temp();
  poll_local_clock();
}

void TelemetryCollector::init_cpu_sensors() {
  cpu_provider_.init();
  ram_provider_.init();
  cpu_provider_.poll(staging_hw_data_);
}

void TelemetryCollector::init_gpu_sensors() {
  std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
  init_gpu_sensors_locked();
}

void TelemetryCollector::init_gpu_sensors_locked() {
  shutdown_sensors_locked();

  // 1. Try NVIDIA NVML first unless disabled
  if (!disable_nvml_) {
    if (nvidia_provider_.init(root_prefix_, bound_vendor_id_, bound_device_id_)) {
      gpu_backend_ = GpuBackendType::NvidiaNvml;
      return;
    }
  }

  // 2. Try Universal Linux DRM sysfs
  bool drm_ok = drm_provider_.init(root_prefix_, bound_vendor_id_, bound_device_id_);
  if (drm_ok) {
    gpu_backend_ = drm_provider_.get_backend_type();

    // Upgrade Intel GPU to Level Zero Sysman if available
    if (drm_provider_.detected_vendor_id() == 0x8086 && !disable_level_zero_) {
      const LevelZeroDispatch* mock_ptr = has_mock_ze_ ? &mock_ze_dispatch_ : nullptr;
      if (levelzero_provider_.init(root_prefix_, mock_ptr)) {
        gpu_backend_ = GpuBackendType::IntelLevelZero;
      }
    }
    return;
  }

  // Fallback if DRM not detected but Level Zero is injected/available
  if (!disable_level_zero_ && has_mock_ze_) {
    if (levelzero_provider_.init(root_prefix_, &mock_ze_dispatch_)) {
      gpu_backend_ = GpuBackendType::IntelLevelZero;
      return;
    }
  }

  gpu_backend_ = GpuBackendType::None;
}

void TelemetryCollector::shutdown_sensors() {
  std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
  shutdown_sensors_locked();
}

void TelemetryCollector::shutdown_sensors_locked() {
  nvidia_provider_.shutdown();
  drm_provider_.shutdown();
  levelzero_provider_.shutdown();
  gpu_backend_ = GpuBackendType::None;
}

void TelemetryCollector::on_frame() {
  auto now = std::chrono::steady_clock::now();
  if (last_frame_time_.time_since_epoch().count() == 0) {
    last_frame_time_ = now;
    last_fps_display_update_ = now;
    return;
  }

  float dt_ms = std::chrono::duration<float, std::milli>(now - last_frame_time_).count();
  last_frame_time_ = now;
  on_simulated_frame_dt(dt_ms, false);
}

void TelemetryCollector::on_simulated_frame_dt(float dt_ms, bool force_display_update) {
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

  // Record frame into Benchmark buffer only when benchmark is active
  if (bench_active_.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> block(bench_mutex_);
    if (bench_frametimes_ms_.size() < 360000) {
      bench_frametimes_ms_.push_back(dt_ms);
    }
  }

  auto now = std::chrono::steady_clock::now();

  // Automated Benchmark Controller (via FPSHUD_BENCHMARK=seconds environment variable)
  static const int s_auto_bench_secs = []() {
    if (const char* env = std::getenv("FPSHUD_BENCHMARK")) {
      return std::max(1, std::atoi(env));
    }
    return 0;
  }();

  if (s_auto_bench_secs > 0) {
    static auto s_first_frame = now;
    static bool s_bench_started = false;
    static bool s_bench_finished = false;

    if (!s_bench_started && !s_bench_finished) {
      double warmup = std::chrono::duration<double>(now - s_first_frame).count();
      if (warmup >= 1.0) {
        start_benchmark();
        s_bench_started = true;
        std::fprintf(stderr, "[FPSHUD] Automated Benchmark started (duration: %d seconds)...\n",
                     s_auto_bench_secs);
      }
    } else if (s_bench_started && !s_bench_finished) {
      double elapsed = std::chrono::duration<double>(now - bench_start_time_).count();
      if (elapsed >= static_cast<double>(s_auto_bench_secs)) {
        stop_benchmark();
        s_bench_finished = true;
        std::fprintf(stderr, "[FPSHUD] Automated Benchmark complete! Results saved to CSV.\n");
        if (const char* exit_env = std::getenv("FPSHUD_BENCHMARK_EXIT");
            exit_env && std::atoi(exit_env) != 0) {
          std::exit(0);
        }
      }
    }
  }
  auto since_update_ms =
      std::chrono::duration<float, std::milli>(now - last_fps_display_update_).count();
  if (since_update_ms >= 200.0f || displayed_fps_.load(std::memory_order_relaxed) == 0.0f ||
      force_display_update) {
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

void TelemetryCollector::start_benchmark() {
  std::lock_guard<std::mutex> block(bench_mutex_);
  bench_frametimes_ms_.clear();
  bench_frametimes_ms_.reserve(16384);
  bench_max_gpu_temp_c_ = 0;
  bench_max_gpu_power_w_ = 0.0f;
  bench_max_cpu_temp_c_ = 0;
  bench_max_cpu_power_w_ = 0.0f;
  bench_start_time_ = std::chrono::steady_clock::now();
  bench_active_.store(true, std::memory_order_relaxed);
}

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

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
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
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
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

void TelemetryCollector::stop_benchmark() {
  if (!bench_active_.exchange(false, std::memory_order_relaxed)) {
    return;
  }

  auto end_time = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> block(bench_mutex_);

  if (bench_frametimes_ms_.empty()) {
    return;
  }

  BenchmarkSummary sum{};
  sum.has_result = true;
  sum.duration_sec = std::chrono::duration<double>(end_time - bench_start_time_).count();
  sum.finished_time = end_time;
  sum.max_gpu_temp_c = bench_max_gpu_temp_c_;
  sum.max_gpu_power_w = bench_max_gpu_power_w_;
  sum.max_cpu_temp_c = bench_max_cpu_temp_c_;
  sum.max_cpu_power_w = bench_max_cpu_power_w_;

  size_t n = bench_frametimes_ms_.size();
  SimdStatsResult stats = compute_frametime_stats_simd(bench_frametimes_ms_);
  double total_ms = stats.total_ms;
  float min_ft_ms = stats.min_val;
  float max_ft_ms = stats.max_val;

  float mean_ft_ms = static_cast<float>(total_ms / static_cast<double>(n));
  sum.avg_fps = (mean_ft_ms > 0.001f) ? (1000.0f / mean_ft_ms) : 0.0f;
  sum.max_fps = (min_ft_ms > 0.001f) ? (1000.0f / min_ft_ms) : 0.0f;
  sum.min_fps = (max_ft_ms > 0.001f) ? (1000.0f / max_ft_ms) : 0.0f;

  std::sort(bench_frametimes_ms_.begin(), bench_frametimes_ms_.end(), std::greater<float>());

  auto calc_bench_time_weighted_low = [&](double tail_ratio) -> float {
    double target_ms = total_ms * tail_ratio;
    double accum_ms = 0.0;
    double weighted_sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
      double ft = static_cast<double>(bench_frametimes_ms_[i]);
      if (accum_ms + ft >= target_ms) {
        double rem = target_ms - accum_ms;
        weighted_sum += rem * ft;
        accum_ms = target_ms;
        break;
      }
      weighted_sum += ft * ft;
      accum_ms += ft;
    }
    if (accum_ms <= 0.0001) {
      return (n > 0 && bench_frametimes_ms_[0] > 0.0001f) ? (1000.0f / bench_frametimes_ms_[0])
                                                          : 0.0f;
    }
    double denom = weighted_sum / accum_ms;
    return (denom > 0.0001) ? static_cast<float>(1000.0 / denom) : 0.0f;
  };

  sum.fps_1pct_low = calc_bench_time_weighted_low(0.01);
  sum.fps_01pct_low = std::min(sum.fps_1pct_low, calc_bench_time_weighted_low(0.001));

  std::string csv_path = get_benchmark_csv_path();

  std::error_code ec;
  bool file_exists = fs::exists(csv_path, ec) && fs::file_size(csv_path, ec) > 0;
  int fd = ::open(csv_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd >= 0) {
    if (FILE* csv = ::fdopen(fd, "a")) {
      if (!file_exists) {
        std::fputs(
            "Timestamp,Duration_Sec,Frames,Avg_FPS,1Pct_Low_FPS,0.1Pct_Low_FPS,Min_FPS,Max_FPS,Max_"
            "GPU_Temp_C,Max_GPU_Watt,Max_CPU_Temp_C,Max_CPU_Watt\n",
            csv);
      }
      std::time_t t = std::time(nullptr);
      std::tm tm_buf{};
      localtime_r(&t, &tm_buf);
      char ts[32];
      std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

      std::fprintf(csv, "%s,%.1f,%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%.1f,%d,%.1f\n", ts,
                   sum.duration_sec, n, sum.avg_fps, sum.fps_1pct_low, sum.fps_01pct_low,
                   sum.min_fps, sum.max_fps, sum.max_gpu_temp_c, sum.max_gpu_power_w,
                   sum.max_cpu_temp_c, sum.max_cpu_power_w);
      std::fflush(csv);
      std::fclose(csv);
    } else {
      ::close(fd);
    }
  }

  last_bench_summary_ = sum;
  bench_has_result_.store(true, std::memory_order_relaxed);
  std::vector<float>().swap(bench_frametimes_ms_);
}

void TelemetryCollector::toggle_benchmark() {
  if (is_benchmark_active()) {
    stop_benchmark();
  } else {
    start_benchmark();
  }
}

namespace {

bool contains_icase(const char* haystack, const char* needle) noexcept {
  if (!haystack || !needle || !*needle) return false;
  size_t nlen = std::strlen(needle);
  for (const char* p = haystack; *p; ++p) {
    if (strncasecmp(p, needle, nlen) == 0) return true;
  }
  return false;
}

}  // namespace

const char* detect_effective_render_api(const char* base_api, const char* maps_path) noexcept {
  if (!base_api || !*base_api) return "VULKAN";
  if (std::strcmp(base_api, "VULKAN") != 0) return base_api;
  if (!maps_path || !*maps_path) return "VULKAN";

  FILE* fp = std::fopen(maps_path, "re");
  if (!fp) return "VULKAN";

  bool has_dx12 = false;
  bool has_dx11 = false;
  bool has_dx9 = false;
  char line[1024];
  while (std::fgets(line, sizeof(line), fp)) {
    if (contains_icase(line, "vkd3d") || contains_icase(line, "d3d12.dll") ||
        contains_icase(line, "d3d12core.dll")) {
      has_dx12 = true;
      break;
    }
    if (contains_icase(line, "dxvk") || contains_icase(line, "d3d11.dll")) {
      has_dx11 = true;
      break;
    } else if (contains_icase(line, "d3d9.dll")) {
      has_dx9 = true;
      break;
    }
  }
  std::fclose(fp);

  if (has_dx12) return "DX12";
  if (has_dx11) return "DX11";
  if (has_dx9) return "DX9";
  return "VULKAN";
}

TelemetryData TelemetryCollector::get_snapshot() const {
  TelemetryData out;
  {
    uint32_t idx = active_hw_buf_.load(std::memory_order_acquire);
    out = hw_buffers_[idx];
  }
  const char* base_api = active_api_.load(std::memory_order_relaxed);
  const char* trans_api = translated_api_.load(std::memory_order_relaxed);
  if (base_api && std::strcmp(base_api, "VULKAN") == 0 && trans_api) {
    out.render_api = trans_api;
  } else {
    out.render_api = base_api ? base_api : "VULKAN";
  }
  out.fps = displayed_fps_.load(std::memory_order_relaxed);
  out.fps_avg = displayed_fps_avg_.load(std::memory_order_relaxed);
  out.fps_1pct_low = displayed_fps_1low_.load(std::memory_order_relaxed);
  out.fps_01pct_low = displayed_fps_01low_.load(std::memory_order_relaxed);
  out.frametime_ms = displayed_frametime_ms_.load(std::memory_order_relaxed);

  if (bench_active_.load(std::memory_order_relaxed) ||
      bench_has_result_.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> block(bench_mutex_);
    out.benchmark_active = bench_active_.load(std::memory_order_relaxed);
    if (out.benchmark_active) {
      auto now = std::chrono::steady_clock::now();
      out.benchmark_elapsed_sec = std::chrono::duration<float>(now - bench_start_time_).count();
    }
    out.last_benchmark = last_bench_summary_;
  }
  return out;
}

void TelemetryCollector::poll_once_sync() {
  poll_cpu_load();
  poll_cpu_clock();
  poll_cpu_temp();
  poll_cpu_power();
  poll_ram();
  poll_game_ram();
  poll_mem_pressure();
  poll_gpu();
  poll_battery();
  poll_disk_io();
  poll_disk_temp();
  poll_net_speed();
  poll_local_clock();

  uint32_t next_idx = 1 - active_hw_buf_.load(std::memory_order_relaxed);
  hw_buffers_[next_idx] = staging_hw_data_;
  active_hw_buf_.store(next_idx, std::memory_order_release);
}

void TelemetryCollector::notify_active_gpu(uint32_t vendor_id, uint32_t device_id) {
  std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
  if (bound_vendor_id_ == vendor_id && bound_device_id_ == device_id &&
      gpu_backend_ != GpuBackendType::None) {
    return;
  }
  bound_vendor_id_ = vendor_id;
  bound_device_id_ = device_id;
  shutdown_sensors_locked();
  init_gpu_sensors_locked();
  if (gpu_backend_ == GpuBackendType::NvidiaNvml) {
    poll_nvidia_gpu();
  } else if (gpu_backend_ == GpuBackendType::IntelLevelZero) {
    poll_intel_level_zero_gpu();
  } else if (gpu_backend_ == GpuBackendType::AmdSysfs ||
             gpu_backend_ == GpuBackendType::IntelSysfs) {
    poll_drm_sysfs_gpu();
  }
}

void TelemetryCollector::update_api_vram_info(uint64_t used_bytes, uint64_t total_bytes) noexcept {
  api_vram_used_bytes_.store(used_bytes, std::memory_order_relaxed);
  api_vram_total_bytes_.store(total_bytes, std::memory_order_relaxed);
  drm_provider_.update_api_vram_info(used_bytes, total_bytes);
}

void TelemetryCollector::worker_loop() {
#if defined(__linux__)
  pthread_setname_np(pthread_self(), "fpshud_telem");
  // Set nice +10 to guarantee game engine and render threads take scheduling priority
  setpriority(PRIO_PROCESS, 0, 10);
#endif
  while (running_.load(std::memory_order_relaxed)) {
    poll_once_sync();
    for (int i = 0; i < 10 && running_.load(std::memory_order_relaxed); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

void TelemetryCollector::poll_local_clock() {
  std::time_t t = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char buf[8];
  std::strftime(buf, sizeof(buf), "%H:%M", &tm_buf);
  if (!translated_api_cached_.load(std::memory_order_relaxed)) {
    static int s_map_scan_attempts = 0;
    std::string maps_path =
        root_prefix_.empty() ? "/proc/self/maps" : (root_prefix_ + "/proc/self/maps");
    const char* detected = detect_effective_render_api("VULKAN", maps_path.c_str());
    if (detected && std::strcmp(detected, "VULKAN") != 0) {
      translated_api_.store(detected, std::memory_order_relaxed);
      translated_api_cached_.store(true, std::memory_order_relaxed);
    } else {
      translated_api_.store("VULKAN", std::memory_order_relaxed);
      if (++s_map_scan_attempts >= 3) {
        // After 3 initial attempts (~1.5s), permanently freeze and stop reading /proc/self/maps
        translated_api_cached_.store(true, std::memory_order_relaxed);
      }
    }
  }
  std::memcpy(staging_hw_data_.local_time_str, buf, sizeof(staging_hw_data_.local_time_str));
}

void TelemetryCollector::init_battery_sensors() {
  std::error_code ec;
  battery_capacity_path_.clear();
  battery_status_path_.clear();
  battery_power_uw_path_.clear();
  battery_current_ua_path_.clear();
  battery_voltage_uv_path_.clear();
  battery_discovered_ = false;

  std::string power_dir = root_prefix_ + "/sys/class/power_supply";
  if (!fs::exists(power_dir, ec)) {
    staging_hw_data_.battery_available = false;
    return;
  }

  for (const auto& entry : fs::directory_iterator(power_dir, ec)) {
    std::string fname = entry.path().filename().string();
    char type_buf[32] = {0};
    fs::path type_path = entry.path() / "type";
    bool is_bat = false;

    if (fname.rfind("BAT", 0) == 0) {
      is_bat = true;
    } else if (read_sysfs_buf(type_path.c_str(), type_buf, sizeof(type_buf))) {
      while (type_buf[0] && (type_buf[std::strlen(type_buf) - 1] == '\n' ||
                             type_buf[std::strlen(type_buf) - 1] == '\r')) {
        type_buf[std::strlen(type_buf) - 1] = '\0';
      }
      if (std::strcmp(type_buf, "Battery") == 0) {
        is_bat = true;
      }
    }

    if (is_bat) {
      fs::path cap_path = entry.path() / "capacity";
      if (fs::exists(cap_path, ec)) {
        battery_capacity_path_ = cap_path.string();
        battery_status_path_ = (entry.path() / "status").string();

        fs::path p_now = entry.path() / "power_now";
        if (fs::exists(p_now, ec)) {
          battery_power_uw_path_ = p_now.string();
        }
        fs::path c_now = entry.path() / "current_now";
        if (fs::exists(c_now, ec)) {
          battery_current_ua_path_ = c_now.string();
        }
        fs::path v_now = entry.path() / "voltage_now";
        if (fs::exists(v_now, ec)) {
          battery_voltage_uv_path_ = v_now.string();
        }

        battery_discovered_ = true;
        break;
      }
    }
  }

  staging_hw_data_.battery_available = battery_discovered_;
}

void TelemetryCollector::poll_battery() {
  if (!battery_discovered_) return;

  char buf[64];
  float cap_pct = 0.0f;
  float power_w = 0.0f;
  char status_str[16] = "Discharging";
  bool is_charging = false;

  if (!battery_capacity_path_.empty() &&
      read_sysfs_buf(battery_capacity_path_.c_str(), buf, sizeof(buf))) {
    cap_pct = std::clamp(std::strtof(buf, nullptr), 0.0f, 100.0f);
  }

  if (!battery_status_path_.empty() &&
      read_sysfs_buf(battery_status_path_.c_str(), buf, sizeof(buf))) {
    while (buf[0] && (buf[std::strlen(buf) - 1] == '\n' || buf[std::strlen(buf) - 1] == '\r')) {
      buf[std::strlen(buf) - 1] = '\0';
    }
    std::snprintf(status_str, sizeof(status_str), "%s", buf);
    is_charging = (std::strcmp(status_str, "Charging") == 0);
  }

  if (!battery_power_uw_path_.empty() &&
      read_sysfs_buf(battery_power_uw_path_.c_str(), buf, sizeof(buf))) {
    long long uw = std::strtoll(buf, nullptr, 10);
    power_w = static_cast<float>(std::abs(uw)) / 1000000.0f;
  } else if (!battery_current_ua_path_.empty() && !battery_voltage_uv_path_.empty()) {
    char c_buf[64] = {0};
    char v_buf[64] = {0};
    if (read_sysfs_buf(battery_current_ua_path_.c_str(), c_buf, sizeof(c_buf)) &&
        read_sysfs_buf(battery_voltage_uv_path_.c_str(), v_buf, sizeof(v_buf))) {
      double ua = std::abs(std::strtod(c_buf, nullptr));
      double uv = std::abs(std::strtod(v_buf, nullptr));
      power_w = static_cast<float>((ua * uv) / 1e12);
    }
  }

  staging_hw_data_.battery_available = true;
  staging_hw_data_.battery_pct = cap_pct;
  staging_hw_data_.battery_power_w = power_w;
  staging_hw_data_.battery_charging = is_charging;
  std::snprintf(staging_hw_data_.battery_status, sizeof(staging_hw_data_.battery_status), "%s",
                status_str);
}

void TelemetryCollector::init_storage_sensors() {
  std::error_code ec;
  disk_temp_path_.clear();

  std::string hwmon_dir = root_prefix_ + "/sys/class/hwmon";
  if (fs::exists(hwmon_dir, ec)) {
    std::string fallback_path;
    for (const auto& entry : fs::directory_iterator(hwmon_dir, ec)) {
      char buf[64] = {0};
      std::string name_path = (entry.path() / "name").string();
      if (!read_sysfs_buf(name_path.c_str(), buf, sizeof(buf))) continue;

      fs::path t1 = entry.path() / "temp1_input";
      if (!fs::exists(t1, ec)) continue;

      if (std::strncmp(buf, "nvme", 4) == 0) {
        disk_temp_path_ = t1.string();
        break;
      }
      if (fallback_path.empty() &&
          (std::strncmp(buf, "drivetemp", 9) == 0 || std::strncmp(buf, "ata", 3) == 0)) {
        fallback_path = t1.string();
      }
    }
    if (disk_temp_path_.empty() && !fallback_path.empty()) {
      disk_temp_path_ = std::move(fallback_path);
    }
  }

  staging_hw_data_.disk_temp_available = !disk_temp_path_.empty();
}

void TelemetryCollector::poll_disk_temp() {
  if (disk_temp_path_.empty()) return;
  char buf[32] = {0};
  if (!read_sysfs_buf(disk_temp_path_.c_str(), buf, sizeof(buf))) return;

  long millideg = std::strtol(buf, nullptr, 10);
  int temp_c = static_cast<int>((millideg + 500) / 1000);
  if (temp_c > -20 && temp_c < 130) {
    staging_hw_data_.disk_temp_available = true;
    staging_hw_data_.disk_temp_c = temp_c;
  }
}

void TelemetryCollector::poll_disk_io() {
  std::string stat_path = root_prefix_ + "/proc/diskstats";
  int fd = open(stat_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  char buf[16384];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return;
  buf[n] = '\0';

  auto is_whole_disk = [](const char* dev) noexcept -> bool {
    if (!dev || !*dev) return false;
    if (std::strncmp(dev, "nvme", 4) == 0) {
      const char* p = dev + 4;
      if (*p < '0' || *p > '9') return false;
      while (*p >= '0' && *p <= '9') ++p;
      if (*p != 'n') return false;
      ++p;
      if (*p < '0' || *p > '9') return false;
      while (*p >= '0' && *p <= '9') ++p;
      return *p == '\0';
    }
    if (std::strncmp(dev, "mmcblk", 6) == 0) {
      const char* p = dev + 6;
      if (*p < '0' || *p > '9') return false;
      while (*p >= '0' && *p <= '9') ++p;
      return *p == '\0';
    }
    if ((dev[0] == 's' || dev[0] == 'v') && dev[1] == 'd') {
      const char* p = dev + 2;
      if (*p < 'a' || *p > 'z') return false;
      while (*p >= 'a' && *p <= 'z') ++p;
      return *p == '\0';
    }
    return false;
  };

  uint64_t total_read_sectors = 0;
  uint64_t total_write_sectors = 0;

  const char* line = buf;
  while (*line) {
    const char* next_line = std::strchr(line, '\n');
    unsigned int maj = 0, min_id = 0;
    char dev_name[64] = {0};
    unsigned long long r_ios = 0, r_merges = 0, r_sec = 0, r_ticks = 0;
    unsigned long long w_ios = 0, w_merges = 0, w_sec = 0;
    if (std::sscanf(line, "%u %u %63s %llu %llu %llu %llu %llu %llu %llu", &maj, &min_id, dev_name,
                    &r_ios, &r_merges, &r_sec, &r_ticks, &w_ios, &w_merges, &w_sec) == 10) {
      if (is_whole_disk(dev_name)) {
        total_read_sectors += static_cast<uint64_t>(r_sec);
        total_write_sectors += static_cast<uint64_t>(w_sec);
      }
    }
    if (!next_line) break;
    line = next_line + 1;
  }

  auto now = std::chrono::steady_clock::now();
  float read_mbs = 0.0f;
  float write_mbs = 0.0f;
  if (prev_disk_poll_time_.time_since_epoch().count() != 0) {
    double dt_sec = std::chrono::duration<double>(now - prev_disk_poll_time_).count();
    if (dt_sec >= 0.01) {
      uint64_t d_read = (total_read_sectors >= prev_disk_read_sectors_)
                            ? (total_read_sectors - prev_disk_read_sectors_)
                            : 0;
      uint64_t d_write = (total_write_sectors >= prev_disk_write_sectors_)
                             ? (total_write_sectors - prev_disk_write_sectors_)
                             : 0;
      constexpr double kSectorToMb = 512.0 / (1024.0 * 1024.0);
      read_mbs = static_cast<float>((static_cast<double>(d_read) * kSectorToMb) / dt_sec);
      write_mbs = static_cast<float>((static_cast<double>(d_write) * kSectorToMb) / dt_sec);
    }
  }
  prev_disk_read_sectors_ = total_read_sectors;
  prev_disk_write_sectors_ = total_write_sectors;
  prev_disk_poll_time_ = now;

  staging_hw_data_.disk_read_mbs = read_mbs;
  staging_hw_data_.disk_write_mbs = write_mbs;
}

void TelemetryCollector::poll_net_speed() {
  std::string net_path = root_prefix_ + "/proc/net/dev";
  int fd = open(net_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  char buf[8192];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return;
  buf[n] = '\0';

  uint64_t total_rx = 0;
  uint64_t total_tx = 0;

  const char* line = buf;
  while (*line) {
    const char* next_line = std::strchr(line, '\n');
    const char* colon = std::strchr(line, ':');
    if (colon && (!next_line || colon < next_line)) {
      const char* p = line;
      while (p < colon && (*p == ' ' || *p == '\t')) ++p;
      size_t name_len = static_cast<size_t>(colon - p);
      while (name_len > 0 && (p[name_len - 1] == ' ' || p[name_len - 1] == '\t')) --name_len;

      bool skip = (name_len == 0) || (name_len == 2 && std::strncmp(p, "lo", 2) == 0) ||
                  (name_len >= 6 && std::strncmp(p, "docker", 6) == 0) ||
                  (name_len >= 4 && std::strncmp(p, "veth", 4) == 0) ||
                  (name_len >= 3 && std::strncmp(p, "br-", 3) == 0) ||
                  (name_len >= 5 && std::strncmp(p, "virbr", 5) == 0);
      if (!skip) {
        unsigned long long rx_b = 0, rx_p = 0, rx_e = 0, rx_d = 0, rx_f = 0, rx_fr = 0, rx_c = 0,
                           rx_m = 0, tx_b = 0;
        if (std::sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu", &rx_b, &rx_p,
                        &rx_e, &rx_d, &rx_f, &rx_fr, &rx_c, &rx_m, &tx_b) == 9) {
          total_rx += static_cast<uint64_t>(rx_b);
          total_tx += static_cast<uint64_t>(tx_b);
        }
      }
    }
    if (!next_line) break;
    line = next_line + 1;
  }

  auto now = std::chrono::steady_clock::now();
  float rx_mbs = 0.0f;
  float tx_mbs = 0.0f;
  if (prev_net_poll_time_.time_since_epoch().count() != 0) {
    double dt_sec = std::chrono::duration<double>(now - prev_net_poll_time_).count();
    if (dt_sec >= 0.01) {
      uint64_t d_rx = (total_rx >= prev_net_rx_bytes_) ? (total_rx - prev_net_rx_bytes_) : 0;
      uint64_t d_tx = (total_tx >= prev_net_tx_bytes_) ? (total_tx - prev_net_tx_bytes_) : 0;
      constexpr double kBytesToMb = 1.0 / (1024.0 * 1024.0);
      rx_mbs = static_cast<float>((static_cast<double>(d_rx) * kBytesToMb) / dt_sec);
      tx_mbs = static_cast<float>((static_cast<double>(d_tx) * kBytesToMb) / dt_sec);
    }
  }
  prev_net_rx_bytes_ = total_rx;
  prev_net_tx_bytes_ = total_tx;
  prev_net_poll_time_ = now;

  staging_hw_data_.net_rx_mbs = rx_mbs;
  staging_hw_data_.net_tx_mbs = tx_mbs;
}

void TelemetryCollector::poll_cpu_load() { cpu_provider_.poll_cpu_load(staging_hw_data_); }

void TelemetryCollector::poll_cpu_clock() { cpu_provider_.poll_cpu_clock(staging_hw_data_); }

void TelemetryCollector::poll_cpu_temp() {
  cpu_provider_.poll_cpu_temp(staging_hw_data_);
  if (bench_active_.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> block(bench_mutex_);
    if (staging_hw_data_.cpu_temp_c > bench_max_cpu_temp_c_) {
      bench_max_cpu_temp_c_ = staging_hw_data_.cpu_temp_c;
    }
  }
}

void TelemetryCollector::poll_cpu_power() {
  cpu_provider_.poll_cpu_power(staging_hw_data_);
  if (bench_active_.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> block(bench_mutex_);
    if (staging_hw_data_.cpu_power_w > bench_max_cpu_power_w_) {
      bench_max_cpu_power_w_ = staging_hw_data_.cpu_power_w;
    }
  }
}

void TelemetryCollector::poll_ram() { ram_provider_.poll_ram(staging_hw_data_); }

void TelemetryCollector::poll_game_ram() { ram_provider_.poll_game_ram(staging_hw_data_); }

void TelemetryCollector::poll_mem_pressure() { ram_provider_.poll_mem_pressure(staging_hw_data_); }

void TelemetryCollector::poll_gpu() {
  std::lock_guard<std::mutex> lock(gpu_sensor_mutex_);
  if (gpu_backend_ == GpuBackendType::NvidiaNvml) {
    poll_nvidia_gpu();
  } else if (gpu_backend_ == GpuBackendType::IntelLevelZero) {
    poll_intel_level_zero_gpu();
  } else if (gpu_backend_ == GpuBackendType::AmdSysfs ||
             gpu_backend_ == GpuBackendType::IntelSysfs) {
    poll_drm_sysfs_gpu();
  }
}

void TelemetryCollector::poll_nvidia_gpu() {
  nvidia_provider_.poll(staging_hw_data_);
  if (bench_active_.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> block(bench_mutex_);
    if (staging_hw_data_.gpu_temp_c > bench_max_gpu_temp_c_) {
      bench_max_gpu_temp_c_ = staging_hw_data_.gpu_temp_c;
    }
    if (staging_hw_data_.gpu_power_w > bench_max_gpu_power_w_) {
      bench_max_gpu_power_w_ = staging_hw_data_.gpu_power_w;
    }
  }
}

void TelemetryCollector::poll_intel_level_zero_gpu() {
  levelzero_provider_.poll(staging_hw_data_);
  if (bench_active_.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> block(bench_mutex_);
    if (staging_hw_data_.gpu_temp_c > bench_max_gpu_temp_c_) {
      bench_max_gpu_temp_c_ = staging_hw_data_.gpu_temp_c;
    }
    if (staging_hw_data_.gpu_power_w > bench_max_gpu_power_w_) {
      bench_max_gpu_power_w_ = staging_hw_data_.gpu_power_w;
    }
  }
}

void TelemetryCollector::poll_drm_sysfs_gpu() {
  drm_provider_.poll(staging_hw_data_);
  if (bench_active_.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> block(bench_mutex_);
    if (staging_hw_data_.gpu_temp_c > bench_max_gpu_temp_c_) {
      bench_max_gpu_temp_c_ = staging_hw_data_.gpu_temp_c;
    }
    if (staging_hw_data_.gpu_power_w > bench_max_gpu_power_w_) {
      bench_max_gpu_power_w_ = staging_hw_data_.gpu_power_w;
    }
  }
}

TelemetryCollector& get_telemetry() {
  static TelemetryCollector instance;
  return instance;
}

}  // namespace fpshud