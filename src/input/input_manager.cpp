#include "input_manager.hpp"
#include "gui_menu.hpp"
#include "recorder_ipc.hpp"
#include "telemetry.hpp"

#include "imgui.h"

#include <linux/input.h>

#include <algorithm>
#include <cfloat>

namespace fpshud {

namespace {

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

InputManager::InputManager()
    : evdev_backend_(std::make_unique<EvdevBackend>()),
      wayland_backend_(std::make_unique<WaylandBackend>()),
      x11_backend_(std::make_unique<X11Backend>()) {
  wayland_backend_->set_input_manager(this);
}

InputManager::~InputManager() {
  set_mouse_grab(false);
  set_x11_grab(false);
  if (evdev_backend_) evdev_backend_->shutdown();
  if (wayland_backend_) wayland_backend_->shutdown();
  if (x11_backend_) x11_backend_->shutdown();
}

bool InputManager::has_wayland_display() const noexcept {
  return wayland_backend_ ? wayland_backend_->has_display() : false;
}

void InputManager::attach_wayland_display(void* display) noexcept {
  if (wayland_backend_) {
    wayland_backend_->attach_display(display, this);
  }
}

void InputManager::detach_wayland_display(void* display) noexcept {
  if (wayland_backend_) {
    wayland_backend_->detach_display(display);
  }
}

void InputManager::register_wayland_seat(void* seat, uint32_t id) noexcept {
  if (wayland_backend_) {
    wayland_backend_->register_seat(seat, id);
  }
}

void InputManager::register_wayland_keyboard(void* seat) noexcept {
  if (wayland_backend_) {
    wayland_backend_->register_keyboard(seat);
  }
}

void InputManager::register_wayland_pointer(void* seat) noexcept {
  if (wayland_backend_) {
    wayland_backend_->register_pointer(seat);
  }
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
      if (!is_player_modal_active()) {
        cfg.gui_open = false;
      }
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
      case KEY_ESC:
        io->AddKeyEvent(ImGuiKey_Escape, down);
        break;
      case KEY_F:
        io->AddKeyEvent(ImGuiKey_F, down);
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

bool InputManager::is_mouse_grabbed() const noexcept {
  return evdev_backend_ ? evdev_backend_->is_mouse_grabbed() : false;
}

void InputManager::set_mouse_grab(bool grab) {
  if (evdev_backend_) {
    evdev_backend_->set_grab(grab);
  }
}

void InputManager::set_x11_grab(bool grab) {
  if (x11_backend_) {
    x11_backend_->set_grab(grab);
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
    pending_ungrab_ = false;
    set_mouse_grab(true);
    set_x11_grab(true);
    // Draw software cursor ONLY when physical mouse is grabbed by kernel.
    // In fallback mode, the OS hardware cursor is already visible so drawing
    // a second cursor causes double cursor.
    io.MouseDrawCursor = is_mouse_grabbed();
  } else if (!cfg.gui_open && prev_gui_open_) {
    // GUI just closed: auto-save current config, release ImGui cursor/inputs, and ungrab mouse back
    // to game
    save_config_to_disk(cfg);
    release_imgui_inputs(io);
    if (!mouse_buttons_[0] && !mouse_buttons_[1] && !mouse_buttons_[2]) {
      set_mouse_grab(false);
      set_x11_grab(false);
      pending_ungrab_ = false;
    } else {
      // Wait for physical mouse button release before ungrabbing so game doesn't receive stray
      // release
      pending_ungrab_ = true;
    }
  } else if (pending_ungrab_ && !mouse_buttons_[0] && !mouse_buttons_[1] && !mouse_buttons_[2]) {
    set_mouse_grab(false);
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

  active_io_ = &io;
  active_cfg_ = &cfg;
  display_width_ = display_width;
  display_height_ = display_height;

  // 1. Wayland in-process client polling (zero sudo, native Wayland)
  if (wayland_backend_ && wayland_backend_->has_display()) {
    wayland_backend_->poll(*this);
  }

  // 2. Evdev device polling (if /dev/input permissions available)
  if (evdev_backend_) {
    if (evdev_backend_->devices().empty()) {
      evdev_backend_->init();
    }
    if (!evdev_backend_->devices().empty()) {
      evdev_backend_->poll(*this);
    } else if (!wayland_backend_ || !wayland_backend_->has_display()) {
      // 3. X11 fallback (X11 / Xwayland desktops)
      if (x11_backend_) {
        x11_backend_->poll(*this);
      }
    }
  }

  sync_grab_state(io, cfg);

  active_io_ = nullptr;
  active_cfg_ = nullptr;
}

InputManager& get_input_manager() {
  static InputManager instance;
  return instance;
}

}  // namespace fpshud
