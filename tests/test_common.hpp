#pragma once

#include "doctest.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

/// Write mock file with automatic parent directory creation and trailing newline
inline void write_mock_file(const fs::path& p, const std::string& content) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::trunc);
  f << content << "\n";
}

/// Read entire text file to string
inline std::string read_text_file(const fs::path& p) {
  std::ifstream f(p);
  if (!f.is_open()) return "";
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/// Find repository root by searching upward for scripts/install.sh
inline fs::path find_repo_root() {
  char exe_buf[4096] = {0};
  ssize_t len = ::readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
  if (len > 0) {
    fs::path p = fs::path(std::string(exe_buf, static_cast<size_t>(len))).parent_path();
    if (fs::exists(p / "scripts" / "install.sh")) return p;
    if (fs::exists(p / ".." / "scripts" / "install.sh")) return (p / "..").lexically_normal();
  }
  if (fs::exists("./scripts/install.sh")) return ".";
  return {};
}

/// Resolve the path to libfpshud.so across common build & installation locations
inline fs::path find_libfpshud_path() {
  std::vector<fs::path> candidates;
  fs::path repo_root = find_repo_root();
  if (!repo_root.empty()) {
    candidates.push_back(repo_root / "build" / "libfpshud.so");
    candidates.push_back(repo_root / "libfpshud.so");
    candidates.push_back(repo_root / "dist" / "libfpshud.so");
  }
  char exe_buf[4096] = {0};
  ssize_t len = ::readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
  if (len > 0) {
    fs::path exe_dir = fs::path(std::string(exe_buf, static_cast<size_t>(len))).parent_path();
    candidates.push_back(exe_dir / "libfpshud.so");
    candidates.push_back(exe_dir / "build" / "libfpshud.so");
  }
  candidates.push_back(fs::path("./build/libfpshud.so"));
  candidates.push_back(fs::path("./libfpshud.so"));

  for (const auto& p : candidates) {
    std::error_code ec;
    if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
      return fs::canonical(p, ec);
    }
  }
  return {};
}

namespace fpshud::test {
using ::find_libfpshud_path;
}  // namespace fpshud::test

/// Resolve the path to fpshud_recorder_bin
inline fs::path find_recorder_bin_path() {
  std::vector<fs::path> candidates;
  char exe_buf[4096] = {0};
  ssize_t len = ::readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
  if (len > 0) {
    fs::path exe_dir = fs::path(std::string(exe_buf, static_cast<size_t>(len))).parent_path();
    candidates.push_back(exe_dir / "fpshud_recorder_bin");
    candidates.push_back(exe_dir / "build" / "fpshud_recorder_bin");
  }
  fs::path repo_root = find_repo_root();
  if (!repo_root.empty()) {
    candidates.push_back(repo_root / "build" / "fpshud_recorder_bin");
    candidates.push_back(repo_root / "fpshud_recorder_bin");
  }
  candidates.push_back(fs::path("./build/fpshud_recorder_bin"));
  candidates.push_back(fs::path("./fpshud_recorder_bin"));

  for (const auto& p : candidates) {
    std::error_code ec;
    if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
      return fs::canonical(p, ec);
    }
  }
  return {};
}

/// Resolve the path to fpshud_recorder launcher
inline fs::path find_recorder_launcher_path() {
  std::vector<fs::path> candidates;
  char exe_buf[4096] = {0};
  ssize_t len = ::readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
  if (len > 0) {
    fs::path exe_dir = fs::path(std::string(exe_buf, static_cast<size_t>(len))).parent_path();
    candidates.push_back(exe_dir / "fpshud_recorder");
    candidates.push_back(exe_dir / "build" / "fpshud_recorder");
  }
  fs::path repo_root = find_repo_root();
  if (!repo_root.empty()) {
    candidates.push_back(repo_root / "build" / "fpshud_recorder");
    candidates.push_back(repo_root / "fpshud_recorder");
  }
  candidates.push_back(fs::path("./build/fpshud_recorder"));
  candidates.push_back(fs::path("./fpshud_recorder"));

  for (const auto& p : candidates) {
    std::error_code ec;
    if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
      return fs::canonical(p, ec);
    }
  }
  return {};
}

/// Compatibility shim bridging monolithic check_cond() to doctest CHECK_MESSAGE()
inline void check_cond(bool ok, std::string_view label, std::string_view detail = "") {
  if (!detail.empty()) {
    CHECK_MESSAGE(ok, label, " | ", detail);
  } else {
    CHECK_MESSAGE(ok, label);
  }
}

inline void check_cond(bool ok, const char* label, const char* detail = nullptr) {
  std::string_view l = (label != nullptr) ? std::string_view(label) : std::string_view("(null)");
  std::string_view d = (detail != nullptr) ? std::string_view(detail) : std::string_view("");
  check_cond(ok, l, d);
}

inline void check_cond(bool ok, const std::string& label, const std::string& detail = "") {
  check_cond(ok, std::string_view(label), std::string_view(detail));
}

/// RAII Temporary Directory Manager for Hardware & Filesystem Mock Isolation
struct ScopedTempDir {
  fs::path path;

  explicit ScopedTempDir(const fs::path& prefix_or_path = "fpshud_mock_") {
    std::string s = prefix_or_path.string();
    if (s.empty()) {
      init_with_template("fpshud_mock_XXXXXX");
    } else if (s.front() == '/') {
      path = prefix_or_path;
      std::error_code ec;
      fs::remove_all(path, ec);
      fs::create_directories(path, ec);
    } else {
      std::string tpl = s;
      if (tpl.find("XXXXXX") == std::string::npos) {
        tpl += "XXXXXX";
      }
      init_with_template(tpl);
    }
  }

  ~ScopedTempDir() { cleanup(); }

  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  ScopedTempDir(ScopedTempDir&& o) noexcept : path(std::move(o.path)) { o.path.clear(); }

  ScopedTempDir& operator=(ScopedTempDir&& o) noexcept {
    if (this != &o) {
      cleanup();
      path = std::move(o.path);
      o.path.clear();
    }
    return *this;
  }

  void cleanup() {
    if (!path.empty()) {
      std::error_code ec;
      fs::remove_all(path, ec);
      path.clear();
    }
  }

  operator const fs::path&() const noexcept { return path; }
  operator std::string() const { return path.string(); }
  const fs::path& get() const noexcept { return path; }
  const char* c_str() const noexcept { return path.c_str(); }
  std::string string() const { return path.string(); }

  fs::path operator/(const fs::path& sub) const { return path / sub; }
  fs::path operator/(std::string_view sub) const { return path / sub; }
  fs::path operator/(const char* sub) const { return path / sub; }

private:
  void init_with_template(const std::string& tpl) {
    std::string full_tpl = "/tmp/" + tpl;
    std::vector<char> buf(full_tpl.begin(), full_tpl.end());
    buf.push_back('\0');
    if (char* res = ::mkdtemp(buf.data())) {
      path = res;
    } else {
      static std::atomic<uint64_t> counter{0};
      path = "/tmp/" + tpl + "_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter.fetch_add(1));
      std::error_code ec;
      fs::remove_all(path, ec);
      fs::create_directories(path, ec);
    }
  }
};
