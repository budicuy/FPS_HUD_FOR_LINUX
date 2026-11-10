#pragma once

#include <cstdint>
#include <mutex>

namespace fpshud {

class InputManager;

class __attribute__((visibility("hidden"))) WaylandBackend {
public:
  WaylandBackend() = default;
  ~WaylandBackend();

  WaylandBackend(const WaylandBackend&) = delete;
  WaylandBackend& operator=(const WaylandBackend&) = delete;
  WaylandBackend(WaylandBackend&&) = delete;
  WaylandBackend& operator=(WaylandBackend&&) = delete;

  bool init();
  void poll(InputManager& mgr);
  void shutdown() noexcept;

  void attach_display(void* display, InputManager* mgr = nullptr) noexcept;
  void detach_display(void* display) noexcept;
  bool has_display() const noexcept;

  void register_seat(void* seat, uint32_t id) noexcept;
  void register_keyboard(void* seat) noexcept;
  void register_pointer(void* seat) noexcept;

  void set_input_manager(InputManager* mgr) noexcept { mgr_ = mgr; }
  InputManager* input_manager() const noexcept { return mgr_; }

private:
  void init_wayland_client_api() noexcept;
  void teardown_locked() noexcept;

  mutable std::recursive_mutex wayland_mutex_;
  InputManager* mgr_ = nullptr;
  void* wayland_lib_ = nullptr;
  void* wayland_display_ = nullptr;
  void* wayland_queue_ = nullptr;
  void* wayland_registry_ = nullptr;
  void* wayland_seat_ = nullptr;
  void* wayland_keyboard_ = nullptr;
  void* wayland_pointer_ = nullptr;
  bool wayland_attempted_ = false;
  bool wayland_attached_ = false;
};

}  // namespace fpshud
