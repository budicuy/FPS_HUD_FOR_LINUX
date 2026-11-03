#include "recorder_ipc.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace fpshud {

namespace {

std::string get_helper_binary_path() {
    // 1. Check next to running libfpshud.so via dladdr
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&get_helper_binary_path), &info) && info.dli_fname) {
        fs::path p = fs::path(info.dli_fname).parent_path() / "fpshud_recorder";
        std::error_code ec;
        if (fs::exists(p, ec)) return p.string();

        fs::path p_bin = fs::path(info.dli_fname).parent_path().parent_path() / "bin" / "fpshud_recorder";
        if (fs::exists(p_bin, ec)) return p_bin.string();

        fs::path p_prefix_bin = fs::path(info.dli_fname).parent_path().parent_path().parent_path() / "bin" / "fpshud_recorder";
        if (fs::exists(p_prefix_bin, ec)) return p_prefix_bin.string();
    }

    // 2. Standard user installation: ~/.local/bin/fpshud_recorder & ~/.local/lib/fpshud/fpshud_recorder
    if (const char* home = std::getenv("HOME"); home && *home) {
        fs::path p1 = fs::path(home) / ".local" / "bin" / "fpshud_recorder";
        std::error_code ec;
        if (fs::exists(p1, ec)) return p1.string();

        fs::path p2 = fs::path(home) / ".local" / "lib" / "fpshud" / "fpshud_recorder";
        if (fs::exists(p2, ec)) return p2.string();
    }

    // 3. Current working directory / build directory
    std::error_code ec;
    if (fs::exists("./build/fpshud_recorder", ec)) {
        return "./build/fpshud_recorder";
    }
    if (fs::exists("./fpshud_recorder", ec)) {
        return "./fpshud_recorder";
    }

    // 4. Default fallback to PATH lookup
    return "fpshud_recorder";
}

} // namespace

RecorderController& RecorderController::instance() {
    static RecorderController s_inst;
    return s_inst;
}

RecorderController::RecorderController() {
    socket_path_ = "/tmp/fpshud_rec_" + std::to_string(getpid()) + ".sock";
}

RecorderController::~RecorderController() {
    stop_recording();
}

bool RecorderController::is_recording() const noexcept {
    return is_recording_.load(std::memory_order_relaxed);
}

bool RecorderController::is_replay_active() const noexcept {
    return is_replay_active_.load(std::memory_order_relaxed);
}

float RecorderController::get_recording_duration() const {
    if (!is_recording_.load(std::memory_order_relaxed)) {
        return elapsed_sec_.load(std::memory_order_relaxed);
    }
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - record_start_time_).count();
}

uint32_t RecorderController::get_recorded_frames() const {
    return recorded_frames_.load(std::memory_order_relaxed);
}

std::string RecorderController::get_last_saved_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_saved_file_;
}

void RecorderController::set_toast(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    toast_msg_ = msg;
    toast_time_ = std::chrono::steady_clock::now();
}

bool RecorderController::get_recent_toast(std::string& out_msg) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (toast_msg_.empty()) return false;
    auto now = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(now - toast_time_).count();
    if (sec <= 3.8) {
        out_msg = toast_msg_;
        return true;
    }
    return false;
}

void RecorderController::toggle_recording(const HudConfig& cfg, float display_w, float display_h) {
    if (is_recording()) {
        stop_recording();
    } else {
        start_recording(cfg, display_w, display_h);
    }
}

void RecorderController::start_recording(const HudConfig& cfg, float display_w, float display_h) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_recording_.load(std::memory_order_relaxed)) {
        return;
    }

    if (!spawn_helper_process(cfg, display_w, display_h)) {
        set_toast("[!] Gagal memulai fpshud_recorder helper!");
        return;
    }

    connect_socket();
    if (socket_fd_ < 0) {
        set_toast("[!] Gagal koneksi socket recorder!");
        return;
    }

    IpcStartPayload payload{};
    payload.width = static_cast<uint32_t>(display_w);
    payload.height = static_cast<uint32_t>(display_h);
    payload.fps = (cfg.record_fps > 0) ? cfg.record_fps : 60;
    payload.bitrate_kbps = cfg.record_bitrate_kbps;
    payload.audio_source = cfg.record_audio_source;
    payload.container = cfg.record_container;
    payload.replay_enabled = cfg.record_replay_enabled ? 1 : 0;
    payload.replay_sec = cfg.record_replay_duration_sec;

    std::string save_dir = get_effective_record_dir(cfg);
    std::error_code ec;
    fs::create_directories(save_dir, ec);
    std::snprintf(payload.save_dir, sizeof(payload.save_dir), "%s", save_dir.c_str());
    std::snprintf(payload.filename_prefix, sizeof(payload.filename_prefix), "FPSHUD");

    send_command(RecorderCommand::StartRecord, &payload, sizeof(payload));

    is_recording_.store(true, std::memory_order_relaxed);
    record_start_time_ = std::chrono::steady_clock::now();
    elapsed_sec_.store(0.0f, std::memory_order_relaxed);
    recorded_frames_.store(0, std::memory_order_relaxed);

    toast_msg_ = "[● REC] Perekaman dimulai (F7 untuk berhenti)";
    toast_time_ = std::chrono::steady_clock::now();
}

void RecorderController::stop_recording() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_recording_.load(std::memory_order_relaxed)) {
        return;
    }

    if (socket_fd_ >= 0) {
        send_command(RecorderCommand::StopRecord, nullptr, 0);

        // Read response with final saved file name
        IpcStatusResponse resp{};
        ssize_t n = recv(socket_fd_, &resp, sizeof(resp), 0);
        if (n >= static_cast<ssize_t>(sizeof(IpcStatusResponse)) && resp.magic == kRecorderIpcMagic) {
            if (resp.last_saved_file[0] != '\0') {
                last_saved_file_ = resp.last_saved_file;
            }
        }
        disconnect_socket();
    }

    is_recording_.store(false, std::memory_order_relaxed);

    // Reap child process with a brief timeout
    if (helper_pid_ > 0) {
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            pid_t p = waitpid(helper_pid_, &status, WNOHANG);
            if (p == helper_pid_ || p == -1) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        helper_pid_ = -1;
    }

    // Clean up socket file
    if (!socket_path_.empty()) {
        unlink(socket_path_.c_str());
    }

    if (!last_saved_file_.empty()) {
        toast_msg_ = "[✓] Video tersimpan: " + last_saved_file_;
    } else {
        toast_msg_ = "[✓] Perekaman dihentikan.";
    }
    toast_time_ = std::chrono::steady_clock::now();
}

void RecorderController::save_instant_replay(const HudConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_fd_ >= 0) {
        send_command(RecorderCommand::SaveReplay, nullptr, 0);

        IpcStatusResponse resp{};
        ssize_t n = recv(socket_fd_, &resp, sizeof(resp), 0);
        if (n >= static_cast<ssize_t>(sizeof(IpcStatusResponse)) && resp.magic == kRecorderIpcMagic) {
            if (resp.last_saved_file[0] != '\0') {
                last_saved_file_ = resp.last_saved_file;
                toast_msg_ = "[✓] Replay tersimpan: " + last_saved_file_;
                toast_time_ = std::chrono::steady_clock::now();
                return;
            }
        }
    }
    toast_msg_ = "[i] Instant Replay: Aktifkan replay di menu terlebih dahulu.";
    toast_time_ = std::chrono::steady_clock::now();
}

bool RecorderController::spawn_helper_process(const HudConfig& cfg, float display_w, float display_h) {
    if (helper_pid_ > 0) {
        // Already running
        return true;
    }

    std::string bin_path = get_helper_binary_path();
    unlink(socket_path_.c_str());

    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        // Child Process: execute fpshud_recorder
        std::string w_str = std::to_string(static_cast<int>(display_w));
        std::string h_str = std::to_string(static_cast<int>(display_h));
        std::string fps_str = std::to_string((cfg.record_fps > 0) ? cfg.record_fps : 60);
        std::string br_str = std::to_string(cfg.record_bitrate_kbps);
        std::string dir_str = get_effective_record_dir(cfg);

        char* argv[] = {
            const_cast<char*>(bin_path.c_str()),
            const_cast<char*>("--socket"), const_cast<char*>(socket_path_.c_str()),
            const_cast<char*>("--width"), const_cast<char*>(w_str.c_str()),
            const_cast<char*>("--height"), const_cast<char*>(h_str.c_str()),
            const_cast<char*>("--fps"), const_cast<char*>(fps_str.c_str()),
            const_cast<char*>("--bitrate"), const_cast<char*>(br_str.c_str()),
            const_cast<char*>("--dir"), const_cast<char*>(dir_str.c_str()),
            nullptr
        };

        execvp(bin_path.c_str(), argv);
        // If execvp fails, try searching in ~/.local/bin directly
        std::string fallback = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.local/bin/fpshud_recorder";
        argv[0] = const_cast<char*>(fallback.c_str());
        execv(fallback.c_str(), argv);
        ::_exit(127);
    }

    helper_pid_ = pid;
    return true;
}

void RecorderController::connect_socket() {
    if (socket_fd_ >= 0) return;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    // Helper process needs a moment to initialize socket listener (up to 300ms)
    bool connected = false;
    for (int i = 0; i < 30; ++i) {
        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!connected) {
        close(fd);
        socket_fd_ = -1;
        return;
    }

    socket_fd_ = fd;
}

void RecorderController::disconnect_socket() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

void RecorderController::send_command(RecorderCommand cmd, const void* payload, size_t payload_sz) {
    if (socket_fd_ < 0) return;

    IpcPacketHeader hdr{};
    hdr.magic = kRecorderIpcMagic;
    hdr.command = static_cast<uint32_t>(cmd);
    hdr.payload_size = static_cast<uint32_t>(payload_sz);

    send(socket_fd_, &hdr, sizeof(hdr), 0);
    if (payload && payload_sz > 0) {
        send(socket_fd_, payload, payload_sz, 0);
    }
}

void RecorderController::on_frame_present(uint32_t width, uint32_t height, int dmabuf_fd) {
    if (!is_recording_.load(std::memory_order_relaxed)) {
        return;
    }

    recorded_frames_.fetch_add(1, std::memory_order_relaxed);

    if (socket_fd_ >= 0 && dmabuf_fd >= 0) {
        struct msghdr msg{};
        struct iovec iov[1];
        IpcPacketHeader hdr{};
        hdr.magic = kRecorderIpcMagic;
        hdr.command = static_cast<uint32_t>(RecorderCommand::SendFrame);
        hdr.payload_size = 0;

        iov[0].iov_base = &hdr;
        iov[0].iov_len = sizeof(hdr);
        msg.msg_iov = iov;
        msg.msg_iovlen = 1;

        union {
            struct cmsghdr cm;
            char control[CMSG_SPACE(sizeof(int))];
        } control_un;
        msg.msg_control = control_un.control;
        msg.msg_controllen = sizeof(control_un.control);

        struct cmsghdr* cmptr = CMSG_FIRSTHDR(&msg);
        cmptr->cmsg_len = CMSG_LEN(sizeof(int));
        cmptr->cmsg_level = SOL_SOCKET;
        cmptr->cmsg_type = SCM_RIGHTS;
        *reinterpret_cast<int*>(CMSG_DATA(cmptr)) = dmabuf_fd;

        sendmsg(socket_fd_, &msg, MSG_DONTWAIT);
    }
}

} // namespace fpshud
