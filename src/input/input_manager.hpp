#pragma once

#include "config.hpp"
#include <bitset>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

struct ImGuiIO;

namespace fpshud {

enum class KeybindTarget {
  None = 0,
  ToggleGui,
  ToggleHud,
  ToggleBenchmark,
  ToggleRecord,
  SaveReplay,
  TakeScreenshot,
};

struct EvdevDevice {
  int fd = -1;
  bool is_mouse = false;
  bool is_grabbed = false;
};

// Check whether a key event matches a configured keybind given currently pressed keys
bool matches_keybind(uint32_t code, const Keybind& kb, const std::bitset<512>& keys_down) noexcept;

class InputManager {
public:
  InputManager() = default;
  ~InputManager();

  // Attach and detach Wayland display pointer from game (vkCreateWaylandSurfaceKHR / EGL)
  void attach_wayland_display(void* display) noexcept;
  void detach_wayland_display(void* display) noexcept;
  bool has_wayland_display() const noexcept;

  // In-process event handler for keyboard inputs (evdev, Wayland, or test harness injection)
  void handle_key_event(uint32_t code, bool down, HudConfig& cfg, float display_width,
                        float display_height, ImGuiIO* io = nullptr) noexcept;

  // In-process pointer event handler for Wayland/EGL mouse
  void handle_pointer_motion(float x, float y, const HudConfig& cfg, ImGuiIO* io) noexcept;
  void handle_pointer_button(uint32_t button, bool down, const HudConfig& cfg,
                             ImGuiIO* io) noexcept;
  void handle_pointer_wheel(float x, float y, const HudConfig& cfg, ImGuiIO* io) noexcept;

  // Wayland listener callback handlers
  void register_wayland_seat(void* seat, uint32_t id) noexcept;
  void register_wayland_keyboard(void* seat) noexcept;
  void register_wayland_pointer(void* seat) noexcept;

  // Polls keyboard & mouse events and updates ImGuiIO + HudConfig::gui_open
  void update_for_frame(ImGuiIO& io, HudConfig& cfg, float display_width, float display_height);

  // Called right after render_gui_menu() in case GUI was closed via a button click
  void sync_grab_state(ImGuiIO& io, const HudConfig& cfg);

  void start_keybind_capture(KeybindTarget target) noexcept;
  void cancel_keybind_capture() noexcept;
  bool is_capturing_keybind() const noexcept { return capture_target_ != KeybindTarget::None; }
  KeybindTarget capturing_target() const noexcept { return capture_target_; }

  ImGuiIO* current_io() const noexcept { return current_io_; }
  HudConfig* current_cfg() const noexcept { return current_cfg_; }
  float current_width() const noexcept { return current_w_; }
  float current_height() const noexcept { return current_h_; }

  float cursor_x() const noexcept { return cursor_x_; }
  float cursor_y() const noexcept { return cursor_y_; }
  bool is_mouse_button_down(int btn) const noexcept {
    return (btn >= 0 && btn < 3) ? mouse_buttons_[btn] : false;
  }
  bool is_pending_ungrab() const noexcept { return pending_ungrab_; }

private:
  void scan_evdev_devices();
  void close_evdev_devices();
  void set_mouse_grab(bool grab);
  void set_x11_grab(bool grab);
  void poll_evdev(ImGuiIO& io, HudConfig& cfg, float display_width, float display_height);

  void init_x11_fallback();
  void poll_x11_fallback(ImGuiIO& io, HudConfig& cfg, float display_width, float display_height);

  // Wayland in-process client hooking
  void init_wayland_client_api() noexcept;
  void teardown_wayland_locked() noexcept;
  void poll_wayland(ImGuiIO& io, HudConfig& cfg, float display_width,
                    float display_height) noexcept;

  std::vector<EvdevDevice> evdev_devices_;
  bool evdev_scanned_ = false;
  bool prev_gui_open_ = false;
  bool pending_ungrab_ = false;

  float cursor_x_ = -1.0f;
  float cursor_y_ = -1.0f;
  bool mouse_buttons_[3] = {false, false, false};
  bool alt_pressed_ = false;
  std::bitset<512> keys_down_{};
  KeybindTarget capture_target_ = KeybindTarget::None;
  uint32_t capture_mod_key_ = 0;

  // Dynamic X11 fallback state
  void* x11_lib_ = nullptr;
  void* x11_display_ = nullptr;
  bool x11_attempted_ = false;
  char prev_x11_keymap_[32] = {0};

  // Dynamic Wayland client state (100% sudo-free in-process)
  mutable std::recursive_mutex wayland_mutex_;
  void* wayland_lib_ = nullptr;
  void* wayland_display_ = nullptr;
  void* wayland_queue_ = nullptr;
  void* wayland_registry_ = nullptr;
  void* wayland_seat_ = nullptr;
  void* wayland_keyboard_ = nullptr;
  void* wayland_pointer_ = nullptr;
  bool wayland_attempted_ = false;
  bool wayland_attached_ = false;

  // Per-dispatch frame context
  ImGuiIO* current_io_ = nullptr;
  HudConfig* current_cfg_ = nullptr;
  float current_w_ = 0.0f;
  float current_h_ = 0.0f;
};

InputManager& get_input_manager();

}  // namespace fpshud
