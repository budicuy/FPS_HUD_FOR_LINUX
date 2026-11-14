#pragma once

#include "backends/evdev_backend.hpp"
#include "backends/wayland_backend.hpp"
#include "backends/x11_backend.hpp"
#include "config.hpp"

#include <bitset>
#include <cstdint>
#include <cstdlib>
#include <memory>

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

// Check whether a key event matches a configured keybind given currently pressed keys
bool matches_keybind(uint32_t code, const Keybind& kb, const std::bitset<512>& keys_down) noexcept;

class InputManager {
public:
  InputManager();
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

  // Polls keyboard & mouse events and updates ImGuiIO + HudConfig::gui_open
  void update_for_frame(ImGuiIO& io, HudConfig& cfg, float display_width, float display_height);

  // Called right after render_gui_menu() in case GUI was closed via a button click
  void sync_grab_state(ImGuiIO& io, const HudConfig& cfg);

  void start_keybind_capture(KeybindTarget target) noexcept;
  void cancel_keybind_capture() noexcept;
  bool is_capturing_keybind() const noexcept { return capture_target_ != KeybindTarget::None; }
  KeybindTarget capturing_target() const noexcept { return capture_target_; }

  float cursor_x() const noexcept { return cursor_x_; }
  float cursor_y() const noexcept { return cursor_y_; }
  void set_cursor_pos(float x, float y) noexcept {
    cursor_x_ = x;
    cursor_y_ = y;
  }
  bool is_mouse_button_down(int btn) const noexcept {
    return (btn >= 0 && btn < 3) ? mouse_buttons_[btn] : false;
  }
  bool is_pending_ungrab() const noexcept { return pending_ungrab_; }
  void set_pending_ungrab(bool pending) noexcept { pending_ungrab_ = pending; }
  bool is_mouse_grabbed() const noexcept;

  // Active frame context cached during update_for_frame
  ImGuiIO* active_io() const noexcept { return active_io_; }
  HudConfig* active_config() const noexcept { return active_cfg_; }
  float display_width() const noexcept { return display_width_; }
  float display_height() const noexcept { return display_height_; }

private:
  void set_mouse_grab(bool grab);
  void set_x11_grab(bool grab);

  std::unique_ptr<EvdevBackend> evdev_backend_;
  std::unique_ptr<WaylandBackend> wayland_backend_;
  std::unique_ptr<X11Backend> x11_backend_;

  bool prev_gui_open_ = false;
  bool pending_ungrab_ = false;

  float cursor_x_ = -1.0f;
  float cursor_y_ = -1.0f;
  bool mouse_buttons_[3] = {false, false, false};
  bool alt_pressed_ = false;
  std::bitset<512> keys_down_{};
  KeybindTarget capture_target_ = KeybindTarget::None;
  uint32_t capture_mod_key_ = 0;

  ImGuiIO* active_io_ = nullptr;
  HudConfig* active_cfg_ = nullptr;
  float display_width_ = 0.0f;
  float display_height_ = 0.0f;
};

InputManager& get_input_manager();

}  // namespace fpshud
