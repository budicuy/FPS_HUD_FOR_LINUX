#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace fpshud {

struct TelemetryData;

struct BenchmarkSummary {
  bool has_result = false;
  double duration_sec = 0.0;
  float avg_fps = 0.0f;
  float fps_1pct_low = 0.0f;
  float fps_01pct_low = 0.0f;
  float min_fps = 0.0f;
  float max_fps = 0.0f;
  int max_gpu_temp_c = 0;
  float max_gpu_power_w = 0.0f;
  int max_cpu_temp_c = 0;
  float max_cpu_power_w = 0.0f;
  std::chrono::steady_clock::time_point finished_time{};
};

class BenchmarkRecorder {
public:
  explicit BenchmarkRecorder(std::string csv_path = "");

  void start();
  void stop();
  void toggle();
  void on_frame(float dt_ms, const TelemetryData& current_snapshot);

  void update_peaks(int cpu_temp_c, float cpu_power_w, int gpu_temp_c, float gpu_power_w);

  [[nodiscard]] bool is_active() const noexcept {
    return bench_active_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool has_result() const noexcept {
    return bench_has_result_.load(std::memory_order_acquire);
  }
  [[nodiscard]] float get_elapsed_sec() const noexcept;
  [[nodiscard]] BenchmarkSummary get_summary() const;

  void set_csv_path(std::string path);
  [[nodiscard]] const std::string& get_csv_path() const noexcept { return custom_csv_path_; }

private:
  mutable std::mutex bench_mutex_;
  std::atomic<bool> bench_active_{false};
  std::atomic<bool> bench_has_result_{false};
  std::chrono::steady_clock::time_point bench_start_time_{};
  std::vector<float> bench_frametimes_ms_;
  int bench_max_gpu_temp_c_ = 0;
  float bench_max_gpu_power_w_ = 0.0f;
  int bench_max_cpu_temp_c_ = 0;
  float bench_max_cpu_power_w_ = 0.0f;
  BenchmarkSummary last_bench_summary_{};
  std::string custom_csv_path_;
};

}  // namespace fpshud
