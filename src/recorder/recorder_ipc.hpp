#pragma once

#include "pack_indexing.hpp"
#include "recorder_playback_protocol.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fpshud {

struct HudConfig;

// Magic packet header for IPC Unix Domain Socket
constexpr uint32_t kRecorderIpcMagic = 0x46505352;  // "FPSR"

enum class RecorderCommand : uint32_t {
  StartRecord = 1,
  StopRecord = 2,
  QueryStatus = 3,
  SendFrame = 4,
  Ping = 5,
  SaveReplay = 6
};

enum class RecorderStatus : uint32_t {
  Idle = 0,
  Starting = 1,
  Recording = 2,
  Stopping = 3,
  Error = 4
};

struct IpcPacketHeader {
  uint32_t magic = kRecorderIpcMagic;
  uint32_t command = 0;
  uint32_t payload_size = 0;
  uint32_t flags = 0;

  // Inspect packet header fields using C++26 pack indexing
  template <std::size_t Index>
  [[nodiscard]] constexpr uint32_t get_field() const noexcept {
    static_assert(
        Index < 4,
        "IpcPacketHeader field index out of range (0=magic, 1=command, 2=payload_size, 3=flags)");
    return pack_element<Index>(magic, command, payload_size, flags);
  }

  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return get_field<0>() == kRecorderIpcMagic;
  }
};

struct IpcStartPayload {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps = 60;
  uint32_t bitrate_kbps = 18000;
  uint32_t audio_source = 0;
  uint32_t container = 0;
  uint32_t codec = 1;          // 0 = H.264, 1 = HEVC (H.265)
  uint32_t quality_mode = 0;   // 0 = CQP Adaptif, 1 = Fixed VBR
  uint32_t cq_level = 22;      // CQP 16-30
  uint32_t vfr_mode = 0;       // 0 = CFR (statis 60 FPS), 1 = Pure Adaptive VFR (1:1 Capture)
  uint32_t vfr_max_fps = 120;  // Safety maximum framerate cap in VFR mode
  uint32_t replay_mode = 0;    // 0 = Normal Record, 1 = Instant Replay Ring Buffer
  uint32_t replay_duration_sec = 30;
  char save_dir[256] = {0};
  char filename_prefix[64] = {0};
};

struct IpcStatusResponse {
  uint32_t magic = kRecorderIpcMagic;
  uint32_t status = 0;  // RecorderStatus
  uint32_t frame_count = 0;
  float elapsed_sec = 0.0f;
  char last_saved_file[256] = {0};
  char error_msg[128] = {0};
};

// Header located at start of POSIX Shared Memory (/dev/shm/fpshud_rec_shm_<PID>)
constexpr uint32_t kShmFrameMagic = 0x4650534D;  // "FPSM"

struct ShmFrameHeader {
  uint32_t magic;  // kShmFrameMagic
  uint32_t width;
  uint32_t height;
  uint32_t format;  // 0 = BGRA, 1 = RGBA, 2 = X2BGR10LE, 3 = X2RGB10LE
  uint32_t flip_y;  // 0 = normal (Vulkan), 1 = flipped (OpenGL)
  uint32_t slot_size;
  std::atomic<uint32_t> write_slot{0};  // 0, 1, or 2 (Triple-buffering)
  std::atomic<uint64_t> sequence{0};
  uint32_t offset[3];  // Slot 0, Slot 1, Slot 2
};

struct IpcFrameNotify {
  IpcPacketHeader hdr;  // command = SendFrame
  uint32_t slot = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t flip_y = 0;
  uint64_t seq = 0;
  uint64_t timestamp_us = 0;
};

// Discovers container host paths for multiarch dynamic libraries (LD_LIBRARY_PATH)
// and hardware VAAPI driver directories (LIBVA_DRIVERS_PATH) under <root>/run/host and
// <root>/var/run/host.
bool build_container_host_env_paths(const std::string& root_prefix, std::string& out_ld_path,
                                    std::string& out_libva_path);

// Secure private runtime directory (0700) for Unix domain sockets and shared memory buffers
std::string get_secure_runtime_dir();

// Sanitizes IPC filename prefixes to prevent path traversal and arbitrary file placement
inline std::string sanitize_filename_prefix(const std::string& input,
                                            const std::string& fallback = "FPSHUD") {
  std::string leaf = std::filesystem::path(input).filename().string();
  std::string sanitized;
  for (char c : leaf) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      sanitized += c;
    }
  }
  if (sanitized.empty()) {
    return fallback;
  }
  return sanitized;
}

// Finds the trusted fpshud_recorder helper binary path (dladdr, user prefix, or PATH)
std::string get_helper_binary_path();

// Singleton controller in libfpshud.so to manage on-demand helper process & IPC
class RecorderController {
public:
  static RecorderController& instance();

  // Toggle start/stop recording on F7 or GUI button
  void toggle_recording(const HudConfig& cfg, float display_w, float display_h);
  void start_recording(const HudConfig& cfg, float display_w, float display_h);
  void stop_recording();

  // Live status queries for HUD overlay & GUI menu
  bool is_recording() const noexcept;
  bool is_replay_active() const noexcept;
  bool is_capturing_frames() const noexcept;
  float get_recording_duration() const;
  uint32_t get_recorded_frames() const;
  std::string get_last_saved_path() const;

  // Instant Replay lifecycle & on-demand save
  void sync_replay_state(const HudConfig& cfg, float display_w, float display_h);
  void trigger_save_replay();

  // Toast notification (returns true if a message should be shown on screen)
  bool get_recent_toast(std::string& out_msg) const;
  void set_toast(const std::string& msg);

  // Submits an uncompressed frame buffer (Vulkan staging buffer or OpenGL read pixels) to shared
  // memory
  void submit_frame(const void* pixels, uint32_t width, uint32_t height, uint32_t format = 0,
                    bool flip_y = false);

  // Screenshot capture pipeline (dependency-free lossless PNG on background worker thread)
  void request_screenshot() noexcept;
  bool consume_screenshot_request() noexcept;
  void submit_screenshot(const void* pixels, uint32_t width, uint32_t height, uint32_t format,
                         bool flip_y, const std::string& save_dir);

private:
  RecorderController();
  ~RecorderController();

  bool setup_shm(uint32_t width, uint32_t height);
  void cleanup_shm();

  bool spawn_helper_process(const HudConfig& cfg, float display_w, float display_h);
  void connect_socket();
  void disconnect_socket();
  void send_command(RecorderCommand cmd, const void* payload = nullptr, size_t payload_sz = 0);

  mutable std::recursive_mutex mutex_;
  std::atomic<bool> is_recording_{false};
  std::atomic<bool> is_replay_active_{false};
  int active_replay_duration_sec_ = 0;
  std::atomic<uint32_t> recorded_frames_{0};
  std::string last_saved_file_;
  int socket_fd_ = -1;
  pid_t helper_pid_ = -1;
  std::string socket_path_;
  std::chrono::steady_clock::time_point record_start_time_{};

  // Background save thread state
  std::thread save_thread_;
  std::atomic<bool> is_finalizing_{false};
  std::thread replay_save_thread_;
  std::atomic<bool> is_saving_replay_{false};
  std::atomic<bool> screenshot_requested_{false};
  std::thread screenshot_thread_;

  // Shared Memory state
  std::string shm_path_;
  int shm_fd_ = -1;
  void* shm_base_ = nullptr;
  size_t shm_size_ = 0;
  uint32_t shm_width_ = 0;
  uint32_t shm_height_ = 0;

  // Toast state (lasts 3.5 seconds)
  std::string toast_msg_;
  std::chrono::steady_clock::time_point toast_time_{};
};

constexpr void resolve_recording_dimensions(int res_mode, int display_w, int display_h,
                                            uint32_t& out_w, uint32_t& out_h) noexcept {
  switch (res_mode) {
    case 1:
      out_w = 2560;
      out_h = 1440;
      break;
    case 2:
      out_w = 1920;
      out_h = 1080;
      break;
    case 3:
      out_w = 1280;
      out_h = 720;
      break;
    default:
      out_w = static_cast<uint32_t>(display_w > 0 ? display_w : 1920);
      out_h = static_cast<uint32_t>(display_h > 0 ? display_h : 1080);
      break;
  }
  out_w &= ~1u;
  out_h &= ~1u;
}

class VideoPlayerController {
public:
  VideoPlayerController();
  ~VideoPlayerController();

  bool open(const std::string& filepath, const std::string& helper_bin_path);
  void close();
  void play();
  void pause();
  void seek(float sec);

  [[nodiscard]] bool is_active() const noexcept;
  [[nodiscard]] const PlaybackShmHeader* header() const noexcept;
  [[nodiscard]] const uint8_t* frame_pixels() const noexcept;
  [[nodiscard]] uint64_t last_consumed_seq() const noexcept;
  void mark_seq_consumed(uint64_t seq) noexcept;

private:
  void send_action(IpcPlaybackAction action, float value);

  int shm_fd_{-1};
  int sock_fd_{-1};
  pid_t worker_pid_{-1};
  PlaybackShmHeader* shm_header_{nullptr};
  uint8_t* shm_pixels_{nullptr};
  size_t shm_size_{0};
  std::string shm_name_;
  std::string socket_path_;
  uint64_t last_consumed_seq_{0};
};

}  // namespace fpshud
