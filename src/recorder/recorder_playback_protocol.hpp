#pragma once
#include <atomic>
#include <cstdint>

namespace fpshud {

constexpr uint32_t kPlaybackShmMagic = 0x504C4159;  // "PLAY"
constexpr const char* kPlaybackShmPrefix = "/fpshud_player_shm_";
constexpr const char* kPlaybackSocketPrefix = "player_";

enum class IpcPlaybackAction : uint32_t {
  Play = 1,
  Pause = 2,
  Seek = 3,  // payload: value = seek_sec
  Close = 4  // Terminate player worker
};

struct IpcPlaybackRequest {
  IpcPlaybackAction action{IpcPlaybackAction::Play};
  float value{0.0f};
};

struct PlaybackShmHeader {
  uint32_t magic{0};
  uint32_t width{0};
  uint32_t height{0};
  float duration_sec{0.0f};
  std::atomic<float> current_time_sec{0.0f};
  std::atomic<bool> is_playing{false};
  std::atomic<bool> is_ended{false};
  std::atomic<uint64_t> frame_seq{0};
  std::atomic<uint32_t> write_index{0};
  // RGBA pixel buffer follows immediately after this struct in shared memory
};

}  // namespace fpshud
