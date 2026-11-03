#pragma once

#include "config.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fpshud {

// Magic packet header for IPC Unix Domain Socket
constexpr uint32_t kRecorderIpcMagic = 0x46505352; // "FPSR"

enum class RecorderCommand : uint32_t {
    StartRecord = 1,
    StopRecord = 2,
    SaveReplay = 3,
    QueryStatus = 4,
    SendFrame = 5,
    Ping = 6
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
};

struct IpcStartPayload {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 60;
    uint32_t bitrate_kbps = 18000;
    uint32_t audio_source = 0;
    uint32_t container = 0;
    uint32_t replay_enabled = 0;
    uint32_t replay_sec = 60;
    char save_dir[256] = {0};
    char filename_prefix[64] = {0};
};

struct IpcStatusResponse {
    uint32_t magic = kRecorderIpcMagic;
    uint32_t status = 0; // RecorderStatus
    uint32_t frame_count = 0;
    float elapsed_sec = 0.0f;
    char last_saved_file[256] = {0};
    char error_msg[128] = {0};
};

// Singleton controller in libfpshud.so to manage on-demand helper process & IPC
class RecorderController {
public:
    static RecorderController& instance();

    // Toggle start/stop recording on F7 or GUI button
    void toggle_recording(const HudConfig& cfg, float display_w, float display_h);
    void start_recording(const HudConfig& cfg, float display_w, float display_h);
    void stop_recording();

    // Save instant replay (F8 or GUI button)
    void save_instant_replay(const HudConfig& cfg);

    // Live status queries for HUD overlay & GUI menu
    bool is_recording() const noexcept;
    bool is_replay_active() const noexcept;
    float get_recording_duration() const;
    uint32_t get_recorded_frames() const;
    std::string get_last_saved_path() const;

    // Toast notification (returns true if a message should be shown on screen)
    bool get_recent_toast(std::string& out_msg) const;
    void set_toast(const std::string& msg);

    // Called from render loop to provide frame dimensions & handle on-demand frame transfers
    void on_frame_present(uint32_t width, uint32_t height, int dmabuf_fd = -1);

private:
    RecorderController();
    ~RecorderController();

    bool spawn_helper_process(const HudConfig& cfg, float display_w, float display_h);
    void connect_socket();
    void disconnect_socket();
    void send_command(RecorderCommand cmd, const void* payload = nullptr, size_t payload_sz = 0);
    void update_status_sync();

    mutable std::recursive_mutex mutex_;
    std::atomic<bool> is_recording_{false};
    std::atomic<bool> is_replay_active_{false};
    std::atomic<float> elapsed_sec_{0.0f};
    std::atomic<uint32_t> recorded_frames_{0};
    std::string last_saved_file_;
    int socket_fd_ = -1;
    pid_t helper_pid_ = -1;
    std::string socket_path_;
    std::chrono::steady_clock::time_point record_start_time_{};

    // Toast state (lasts 3.5 seconds)
    std::string toast_msg_;
    std::chrono::steady_clock::time_point toast_time_{};
};

} // namespace fpshud
