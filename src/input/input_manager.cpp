#include "input_manager.hpp"
#include "input/input_hooks.hpp"
#include "recorder_ipc.hpp"
#include "telemetry.hpp"

#include "imgui.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#ifdef None
#undef None
#endif
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace fpshud {

// ----------------------------------------------------------------------------
// Pure Dynamic Wayland Client API Definitions (Zero DT_NEEDED)
// ----------------------------------------------------------------------------
struct wl_display;
struct wl_event_queue;
struct wl_proxy;
struct wl_registry;
struct wl_seat;
struct wl_keyboard;
struct wl_pointer;
struct wl_surface;
struct wl_array;

struct wl_interface {
  const char* name;
  int version;
  int method_count;
  const void* methods;
  int event_count;
  const void* events;
};

struct wl_registry_listener {
  void (*global)(void* data, struct wl_registry* registry, uint32_t name, const char* interface,
                 uint32_t version);
  void (*global_remove)(void* data, struct wl_registry* registry, uint32_t name);
};

struct wl_seat_listener {
  void (*capabilities)(void* data, struct wl_seat* seat, uint32_t capabilities);
  void (*name)(void* data, struct wl_seat* seat, const char* name);
};

struct wl_keyboard_listener {
  void (*keymap)(void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd,
                 uint32_t size);
  void (*enter)(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                struct wl_surface* surface, struct wl_array* keys);
  void (*leave)(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                struct wl_surface* surface);
  void (*key)(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time,
              uint32_t key, uint32_t state);
  void (*modifiers)(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                    uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
                    uint32_t group);
  void (*repeat_info)(void* data, struct wl_keyboard* keyboard, int32_t rate, int32_t delay);
};

struct wl_pointer_listener {
  void (*enter)(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface,
                int32_t sx, int32_t sy);
  void (*leave)(void* data, struct wl_pointer* pointer, uint32_t serial,
                struct wl_surface* surface);
  void (*motion)(void* data, struct wl_pointer* pointer, uint32_t time, int32_t sx, int32_t sy);
  void (*button)(void* data, struct wl_pointer* pointer, uint32_t serial, uint32_t time,
                 uint32_t button, uint32_t state);
  void (*axis)(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis, int32_t value);
  void (*frame)(void* data, struct wl_pointer* pointer);
  void (*axis_source)(void* data, struct wl_pointer* pointer, uint32_t axis_source);
  void (*axis_stop)(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis);
  void (*axis_discrete)(void* data, struct wl_pointer* pointer, uint32_t axis, int32_t discrete);
  void (*axis_value120)(void* data, struct wl_pointer* pointer, uint32_t axis, int32_t value120);
  void (*axis_relative_direction)(void* data, struct wl_pointer* pointer, uint32_t axis,
                                  uint32_t direction);
};

struct WaylandClientApi {
  using PFN_create_queue = struct wl_event_queue* (*)(struct wl_display*);
  using PFN_destroy_queue = void (*)(struct wl_event_queue*);
  using PFN_roundtrip_queue = int (*)(struct wl_display*, struct wl_event_queue*);
  using PFN_dispatch_queue_pending = int (*)(struct wl_display*, struct wl_event_queue*);
  using PFN_proxy_set_queue = void (*)(struct wl_proxy*, struct wl_event_queue*);
  using PFN_proxy_destroy = void (*)(struct wl_proxy*);
  using PFN_proxy_add_listener = int (*)(struct wl_proxy*, void (**)(void), void*);
  using PFN_proxy_marshal_constructor = struct wl_proxy* (*)(struct wl_proxy*, uint32_t,
                                                             const struct wl_interface*, ...);
  using PFN_proxy_marshal_constructor_versioned = struct wl_proxy* (*)(struct wl_proxy*, uint32_t,
                                                                       const struct wl_interface*,
                                                                       uint32_t, ...);

  PFN_create_queue fn_display_create_queue = nullptr;
  PFN_destroy_queue fn_event_queue_destroy = nullptr;
  PFN_roundtrip_queue fn_display_roundtrip_queue = nullptr;
  PFN_dispatch_queue_pending fn_display_dispatch_queue_pending = nullptr;
  PFN_proxy_set_queue fn_proxy_set_queue = nullptr;
  PFN_proxy_destroy fn_proxy_destroy = nullptr;
  PFN_proxy_add_listener fn_proxy_add_listener = nullptr;
  PFN_proxy_marshal_constructor fn_marshal_constructor = nullptr;
  PFN_proxy_marshal_constructor_versioned fn_marshal_versioned = nullptr;

  const struct wl_interface* registry_interface = nullptr;
  const struct wl_interface* seat_interface = nullptr;
  const struct wl_interface* keyboard_interface = nullptr;
  const struct wl_interface* pointer_interface = nullptr;
};

static WaylandClientApi g_wl;

namespace {

bool test_bit(int bit, const unsigned long* array) {
  return (array[bit / (sizeof(unsigned long) * 8)] >> (bit % (sizeof(unsigned long) * 8))) & 1;
}

void release_imgui_inputs(ImGuiIO& io) {
  io.MouseDrawCursor = false;
  io.AddMouseButtonEvent(0, false);
  io.AddMouseButtonEvent(1, false);
  io.AddMouseButtonEvent(2, false);
  io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  io.ClearInputKeys();
  io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
}

bool is_gui_toggle_key(unsigned int code, int mode) {
  switch (mode) {
    case 0:
      return code == KEY_INSERT || code == KEY_F12;
    case 1:
      return code == KEY_INSERT;
    case 2:
      return code == KEY_F10;
    case 3:
      return code == KEY_HOME;
    case 4:
      return code == KEY_F12;
    default:
      return code == KEY_INSERT || code == KEY_F12;
  }
}

bool is_hud_toggle_key(unsigned int code, int mode) {
  switch (mode) {
    case 0:
      return code == KEY_F11;
    case 1:
      return code == KEY_END;
    default:
      return code == KEY_F11;
  }
}

bool is_bench_toggle_key(unsigned int code, int mode) {
  switch (mode) {
    case 0:
      return code == KEY_F9;
    case 1:
      return code == KEY_PAGEUP;
    default:
      return code == KEY_F9;
  }
}

bool is_record_toggle_key(unsigned int code, int mode, bool alt_down) {
  switch (mode) {
    case 0:
      return code == KEY_F7;
    case 1:
      return (code == KEY_F9 && alt_down);
    case 2:
      return code == KEY_SCROLLLOCK;
    default:
      return code == KEY_F7;
  }
}

bool is_modifier_key(uint32_t code) noexcept {
  return code == KEY_LEFTALT || code == KEY_RIGHTALT || code == KEY_LEFTCTRL ||
         code == KEY_RIGHTCTRL || code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT ||
         code == KEY_LEFTMETA || code == KEY_RIGHTMETA;
}

bool is_mod_held(uint32_t mod_key, const std::bitset<512>& keys_down) noexcept {
  if (mod_key == 0) return true;
  if (mod_key == KEY_LEFTALT || mod_key == KEY_RIGHTALT) {
    return keys_down.test(KEY_LEFTALT) || keys_down.test(KEY_RIGHTALT);
  }
  if (mod_key == KEY_LEFTCTRL || mod_key == KEY_RIGHTCTRL) {
    return keys_down.test(KEY_LEFTCTRL) || keys_down.test(KEY_RIGHTCTRL);
  }
  if (mod_key == KEY_LEFTSHIFT || mod_key == KEY_RIGHTSHIFT) {
    return keys_down.test(KEY_LEFTSHIFT) || keys_down.test(KEY_RIGHTSHIFT);
  }
  if (mod_key == KEY_LEFTMETA || mod_key == KEY_RIGHTMETA) {
    return keys_down.test(KEY_LEFTMETA) || keys_down.test(KEY_RIGHTMETA);
  }
  return (mod_key < 512) ? keys_down.test(mod_key) : false;
}

}  // namespace

bool matches_keybind(uint32_t code, const Keybind& kb, const std::bitset<512>& keys_down) noexcept {
  if (kb.main_key == 0 || code != kb.main_key) return false;
  if (kb.mod_key != 0) {
    return is_mod_held(kb.mod_key, keys_down);
  }
  // When mod_key is 0 (bare key), ensure no standard modifiers are held
  // so modifier combos (e.g. Shift+F12, Ctrl+F7) are not masked by single-key binds.
  return !(keys_down.test(KEY_LEFTSHIFT) || keys_down.test(KEY_RIGHTSHIFT) ||
           keys_down.test(KEY_LEFTCTRL) || keys_down.test(KEY_RIGHTCTRL) ||
           keys_down.test(KEY_LEFTALT) || keys_down.test(KEY_RIGHTALT) ||
           keys_down.test(KEY_LEFTMETA) || keys_down.test(KEY_RIGHTMETA));
}

namespace {

// Forward-declared listener callback trampolines
void wl_reg_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface,
                   uint32_t version);
void wl_reg_global_remove(void* data, struct wl_registry* registry, uint32_t name);

void wl_seat_caps(void* data, struct wl_seat* seat, uint32_t capabilities);
void wl_seat_name(void* data, struct wl_seat* seat, const char* name);

void wl_kbd_keymap(void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd,
                   uint32_t size);
void wl_kbd_enter(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                  struct wl_surface* surface, struct wl_array* keys);
void wl_kbd_leave(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                  struct wl_surface* surface);
void wl_kbd_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time,
                uint32_t key, uint32_t state);
void wl_kbd_modifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                      uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
                      uint32_t group);
void wl_kbd_repeat_info(void* data, struct wl_keyboard* keyboard, int32_t rate, int32_t delay);

void wl_ptr_enter(void* data, struct wl_pointer* pointer, uint32_t serial,
                  struct wl_surface* surface, int32_t sx, int32_t sy);
void wl_ptr_leave(void* data, struct wl_pointer* pointer, uint32_t serial,
                  struct wl_surface* surface);
void wl_ptr_motion(void* data, struct wl_pointer* pointer, uint32_t time, int32_t sx, int32_t sy);
void wl_ptr_button(void* data, struct wl_pointer* pointer, uint32_t serial, uint32_t time,
                   uint32_t button, uint32_t state);
void wl_ptr_axis(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis,
                 int32_t value);
void wl_ptr_frame(void* data, struct wl_pointer* pointer);
void wl_ptr_axis_source(void* data, struct wl_pointer* pointer, uint32_t axis_source);
void wl_ptr_axis_stop(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis);
void wl_ptr_axis_discrete(void* data, struct wl_pointer* pointer, uint32_t axis, int32_t discrete);
void wl_ptr_axis_value120(void* data, struct wl_pointer* pointer, uint32_t axis, int32_t value120);
void wl_ptr_axis_relative_direction(void* data, struct wl_pointer* pointer, uint32_t axis,
                                    uint32_t direction);

struct wl_registry_listener g_registry_listener = {
    wl_reg_global,
    wl_reg_global_remove,
};

struct wl_seat_listener g_seat_listener = {
    wl_seat_caps,
    wl_seat_name,
};

struct wl_keyboard_listener g_keyboard_listener = {
    wl_kbd_keymap, wl_kbd_enter, wl_kbd_leave, wl_kbd_key, wl_kbd_modifiers, wl_kbd_repeat_info,
};

struct wl_pointer_listener g_pointer_listener = {
    wl_ptr_enter,
    wl_ptr_leave,
    wl_ptr_motion,
    wl_ptr_button,
    wl_ptr_axis,
    wl_ptr_frame,
    wl_ptr_axis_source,
    wl_ptr_axis_stop,
    wl_ptr_axis_discrete,
    wl_ptr_axis_value120,
    wl_ptr_axis_relative_direction,
};

}  // namespace

InputManager::~InputManager() {
  set_mouse_grab(false);
  close_evdev_devices();
  {
    std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
    teardown_wayland_locked();
    if (wayland_lib_) {
      dlclose(wayland_lib_);
      wayland_lib_ = nullptr;
    }
  }
  if (x11_display_ && x11_lib_) {
    using PFN_XCloseDisplay = int (*)(Display*);
    auto fn = reinterpret_cast<PFN_XCloseDisplay>(dlsym(x11_lib_, "XCloseDisplay"));
    if (fn) fn(static_cast<Display*>(x11_display_));
    x11_display_ = nullptr;
  }
  if (x11_lib_) {
    dlclose(x11_lib_);
    x11_lib_ = nullptr;
  }
}

bool InputManager::has_wayland_display() const noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  return wayland_attached_;
}

void InputManager::init_wayland_client_api() noexcept {
  if (wayland_attempted_) return;
  wayland_attempted_ = true;

  wayland_lib_ = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
  if (!wayland_lib_) {
    wayland_lib_ = dlopen("libwayland-client.so", RTLD_LAZY | RTLD_LOCAL);
  }
  if (!wayland_lib_) return;

  g_wl.fn_display_create_queue = reinterpret_cast<WaylandClientApi::PFN_create_queue>(
      dlsym(wayland_lib_, "wl_display_create_queue"));
  g_wl.fn_event_queue_destroy = reinterpret_cast<WaylandClientApi::PFN_destroy_queue>(
      dlsym(wayland_lib_, "wl_event_queue_destroy"));
  g_wl.fn_display_roundtrip_queue = reinterpret_cast<WaylandClientApi::PFN_roundtrip_queue>(
      dlsym(wayland_lib_, "wl_display_roundtrip_queue"));
  g_wl.fn_display_dispatch_queue_pending =
      reinterpret_cast<WaylandClientApi::PFN_dispatch_queue_pending>(
          dlsym(wayland_lib_, "wl_display_dispatch_queue_pending"));
  g_wl.fn_proxy_set_queue = reinterpret_cast<WaylandClientApi::PFN_proxy_set_queue>(
      dlsym(wayland_lib_, "wl_proxy_set_queue"));
  g_wl.fn_proxy_destroy = reinterpret_cast<WaylandClientApi::PFN_proxy_destroy>(
      dlsym(wayland_lib_, "wl_proxy_destroy"));
  g_wl.fn_proxy_add_listener = reinterpret_cast<WaylandClientApi::PFN_proxy_add_listener>(
      dlsym(wayland_lib_, "wl_proxy_add_listener"));
  g_wl.fn_marshal_constructor = reinterpret_cast<WaylandClientApi::PFN_proxy_marshal_constructor>(
      dlsym(wayland_lib_, "wl_proxy_marshal_constructor"));
  g_wl.fn_marshal_versioned =
      reinterpret_cast<WaylandClientApi::PFN_proxy_marshal_constructor_versioned>(
          dlsym(wayland_lib_, "wl_proxy_marshal_constructor_versioned"));

  g_wl.registry_interface =
      static_cast<const struct wl_interface*>(dlsym(wayland_lib_, "wl_registry_interface"));
  g_wl.seat_interface =
      static_cast<const struct wl_interface*>(dlsym(wayland_lib_, "wl_seat_interface"));
  g_wl.keyboard_interface =
      static_cast<const struct wl_interface*>(dlsym(wayland_lib_, "wl_keyboard_interface"));
  g_wl.pointer_interface =
      static_cast<const struct wl_interface*>(dlsym(wayland_lib_, "wl_pointer_interface"));
}

void InputManager::teardown_wayland_locked() noexcept {
  if (wayland_pointer_ && g_wl.fn_proxy_destroy) {
    g_wl.fn_proxy_destroy(static_cast<struct wl_proxy*>(wayland_pointer_));
    wayland_pointer_ = nullptr;
  }
  if (wayland_keyboard_ && g_wl.fn_proxy_destroy) {
    g_wl.fn_proxy_destroy(static_cast<struct wl_proxy*>(wayland_keyboard_));
    wayland_keyboard_ = nullptr;
  }
  if (wayland_seat_ && g_wl.fn_proxy_destroy) {
    g_wl.fn_proxy_destroy(static_cast<struct wl_proxy*>(wayland_seat_));
    wayland_seat_ = nullptr;
  }
  if (wayland_registry_ && g_wl.fn_proxy_destroy) {
    g_wl.fn_proxy_destroy(static_cast<struct wl_proxy*>(wayland_registry_));
    wayland_registry_ = nullptr;
  }
  if (wayland_queue_ && g_wl.fn_event_queue_destroy) {
    g_wl.fn_event_queue_destroy(static_cast<struct wl_event_queue*>(wayland_queue_));
    wayland_queue_ = nullptr;
  }
  wayland_display_ = nullptr;
  wayland_attached_ = false;
}

void InputManager::attach_wayland_display(void* display) noexcept {
  if (!display) return;
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (wayland_display_ == display) return;

  teardown_wayland_locked();
  wayland_display_ = display;
  wayland_attached_ = true;

  // Mock / dummy pointer for test harness
  if (display == reinterpret_cast<void*>(0x5678)) {
    return;
  }

  init_wayland_client_api();
  if (!wayland_lib_ || !g_wl.fn_display_create_queue || !g_wl.fn_marshal_constructor ||
      !g_wl.registry_interface) {
    return;
  }

  auto* dpy = static_cast<struct wl_display*>(display);
  wayland_queue_ = g_wl.fn_display_create_queue(dpy);
  if (!wayland_queue_) return;

  // Opcode 1 = wl_display.get_registry
  struct wl_proxy* reg = g_wl.fn_marshal_constructor(reinterpret_cast<struct wl_proxy*>(dpy), 1,
                                                     g_wl.registry_interface, nullptr);
  if (!reg) return;

  if (g_wl.fn_proxy_set_queue) {
    g_wl.fn_proxy_set_queue(reg, static_cast<struct wl_event_queue*>(wayland_queue_));
  }
  if (g_wl.fn_proxy_add_listener) {
    g_wl.fn_proxy_add_listener(reg, reinterpret_cast<void (**)(void)>(&g_registry_listener), this);
  }
  wayland_registry_ = reg;

  // Initial roundtrip to discover seat globals and bind keyboard
  if (g_wl.fn_display_roundtrip_queue) {
    g_wl.fn_display_roundtrip_queue(dpy, static_cast<struct wl_event_queue*>(wayland_queue_));
    if (wayland_seat_) {
      g_wl.fn_display_roundtrip_queue(dpy, static_cast<struct wl_event_queue*>(wayland_queue_));
    }
  }
}

void InputManager::detach_wayland_display(void* display) noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (display && wayland_display_ != display) return;
  teardown_wayland_locked();
}

void InputManager::start_keybind_capture(KeybindTarget target) noexcept {
  capture_target_ = target;
  capture_mod_key_ = 0;
}

void InputManager::cancel_keybind_capture() noexcept {
  capture_target_ = KeybindTarget::None;
  capture_mod_key_ = 0;
}

void InputManager::handle_key_event(uint32_t code, bool down, HudConfig& cfg, float display_width,
                                    float display_height, ImGuiIO* io) noexcept {
  if (code < 512) {
    keys_down_.set(code, down);
  }
  if (code == KEY_LEFTALT || code == KEY_RIGHTALT) {
    alt_pressed_ = keys_down_.test(KEY_LEFTALT) || keys_down_.test(KEY_RIGHTALT);
  }

  // Interactive 1-or-2-key shortcut capture mode
  if (capture_target_ != KeybindTarget::None) {
    if (code >= BTN_MISC) return;  // Ignore mouse/gamepad buttons during keyboard capture
    if (down && code == KEY_ESC) {
      cancel_keybind_capture();
      return;
    }

    auto commit_capture = [&](Keybind kb) noexcept {
      switch (capture_target_) {
        case KeybindTarget::ToggleGui:
          cfg.bind_toggle_gui = kb;
          cfg.key_toggle_gui = -1;
          break;
        case KeybindTarget::ToggleHud:
          cfg.bind_toggle_hud = kb;
          cfg.key_toggle_hud = -1;
          break;
        case KeybindTarget::ToggleBenchmark:
          cfg.bind_benchmark = kb;
          cfg.key_benchmark = -1;
          break;
        case KeybindTarget::ToggleRecord:
          cfg.bind_record = kb;
          cfg.key_record = -1;
          break;
        case KeybindTarget::SaveReplay:
          cfg.bind_save_replay = kb;
          break;
        case KeybindTarget::TakeScreenshot:
          cfg.bind_screenshot = kb;
          break;
        case KeybindTarget::None:
          break;
      }
      capture_target_ = KeybindTarget::None;
      capture_mod_key_ = 0;
    };

    if (down) {
      if (capture_mod_key_ == 0 && is_modifier_key(code)) {
        capture_mod_key_ = code;
      } else if (capture_mod_key_ != 0 && code != capture_mod_key_) {
        commit_capture(Keybind{capture_mod_key_, code});
      } else if (capture_mod_key_ == 0 && !is_modifier_key(code)) {
        commit_capture(Keybind{0, code});
      }
    } else if (capture_mod_key_ != 0 && code == capture_mod_key_) {
      // User pressed and released a modifier key by itself -> bind as single key
      commit_capture(Keybind{0, capture_mod_key_});
    }
    return;
  }

  // Hotkey toggle shortcuts trigger on key press (down == true)
  if (down) {
    const bool has_mod = (keys_down_.test(KEY_LEFTSHIFT) || keys_down_.test(KEY_RIGHTSHIFT) ||
                          keys_down_.test(KEY_LEFTCTRL) || keys_down_.test(KEY_RIGHTCTRL) ||
                          keys_down_.test(KEY_LEFTALT) || keys_down_.test(KEY_RIGHTALT) ||
                          keys_down_.test(KEY_LEFTMETA) || keys_down_.test(KEY_RIGHTMETA));

    // Check custom multi-key combo keybinds (mod_key != 0) first to prevent masking
    if (matches_keybind(code, cfg.bind_save_replay, keys_down_)) {
      RecorderController::instance().trigger_save_replay();
    } else if (matches_keybind(code, cfg.bind_screenshot, keys_down_)) {
      RecorderController::instance().request_screenshot();
    } else if (matches_keybind(code, cfg.bind_toggle_gui, keys_down_) ||
               (!has_mod && cfg.bind_toggle_gui.mod_key == 0 &&
                cfg.bind_toggle_gui.main_key == KEY_F12 && cfg.key_toggle_gui >= 0 &&
                is_gui_toggle_key(code, cfg.key_toggle_gui))) {
      cfg.gui_open = !cfg.gui_open;
    } else if (matches_keybind(code, cfg.bind_toggle_hud, keys_down_) ||
               (!has_mod && cfg.bind_toggle_hud.mod_key == 0 &&
                cfg.bind_toggle_hud.main_key == KEY_F11 && cfg.key_toggle_hud >= 0 &&
                is_hud_toggle_key(code, cfg.key_toggle_hud))) {
      cfg.hud_visible = !cfg.hud_visible;
    } else if (matches_keybind(code, cfg.bind_benchmark, keys_down_) ||
               (!has_mod && cfg.bind_benchmark.mod_key == 0 &&
                cfg.bind_benchmark.main_key == KEY_F9 && cfg.key_benchmark >= 0 &&
                is_bench_toggle_key(code, cfg.key_benchmark))) {
      get_telemetry().toggle_benchmark();
    } else if (matches_keybind(code, cfg.bind_record, keys_down_) ||
               (!has_mod && cfg.bind_record.mod_key == 0 && cfg.bind_record.main_key == KEY_F7 &&
                cfg.key_record >= 0 && is_record_toggle_key(code, cfg.key_record, alt_pressed_))) {
      RecorderController::instance().toggle_recording(cfg, display_width, display_height);
    } else if (code == KEY_ESC && cfg.gui_open) {
      cfg.gui_open = false;
    }
  }

  // Keyboard navigation when GUI menu is active
  if (io && cfg.gui_open) {
    switch (code) {
      case KEY_UP:
        io->AddKeyEvent(ImGuiKey_UpArrow, down);
        break;
      case KEY_DOWN:
        io->AddKeyEvent(ImGuiKey_DownArrow, down);
        break;
      case KEY_LEFT:
        io->AddKeyEvent(ImGuiKey_LeftArrow, down);
        break;
      case KEY_RIGHT:
        io->AddKeyEvent(ImGuiKey_RightArrow, down);
        break;
      case KEY_TAB:
        io->AddKeyEvent(ImGuiKey_Tab, down);
        break;
      case KEY_ENTER:
      case KEY_KPENTER:
        io->AddKeyEvent(ImGuiKey_Enter, down);
        break;
      case KEY_SPACE:
        io->AddKeyEvent(ImGuiKey_Space, down);
        break;
      case KEY_BACKSPACE:
        io->AddKeyEvent(ImGuiKey_Backspace, down);
        break;
      default:
        break;
    }
  }
}

void InputManager::handle_pointer_motion(float x, float y, const HudConfig& cfg,
                                         ImGuiIO* io) noexcept {
  cursor_x_ = x;
  cursor_y_ = y;
  if (io && cfg.gui_open) {
    io->AddMousePosEvent(x, y);
  }
}

void InputManager::handle_pointer_button(uint32_t button, bool down, const HudConfig& cfg,
                                         ImGuiIO* io) noexcept {
  int btn_idx = -1;
  // Linux evdev BTN_LEFT=0x110 (272), BTN_RIGHT=0x111 (273), BTN_MIDDLE=0x112 (274)
  if (button == BTN_LEFT)
    btn_idx = 0;
  else if (button == BTN_RIGHT)
    btn_idx = 1;
  else if (button == BTN_MIDDLE)
    btn_idx = 2;

  if (btn_idx >= 0) {
    mouse_buttons_[btn_idx] = down;
    if (io && (cfg.gui_open || !down)) {
      io->AddMouseButtonEvent(btn_idx, down);
    }
    if (pending_ungrab_ && !mouse_buttons_[0] && !mouse_buttons_[1] && !mouse_buttons_[2]) {
      set_mouse_grab(false);
      fpshud::on_gui_state_changed(false);
      set_x11_grab(false);
      pending_ungrab_ = false;
    }
  }
}

void InputManager::handle_pointer_wheel(float x, float y, const HudConfig& cfg,
                                        ImGuiIO* io) noexcept {
  if (io && cfg.gui_open) {
    io->AddMouseWheelEvent(x, y);
  }
}

void InputManager::register_wayland_seat(void* seat, uint32_t id) noexcept {
  (void)id;
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (!seat || !wayland_queue_) return;
  wayland_seat_ = seat;
  if (g_wl.fn_proxy_set_queue) {
    g_wl.fn_proxy_set_queue(static_cast<struct wl_proxy*>(seat),
                            static_cast<struct wl_event_queue*>(wayland_queue_));
  }
  if (g_wl.fn_proxy_add_listener) {
    g_wl.fn_proxy_add_listener(static_cast<struct wl_proxy*>(seat),
                               reinterpret_cast<void (**)(void)>(&g_seat_listener), this);
  }
}

void InputManager::register_wayland_keyboard(void* seat) noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (!seat || !wayland_queue_ || wayland_keyboard_ || !g_wl.keyboard_interface) return;
  if (!g_wl.fn_marshal_constructor) return;

  struct wl_proxy* kbd = g_wl.fn_marshal_constructor(static_cast<struct wl_proxy*>(seat), 1,
                                                     g_wl.keyboard_interface, nullptr);
  if (!kbd) return;

  if (g_wl.fn_proxy_set_queue) {
    g_wl.fn_proxy_set_queue(kbd, static_cast<struct wl_event_queue*>(wayland_queue_));
  }
  if (g_wl.fn_proxy_add_listener) {
    g_wl.fn_proxy_add_listener(kbd, reinterpret_cast<void (**)(void)>(&g_keyboard_listener), this);
  }
  wayland_keyboard_ = kbd;
}

void InputManager::register_wayland_pointer(void* seat) noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (!seat || !wayland_queue_ || wayland_pointer_ || !g_wl.pointer_interface) return;
  if (!g_wl.fn_marshal_constructor) return;

  struct wl_proxy* ptr = g_wl.fn_marshal_constructor(static_cast<struct wl_proxy*>(seat), 0,
                                                     g_wl.pointer_interface, nullptr);
  if (!ptr) return;

  if (g_wl.fn_proxy_set_queue) {
    g_wl.fn_proxy_set_queue(ptr, static_cast<struct wl_event_queue*>(wayland_queue_));
  }
  if (g_wl.fn_proxy_add_listener) {
    g_wl.fn_proxy_add_listener(ptr, reinterpret_cast<void (**)(void)>(&g_pointer_listener), this);
  }
  wayland_pointer_ = ptr;
}

void InputManager::poll_wayland(ImGuiIO& /*io*/, HudConfig& /*cfg*/, float /*display_width*/,
                                float /*display_height*/) noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (!wayland_display_ || !wayland_queue_ || !g_wl.fn_display_dispatch_queue_pending) {
    return;
  }

  g_wl.fn_display_dispatch_queue_pending(static_cast<struct wl_display*>(wayland_display_),
                                         static_cast<struct wl_event_queue*>(wayland_queue_));
}

namespace {

void wl_reg_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface,
                   uint32_t version) {
  auto* self = static_cast<InputManager*>(data);
  if (!self || !interface) return;
  if (std::strcmp(interface, "wl_seat") == 0 && g_wl.seat_interface) {
    uint32_t bind_ver = std::min(version, 7u);
    struct wl_proxy* seat = nullptr;
    if (g_wl.fn_marshal_versioned) {
      seat = g_wl.fn_marshal_versioned(reinterpret_cast<struct wl_proxy*>(registry), 0,
                                       g_wl.seat_interface, bind_ver, name,
                                       g_wl.seat_interface->name, bind_ver, nullptr);
    } else if (g_wl.fn_marshal_constructor) {
      seat = g_wl.fn_marshal_constructor(reinterpret_cast<struct wl_proxy*>(registry), 0,
                                         g_wl.seat_interface, name, g_wl.seat_interface->name,
                                         bind_ver, nullptr);
    }
    if (seat) {
      self->register_wayland_seat(seat, name);
    }
  }
}

void wl_reg_global_remove(void* /*data*/, struct wl_registry* /*registry*/, uint32_t /*name*/) {}

void wl_seat_caps(void* data, struct wl_seat* seat, uint32_t capabilities) {
  auto* self = static_cast<InputManager*>(data);
  if (!self || !seat) return;
  if (capabilities & 1) {
    self->register_wayland_keyboard(seat);
  }
  if (capabilities & 2) {
    self->register_wayland_pointer(seat);
  }
}

void wl_seat_name(void* /*data*/, struct wl_seat* /*seat*/, const char* /*name*/) {}

void wl_kbd_keymap(void* /*data*/, struct wl_keyboard* /*keyboard*/, uint32_t /*format*/,
                   int32_t fd, uint32_t /*size*/) {
  if (fd >= 0) close(fd);
}

void wl_kbd_enter(void* /*data*/, struct wl_keyboard* /*keyboard*/, uint32_t /*serial*/,
                  struct wl_surface* /*surface*/, struct wl_array* /*keys*/) {}

void wl_kbd_leave(void* /*data*/, struct wl_keyboard* /*keyboard*/, uint32_t /*serial*/,
                  struct wl_surface* /*surface*/) {}

void wl_kbd_key(void* data, struct wl_keyboard* /*keyboard*/, uint32_t /*serial*/,
                uint32_t /*time*/, uint32_t key, uint32_t state) {
  auto* self = static_cast<InputManager*>(data);
  if (!self) return;
  // Route key event through handle_key_event
  HudConfig& cfg = get_mutable_config();
  self->handle_key_event(key, state == 1, cfg, 0.0f, 0.0f, nullptr);
}

void wl_kbd_modifiers(void* /*data*/, struct wl_keyboard* /*keyboard*/, uint32_t /*serial*/,
                      uint32_t /*mods_depressed*/, uint32_t /*mods_latched*/,
                      uint32_t /*mods_locked*/, uint32_t /*group*/) {}

void wl_kbd_repeat_info(void* /*data*/, struct wl_keyboard* /*keyboard*/, int32_t /*rate*/,
                        int32_t /*delay*/) {}

void wl_ptr_enter(void* /*data*/, struct wl_pointer* /*pointer*/, uint32_t /*serial*/,
                  struct wl_surface* /*surface*/, int32_t /*sx*/, int32_t /*sy*/) {}

void wl_ptr_leave(void* /*data*/, struct wl_pointer* /*pointer*/, uint32_t /*serial*/,
                  struct wl_surface* /*surface*/) {}

void wl_ptr_motion(void* data, struct wl_pointer* /*pointer*/, uint32_t /*time*/, int32_t sx,
                   int32_t sy) {
  auto* self = static_cast<InputManager*>(data);
  if (!self) return;
  float x = static_cast<float>(sx) / 256.0f;
  float y = static_cast<float>(sy) / 256.0f;
  self->handle_pointer_motion(x, y, get_global_config(), nullptr);
}

void wl_ptr_button(void* data, struct wl_pointer* /*pointer*/, uint32_t /*serial*/,
                   uint32_t /*time*/, uint32_t button, uint32_t state) {
  auto* self = static_cast<InputManager*>(data);
  if (!self) return;
  self->handle_pointer_button(button, state == 1, get_global_config(), nullptr);
}

void wl_ptr_axis(void* /*data*/, struct wl_pointer* /*pointer*/, uint32_t /*time*/,
                 uint32_t /*axis*/, int32_t /*value*/) {}
void wl_ptr_frame(void* /*data*/, struct wl_pointer* /*pointer*/) {}
void wl_ptr_axis_source(void* /*data*/, struct wl_pointer* /*pointer*/, uint32_t /*axis_source*/) {}
void wl_ptr_axis_stop(void* /*data*/, struct wl_pointer* /*pointer*/, uint32_t /*time*/,
                      uint32_t /*axis*/) {}
void wl_ptr_axis_discrete(void* /*data*/, struct wl_pointer* /*pointer*/, uint32_t /*axis*/,
                          int32_t /*discrete*/) {}
void wl_ptr_axis_value120(void* /*data*/, struct wl_pointer* /*pointer*/, uint32_t /*axis*/,
                          int32_t /*val120*/) {}
void wl_ptr_axis_relative_direction(void* /*data*/, struct wl_pointer* /*pointer*/,
                                    uint32_t /*axis*/, uint32_t /*dir*/) {}

}  // namespace

void InputManager::scan_evdev_devices() {
  close_evdev_devices();
  std::error_code ec;
  if (!fs::exists("/dev/input", ec)) return;

  for (const auto& entry : fs::directory_iterator("/dev/input", ec)) {
    std::string fname = entry.path().filename().string();
    if (fname.rfind("event", 0) != 0) continue;

    int fd = open(entry.path().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;

    unsigned long evbit[(EV_MAX + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] = {
        0};
    unsigned long relbit[(REL_MAX + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] =
        {0};
    unsigned long keybit[(KEY_MAX + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] =
        {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0) {
      bool has_key = test_bit(EV_KEY, evbit);
      bool has_rel = test_bit(EV_REL, evbit);
      if (has_rel) ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit);
      if (has_key) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);

      bool is_mouse_dev = has_rel && test_bit(REL_X, relbit) && test_bit(BTN_LEFT, keybit);
      if (has_key || has_rel) {
        evdev_devices_.push_back({fd, is_mouse_dev, false});
        continue;
      }
    }
    close(fd);
  }
}

void InputManager::close_evdev_devices() {
  for (auto& dev : evdev_devices_) {
    if (dev.fd >= 0) {
      if (dev.is_grabbed) {
        ioctl(dev.fd, EVIOCGRAB, 0);
        dev.is_grabbed = false;
      }
      close(dev.fd);
    }
  }
  evdev_devices_.clear();
}

void InputManager::set_mouse_grab(bool grab) {
  for (auto& dev : evdev_devices_) {
    if (!dev.is_mouse || dev.fd < 0) continue;
    if (grab && !dev.is_grabbed) {
      if (ioctl(dev.fd, EVIOCGRAB, 1) == 0) {
        dev.is_grabbed = true;
      }
    } else if (!grab && dev.is_grabbed) {
      ioctl(dev.fd, EVIOCGRAB, 0);
      dev.is_grabbed = false;
    }
  }
}

void InputManager::set_x11_grab(bool grab) {
  init_x11_fallback();
  if (!x11_display_ || !x11_lib_) return;

  using PFN_XDefaultRootWindow = Window (*)(Display*);
  using PFN_XGrabPointer =
      int (*)(Display*, Window, Bool, unsigned int, int, int, Window, Cursor, Time);
  using PFN_XUngrabPointer = int (*)(Display*, Time);
  using PFN_XFlush = int (*)(Display*);

  auto* dpy = static_cast<Display*>(x11_display_);
  auto fnFlush = reinterpret_cast<PFN_XFlush>(dlsym(x11_lib_, "XFlush"));

  if (grab) {
    auto fnDefaultRoot =
        reinterpret_cast<PFN_XDefaultRootWindow>(dlsym(x11_lib_, "XDefaultRootWindow"));
    auto fnGrab = reinterpret_cast<PFN_XGrabPointer>(dlsym(x11_lib_, "XGrabPointer"));
    if (fnDefaultRoot && fnGrab) {
      Window root = fnDefaultRoot(dpy);
      // PointerMotionMask (64) | ButtonPressMask (4) | ButtonReleaseMask (8) = 76
      // GrabModeAsync (1)
      fnGrab(dpy, root, False, 76, 1, 1, 0, 0, 0);
    }
  } else {
    auto fnUngrab = reinterpret_cast<PFN_XUngrabPointer>(dlsym(x11_lib_, "XUngrabPointer"));
    if (fnUngrab) {
      fnUngrab(dpy, 0);
    }
  }
  if (fnFlush) {
    fnFlush(dpy);
  }
}

void InputManager::poll_evdev(ImGuiIO& io, HudConfig& cfg, float display_width,
                              float display_height) {
  struct input_event ev;
  for (auto& dev : evdev_devices_) {
    while (read(dev.fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
      if (ev.type == EV_KEY) {
        bool down = (ev.value != 0);

        if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE) {
          handle_pointer_button(ev.code, down, cfg, &io);
        } else {
          handle_key_event(ev.code, down, cfg, display_width, display_height, &io);
        }
      } else if (ev.type == EV_REL && cfg.gui_open) {
        if (ev.code == REL_X) {
          cursor_x_ = std::clamp(cursor_x_ + static_cast<float>(ev.value), 0.0f, display_width);
          io.AddMousePosEvent(cursor_x_, cursor_y_);
        } else if (ev.code == REL_Y) {
          cursor_y_ = std::clamp(cursor_y_ + static_cast<float>(ev.value), 0.0f, display_height);
          io.AddMousePosEvent(cursor_x_, cursor_y_);
        } else if (ev.code == REL_WHEEL) {
          io.AddMouseWheelEvent(0.0f, static_cast<float>(ev.value));
        }
      }
    }
  }

  // If ungrab was deferred until mouse button release, complete it now
  if (pending_ungrab_ && !mouse_buttons_[0] && !mouse_buttons_[1] && !mouse_buttons_[2]) {
    set_mouse_grab(false);
    fpshud::on_gui_state_changed(false);
    set_x11_grab(false);
    pending_ungrab_ = false;
  }
}

void InputManager::init_x11_fallback() {
  if (x11_attempted_) return;
  x11_attempted_ = true;

  x11_lib_ = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
  if (!x11_lib_) return;

  using PFN_XOpenDisplay = Display* (*)(const char*);
  auto fnOpen = reinterpret_cast<PFN_XOpenDisplay>(dlsym(x11_lib_, "XOpenDisplay"));
  if (fnOpen) {
    x11_display_ = fnOpen(nullptr);
  }
}

void InputManager::poll_x11_fallback(ImGuiIO& io, HudConfig& cfg, float display_width,
                                     float display_height) {
  init_x11_fallback();
  if (!x11_display_ || !x11_lib_) return;

  using PFN_XQueryKeymap = int (*)(Display*, char[32]);
  using PFN_XDefaultRootWindow = Window (*)(Display*);
  using PFN_XQueryPointer =
      Bool (*)(Display*, Window, Window*, Window*, int*, int*, int*, int*, unsigned int*);

  auto fnQueryKeymap = reinterpret_cast<PFN_XQueryKeymap>(dlsym(x11_lib_, "XQueryKeymap"));
  auto fnDefaultRoot =
      reinterpret_cast<PFN_XDefaultRootWindow>(dlsym(x11_lib_, "XDefaultRootWindow"));
  auto fnQueryPointer = reinterpret_cast<PFN_XQueryPointer>(dlsym(x11_lib_, "XQueryPointer"));

  auto* dpy = static_cast<Display*>(x11_display_);
  if (fnQueryKeymap && evdev_devices_.empty()) {
    char keys[32] = {0};
    fnQueryKeymap(dpy, keys);

    // On Linux X11/Xwayland (evdev XKB), X11 KeyCode == Linux evdev code + 8.
    // Forward any key state transitions through handle_key_event so custom 1/2-key
    // shortcuts and interactive keybind capture work identically on X11 fallback.
    for (unsigned int xkc = 8; xkc < 256; ++xkc) {
      bool down = (keys[xkc >> 3] & (1 << (xkc & 7))) != 0;
      bool prev_down = (prev_x11_keymap_[xkc >> 3] & (1 << (xkc & 7))) != 0;
      if (down != prev_down) {
        uint32_t evdev_code = xkc - 8;
        handle_key_event(evdev_code, down, cfg, display_width, display_height, &io);
      }
    }
    std::memcpy(prev_x11_keymap_, keys, sizeof(prev_x11_keymap_));
  }

  if (cfg.gui_open && fnDefaultRoot && fnQueryPointer && evdev_devices_.empty() &&
      display_width > 0.0f && display_height > 0.0f) {
    Window root = fnDefaultRoot(dpy);
    Window ret_root, ret_child;
    int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
    unsigned int mask = 0;
    if (fnQueryPointer(dpy, root, &ret_root, &ret_child, &root_x, &root_y, &win_x, &win_y, &mask)) {
      int dw = static_cast<int>(display_width);
      int dh = static_cast<int>(display_height);
      cursor_x_ = std::clamp(static_cast<float>(dw > 0 ? (root_x % dw) : 0), 0.0f, display_width);
      cursor_y_ = std::clamp(static_cast<float>(dh > 0 ? (root_y % dh) : 0), 0.0f, display_height);
      io.AddMousePosEvent(cursor_x_, cursor_y_);
      io.AddMouseButtonEvent(0, (mask & Button1Mask) != 0);
      io.AddMouseButtonEvent(1, (mask & Button3Mask) != 0);
      io.AddMouseButtonEvent(2, (mask & Button2Mask) != 0);
    }
  }
}

void InputManager::sync_grab_state(ImGuiIO& io, const HudConfig& cfg) {
  if (cfg.gui_open && !prev_gui_open_) {
    // GUI just opened: center ImGui cursor and grab mouse device so clicks don't leak into game
    if (cursor_x_ < 0.0f || cursor_y_ < 0.0f) {
      cursor_x_ = io.DisplaySize.x * 0.5f;
      cursor_y_ = io.DisplaySize.y * 0.5f;
    }
    io.AddMousePosEvent(cursor_x_, cursor_y_);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.MouseDrawCursor = true;
    pending_ungrab_ = false;
    set_mouse_grab(true);
    fpshud::on_gui_state_changed(true);
    set_x11_grab(true);
  } else if (!cfg.gui_open && prev_gui_open_) {
    // GUI just closed: auto-save current config, release ImGui cursor/inputs, and ungrab mouse back
    // to game
    save_config_to_disk(cfg);
    release_imgui_inputs(io);
    if (!mouse_buttons_[0] && !mouse_buttons_[1] && !mouse_buttons_[2]) {
      set_mouse_grab(false);
      fpshud::on_gui_state_changed(false);
      set_x11_grab(false);
      pending_ungrab_ = false;
    } else {
      // Wait for physical mouse button release before ungrabbing so game doesn't receive stray
      // release
      pending_ungrab_ = true;
    }
  } else if (pending_ungrab_ && !mouse_buttons_[0] && !mouse_buttons_[1] && !mouse_buttons_[2]) {
    set_mouse_grab(false);
    fpshud::on_gui_state_changed(false);
    set_x11_grab(false);
    pending_ungrab_ = false;
  }
  prev_gui_open_ = cfg.gui_open;
}

void InputManager::update_for_frame(ImGuiIO& io, HudConfig& cfg, float display_width,
                                    float display_height) {
  if (!cfg.enabled) {
    if (prev_gui_open_) {
      release_imgui_inputs(io);
      set_mouse_grab(false);
      fpshud::on_gui_state_changed(false);
      set_x11_grab(false);
      prev_gui_open_ = false;
      pending_ungrab_ = false;
    }
    return;
  }

  if (cursor_x_ < 0.0f || cursor_y_ < 0.0f) {
    cursor_x_ = display_width * 0.5f;
    cursor_y_ = display_height * 0.5f;
  }

  // 1. Wayland in-process client polling (zero sudo, native Wayland)
  if (wayland_attached_) {
    poll_wayland(io, cfg, display_width, display_height);
  }

  // 2. Evdev device polling (if /dev/input permissions available)
  if (!evdev_scanned_) {
    evdev_scanned_ = true;
    scan_evdev_devices();
  }

  if (!evdev_devices_.empty()) {
    poll_evdev(io, cfg, display_width, display_height);
  } else if (!wayland_attached_) {
    // 3. X11 fallback (X11 / Xwayland desktops)
    poll_x11_fallback(io, cfg, display_width, display_height);
  }

  sync_grab_state(io, cfg);
}

InputManager& get_input_manager() {
  static InputManager instance;
  return instance;
}

}  // namespace fpshud
