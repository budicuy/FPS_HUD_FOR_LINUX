#pragma once

#include "pack_indexing.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fpshud {

struct HudConfig;

// Magic packet header for IPC Unix Domain Socket
constexpr uint32_t kRecorderIpcMagic = 0x46505352; // "FPSR"

enum class RecorderCommand : uint32_t {
    StartRecord = 1,
    StopRecord = 2,
    QueryStatus = 3,
    SendFrame = 4,
    Ping = 5
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
        static_assert(Index < 4, "IpcPacketHeader field index out of range (0=magic, 1=command, 2=payload_size, 3=flags)");
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
    uint32_t codec = 1;        // 0 = H.264, 1 = HEVC (H.265)
    uint32_t quality_mode = 0; // 0 = CQP Adaptif, 1 = Fixed VBR
    uint32_t cq_level = 22;    // CQP 16-30
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

// Header located at start of POSIX Shared Memory (/dev/shm/fpshud_rec_shm_<PID>)
constexpr uint32_t kShmFrameMagic = 0x4650534D; // "FPSM"

struct ShmFrameHeader {
    uint32_t magic;                      // kShmFrameMagic
    uint32_t width;
    uint32_t height;
    uint32_t format;                     // 0 = BGRA, 1 = RGBA
    uint32_t flip_y;                     // 0 = normal (Vulkan), 1 = flipped (OpenGL)
    uint32_t slot_size;
    std::atomic<uint32_t> write_slot{0}; // 0, 1, or 2 (Triple-buffering)
    std::atomic<uint64_t> sequence{0};
    uint32_t offset[3];                  // Slot 0, Slot 1, Slot 2
};

struct IpcFrameNotify {
    IpcPacketHeader hdr;                 // command = SendFrame
    uint32_t slot = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t flip_y = 0;
    uint64_t seq = 0;
};

// Discovers container host paths for multiarch dynamic libraries (LD_LIBRARY_PATH)
// and hardware VAAPI driver directories (LIBVA_DRIVERS_PATH) under <root>/run/host and <root>/var/run/host.
bool build_container_host_env_paths(const std::string& root_prefix, std::string& out_ld_path, std::string& out_libva_path);

// Secure private runtime directory (0700) for Unix domain sockets and shared memory buffers
std::string get_secure_runtime_dir();

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
    float get_recording_duration() const;
    uint32_t get_recorded_frames() const;
    std::string get_last_saved_path() const;

    // Toast notification (returns true if a message should be shown on screen)
    bool get_recent_toast(std::string& out_msg) const;
    void set_toast(const std::string& msg);

    // Called from render loop to provide frame dimensions & handle on-demand frame transfers
    void on_frame_present(uint32_t width, uint32_t height, int dmabuf_fd = -1);

    // Submits an uncompressed frame buffer (Vulkan staging buffer or OpenGL read pixels) to shared memory
    void submit_frame(const void* pixels, uint32_t width, uint32_t height, uint32_t format = 0, bool flip_y = false);

private:
    RecorderController();
    ~RecorderController();

    bool setup_shm(uint32_t width, uint32_t height);
    void cleanup_shm();

    bool spawn_helper_process(const HudConfig& cfg, float display_w, float display_h);
    void connect_socket();
    void disconnect_socket();
    void send_command(RecorderCommand cmd, const void* payload = nullptr, size_t payload_sz = 0);
    void update_status_sync();

    mutable std::recursive_mutex mutex_;
    std::atomic<bool> is_recording_{false};
    std::atomic<float> elapsed_sec_{0.0f};
    std::atomic<uint32_t> recorded_frames_{0};
    std::string last_saved_file_;
    int socket_fd_ = -1;
    pid_t helper_pid_ = -1;
    std::string socket_path_;
    std::chrono::steady_clock::time_point record_start_time_{};

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

} // namespace fpshud
