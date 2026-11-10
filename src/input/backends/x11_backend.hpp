#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

struct ImGuiIO;

namespace fpshud {

class InputManager;
struct HudConfig;

class __attribute__((visibility("hidden"))) X11Backend {
public:
  X11Backend() = default;
  ~X11Backend();

  X11Backend(const X11Backend&) = delete;
  X11Backend& operator=(const X11Backend&) = delete;
  X11Backend(X11Backend&&) = delete;
  X11Backend& operator=(X11Backend&&) = delete;

  bool init();
  void poll(InputManager& mgr);
  void poll(InputManager& mgr, ImGuiIO* io, HudConfig* cfg, float display_width,
            float display_height);
  void set_grab(bool grab);
  void shutdown() noexcept;

  bool is_active() const noexcept;

private:
  void init_x11();
  void worker_loop();
  void stop_worker();

  void* x11_lib_ = nullptr;
  void* x11_display_ = nullptr;
  bool x11_attempted_ = false;
  char prev_x11_keymap_[32] = {0};

  using PFN_XQueryKeymap = int (*)(void*, char[32]);
  using PFN_XDefaultRootWindow = unsigned long (*)(void*);
  using PFN_XQueryPointer = int (*)(void*, unsigned long, unsigned long*, unsigned long*, int*,
                                    int*, int*, int*, unsigned int*);
  using PFN_XCloseDisplay = int (*)(void*);

  PFN_XQueryKeymap fn_xquery_keymap_ = nullptr;
  PFN_XDefaultRootWindow fn_xdefault_root_ = nullptr;
  PFN_XQueryPointer fn_xquery_pointer_ = nullptr;
  PFN_XCloseDisplay fn_xclose_display_ = nullptr;

  struct PendingKeyEvent {
    uint32_t code = 0;
    bool down = false;
  };

  std::atomic<bool> worker_running_{false};
  std::thread worker_thread_;
  mutable std::mutex dpy_mutex_;
  mutable std::mutex event_mutex_;
  std::vector<PendingKeyEvent> pending_key_events_;
  std::atomic<float> cursor_x_{-1.0f};
  std::atomic<float> cursor_y_{-1.0f};
  std::atomic<bool> mouse_down_[3]{false, false, false};
  std::atomic<bool> gui_active_{false};
};

}  // namespace fpshud
