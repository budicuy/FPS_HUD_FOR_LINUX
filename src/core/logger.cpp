#include "logger.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace fpshud::log {

namespace {

std::atomic<Level> g_min_level{Level::Warn};
std::atomic<bool> g_initialized{false};
std::mutex g_log_mutex;
int g_log_fd = STDERR_FILENO;
bool g_is_custom_file = false;

void ensure_parent_dir(const char* path) noexcept {
  char dir[1024];
  std::snprintf(dir, sizeof(dir), "%s", path);
  char* slash = std::strrchr(dir, '/');
  if (!slash || slash == dir) {
    return;
  }
  *slash = '\0';

  for (char* p = dir + 1; *p; ++p) {
    if (*p == '/') {
      *p = '\0';
      ::mkdir(dir, 0755);
      *p = '/';
    }
  }
  ::mkdir(dir, 0755);
}

bool str_iequals(const char* a, const char* b) noexcept {
  while (*a && *b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
    if (ca != cb) {
      return false;
    }
    ++a;
    ++b;
  }
  return (*a == '\0' && *b == '\0');
}

bool parse_level_str(const char* s, Level& out) noexcept {
  if (str_iequals(s, "0") || str_iequals(s, "none") || str_iequals(s, "off")) {
    out = Level::None;
    return true;
  }
  if (str_iequals(s, "trace")) {
    out = Level::Trace;
    return true;
  }
  if (str_iequals(s, "debug") || str_iequals(s, "1")) {
    out = Level::Debug;
    return true;
  }
  if (str_iequals(s, "info")) {
    out = Level::Info;
    return true;
  }
  if (str_iequals(s, "warn") || str_iequals(s, "warning")) {
    out = Level::Warn;
    return true;
  }
  if (str_iequals(s, "error")) {
    out = Level::Error;
    return true;
  }
  return false;
}

}  // namespace

void init() noexcept {
  std::lock_guard<std::mutex> lock(g_log_mutex);

  static char s_last_env_str[128] = {0};
  static bool s_has_run = false;

  const char* env = std::getenv("FPS_HUD_LOG");
  const char* cur_env = env ? env : "";

  if (s_has_run && g_initialized.load(std::memory_order_acquire) &&
      std::strncmp(cur_env, s_last_env_str, sizeof(s_last_env_str)) == 0) {
    return;
  }
  s_has_run = true;
  std::snprintf(s_last_env_str, sizeof(s_last_env_str), "%s", cur_env);

  // Close previous file descriptor if custom
  if (g_is_custom_file && g_log_fd != STDERR_FILENO && g_log_fd >= 0) {
    ::close(g_log_fd);
  }
  g_log_fd = STDERR_FILENO;
  g_is_custom_file = false;

  if (!env || env[0] == '\0') {
    g_min_level.store(Level::Warn, std::memory_order_release);
    g_initialized.store(true, std::memory_order_release);
    return;
  }

  Level parsed_level = Level::Debug;
  if (parse_level_str(env, parsed_level)) {
    g_min_level.store(parsed_level, std::memory_order_release);
    g_initialized.store(true, std::memory_order_release);
    return;
  }

  // Check if it's level:filepath format, e.g. "debug:/tmp/log.txt"
  const char* colon = std::strchr(env, ':');
  const char* file_path = env;
  if (colon) {
    char lvl_buf[32];
    size_t lvl_len = static_cast<size_t>(colon - env);
    if (lvl_len < sizeof(lvl_buf)) {
      std::memcpy(lvl_buf, env, lvl_len);
      lvl_buf[lvl_len] = '\0';
      if (parse_level_str(lvl_buf, parsed_level)) {
        file_path = colon + 1;
      }
    }
  }

  // Treat file_path as a destination file
  char resolved_path[1024];
  if (file_path[0] == '~' && file_path[1] == '/') {
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
      std::snprintf(resolved_path, sizeof(resolved_path), "%s/%s", home, file_path + 2);
    } else {
      std::snprintf(resolved_path, sizeof(resolved_path), "%s", file_path);
    }
  } else {
    std::snprintf(resolved_path, sizeof(resolved_path), "%s", file_path);
  }

  ensure_parent_dir(resolved_path);
  int fd = ::open(resolved_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd >= 0) {
    g_log_fd = fd;
    g_is_custom_file = true;
    g_min_level.store(parsed_level, std::memory_order_release);
  } else {
    g_log_fd = STDERR_FILENO;
    g_is_custom_file = false;
    g_min_level.store(parsed_level, std::memory_order_release);
  }

  g_initialized.store(true, std::memory_order_release);
}

bool is_enabled(Level level) noexcept {
  if (level == Level::None) {
    return false;
  }
  if (!g_initialized.load(std::memory_order_acquire)) {
    init();
  }
  const auto min_lvl = g_min_level.load(std::memory_order_relaxed);
  if (min_lvl == Level::None) {
    return false;
  }
  return static_cast<uint8_t>(level) >= static_cast<uint8_t>(min_lvl);
}

void write(Level level, Subsystem subsys, const char* file, int line, const char* fmt,
           ...) noexcept {
  if (!g_initialized.load(std::memory_order_acquire)) {
    init();
  }
  if (!is_enabled(level)) {
    return;
  }

  struct timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm_buf{};
  localtime_r(&ts.tv_sec, &tm_buf);
  const long ms = ts.tv_nsec / 1'000'000L;

  const char* level_name = "INFO";
  switch (level) {
    case Level::Trace:
      level_name = "TRACE";
      break;
    case Level::Debug:
      level_name = "DEBUG";
      break;
    case Level::Info:
      level_name = "INFO";
      break;
    case Level::Warn:
      level_name = "WARN";
      break;
    case Level::Error:
      level_name = "ERROR";
      break;
    case Level::None:
      return;
  }

  const char* subsys_name = "CORE";
  switch (subsys) {
    case Subsystem::Core:
      subsys_name = "CORE";
      break;
    case Subsystem::Telemetry:
      subsys_name = "TELEM";
      break;
    case Subsystem::Vulkan:
      subsys_name = "VULKAN";
      break;
    case Subsystem::OpenGL:
      subsys_name = "OPENGL";
      break;
    case Subsystem::Input:
      subsys_name = "INPUT";
      break;
    case Subsystem::Recorder:
      subsys_name = "RECORD";
      break;
    case Subsystem::UI:
      subsys_name = "UI";
      break;
  }

  const char* base_file = "unknown";
  if (file) {
    const char* slash = std::strrchr(file, '/');
    base_file = slash ? (slash + 1) : file;
  }

  char lvl_tag[16];
  std::snprintf(lvl_tag, sizeof(lvl_tag), "[%s]", level_name);

  char sub_tag[16];
  std::snprintf(sub_tag, sizeof(sub_tag), "[%s]", subsys_name);

  constexpr size_t kMaxLogLine = 4096;
  char buffer[kMaxLogLine];

  int prefix_len = std::snprintf(
      buffer, sizeof(buffer), "[%02d:%02d:%02d.%03ld] %-7s %-8s (%s:%d) ", tm_buf.tm_hour,
      tm_buf.tm_min, tm_buf.tm_sec, ms, lvl_tag, sub_tag, base_file, line);

  if (prefix_len < 0) {
    return;
  }
  if (static_cast<size_t>(prefix_len) >= sizeof(buffer) - 2) {
    prefix_len = sizeof(buffer) - 2;
  }

  va_list args;
  va_start(args, fmt);
  int msg_len = std::vsnprintf(buffer + prefix_len, sizeof(buffer) - prefix_len - 1, fmt, args);
  va_end(args);

  if (msg_len < 0) {
    return;
  }

  size_t total_len = static_cast<size_t>(prefix_len) + static_cast<size_t>(msg_len);
  if (total_len >= sizeof(buffer) - 1) {
    total_len = sizeof(buffer) - 2;
  }

  if (total_len == 0 || buffer[total_len - 1] != '\n') {
    buffer[total_len++] = '\n';
    buffer[total_len] = '\0';
  }

  std::lock_guard<std::mutex> lock(g_log_mutex);
  int fd = g_log_fd;
  if (fd >= 0) {
    const char* ptr = buffer;
    size_t remaining = total_len;
    while (remaining > 0) {
      ssize_t written = ::write(fd, ptr, remaining);
      if (written <= 0) {
        break;
      }
      remaining -= static_cast<size_t>(written);
      ptr += written;
    }
  }
}

}  // namespace fpshud::log
