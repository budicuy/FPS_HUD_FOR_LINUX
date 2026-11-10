#pragma once

#include <cstdint>

namespace fpshud::log {

enum class Level : uint8_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  None = 5,
};

enum class Subsystem : uint8_t {
  Core,
  Telemetry,
  Vulkan,
  OpenGL,
  Input,
  Recorder,
  UI,
};

void init() noexcept;

void write(Level level, Subsystem subsys, const char* file, int line, const char* fmt, ...) noexcept
    __attribute__((format(printf, 5, 6)));

[[nodiscard]] bool is_enabled(Level level) noexcept;

}  // namespace fpshud::log

#if defined(FPSHUD_ENABLE_DEBUG_LOGGING)
#define FPSHUD_LOG_TRACE(subsys, ...) \
  ::fpshud::log::write(::fpshud::log::Level::Trace, (subsys), __FILE__, __LINE__, __VA_ARGS__)
#define FPSHUD_LOG_DEBUG(subsys, ...) \
  ::fpshud::log::write(::fpshud::log::Level::Debug, (subsys), __FILE__, __LINE__, __VA_ARGS__)
#define FPSHUD_LOG_INFO(subsys, ...) \
  ::fpshud::log::write(::fpshud::log::Level::Info, (subsys), __FILE__, __LINE__, __VA_ARGS__)
#else
#define FPSHUD_LOG_TRACE(subsys, ...) ((void)0)
#define FPSHUD_LOG_DEBUG(subsys, ...) ((void)0)
#define FPSHUD_LOG_INFO(subsys, ...) ((void)0)
#endif

#define FPSHUD_LOG_WARN(subsys, ...) \
  ::fpshud::log::write(::fpshud::log::Level::Warn, (subsys), __FILE__, __LINE__, __VA_ARGS__)
#define FPSHUD_LOG_ERROR(subsys, ...) \
  ::fpshud::log::write(::fpshud::log::Level::Error, (subsys), __FILE__, __LINE__, __VA_ARGS__)
