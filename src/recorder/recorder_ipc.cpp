#include "recorder_ipc.hpp"
#include "config.hpp"
#include "png_writer.hpp"

#include <ctime>
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

std::string get_helper_binary_path() {
  // 1. Check next to running libfpshud.so via dladdr
  Dl_info info;
  if (dladdr(reinterpret_cast<void*>(&get_helper_binary_path), &info) && info.dli_fname) {
    std::error_code ec;
    fs::path base = fs::canonical(fs::path(info.dli_fname), ec);
    if (ec) {
      base = fs::absolute(fs::path(info.dli_fname), ec);
    }

    fs::path p = base.parent_path() / "fpshud_recorder";
    if (fs::exists(p, ec)) return fs::canonical(p, ec).string();

    fs::path p_bin = base.parent_path().parent_path() / "bin" / "fpshud_recorder";
    if (fs::exists(p_bin, ec)) return fs::canonical(p_bin, ec).string();

    fs::path p_prefix_bin =
        base.parent_path().parent_path().parent_path() / "bin" / "fpshud_recorder";
    if (fs::exists(p_prefix_bin, ec)) return fs::canonical(p_prefix_bin, ec).string();
  }

  // 2. Standard user installation: ~/.local/bin/fpshud_recorder &
  // ~/.local/lib/fpshud/fpshud_recorder
  if (const char* home = std::getenv("HOME"); home && *home) {
    fs::path p1 = fs::path(home) / ".local" / "bin" / "fpshud_recorder";
    std::error_code ec;
    if (fs::exists(p1, ec)) return fs::canonical(p1, ec).string();

    fs::path p2 = fs::path(home) / ".local" / "lib" / "fpshud" / "fpshud_recorder";
    if (fs::exists(p2, ec)) return fs::canonical(p2, ec).string();
  }

  // 3. Fallback to system PATH lookup (relative ./ search removed for security hardening)
  return "fpshud_recorder";
}

namespace {}  // namespace

std::string get_secure_runtime_dir() {
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg && xdg[0] != '\0') {
    std::string dir = std::string(xdg) + "/fpshud";
    ::mkdir(dir.c_str(), 0700);
    ::chmod(dir.c_str(), 0700);
    return dir;
  }

  std::string fallback = "/tmp/fpshud-" + std::to_string(::getuid());
  struct stat st{};
  if (::lstat(fallback.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && st.st_uid == ::getuid() &&
        ((st.st_mode & 0777) == 0700)) {
      return fallback;
    }
    if (st.st_uid == ::getuid() && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
      if (::chmod(fallback.c_str(), 0700) == 0) {
        return fallback;
      }
    }
  } else {
    if (::mkdir(fallback.c_str(), 0700) == 0) {
      ::chmod(fallback.c_str(), 0700);
      return fallback;
    }
  }

  // Untrusted or compromised fallback; create a secure private directory via mkdtemp
  char tmpl[] = "/tmp/fpshud-sec-XXXXXX";
  char* safe_dir = ::mkdtemp(tmpl);
  if (safe_dir) {
    ::chmod(safe_dir, 0700);
    return std::string(safe_dir);
  }
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
  if (socket_fd_ >= 0) {
    ::shutdown(socket_fd_, SHUT_RDWR);
  }
  if (replay_save_thread_.joinable()) {
    replay_save_thread_.join();
  }
  if (is_replay_active_.load(std::memory_order_relaxed)) {
    is_replay_active_.store(false, std::memory_order_relaxed);
    disconnect_socket();
    if (helper_pid_ > 0) {
      kill(helper_pid_, SIGTERM);
      int status = 0;
      ::waitpid(helper_pid_, &status, 0);
      helper_pid_ = -1;
    }
  }
  if (save_thread_.joinable()) {
    save_thread_.join();
  }
  if (screenshot_thread_.joinable()) {
    screenshot_thread_.join();
  }
  cleanup_shm();
}

bool RecorderController::is_recording() const noexcept {
  return is_recording_.load(std::memory_order_relaxed);
}

bool RecorderController::is_replay_active() const noexcept {
  return is_replay_active_.load(std::memory_order_relaxed);
}

bool RecorderController::is_capturing_frames() const noexcept {
  return is_recording_.load(std::memory_order_relaxed) ||
         is_replay_active_.load(std::memory_order_relaxed);
}

float RecorderController::get_recording_duration() const {
  if (!is_recording_.load(std::memory_order_relaxed)) {
    return 0.0f;
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
  has_active_toast_.store(!msg.empty(), std::memory_order_release);
}

bool RecorderController::get_recent_toast(std::string& out_msg) const {
  if (!has_active_toast_.load(std::memory_order_acquire)) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (toast_msg_.empty()) {
    has_active_toast_.store(false, std::memory_order_release);
    return false;
  }
  auto now = std::chrono::steady_clock::now();
  double sec = std::chrono::duration<double>(now - toast_time_).count();
  if (sec <= 3.8) {
    out_msg = toast_msg_;
    return true;
  }
  has_active_toast_.store(false, std::memory_order_release);
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
  std::thread prev_save;
  std::thread prev_replay_save;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (is_recording_.load(std::memory_order_relaxed)) {
      return;
    }
    if (socket_fd_ >= 0 && is_replay_active_.load(std::memory_order_relaxed) &&
        is_saving_replay_.load(std::memory_order_relaxed)) {
      ::shutdown(socket_fd_, SHUT_RDWR);
    }
    if (replay_save_thread_.joinable()) {
      prev_replay_save = std::move(replay_save_thread_);
    }
    if (save_thread_.joinable()) {
      prev_save = std::move(save_thread_);
    }
  }
  if (prev_save.joinable()) {
    prev_save.join();
  }
  if (prev_replay_save.joinable()) {
    prev_replay_save.join();
  }

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (is_recording_.load(std::memory_order_relaxed)) {
    return;
  }

  // If Instant Replay daemon is currently running, cleanly shut it down before starting manual
  // recording
  if (is_replay_active_.load(std::memory_order_relaxed)) {
    is_replay_active_.store(false, std::memory_order_relaxed);
    active_replay_duration_sec_ = 0;
    disconnect_socket();
    if (helper_pid_ > 0) {
      pid_t rpid = helper_pid_;
      helper_pid_ = -1;
      kill(rpid, SIGTERM);
      int status = 0;
      ::waitpid(rpid, &status, 0);
    }
    cleanup_shm();
  }

  is_finalizing_.store(false, std::memory_order_relaxed);

  if (!spawn_helper_process(cfg, display_w, display_h)) {
    cleanup_shm();
    is_recording_.store(false, std::memory_order_relaxed);
    set_toast("[!] Failed to launch fpshud_recorder binary.");
    return;
  }

  connect_socket();
  if (socket_fd_ < 0) {
    if (helper_pid_ > 0) {
      pid_t pid = helper_pid_;
      kill(pid, SIGTERM);
      std::thread([pid]() {
        int status = 0;
        ::waitpid(pid, &status, 0);
      }).detach();
      helper_pid_ = -1;
    }
    cleanup_shm();
    is_recording_.store(false, std::memory_order_relaxed);
    set_toast("[!] Failed to connect to recorder daemon.");
    return;
  }

  IpcStartPayload payload{};
  resolve_recording_dimensions(cfg.record_resolution, static_cast<int>(display_w),
                               static_cast<int>(display_h), payload.width, payload.height);
  payload.fps = (cfg.record_fps > 0) ? cfg.record_fps : 60;
  payload.bitrate_kbps = cfg.record_bitrate_kbps;
  payload.audio_source = cfg.record_audio_source;
  payload.container = cfg.record_container;
  payload.codec = static_cast<uint32_t>(cfg.record_codec);
  payload.quality_mode = static_cast<uint32_t>(cfg.record_quality_mode);
  payload.cq_level = static_cast<uint32_t>(cfg.record_cq_level);
  payload.vfr_mode = cfg.record_adaptive_vfr ? 1 : 0;
  payload.vfr_max_fps =
      static_cast<uint32_t>(cfg.record_vfr_max_fps > 0 ? cfg.record_vfr_max_fps : 120);

  std::string save_dir = get_effective_record_dir(cfg);
  std::error_code ec;
  fs::create_directories(save_dir, ec);
  std::snprintf(payload.save_dir, sizeof(payload.save_dir), "%s", save_dir.c_str());
  std::snprintf(payload.filename_prefix, sizeof(payload.filename_prefix), "FPSHUD");

  send_command(RecorderCommand::StartRecord, &payload, sizeof(payload));

  is_recording_.store(true, std::memory_order_relaxed);
  record_start_time_ = std::chrono::steady_clock::now();
  recorded_frames_.store(0, std::memory_order_relaxed);

  toast_msg_ = "[REC] Recording started (Press F7 to stop)";
  toast_time_ = std::chrono::steady_clock::now();
  has_active_toast_.store(true, std::memory_order_release);
}

void RecorderController::stop_recording() {
  std::thread prev_save;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_recording_.load(std::memory_order_relaxed)) {
      return;
    }
    if (save_thread_.joinable()) {
      prev_save = std::move(save_thread_);
    }
  }
  if (prev_save.joinable()) {
    prev_save.join();
  }

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!is_recording_.load(std::memory_order_relaxed)) {
    return;
  }

  is_recording_.store(false, std::memory_order_relaxed);
  is_finalizing_.store(true, std::memory_order_relaxed);

  int sfd = socket_fd_;
  socket_fd_ = -1;
  pid_t lingering_pid = helper_pid_;
  helper_pid_ = -1;
  std::string s_path = socket_path_;
  // Note: socket_path_ is intentionally preserved so subsequent recordings can reuse it!

  // Show immediate non-blocking toast indicating background save in progress
  toast_msg_ = "[REC] Saving video in background...";
  toast_time_ = std::chrono::steady_clock::now();
  has_active_toast_.store(true, std::memory_order_release);

  // Offload final socket communication, disk remuxing wait, child process reaping,
  // and shared memory cleanup to background thread so game render thread never freezes or drops
  // tail frames.
  save_thread_ = std::thread([this, sfd, lingering_pid, s_path]() {
    std::string saved_path;
    if (sfd >= 0) {
      struct timeval tv{};
      tv.tv_sec = 10;
      tv.tv_usec = 0;
      setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      IpcPacketHeader stop_hdr{};
      stop_hdr.magic = kRecorderIpcMagic;
      stop_hdr.command = static_cast<uint32_t>(RecorderCommand::StopRecord);
      stop_hdr.payload_size = 0;
      ::send(sfd, &stop_hdr, sizeof(stop_hdr), MSG_NOSIGNAL);

      IpcStatusResponse resp{};
      ssize_t n = ::recv(sfd, &resp, sizeof(resp), MSG_WAITALL);
      if (n >= static_cast<ssize_t>(sizeof(IpcStatusResponse)) && resp.magic == kRecorderIpcMagic) {
        if (resp.last_saved_file[0] != '\0') {
          saved_path = resp.last_saved_file;
        }
      }
      ::close(sfd);
    }

    if (lingering_pid > 0) {
      int status = 0;
      ::waitpid(lingering_pid, &status, 0);
    }

    if (!s_path.empty()) {
      ::unlink(s_path.c_str());
    }

    // Now that the daemon has finalized the video, clean up shared memory
    cleanup_shm();

    {
      std::lock_guard<std::recursive_mutex> lk(this->mutex_);
      if (!saved_path.empty()) {
        this->last_saved_file_ = saved_path;
        this->toast_msg_ = "[✓] Video saved: " + saved_path;
        this->toast_time_ = std::chrono::steady_clock::now();
        this->has_active_toast_.store(true, std::memory_order_release);
      }
      this->is_finalizing_.store(false, std::memory_order_relaxed);
    }
  });
}

bool build_container_host_env_paths(const std::string& root_prefix, std::string& out_ld_path,
                                    std::string& out_libva_path) {
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

  static const char* const kLdCandidates[] = {"/usr/lib/x86_64-linux-gnu",
                                              "/usr/lib/x86_64-linux-gnu/pulseaudio",
                                              "/usr/lib64",
                                              "/usr/lib64/pulseaudio",
                                              "/usr/lib",
                                              "/usr/lib/pulseaudio"};

  static const char* const kLibvaCandidates[] = {"/usr/lib/x86_64-linux-gnu/dri", "/usr/lib64/dri",
                                                 "/usr/lib/dri"};

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

bool RecorderController::spawn_helper_process(const HudConfig& cfg, float display_w,
                                              float display_h) {
  if (helper_pid_ > 0) {
    int status = 0;
    pid_t p = waitpid(helper_pid_, &status, WNOHANG);
    if (p == 0) {
      return true;
    }
    helper_pid_ = -1;
  }

  if (socket_path_.empty()) {
    std::string rdir = get_secure_runtime_dir();
    socket_path_ = rdir + "/fpshud_rec_" + std::to_string(getpid()) + ".sock";
  }
  if (shm_path_.empty()) {
    std::string rdir = get_secure_runtime_dir();
    shm_path_ = rdir + "/fpshud_rec_shm_" + std::to_string(getpid());
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

  char* argv[] = {const_cast<char*>(bin_path.c_str()),     const_cast<char*>("--socket"),
                  const_cast<char*>(socket_path_.c_str()), const_cast<char*>("--shm"),
                  const_cast<char*>(shm_path_.c_str()),    const_cast<char*>("--width"),
                  const_cast<char*>(w_str.c_str()),        const_cast<char*>("--height"),
                  const_cast<char*>(h_str.c_str()),        const_cast<char*>("--fps"),
                  const_cast<char*>(fps_str.c_str()),      const_cast<char*>("--bitrate"),
                  const_cast<char*>(br_str.c_str()),       const_cast<char*>("--codec"),
                  const_cast<char*>(codec_str.c_str()),    const_cast<char*>("--cqp-mode"),
                  const_cast<char*>(qmode_str.c_str()),    const_cast<char*>("--cqp"),
                  const_cast<char*>(cq_str.c_str()),       const_cast<char*>("--dir"),
                  const_cast<char*>(dir_str.c_str()),      nullptr};

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
      continue;  // We will add our updated LD_LIBRARY_PATH
    }
    if (e.rfind("LD_PRELOAD=", 0) == 0) {
      continue;  // Do not leak game's LD_PRELOAD into daemon
    }
    if (!has_user_libva && e.rfind("LIBVA_DRIVERS_PATH=", 0) == 0) {
      continue;  // Will inject container LIBVA_DRIVERS_PATH if needed
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
    std::string fallback =
        std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.local/bin/fpshud_recorder";
    argv[0] = const_cast<char*>(fallback.c_str());
    res = posix_spawnp(&pid, fallback.c_str(), nullptr, nullptr, argv, envp.data());
    if (res != 0 || pid <= 0) {
      std::string fallback2 = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") +
                              "/.local/lib/fpshud/fpshud_recorder";
      argv[0] = const_cast<char*>(fallback2.c_str());
      res = posix_spawnp(&pid, fallback2.c_str(), nullptr, nullptr, argv, envp.data());
      if (res != 0 || pid <= 0) {
        cleanup_shm();
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

void RecorderController::submit_frame(const void* pixels, uint32_t width, uint32_t height,
                                      uint32_t format, bool flip_y) {
  if (!is_capturing_frames() || !pixels || width == 0 || height == 0) {
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!is_capturing_frames() || socket_fd_ < 0) {
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
    return;  // Guard against SHM buffer overflow and integer wrap-around
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
    notify.timestamp_us =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());

    send(socket_fd_, &notify, sizeof(notify), MSG_DONTWAIT | MSG_NOSIGNAL);
  }
}

void RecorderController::sync_replay_state(const HudConfig& cfg, float display_w, float display_h) {
  // Fast lock-free check on common steady-state path
  bool want_replay = cfg.replay_enabled && !is_recording_.load(std::memory_order_relaxed);
  bool cur_replay = is_replay_active_.load(std::memory_order_relaxed);
  if (!want_replay && !cur_replay) {
    return;
  }
  if (want_replay && cur_replay && active_replay_duration_sec_ == cfg.replay_duration_sec) {
    return;
  }
  if (is_saving_replay_.load(std::memory_order_relaxed)) {
    return;
  }

  std::thread finished_replay_save;
  std::thread finished_save;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (is_saving_replay_.load(std::memory_order_relaxed)) {
      return;
    }
    if (replay_save_thread_.joinable()) {
      finished_replay_save = std::move(replay_save_thread_);
    }
    if (save_thread_.joinable()) {
      finished_save = std::move(save_thread_);
    }
  }
  if (finished_replay_save.joinable()) {
    finished_replay_save.join();
  }
  if (finished_save.joinable()) {
    finished_save.join();
  }

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (is_recording_.load(std::memory_order_relaxed) ||
      is_finalizing_.load(std::memory_order_relaxed) ||
      is_saving_replay_.load(std::memory_order_relaxed)) {
    return;
  }

  // Stop replay if disabled or duration changed
  if (is_replay_active_.load(std::memory_order_relaxed) &&
      (!cfg.replay_enabled || active_replay_duration_sec_ != cfg.replay_duration_sec)) {
    is_replay_active_.store(false, std::memory_order_relaxed);
    active_replay_duration_sec_ = 0;
    disconnect_socket();
    if (helper_pid_ > 0) {
      pid_t rpid = helper_pid_;
      helper_pid_ = -1;
      kill(rpid, SIGTERM);
      if (cfg.replay_enabled) {
        int status = 0;
        ::waitpid(rpid, &status, 0);
      } else {
        std::thread([rpid]() {
          int status = 0;
          ::waitpid(rpid, &status, 0);
        }).detach();
      }
    }
    if (!socket_path_.empty()) {
      ::unlink(socket_path_.c_str());
    }
    cleanup_shm();
    if (!cfg.replay_enabled) {
      set_toast("[REPLAY] Instant Replay disabled (0 MB RAM).");
      return;
    }
  }

  if (cfg.replay_enabled && !is_replay_active_.load(std::memory_order_relaxed) &&
      display_w >= 64.0f && display_h >= 64.0f) {
    if (!spawn_helper_process(cfg, display_w, display_h)) {
      cleanup_shm();
      return;
    }
    connect_socket();
    if (socket_fd_ < 0) {
      if (helper_pid_ > 0) {
        pid_t rpid = helper_pid_;
        helper_pid_ = -1;
        kill(rpid, SIGTERM);
        std::thread([rpid]() {
          int status = 0;
          ::waitpid(rpid, &status, 0);
        }).detach();
      }
      cleanup_shm();
      return;
    }

    IpcStartPayload payload{};
    resolve_recording_dimensions(cfg.record_resolution, static_cast<int>(display_w),
                                 static_cast<int>(display_h), payload.width, payload.height);
    payload.fps = (cfg.record_fps > 0) ? cfg.record_fps : 60;
    payload.bitrate_kbps = cfg.record_bitrate_kbps;
    payload.audio_source = cfg.record_audio_source;
    payload.container = cfg.record_container;
    payload.codec = static_cast<uint32_t>(cfg.record_codec);
    payload.quality_mode = static_cast<uint32_t>(cfg.record_quality_mode);
    payload.cq_level = static_cast<uint32_t>(cfg.record_cq_level);
    payload.vfr_mode = cfg.record_adaptive_vfr ? 1 : 0;
    payload.vfr_max_fps =
        static_cast<uint32_t>(cfg.record_vfr_max_fps > 0 ? cfg.record_vfr_max_fps : 120);
    payload.replay_mode = 1;
    payload.replay_duration_sec =
        static_cast<uint32_t>(std::clamp(cfg.replay_duration_sec, 15, 120));

    std::string save_dir = get_effective_record_dir(cfg);
    std::error_code ec;
    fs::create_directories(save_dir, ec);
    std::snprintf(payload.save_dir, sizeof(payload.save_dir), "%s", save_dir.c_str());
    std::snprintf(payload.filename_prefix, sizeof(payload.filename_prefix), "Replay");

    send_command(RecorderCommand::StartRecord, &payload, sizeof(payload));
    is_replay_active_.store(true, std::memory_order_relaxed);
    active_replay_duration_sec_ = cfg.replay_duration_sec;
    set_toast("[REPLAY] Instant Replay active (" + std::to_string(cfg.replay_duration_sec) + "s)");
  }
}

void RecorderController::trigger_save_replay() {
  if (!is_replay_active_.load(std::memory_order_relaxed)) {
    set_toast("[!] Instant Replay is OFF");
    return;
  }
  if (is_saving_replay_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  std::thread prev_thread;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (replay_save_thread_.joinable()) {
      prev_thread = std::move(replay_save_thread_);
    }
  }
  if (prev_thread.joinable()) {
    prev_thread.join();
  }

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!is_replay_active_.load(std::memory_order_relaxed) || socket_fd_ < 0) {
    is_saving_replay_.store(false, std::memory_order_release);
    set_toast("[!] Instant Replay is OFF");
    return;
  }

  struct timeval tv{};
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  send_command(RecorderCommand::SaveReplay, nullptr, 0);
  set_toast("[REPLAY] Saving Instant Replay clip...");
  int sfd = socket_fd_;

  replay_save_thread_ = std::thread([this, sfd]() {
    IpcStatusResponse resp{};
    ssize_t n = ::recv(sfd, &resp, sizeof(resp), 0);
    if (n >= static_cast<ssize_t>(sizeof(IpcStatusResponse)) && resp.magic == kRecorderIpcMagic &&
        resp.last_saved_file[0] != '\0') {
      std::lock_guard<std::recursive_mutex> guard(mutex_);
      last_saved_file_ = resp.last_saved_file;
      set_toast("[✓] Replay saved: " + last_saved_file_);
    } else {
      set_toast("[!] Replay buffer does not have enough keyframes yet.");
    }
    is_saving_replay_.store(false, std::memory_order_release);
  });
}

void RecorderController::request_screenshot() noexcept {
  screenshot_requested_.store(true, std::memory_order_release);
}

bool RecorderController::consume_screenshot_request() noexcept {
  return screenshot_requested_.exchange(false, std::memory_order_acq_rel);
}

void RecorderController::submit_screenshot(const void* pixels, uint32_t width, uint32_t height,
                                           uint32_t format, bool flip_y,
                                           const std::string& save_dir) {
  if (!pixels || width == 0 || height == 0 || width > 16384 || height > 16384) {
    return;
  }

  size_t total_bytes = static_cast<size_t>(width) * height * 4;
  std::vector<uint8_t> raw_copy(total_bytes);
  std::memcpy(raw_copy.data(), pixels, total_bytes);

  std::string target_dir = save_dir;
  if (target_dir.empty()) {
    HudConfig def_cfg;
    target_dir = get_effective_record_dir(def_cfg);
  }

  std::thread prev_thread;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (screenshot_thread_.joinable()) {
      prev_thread = std::move(screenshot_thread_);
    }
  }
  if (prev_thread.joinable()) {
    prev_thread.join();
  }

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  screenshot_thread_ =
      std::thread([this, buf = std::move(raw_copy), width, height, format, flip_y, target_dir]() {
        std::error_code ec;
        fs::create_directories(target_dir, ec);

        std::time_t now_t = std::time(nullptr);
        struct tm tm_buf{};
        localtime_r(&now_t, &tm_buf);
        char ts[64] = {0};
        std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_buf);

        std::string out_path = target_dir + "/Screenshot_" + ts + ".png";
        bool ok = write_rgba_to_png(out_path, buf.data(), width, height, format, flip_y);

        std::lock_guard<std::recursive_mutex> lk(this->mutex_);
        if (ok) {
          this->last_saved_file_ = out_path;
          this->toast_msg_ = "[✓] Screenshot saved: " + out_path;
        } else {
          this->toast_msg_ = "[!] Failed to save PNG screenshot.";
        }
        this->toast_time_ = std::chrono::steady_clock::now();
        this->has_active_toast_.store(true, std::memory_order_release);
      });
}

// ----------------------------------------------------------------------------
// VideoPlayerController Implementation
// ----------------------------------------------------------------------------

VideoPlayerController::VideoPlayerController() = default;

VideoPlayerController::~VideoPlayerController() { close(); }

bool VideoPlayerController::open(const std::string& filepath, const std::string& helper_bin_path) {
  close();

  if (filepath.empty() || helper_bin_path.empty()) {
    return false;
  }

  pid_t my_pid = ::getpid();
  auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();

  shm_name_ =
      std::string(kPlaybackShmPrefix) + std::to_string(my_pid) + "_" + std::to_string(now_us);
  std::string runtime_dir = get_secure_runtime_dir();
  socket_path_ = runtime_dir + "/" + kPlaybackSocketPrefix + std::to_string(my_pid) + "_" +
                 std::to_string(now_us) + ".sock";

  const char* argv[] = {helper_bin_path.c_str(), "--player",       shm_name_.c_str(),
                        socket_path_.c_str(),    filepath.c_str(), nullptr};

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
      continue;
    }
    if (e.rfind("LD_PRELOAD=", 0) == 0) {
      continue;  // Do not leak game's LD_PRELOAD into player worker
    }
    if (!has_user_libva && e.rfind("LIBVA_DRIVERS_PATH=", 0) == 0) {
      continue;
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

  int res = ::posix_spawnp(&worker_pid_, helper_bin_path.c_str(), nullptr, nullptr,
                           const_cast<char* const*>(argv), envp.data());
  if (res != 0 || worker_pid_ <= 0) {
    worker_pid_ = -1;
    close();
    return false;
  }

  bool shm_ready = false;
  auto start_wait = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start_wait < std::chrono::milliseconds(1500)) {
    int status = 0;
    pid_t wp = ::waitpid(worker_pid_, &status, WNOHANG);
    if (wp > 0) {
      worker_pid_ = -1;
      close();
      return false;
    }

    if (shm_fd_ < 0) {
      shm_fd_ = ::shm_open(shm_name_.c_str(), O_RDWR, 0600);
    }
    if (shm_fd_ >= 0) {
      struct stat st{};
      if (::fstat(shm_fd_, &st) == 0 &&
          static_cast<size_t>(st.st_size) >= sizeof(PlaybackShmHeader)) {
        shm_size_ = static_cast<size_t>(st.st_size);
        void* ptr = ::mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
        if (ptr != MAP_FAILED) {
          shm_header_ = reinterpret_cast<PlaybackShmHeader*>(ptr);
          if (shm_header_->magic == kPlaybackShmMagic) {
            shm_pixels_ = reinterpret_cast<uint8_t*>(ptr) + sizeof(PlaybackShmHeader);
            shm_ready = true;
            break;
          } else {
            ::munmap(ptr, shm_size_);
            shm_header_ = nullptr;
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (!shm_ready) {
    close();
    return false;
  }

  sock_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd_ >= 0) {
    sockaddr_un saddr{};
    saddr.sun_family = AF_UNIX;
    std::strncpy(saddr.sun_path, socket_path_.c_str(), sizeof(saddr.sun_path) - 1);
    auto sock_wait = std::chrono::steady_clock::now();
    bool connected = false;
    while (std::chrono::steady_clock::now() - sock_wait < std::chrono::milliseconds(500)) {
      if (::connect(sock_fd_, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) == 0) {
        connected = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!connected) {
      ::close(sock_fd_);
      sock_fd_ = -1;
    }
  }

  last_consumed_seq_ = 0;
  return true;
}

void VideoPlayerController::close() {
  if (worker_pid_ > 0) {
    send_action(IpcPlaybackAction::Close, 0.0f);
    if (sock_fd_ >= 0) {
      ::close(sock_fd_);
      sock_fd_ = -1;
    }
    auto start_wait = std::chrono::steady_clock::now();
    bool exited = false;
    while (std::chrono::steady_clock::now() - start_wait < std::chrono::milliseconds(150)) {
      int status = 0;
      if (::waitpid(worker_pid_, &status, WNOHANG) > 0) {
        exited = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!exited) {
      ::kill(worker_pid_, SIGTERM);
      int status = 0;
      ::waitpid(worker_pid_, &status, 0);
    }
    worker_pid_ = -1;
  }

  if (sock_fd_ >= 0) {
    ::close(sock_fd_);
    sock_fd_ = -1;
  }
  if (!socket_path_.empty()) {
    ::unlink(socket_path_.c_str());
    socket_path_.clear();
  }

  if (shm_header_ != nullptr) {
    ::munmap(shm_header_, shm_size_);
    shm_header_ = nullptr;
    shm_pixels_ = nullptr;
  }
  if (shm_fd_ >= 0) {
    ::close(shm_fd_);
    shm_fd_ = -1;
  }
  if (!shm_name_.empty()) {
    ::shm_unlink(shm_name_.c_str());
    shm_name_.clear();
  }

  shm_size_ = 0;
  last_consumed_seq_ = 0;
}

void VideoPlayerController::play() { send_action(IpcPlaybackAction::Play, 0.0f); }

void VideoPlayerController::pause() { send_action(IpcPlaybackAction::Pause, 0.0f); }

void VideoPlayerController::seek(float sec) { send_action(IpcPlaybackAction::Seek, sec); }

bool VideoPlayerController::is_active() const noexcept {
  return worker_pid_ > 0 && shm_header_ != nullptr && shm_header_->magic == kPlaybackShmMagic;
}

const PlaybackShmHeader* VideoPlayerController::header() const noexcept {
  return is_active() ? shm_header_ : nullptr;
}

const uint8_t* VideoPlayerController::frame_pixels() const noexcept {
  return is_active() ? shm_pixels_ : nullptr;
}

uint64_t VideoPlayerController::last_consumed_seq() const noexcept { return last_consumed_seq_; }

void VideoPlayerController::mark_seq_consumed(uint64_t seq) noexcept { last_consumed_seq_ = seq; }

void VideoPlayerController::send_action(IpcPlaybackAction action, float value) {
  if (sock_fd_ >= 0) {
    IpcPlaybackRequest req{action, value};
    ::send(sock_fd_, &req, sizeof(req), MSG_NOSIGNAL);
  }
}

}  // namespace fpshud
