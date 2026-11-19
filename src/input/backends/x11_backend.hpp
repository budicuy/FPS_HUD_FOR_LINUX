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

class X11Backend {
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

  void set_target_window(uint64_t window_id) noexcept {
    target_window_.store(window_id, std::memory_order_relaxed);
  }
  void clear_target_window_if_matches(uint64_t window_id) noexcept;
  uint64_t target_window() const noexcept { return target_window_.load(std::memory_order_relaxed); }

  bool is_grabbed() const noexcept { return is_grabbed_.load(std::memory_order_relaxed); }
  bool is_pointer_cursor_hidden() const noexcept {
    return pointer_cursor_hidden_.load(std::memory_order_relaxed);
  }

  void dispatch_mouse_buttons(InputManager& mgr, bool left, bool right, bool middle,
                              const HudConfig& cfg, ImGuiIO* io);

private:
  void init_x11();
  void worker_loop();
  void stop_worker();
  void ensure_blank_cursor_locked(void* dpy, unsigned long root) noexcept;

  void* x11_lib_ = nullptr;
  void* x11_display_ = nullptr;
  unsigned long blank_cursor_ = 0;
  bool x11_attempted_ = false;
  char prev_x11_keymap_[32] = {0};

  using PFN_XQueryKeymap = int (*)(void*, char[32]);
  using PFN_XDefaultRootWindow = unsigned long (*)(void*);
  using PFN_XQueryPointer = int (*)(void*, unsigned long, unsigned long*, unsigned long*, int*,
                                    int*, int*, int*, unsigned int*);
  using PFN_XCloseDisplay = int (*)(void*);
  using PFN_XGetInputFocus = int (*)(void*, unsigned long*, int*);
  using PFN_XTranslateCoordinates = int (*)(void*, unsigned long, unsigned long, int, int, int*,
                                            int*, unsigned long*);
  using PFN_XGrabPointer = int (*)(void*, unsigned long, int, unsigned int, int, int, unsigned long,
                                   unsigned long, unsigned long);
  using PFN_XUngrabPointer = int (*)(void*, unsigned long);
  using PFN_XGrabKeyboard = int (*)(void*, unsigned long, int, int, int, unsigned long);
  using PFN_XUngrabKeyboard = int (*)(void*, unsigned long);
  using PFN_XFlush = int (*)(void*);
  using PFN_XSync = int (*)(void*, int);
  using PFN_XFreeCursor = int (*)(void*, unsigned long);

  PFN_XQueryKeymap fn_xquery_keymap_ = nullptr;
  PFN_XDefaultRootWindow fn_xdefault_root_ = nullptr;
  PFN_XQueryPointer fn_xquery_pointer_ = nullptr;
  PFN_XCloseDisplay fn_xclose_display_ = nullptr;
  PFN_XGetInputFocus fn_xget_input_focus_ = nullptr;
  PFN_XTranslateCoordinates fn_xtranslate_coordinates_ = nullptr;
  PFN_XGrabPointer fn_xgrab_pointer_ = nullptr;
  PFN_XUngrabPointer fn_xungrab_pointer_ = nullptr;
  PFN_XGrabKeyboard fn_xgrab_keyboard_ = nullptr;
  PFN_XUngrabKeyboard fn_xungrab_keyboard_ = nullptr;
  PFN_XFlush fn_xflush_ = nullptr;
  PFN_XSync fn_xsync_ = nullptr;
  PFN_XFreeCursor fn_xfree_cursor_ = nullptr;

  struct PendingKeyEvent {
    uint32_t code = 0;
    bool down = false;
  };

  std::atomic<uint64_t> target_window_{0};
  std::atomic<bool> is_grabbed_{false};
  std::atomic<bool> keyboard_grabbed_{false};
  std::atomic<bool> pointer_cursor_hidden_{false};
  std::atomic<bool> coords_translated_{false};
  std::atomic<bool> worker_running_{false};
  std::thread worker_thread_;
  mutable std::mutex dpy_mutex_;
  mutable std::mutex event_mutex_;
  std::vector<PendingKeyEvent> pending_key_events_;
  std::atomic<float> cursor_x_{-1.0f};
  std::atomic<float> cursor_y_{-1.0f};
  std::atomic<bool> mouse_down_[3]{false, false, false};
  bool prev_mouse_down_[3]{false, false, false};
  std::atomic<bool> gui_active_{false};
};

}  // namespace fpshud
