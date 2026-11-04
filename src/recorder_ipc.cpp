#include "recorder_ipc.hpp"
#include "config.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
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

extern char** environ;

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

std::string get_secure_runtime_dir() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') {
        std::string dir = std::string(xdg) + "/fpshud";
        ::mkdir(dir.c_str(), 0700);
        ::chmod(dir.c_str(), 0700);
        return dir;
    }
    std::string fallback = "/tmp/fpshud-" + std::to_string(::getuid());
    ::mkdir(fallback.c_str(), 0700);
    ::chmod(fallback.c_str(), 0700);
    return fallback;
}

RecorderController& RecorderController::instance() {
    static RecorderController s_inst;
    return s_inst;
}

RecorderController::RecorderController() {
    std::string rdir = get_secure_runtime_dir();
    socket_path_ = rdir + "/fpshud_rec_" + std::to_string(getpid()) + ".sock";
    shm_path_ = rdir + "/fpshud_rec_shm_" + std::to_string(getpid());
}

RecorderController::~RecorderController() {
    stop_recording();
    cleanup_shm();
}

bool RecorderController::is_recording() const noexcept {
    return is_recording_.load(std::memory_order_relaxed);
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return last_saved_file_;
}

void RecorderController::set_toast(const std::string& msg) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    toast_msg_ = msg;
    toast_time_ = std::chrono::steady_clock::now();
}

bool RecorderController::get_recent_toast(std::string& out_msg) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (is_recording_.load(std::memory_order_relaxed)) {
        return;
    }

    if (!spawn_helper_process(cfg, display_w, display_h)) {
        is_recording_.store(false, std::memory_order_relaxed);
        set_toast("[!] Gagal menjalankan fpshud_recorder binary.");
        return;
    }

    connect_socket();
    if (socket_fd_ < 0) {
        if (helper_pid_ > 0) {
            kill(helper_pid_, SIGTERM);
            waitpid(helper_pid_, nullptr, WNOHANG);
            helper_pid_ = -1;
        }
        is_recording_.store(false, std::memory_order_relaxed);
        set_toast("[!] Gagal menghubungkan ke recorder daemon.");
        return;
    }

    IpcStartPayload payload{};
    payload.width = static_cast<uint32_t>(display_w);
    payload.height = static_cast<uint32_t>(display_h);
    payload.fps = (cfg.record_fps > 0) ? cfg.record_fps : 60;
    payload.bitrate_kbps = cfg.record_bitrate_kbps;
    payload.audio_source = cfg.record_audio_source;
    payload.container = cfg.record_container;
    payload.codec = static_cast<uint32_t>(cfg.record_codec);
    payload.quality_mode = static_cast<uint32_t>(cfg.record_quality_mode);
    payload.cq_level = static_cast<uint32_t>(cfg.record_cq_level);

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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_recording_.load(std::memory_order_relaxed)) {
        return;
    }

    if (socket_fd_ >= 0) {
        // Enforce 1.5s socket timeout so the game render thread NEVER freezes
        struct timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 500000;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

    // Clean up socket and shared memory files
    if (!socket_path_.empty()) {
        unlink(socket_path_.c_str());
    }
    cleanup_shm();

    if (!last_saved_file_.empty()) {
        toast_msg_ = "[✓] Video tersimpan: " + last_saved_file_;
    } else {
        toast_msg_ = "[✓] Perekaman dihentikan.";
    }
    toast_time_ = std::chrono::steady_clock::now();
}

bool build_container_host_env_paths(const std::string& root_prefix, std::string& out_ld_path, std::string& out_libva_path) {
    out_ld_path.clear();
    out_libva_path.clear();

    const char* const host_roots[] = {"/run/host", "/var/run/host"};
    const char* host_root = nullptr;
    std::error_code ec;

    for (const char* hr : host_roots) {
        std::string test_path = root_prefix + hr;
        if (fs::exists(test_path, ec)) {
            host_root = hr;
            break;
        }
    }

    if (!host_root) {
        return false;
    }

    static const char* const kLdCandidates[] = {
        "/usr/lib/x86_64-linux-gnu",
        "/usr/lib/x86_64-linux-gnu/pulseaudio",
        "/usr/lib64",
        "/usr/lib64/pulseaudio",
        "/usr/lib",
        "/usr/lib/pulseaudio"
    };

    static const char* const kLibvaCandidates[] = {
        "/usr/lib/x86_64-linux-gnu/dri",
        "/usr/lib64/dri",
        "/usr/lib/dri"
    };

    for (const char* cand : kLdCandidates) {
        std::string full_scan_path = root_prefix + host_root + cand;
        if (fs::exists(full_scan_path, ec)) {
            if (!out_ld_path.empty()) {
                out_ld_path += ":";
            }
            out_ld_path += std::string(host_root) + cand;
        }
    }

    for (const char* cand : kLibvaCandidates) {
        std::string full_scan_path = root_prefix + host_root + cand;
        if (fs::exists(full_scan_path, ec)) {
            if (!out_libva_path.empty()) {
                out_libva_path += ":";
            }
            out_libva_path += std::string(host_root) + cand;
        }
    }

    return (!out_ld_path.empty() || !out_libva_path.empty());
}

bool RecorderController::spawn_helper_process(const HudConfig& cfg, float display_w, float display_h) {
    if (helper_pid_ > 0) {
        int status = 0;
        pid_t p = waitpid(helper_pid_, &status, WNOHANG);
        if (p == 0) {
            return true;
        }
        helper_pid_ = -1;
    }

    std::string bin_path = get_helper_binary_path();
    unlink(socket_path_.c_str());

    // Prepare shared memory for zero-copy frame transport
    setup_shm(static_cast<uint32_t>(display_w), static_cast<uint32_t>(display_h));

    std::string w_str = std::to_string(static_cast<int>(display_w));
    std::string h_str = std::to_string(static_cast<int>(display_h));
    std::string fps_str = std::to_string((cfg.record_fps > 0) ? cfg.record_fps : 60);
    std::string br_str = std::to_string(cfg.record_bitrate_kbps);
    std::string codec_str = (cfg.record_codec == 1) ? "hevc" : "h264";
    std::string qmode_str = std::to_string(cfg.record_quality_mode);
    std::string cq_str = std::to_string(cfg.record_cq_level);
    std::string dir_str = get_effective_record_dir(cfg);

    char* argv[] = {
        const_cast<char*>(bin_path.c_str()),
        const_cast<char*>("--socket"), const_cast<char*>(socket_path_.c_str()),
        const_cast<char*>("--shm"), const_cast<char*>(shm_path_.c_str()),
        const_cast<char*>("--width"), const_cast<char*>(w_str.c_str()),
        const_cast<char*>("--height"), const_cast<char*>(h_str.c_str()),
        const_cast<char*>("--fps"), const_cast<char*>(fps_str.c_str()),
        const_cast<char*>("--bitrate"), const_cast<char*>(br_str.c_str()),
        const_cast<char*>("--codec"), const_cast<char*>(codec_str.c_str()),
        const_cast<char*>("--cqp-mode"), const_cast<char*>(qmode_str.c_str()),
        const_cast<char*>("--cqp"), const_cast<char*>(cq_str.c_str()),
        const_cast<char*>("--dir"), const_cast<char*>(dir_str.c_str()),
        nullptr
    };

    // Prepare clean environment variables with container / pressure-vessel library path support
    std::vector<std::string> env_strings;
    std::string new_ld_path;
    std::string host_libva_path;

    build_container_host_env_paths("", new_ld_path, host_libva_path);

    const char* cur_ld = std::getenv("LD_LIBRARY_PATH");
    if (cur_ld && *cur_ld) {
        if (!new_ld_path.empty()) {
            new_ld_path += ":";
        }
        new_ld_path += cur_ld;
    }

    const char* cur_libva = std::getenv("LIBVA_DRIVERS_PATH");
    bool has_user_libva = (cur_libva && *cur_libva);

    for (char** ep = environ; ep && *ep; ++ep) {
        std::string e = *ep;
        if (e.rfind("LD_LIBRARY_PATH=", 0) == 0) {
            continue; // We will add our updated LD_LIBRARY_PATH
        }
        if (e.rfind("LD_PRELOAD=", 0) == 0) {
            continue; // Do not leak game's LD_PRELOAD into daemon
        }
        if (!has_user_libva && e.rfind("LIBVA_DRIVERS_PATH=", 0) == 0) {
            continue; // Will inject container LIBVA_DRIVERS_PATH if needed
        }
        env_strings.push_back(std::move(e));
    }

    if (!new_ld_path.empty()) {
        env_strings.push_back("LD_LIBRARY_PATH=" + new_ld_path);
    }

    if (!has_user_libva && !host_libva_path.empty()) {
        env_strings.push_back("LIBVA_DRIVERS_PATH=" + host_libva_path);
    }

    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    pid_t pid = 0;
    int res = posix_spawnp(&pid, bin_path.c_str(), nullptr, nullptr, argv, envp.data());
    if (res != 0 || pid <= 0) {
        // Fallback to ~/.local/bin/fpshud_recorder or ~/.local/lib/fpshud/fpshud_recorder
        std::string fallback = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.local/bin/fpshud_recorder";
        argv[0] = const_cast<char*>(fallback.c_str());
        res = posix_spawnp(&pid, fallback.c_str(), nullptr, nullptr, argv, envp.data());
        if (res != 0 || pid <= 0) {
            std::string fallback2 = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.local/lib/fpshud/fpshud_recorder";
            argv[0] = const_cast<char*>(fallback2.c_str());
            res = posix_spawnp(&pid, fallback2.c_str(), nullptr, nullptr, argv, envp.data());
            if (res != 0 || pid <= 0) {
                return false;
            }
        }
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

    // Helper process needs a moment to initialize socket listener (up to 250ms)
    bool connected = false;
    for (int i = 0; i < 25; ++i) {
        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            connected = true;
            break;
        }
        // Early abort if helper child process terminated prematurely (e.g. library missing)
        if (helper_pid_ > 0) {
            int st = 0;
            if (waitpid(helper_pid_, &st, WNOHANG) > 0) {
                helper_pid_ = -1;
                break;
            }
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

    send(socket_fd_, &hdr, sizeof(hdr), MSG_NOSIGNAL);
    if (payload && payload_sz > 0) {
        send(socket_fd_, payload, payload_sz, MSG_NOSIGNAL);
    }
}

bool RecorderController::setup_shm(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384) return false;
    if (shm_base_ && shm_width_ == width && shm_height_ == height) {
        return true;
    }

    cleanup_shm();

    std::string rdir = get_secure_runtime_dir();
    shm_path_ = rdir + "/fpshud_rec_shm_" + std::to_string(getpid());
    size_t slot_sz = static_cast<size_t>(width) * height * 4;
    size_t total_sz = 4096 + slot_sz * 3;

    shm_fd_ = open(shm_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (shm_fd_ < 0) {
        return false;
    }

    if (ftruncate(shm_fd_, total_sz) != 0) {
        close(shm_fd_);
        shm_fd_ = -1;
        unlink(shm_path_.c_str());
        return false;
    }

    shm_base_ = mmap(nullptr, total_sz, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (shm_base_ == MAP_FAILED) {
        shm_base_ = nullptr;
        close(shm_fd_);
        shm_fd_ = -1;
        unlink(shm_path_.c_str());
        return false;
    }

    shm_size_ = total_sz;
    shm_width_ = width;
    shm_height_ = height;

    auto* hdr = reinterpret_cast<ShmFrameHeader*>(shm_base_);
    hdr->magic = kShmFrameMagic;
    hdr->width = width;
    hdr->height = height;
    hdr->format = 0;
    hdr->flip_y = 0;
    hdr->slot_size = slot_sz;
    hdr->write_slot.store(0, std::memory_order_relaxed);
    hdr->sequence.store(0, std::memory_order_relaxed);
    hdr->offset[0] = 4096;
    hdr->offset[1] = 4096 + slot_sz;
    hdr->offset[2] = 4096 + slot_sz * 2;

    return true;
}

void RecorderController::cleanup_shm() {
    if (shm_base_ && shm_size_ > 0) {
        munmap(shm_base_, shm_size_);
        shm_base_ = nullptr;
        shm_size_ = 0;
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_fd_ = -1;
    }
    if (!shm_path_.empty()) {
        unlink(shm_path_.c_str());
        shm_path_.clear();
    }
    shm_width_ = 0;
    shm_height_ = 0;
}

void RecorderController::submit_frame(const void* pixels, uint32_t width, uint32_t height, uint32_t format, bool flip_y) {
    if (!is_recording_.load(std::memory_order_relaxed) || !pixels || width == 0 || height == 0) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_recording_.load(std::memory_order_relaxed) || socket_fd_ < 0) {
        return;
    }

    if (!shm_base_ || shm_width_ != width || shm_height_ != height) {
        if (!setup_shm(width, height)) {
            return;
        }
    }

    auto* hdr = reinterpret_cast<ShmFrameHeader*>(shm_base_);
    if (!hdr) return;

    uint32_t cur_slot = hdr->write_slot.load(std::memory_order_relaxed);
    uint32_t next_slot = (cur_slot + 1) % 3;

    size_t frame_bytes = static_cast<size_t>(width) * height * 4;
    size_t slot_offset = hdr->offset[next_slot];
    if (slot_offset > shm_size_ || frame_bytes > (shm_size_ - slot_offset)) {
        return; // Guard against SHM buffer overflow and integer wrap-around
    }

    uint8_t* dst = reinterpret_cast<uint8_t*>(shm_base_) + slot_offset;
    std::memcpy(dst, pixels, frame_bytes);

    hdr->format = format;
    hdr->flip_y = flip_y ? 1 : 0;
    hdr->write_slot.store(next_slot, std::memory_order_release);
    uint64_t s = hdr->sequence.fetch_add(1, std::memory_order_release) + 1;

    recorded_frames_.fetch_add(1, std::memory_order_relaxed);

    if (socket_fd_ >= 0) {
        IpcFrameNotify notify{};
        notify.hdr.magic = kRecorderIpcMagic;
        notify.hdr.command = static_cast<uint32_t>(RecorderCommand::SendFrame);
        notify.hdr.payload_size = sizeof(notify) - sizeof(IpcPacketHeader);
        notify.slot = next_slot;
        notify.width = width;
        notify.height = height;
        notify.format = format;
        notify.flip_y = flip_y ? 1 : 0;
        notify.seq = s;

        send(socket_fd_, &notify, sizeof(notify), MSG_DONTWAIT | MSG_NOSIGNAL);
    }
}

void RecorderController::on_frame_present(uint32_t width, uint32_t height, int dmabuf_fd) {
    (void)width;
    (void)height;
    (void)dmabuf_fd;
    if (!is_recording_.load(std::memory_order_relaxed)) {
        return;
    }
    recorded_frames_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace fpshud
