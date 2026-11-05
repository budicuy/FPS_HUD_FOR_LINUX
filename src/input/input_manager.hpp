#pragma once

#include "config.hpp"
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

struct ImGuiIO;

namespace fpshud {

struct EvdevDevice {
  int fd = -1;
  bool is_mouse = false;
  bool is_grabbed = false;
};

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

  // Wayland listener callback handlers
  void register_wayland_seat(void* seat, uint32_t id) noexcept;
  void register_wayland_keyboard(void* seat) noexcept;
  void register_wayland_pointer(void* seat) noexcept;

  // Polls keyboard & mouse events and updates ImGuiIO + HudConfig::gui_open
  void update_for_frame(ImGuiIO& io, HudConfig& cfg, float display_width, float display_height);

  // Called right after render_gui_menu() in case GUI was closed via a button click
  void sync_grab_state(ImGuiIO& io, const HudConfig& cfg);

  bool has_evdev_access() const { return !evdev_devices_.empty(); }

private:
  void scan_evdev_devices();
  void close_evdev_devices();
  void set_mouse_grab(bool grab);
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

  // Dynamic X11 fallback state
  void* x11_lib_ = nullptr;
  void* x11_display_ = nullptr;
  bool x11_attempted_ = false;
  bool prev_x11_f12_ = false;
  bool prev_x11_insert_ = false;
  bool prev_x11_f7_ = false;

  // Dynamic Wayland client state (100% sudo-free in-process)
  mutable std::mutex wayland_mutex_;
  void* wayland_lib_ = nullptr;
  void* wayland_display_ = nullptr;
  void* wayland_queue_ = nullptr;
  void* wayland_registry_ = nullptr;
  void* wayland_seat_ = nullptr;
  void* wayland_keyboard_ = nullptr;
  void* wayland_pointer_ = nullptr;
  uint32_t wayland_seat_id_ = 0;
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
