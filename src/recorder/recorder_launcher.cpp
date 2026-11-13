#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::array<std::string_view, 6> kLdCandidates = {
    "/usr/lib/x86_64-linux-gnu",
    "/usr/lib/x86_64-linux-gnu/pulseaudio",
    "/usr/lib64",
    "/usr/lib64/pulseaudio",
    "/usr/lib",
    "/usr/lib/pulseaudio",
};

constexpr std::array<std::string_view, 3> kLibvaCandidates = {
    "/usr/lib/x86_64-linux-gnu/dri",
    "/usr/lib64/dri",
    "/usr/lib/dri",
};

}  // namespace

/// Helper function to build LD_LIBRARY_PATH and LIBVA_DRIVERS_PATH from container host roots
bool build_container_env(std::string_view root_prefix, std::string& out_ld_path,
                         std::string& out_libva_path) {
  out_ld_path.clear();
  out_libva_path.clear();

  constexpr std::array<std::string_view, 2> kHostRoots = {"/run/host", "/var/run/host"};
  std::string_view host_root;
  struct stat st{};

  for (std::string_view hr : kHostRoots) {
    std::string test_path = std::string(root_prefix) + std::string(hr);
    if (::stat(test_path.c_str(), &st) == 0) {
      host_root = hr;
      break;
    }
  }

  if (host_root.empty()) {
    return false;
  }

  for (std::string_view cand : kLdCandidates) {
    std::string full_path = std::string(root_prefix) + std::string(host_root) + std::string(cand);
    if (::stat(full_path.c_str(), &st) == 0) {
      if (!out_ld_path.empty()) {
        out_ld_path += ':';
      }
      out_ld_path += std::string(host_root) + std::string(cand);
    }
  }

  for (std::string_view cand : kLibvaCandidates) {
    std::string full_path = std::string(root_prefix) + std::string(host_root) + std::string(cand);
    if (::stat(full_path.c_str(), &st) == 0) {
      if (!out_libva_path.empty()) {
        out_libva_path += ':';
      }
      out_libva_path += std::string(host_root) + std::string(cand);
    }
  }

  return (!out_ld_path.empty() || !out_libva_path.empty());
}

static void inject_container_host_env() {
  std::string host_ld;
  std::string host_libva;
  if (!build_container_env("", host_ld, host_libva)) {
    return;
  }

  if (!host_ld.empty()) {
    const char* cur_ld = std::getenv("LD_LIBRARY_PATH");
    if (cur_ld && *cur_ld) {
      std::string new_ld = host_ld + ":" + cur_ld;
      ::setenv("LD_LIBRARY_PATH", new_ld.c_str(), 1);
    } else {
      ::setenv("LD_LIBRARY_PATH", host_ld.c_str(), 1);
    }
  }

  const char* cur_libva = std::getenv("LIBVA_DRIVERS_PATH");
  if ((!cur_libva || !*cur_libva) && !host_libva.empty()) {
    ::setenv("LIBVA_DRIVERS_PATH", host_libva.c_str(), 1);
  }
}

int main(int argc, char* argv[]) {
  // 1. Determine directory where launcher is located
  std::string dir;
  std::string exe_buf(4096, '\0');
  ssize_t len = ::readlink("/proc/self/exe", exe_buf.data(), exe_buf.size() - 1);
  if (len > 0) {
    exe_buf.resize(static_cast<size_t>(len));
    auto last_slash = exe_buf.find_last_of('/');
    if (last_slash != std::string::npos) {
      dir = exe_buf.substr(0, last_slash);
    }
  }

  // 2. Locate the real binary fpshud_recorder_bin
  std::string target;
  if (!dir.empty()) {
    target = dir + "/fpshud_recorder_bin";
  }

  struct stat st{};
  if (target.empty() || ::stat(target.c_str(), &st) != 0) {
    if (const char* home = std::getenv("HOME"); home && *home) {
      std::string cand1 = std::string(home) + "/.local/lib/fpshud/fpshud_recorder_bin";
      if (::stat(cand1.c_str(), &st) == 0) {
        target = std::move(cand1);
      } else {
        std::string cand2 = std::string(home) + "/.local/bin/fpshud_recorder_bin";
        if (::stat(cand2.c_str(), &st) == 0) {
          target = std::move(cand2);
        }
      }
    }
  }

  if (target.empty()) {
    target = "fpshud_recorder_bin";
  }

  // 3. Inject container host library paths if inside pressure-vessel / bubblewrap / flatpak
  inject_container_host_env();

  // Never leak game hooks or preloads into recorder process
  ::unsetenv("LD_PRELOAD");

  // 4. Execute the real binary
  std::vector<char*> exec_args;
  exec_args.reserve(static_cast<size_t>(argc) + 1u);
  exec_args.push_back(target.data());
  for (int i = 1; i < argc; ++i) {
    exec_args.push_back(argv[i]);
  }
  exec_args.push_back(nullptr);

  ::execv(target.c_str(), exec_args.data());

  // If execv failed, print diagnostic
  std::perror("fpshud_recorder: execv failed");
  return 127;
}
