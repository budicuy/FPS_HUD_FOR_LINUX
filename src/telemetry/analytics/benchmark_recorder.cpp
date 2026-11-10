#include "benchmark_recorder.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "frametime_analyzer.hpp"
#include "telemetry.hpp"

namespace fs = std::filesystem;

namespace fpshud {

namespace {

std::string get_default_benchmark_csv_path() {
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

BenchmarkRecorder::BenchmarkRecorder(std::string csv_path)
    : custom_csv_path_(std::move(csv_path)) {}

void BenchmarkRecorder::start() {
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

void BenchmarkRecorder::stop() {
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

  std::string csv_path =
      custom_csv_path_.empty() ? get_default_benchmark_csv_path() : custom_csv_path_;

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

void BenchmarkRecorder::toggle() {
  if (is_active()) {
    stop();
  } else {
    start();
  }
}

void BenchmarkRecorder::on_frame(float dt_ms, const TelemetryData& current_snapshot) {
  if (!bench_active_.load(std::memory_order_relaxed)) {
    return;
  }
  std::lock_guard<std::mutex> block(bench_mutex_);
  if (bench_frametimes_ms_.size() < 360000) {
    bench_frametimes_ms_.push_back(dt_ms);
  }
  if (current_snapshot.gpu_temp_c > bench_max_gpu_temp_c_) {
    bench_max_gpu_temp_c_ = current_snapshot.gpu_temp_c;
  }
  if (current_snapshot.gpu_power_w > bench_max_gpu_power_w_) {
    bench_max_gpu_power_w_ = current_snapshot.gpu_power_w;
  }
  if (current_snapshot.cpu_temp_c > bench_max_cpu_temp_c_) {
    bench_max_cpu_temp_c_ = current_snapshot.cpu_temp_c;
  }
  if (current_snapshot.cpu_power_w > bench_max_cpu_power_w_) {
    bench_max_cpu_power_w_ = current_snapshot.cpu_power_w;
  }
}

void BenchmarkRecorder::update_peaks(int cpu_temp_c, float cpu_power_w, int gpu_temp_c,
                                     float gpu_power_w) {
  if (!bench_active_.load(std::memory_order_relaxed)) {
    return;
  }
  std::lock_guard<std::mutex> block(bench_mutex_);
  if (cpu_temp_c > bench_max_cpu_temp_c_) {
    bench_max_cpu_temp_c_ = cpu_temp_c;
  }
  if (cpu_power_w > bench_max_cpu_power_w_) {
    bench_max_cpu_power_w_ = cpu_power_w;
  }
  if (gpu_temp_c > bench_max_gpu_temp_c_) {
    bench_max_gpu_temp_c_ = gpu_temp_c;
  }
  if (gpu_power_w > bench_max_gpu_power_w_) {
    bench_max_gpu_power_w_ = gpu_power_w;
  }
}

float BenchmarkRecorder::get_elapsed_sec() const noexcept {
  if (!bench_active_.load(std::memory_order_relaxed)) {
    return 0.0f;
  }
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration<float>(now - bench_start_time_).count();
}

BenchmarkSummary BenchmarkRecorder::get_summary() const {
  std::lock_guard<std::mutex> block(bench_mutex_);
  return last_bench_summary_;
}

void BenchmarkRecorder::set_csv_path(std::string path) {
  std::lock_guard<std::mutex> block(bench_mutex_);
  custom_csv_path_ = std::move(path);
}

}  // namespace fpshud
