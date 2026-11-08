#include "input/input_hooks.hpp"
#include "core/config.hpp"
#include "input/input_manager.hpp"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <linux/input-event-codes.h>

// Self-contained minimal SDL2 / SDL3 event structures (Zero static libSDL dependency)
namespace {

constexpr uint32_t kSdlMouseMotion = 0x400;
constexpr uint32_t kSdlMouseButtonDown = 0x401;
constexpr uint32_t kSdlMouseButtonUp = 0x402;
constexpr uint32_t kSdlMouseWheel = 0x403;
constexpr uint32_t kSdlKeyDown = 0x300;

constexpr uint8_t kSdlButtonMiddle = 2;
constexpr uint8_t kSdlButtonRight = 3;

#pragma pack(push, 1)
struct SdlCommonEvent {
  uint32_t type;
  uint32_t timestamp;
};

struct SdlMouseMotionEvent {
  uint32_t type;
  uint32_t timestamp;
  uint32_t window_id;
  uint32_t which;
  uint32_t state;
  int32_t x;
  int32_t y;
  int32_t xrel;
  int32_t yrel;
};

struct SdlMouseButtonEvent {
  uint32_t type;
  uint32_t timestamp;
  uint32_t window_id;
  uint32_t which;
  uint8_t button;
  uint8_t state;
  uint8_t clicks;
  uint8_t padding1;
  int32_t x;
  int32_t y;
};

struct SdlMouseWheelEvent {
  uint32_t type;
  uint32_t timestamp;
  uint32_t window_id;
  uint32_t which;
  int32_t x;
  int32_t y;
  uint32_t direction;
  float precise_x;
  float precise_y;
};

struct SdlKeyboardEvent {
  uint32_t type;
  uint32_t timestamp;
  uint32_t window_id;
  uint8_t state;
  uint8_t repeat;
  uint8_t padding2;
  uint8_t padding3;
  struct {
    int32_t scancode;
    int32_t sym;
    uint16_t mod;
    uint32_t unused;
  } keysym;
};

union SdlRawEvent {
  uint32_t type;
  SdlCommonEvent common;
  SdlMouseMotionEvent motion;
  SdlMouseButtonEvent button;
  SdlMouseWheelEvent wheel;
  SdlKeyboardEvent key;
  uint8_t padding[128];
};
#pragma pack(pop)

using PFN_SdlPollEvent = int (*)(void* event);
using PFN_SdlPeepEvents = int (*)(void* events, int numevents, int action, uint32_t minType,
                                  uint32_t maxType);
using PFN_SdlWaitEvent = int (*)(void* event);
using PFN_SdlWaitEventTimeout = int (*)(void* event, int timeout);
using PFN_SdlGetRelativeMouseMode = int (*)();
using PFN_SdlSetRelativeMouseMode = int (*)(int enabled);
using PFN_SdlShowCursor = int (*)(int toggle);
using PFN_XNextEvent = int (*)(void* dpy, void* ev);

PFN_SdlPollEvent g_real_sdl_poll_event = nullptr;
PFN_SdlPeepEvents g_real_sdl_peep_events = nullptr;
PFN_SdlWaitEvent g_real_sdl_wait_event = nullptr;
PFN_SdlWaitEventTimeout g_real_sdl_wait_event_timeout = nullptr;
PFN_SdlGetRelativeMouseMode g_real_sdl_get_relative_mouse_mode = nullptr;
PFN_SdlSetRelativeMouseMode g_real_sdl_set_relative_mouse_mode = nullptr;
PFN_SdlShowCursor g_real_sdl_show_cursor = nullptr;
PFN_XNextEvent g_real_x_next_event = nullptr;

std::atomic<bool> g_hooks_initialized{false};
std::atomic<bool> g_was_relative_mouse{false};
std::atomic<bool> g_mouse_isolation_active{false};
thread_local bool t_in_hook = false;

void ensure_input_hooks_initialized() noexcept {
  if (g_hooks_initialized.load(std::memory_order_acquire)) return;

  g_real_sdl_poll_event = reinterpret_cast<PFN_SdlPollEvent>(dlsym(RTLD_NEXT, "SDL_PollEvent"));
  g_real_sdl_peep_events = reinterpret_cast<PFN_SdlPeepEvents>(dlsym(RTLD_NEXT, "SDL_PeepEvents"));
  g_real_sdl_wait_event = reinterpret_cast<PFN_SdlWaitEvent>(dlsym(RTLD_NEXT, "SDL_WaitEvent"));
  g_real_sdl_wait_event_timeout =
      reinterpret_cast<PFN_SdlWaitEventTimeout>(dlsym(RTLD_NEXT, "SDL_WaitEventTimeout"));
  g_real_sdl_get_relative_mouse_mode =
      reinterpret_cast<PFN_SdlGetRelativeMouseMode>(dlsym(RTLD_NEXT, "SDL_GetRelativeMouseMode"));
  g_real_sdl_set_relative_mouse_mode =
      reinterpret_cast<PFN_SdlSetRelativeMouseMode>(dlsym(RTLD_NEXT, "SDL_SetRelativeMouseMode"));
  g_real_sdl_show_cursor = reinterpret_cast<PFN_SdlShowCursor>(dlsym(RTLD_NEXT, "SDL_ShowCursor"));
  g_real_x_next_event = reinterpret_cast<PFN_XNextEvent>(dlsym(RTLD_NEXT, "XNextEvent"));

  g_hooks_initialized.store(true, std::memory_order_release);
}

}  // namespace

namespace fpshud {

void init_input_hooks() noexcept { ensure_input_hooks_initialized(); }

bool is_mouse_isolation_active() noexcept {
  return g_mouse_isolation_active.load(std::memory_order_relaxed) || get_global_config().gui_open;
}

void on_gui_state_changed(bool gui_open) noexcept {
  ensure_input_hooks_initialized();
  g_mouse_isolation_active.store(gui_open, std::memory_order_release);

  if (gui_open) {
    if (g_real_sdl_get_relative_mouse_mode) {
      int rel = g_real_sdl_get_relative_mouse_mode();
      if (rel == 1) {
        g_was_relative_mouse.store(true, std::memory_order_relaxed);
        if (g_real_sdl_set_relative_mouse_mode) {
          g_real_sdl_set_relative_mouse_mode(0);
        }
      }
    }
    if (g_real_sdl_show_cursor) {
      g_real_sdl_show_cursor(1);
    }
  } else {
    if (g_was_relative_mouse.exchange(false, std::memory_order_relaxed)) {
      if (g_real_sdl_set_relative_mouse_mode) {
        g_real_sdl_set_relative_mouse_mode(1);
      }
      if (g_real_sdl_show_cursor) {
        g_real_sdl_show_cursor(0);
      }
    }
  }
}

}  // namespace fpshud

// Exported C ABI Hooks for SDL Event Loop Interception
extern "C" {

__attribute__((visibility("default"))) int SDL_PollEvent(void* event) {
  ensure_input_hooks_initialized();
  if (!g_real_sdl_poll_event) return 0;
  if (t_in_hook) return g_real_sdl_poll_event(event);

  t_in_hook = true;
  int res = 0;

  while ((res = g_real_sdl_poll_event(event)) != 0) {
    if (!fpshud::is_mouse_isolation_active()) {
      // Normal passthrough when overlay menu is not open
      break;
    }

    auto& cfg = fpshud::get_mutable_config();

    auto* raw = static_cast<SdlRawEvent*>(event);
    auto& im = fpshud::get_input_manager();
    ImGuiIO* io = im.current_io();

    if (raw->type == kSdlMouseMotion) {
      // Absorb mouse motion: update ImGui cursor coordinates and drop event from game
      im.handle_pointer_motion(static_cast<float>(raw->motion.x), static_cast<float>(raw->motion.y),
                               cfg, io);
      continue;
    } else if (raw->type == kSdlMouseButtonDown || raw->type == kSdlMouseButtonUp) {
      // Absorb mouse buttons: route to ImGui and drop event from game
      bool down = (raw->type == kSdlMouseButtonDown);
      uint32_t btn = BTN_LEFT;
      if (raw->button.button == kSdlButtonRight)
        btn = BTN_RIGHT;
      else if (raw->button.button == kSdlButtonMiddle)
        btn = BTN_MIDDLE;

      im.handle_pointer_button(btn, down, cfg, io);
      continue;
    } else if (raw->type == kSdlMouseWheel) {
      // Absorb mouse wheel
      im.handle_pointer_wheel(static_cast<float>(raw->wheel.x), static_cast<float>(raw->wheel.y),
                              cfg, io);
      continue;
    } else if (raw->type == kSdlKeyDown && raw->key.keysym.sym == 1073741893 /* SDLK_F12 */) {
      // F12 pressed while GUI open -> toggle GUI closed cleanly
      cfg.gui_open = false;
      fpshud::on_gui_state_changed(false);
      continue;
    } else if (raw->type == kSdlKeyDown && raw->key.keysym.sym == 27 /* SDLK_ESCAPE */) {
      // ESC pressed while GUI open -> close GUI
      cfg.gui_open = false;
      fpshud::on_gui_state_changed(false);
      continue;
    }

    // Non-mouse event -> deliver to game
    break;
  }

  t_in_hook = false;
  return res;
}

__attribute__((visibility("default"))) int SDL_WaitEventTimeout(void* event, int timeout) {
  ensure_input_hooks_initialized();
  if (!g_real_sdl_wait_event_timeout) return 0;
  if (t_in_hook) return g_real_sdl_wait_event_timeout(event, timeout);

  // When GUI is open, use non-blocking poll loop to filter mouse events
  if (fpshud::is_mouse_isolation_active()) {
    return SDL_PollEvent(event);
  }
  return g_real_sdl_wait_event_timeout(event, timeout);
}

__attribute__((visibility("default"))) int SDL_WaitEvent(void* event) {
  ensure_input_hooks_initialized();
  if (t_in_hook) {
    return g_real_sdl_wait_event ? g_real_sdl_wait_event(event) : 0;
  }
  if (fpshud::is_mouse_isolation_active()) {
    return SDL_PollEvent(event);
  }
  if (g_real_sdl_wait_event) {
    return g_real_sdl_wait_event(event);
  }
  return SDL_WaitEventTimeout(event, -1);
}

__attribute__((visibility("default"))) int SDL_PeepEvents(void* events, int numevents, int action,
                                                          uint32_t minType, uint32_t maxType) {
  ensure_input_hooks_initialized();
  if (!g_real_sdl_peep_events) return 0;
  if (t_in_hook) return g_real_sdl_peep_events(events, numevents, action, minType, maxType);

  t_in_hook = true;
  int count = g_real_sdl_peep_events(events, numevents, action, minType, maxType);
  auto& cfg = fpshud::get_mutable_config();

  // If GUI is active and action retrieves events (SDL_GETEVENT = 2), filter mouse events
  if (count > 0 && fpshud::is_mouse_isolation_active() && action == 2 && events != nullptr) {
    auto* raw_events = static_cast<SdlRawEvent*>(events);
    auto& im = fpshud::get_input_manager();
    ImGuiIO* io = im.current_io();
    int write_idx = 0;

    for (int i = 0; i < count; ++i) {
      uint32_t t = raw_events[i].type;
      if (t == kSdlMouseMotion) {
        im.handle_pointer_motion(static_cast<float>(raw_events[i].motion.x),
                                 static_cast<float>(raw_events[i].motion.y), cfg, io);
      } else if (t == kSdlMouseButtonDown || t == kSdlMouseButtonUp) {
        bool down = (t == kSdlMouseButtonDown);
        uint32_t btn =
            (raw_events[i].button.button == kSdlButtonRight)
                ? BTN_RIGHT
                : ((raw_events[i].button.button == kSdlButtonMiddle) ? BTN_MIDDLE : BTN_LEFT);
        im.handle_pointer_button(btn, down, cfg, io);
      } else if (t == kSdlMouseWheel) {
        im.handle_pointer_wheel(static_cast<float>(raw_events[i].wheel.x),
                                static_cast<float>(raw_events[i].wheel.y), cfg, io);
      } else {
        if (write_idx != i) {
          raw_events[write_idx] = raw_events[i];
        }
        write_idx++;
      }
    }
    count = write_idx;
  }

  t_in_hook = false;
  return count;
}

__attribute__((visibility("default"))) int XNextEvent(void* display, void* event) {
  ensure_input_hooks_initialized();
  if (!g_real_x_next_event) return -1;
  if (t_in_hook) return g_real_x_next_event(display, event);

  t_in_hook = true;
  int res = 0;

  while ((res = g_real_x_next_event(display, event)) == 0) {
    if (!fpshud::is_mouse_isolation_active() || !event) {
      break;
    }

    int type = *reinterpret_cast<const int*>(event);

    // X11 Event Types: MotionNotify = 6, ButtonPress = 4, ButtonRelease = 5
    if (type == 6) {
      // struct XMotionEvent offset: int x, y at offset 40 (64-bit) or 32 (32-bit)
      // On 64-bit Linux: type(4)+pad(4) + serial(8) + send_event(4)+pad(4) + display(8) +
      // window(8) + root(8) + subwindow(8) + time(8) + x(4) + y(4) = offset 64
      // Safer: handle via XQueryPointer or offset
      // Absorb motion event
      continue;
    } else if (type == 4 || type == 5) {
      // Absorb button event
      continue;
    }
    break;
  }

  t_in_hook = false;
  return res;
}

}  // extern "C"
