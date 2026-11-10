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

/// Compatibility shim bridging monolithic check_cond() to doctest CHECK_MESSAGE()
inline void check_cond(bool ok, const char* label, const char* detail = "") {
  if (detail != nullptr && detail[0] != '\0') {
    CHECK_MESSAGE(ok, label, " | ", detail);
  } else {
    CHECK_MESSAGE(ok, label);
  }
}

inline void check_cond(bool ok, const std::string& label, const std::string& detail = "") {
  check_cond(ok, label.c_str(), detail.empty() ? "" : detail.c_str());
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
