#ifndef FPSHUD_RECORDER_PULSE_AUDIO_UTIL_HPP
#define FPSHUD_RECORDER_PULSE_AUDIO_UTIL_HPP

#include <string>
#include <string_view>

namespace fpshud {

enum class PulseInitStatus {
  Connected,
  Cancelled,
  TimedOut,
  Error,
};

enum class AudioRole {
  Monitor,
  Microphone,
};

struct PulseInitResult {
  PulseInitStatus status = PulseInitStatus::Error;
  std::string detail;
};

[[nodiscard]] constexpr bool should_log_pulse_init_failure(PulseInitStatus status) noexcept {
  return status == PulseInitStatus::TimedOut || status == PulseInitStatus::Error;
}

[[nodiscard]] inline std::string format_pulse_init_warning(const PulseInitResult& result) {
  if (!should_log_pulse_init_failure(result.status)) {
    return {};
  }
  if (result.status == PulseInitStatus::TimedOut) {
    return "[FPSHUD Recorder] PulseAudio connection timed out, audio capture disabled.\n";
  }
  std::string msg = "[FPSHUD Recorder] PulseAudio connection failed";
  if (!result.detail.empty()) {
    msg += " (";
    msg += result.detail;
    msg += ")";
  }
  msg += ", audio capture disabled.\n";
  return msg;
}

[[nodiscard]] inline std::string resolve_monitor_source_name(const char* default_sink_name) {
  if (!default_sink_name || *default_sink_name == '\0') {
    return "@DEFAULT_MONITOR@";
  }
  std::string_view sv(default_sink_name);
  while (!sv.empty() &&
         (sv.back() == '\n' || sv.back() == '\r' || sv.back() == ' ' || sv.back() == '\t')) {
    sv.remove_suffix(1);
  }
  while (!sv.empty() &&
         (sv.front() == '\n' || sv.front() == '\r' || sv.front() == ' ' || sv.front() == '\t')) {
    sv.remove_prefix(1);
  }
  if (sv.empty()) {
    return "@DEFAULT_MONITOR@";
  }
  if (sv.ends_with(".monitor")) {
    return std::string(sv);
  }
  return std::string(sv) + ".monitor";
}

[[nodiscard]] inline std::string resolve_mic_source_name(
    const char* default_source_name, [[maybe_unused]] const char* default_sink_name) {
  if (!default_source_name || *default_source_name == '\0') {
    return "@DEFAULT_SOURCE@";
  }
  std::string_view sv(default_source_name);
  while (!sv.empty() &&
         (sv.back() == '\n' || sv.back() == '\r' || sv.back() == ' ' || sv.back() == '\t')) {
    sv.remove_suffix(1);
  }
  while (!sv.empty() &&
         (sv.front() == '\n' || sv.front() == '\r' || sv.front() == ' ' || sv.front() == '\t')) {
    sv.remove_prefix(1);
  }
  if (sv.empty() || sv.ends_with(".monitor")) {
    return "@DEFAULT_SOURCE@";
  }
  return std::string(sv);
}

}  // namespace fpshud

#endif  // FPSHUD_RECORDER_PULSE_AUDIO_UTIL_HPP
