#include "backends/x11_backend.hpp"
#include "config.hpp"
#include "imgui.h"
#include "input_manager.hpp"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#ifdef None
#undef None
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace fpshud {

X11Backend::~X11Backend() { shutdown(); }

bool X11Backend::init() {
  init_x11();
  return (x11_display_ != nullptr);
}

void X11Backend::clear_target_window_if_matches(uint64_t window_id) noexcept {
  uint64_t expected = window_id;
  target_window_.compare_exchange_strong(expected, 0ULL, std::memory_order_relaxed);
}

void X11Backend::shutdown() noexcept {
  stop_worker();
  const bool was_ptr_grabbed = is_grabbed_.exchange(false, std::memory_order_relaxed);
  const bool was_kbd_grabbed = keyboard_grabbed_.exchange(false, std::memory_order_relaxed);
  pointer_cursor_hidden_.store(false, std::memory_order_relaxed);
  coords_translated_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> dpy_lock(dpy_mutex_);
    if (x11_display_) {
      if (was_ptr_grabbed && fn_xungrab_pointer_) {
        fn_xungrab_pointer_(x11_display_, 0);
      }
      if (was_kbd_grabbed && fn_xungrab_keyboard_) {
        fn_xungrab_keyboard_(x11_display_, 0);
      }
      if (blank_cursor_ != 0 && fn_xfree_cursor_) {
        fn_xfree_cursor_(x11_display_, blank_cursor_);
        blank_cursor_ = 0;
      }
      if (fn_xclose_display_) {
        fn_xclose_display_(x11_display_);
      }
      x11_display_ = nullptr;
    }
  }
  if (x11_lib_) {
    dlclose(x11_lib_);
    x11_lib_ = nullptr;
  }
  x11_attempted_ = false;
  target_window_.store(0ULL, std::memory_order_relaxed);
  cursor_x_.store(-1.0f, std::memory_order_relaxed);
  cursor_y_.store(-1.0f, std::memory_order_relaxed);
  for (int i = 0; i < 3; ++i) {
    mouse_down_[i].store(false, std::memory_order_relaxed);
    prev_mouse_down_[i] = false;
  }
  std::memset(prev_x11_keymap_, 0, sizeof(prev_x11_keymap_));
  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    pending_key_events_.clear();
  }
}

void X11Backend::ensure_blank_cursor_locked(void* dpy, unsigned long root) noexcept {
  if (blank_cursor_ != 0 || !dpy || root == 0 || !x11_lib_) return;

  using PFN_XCreateBitmapFromData =
      Pixmap (*)(Display*, Drawable, const char*, unsigned int, unsigned int);
  using PFN_XCreatePixmapCursor =
      Cursor (*)(Display*, Pixmap, Pixmap, XColor*, XColor*, unsigned int, unsigned int);
  using PFN_XFreePixmap = int (*)(Display*, Pixmap);

  auto fn_create_bitmap =
      reinterpret_cast<PFN_XCreateBitmapFromData>(dlsym(x11_lib_, "XCreateBitmapFromData"));
  auto fn_create_cursor =
      reinterpret_cast<PFN_XCreatePixmapCursor>(dlsym(x11_lib_, "XCreatePixmapCursor"));
  auto fn_free_pixmap = reinterpret_cast<PFN_XFreePixmap>(dlsym(x11_lib_, "XFreePixmap"));
  if (!fn_create_bitmap || !fn_create_cursor || !fn_free_pixmap) return;

  static constexpr char kEmptyBitmap[1] = {0};
  auto* x_dpy = static_cast<Display*>(dpy);
  Pixmap pm = fn_create_bitmap(x_dpy, static_cast<Drawable>(root), kEmptyBitmap, 1u, 1u);
  if (pm != 0) {
    XColor black{};
    blank_cursor_ =
        static_cast<unsigned long>(fn_create_cursor(x_dpy, pm, pm, &black, &black, 0u, 0u));
    fn_free_pixmap(x_dpy, pm);
  }
}

void X11Backend::init_x11() {
  if (x11_attempted_) return;
  x11_attempted_ = true;

  x11_lib_ = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
  if (!x11_lib_) return;

  using PFN_XOpenDisplay = Display* (*)(const char*);
  using PFN_XInitThreads = int (*)();
  auto fnInitThreads = reinterpret_cast<PFN_XInitThreads>(dlsym(x11_lib_, "XInitThreads"));
  if (fnInitThreads) {
    fnInitThreads();
  }

  auto fnOpen = reinterpret_cast<PFN_XOpenDisplay>(dlsym(x11_lib_, "XOpenDisplay"));
  fn_xclose_display_ = reinterpret_cast<PFN_XCloseDisplay>(dlsym(x11_lib_, "XCloseDisplay"));
  fn_xquery_keymap_ = reinterpret_cast<PFN_XQueryKeymap>(dlsym(x11_lib_, "XQueryKeymap"));
  fn_xdefault_root_ =
      reinterpret_cast<PFN_XDefaultRootWindow>(dlsym(x11_lib_, "XDefaultRootWindow"));
  fn_xquery_pointer_ = reinterpret_cast<PFN_XQueryPointer>(dlsym(x11_lib_, "XQueryPointer"));
  fn_xget_input_focus_ = reinterpret_cast<PFN_XGetInputFocus>(dlsym(x11_lib_, "XGetInputFocus"));
  fn_xtranslate_coordinates_ =
      reinterpret_cast<PFN_XTranslateCoordinates>(dlsym(x11_lib_, "XTranslateCoordinates"));
  fn_xgrab_pointer_ = reinterpret_cast<PFN_XGrabPointer>(dlsym(x11_lib_, "XGrabPointer"));
  fn_xungrab_pointer_ = reinterpret_cast<PFN_XUngrabPointer>(dlsym(x11_lib_, "XUngrabPointer"));
  fn_xgrab_keyboard_ = reinterpret_cast<PFN_XGrabKeyboard>(dlsym(x11_lib_, "XGrabKeyboard"));
  fn_xungrab_keyboard_ = reinterpret_cast<PFN_XUngrabKeyboard>(dlsym(x11_lib_, "XUngrabKeyboard"));
  fn_xflush_ = reinterpret_cast<PFN_XFlush>(dlsym(x11_lib_, "XFlush"));
  fn_xsync_ = reinterpret_cast<PFN_XSync>(dlsym(x11_lib_, "XSync"));
  fn_xfree_cursor_ = reinterpret_cast<PFN_XFreeCursor>(dlsym(x11_lib_, "XFreeCursor"));

  if (fnOpen) {
    x11_display_ = fnOpen(nullptr);
  }

  if (x11_display_ && fn_xquery_keymap_) {
    worker_running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&X11Backend::worker_loop, this);
  }
}

void X11Backend::stop_worker() {
  if (worker_running_.exchange(false, std::memory_order_acq_rel)) {
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
  }
}

void X11Backend::worker_loop() {
#if defined(__linux__)
  pthread_setname_np(pthread_self(), "fpshud_x11");
  setpriority(PRIO_PROCESS, 0, 10);
#endif
  while (worker_running_.load(std::memory_order_relaxed)) {
    if (!x11_display_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    auto* dpy = static_cast<Display*>(x11_display_);

    // 1. Poll keys via background worker thread (zero render thread stalling)
    if (fn_xquery_keymap_) {
      char keys[32] = {0};
      {
        std::lock_guard<std::mutex> dpy_lock(dpy_mutex_);
        fn_xquery_keymap_(dpy, keys);
      }

      std::vector<PendingKeyEvent> batch;
      for (unsigned int xkc = 8; xkc < 256; ++xkc) {
        uint8_t bit = static_cast<uint8_t>(1u << (xkc & 7u));
        bool down = (static_cast<uint8_t>(keys[xkc >> 3u]) & bit) != 0u;
        bool prev_down = (static_cast<uint8_t>(prev_x11_keymap_[xkc >> 3u]) & bit) != 0u;
        if (down != prev_down) {
          batch.push_back({xkc - 8, down});
        }
      }
      std::memcpy(prev_x11_keymap_, keys, sizeof(prev_x11_keymap_));

      if (!batch.empty()) {
        std::lock_guard<std::mutex> lock(event_mutex_);
        pending_key_events_.insert(pending_key_events_.end(), batch.begin(), batch.end());
        if (pending_key_events_.size() > 256) {
          pending_key_events_.erase(
              pending_key_events_.begin(),
              pending_key_events_.begin() +
                  static_cast<std::ptrdiff_t>(pending_key_events_.size() - 256));
        }
      }
    }

    // 2. Poll pointer coordinates and mouse buttons while HUD GUI is open or X11 grab is active
    const bool ptr_grab_active = is_grabbed_.load(std::memory_order_relaxed);
    const bool kbd_grab_active = keyboard_grabbed_.load(std::memory_order_relaxed);
    if ((gui_active_.load(std::memory_order_relaxed) || ptr_grab_active || kbd_grab_active) &&
        fn_xdefault_root_ && fn_xquery_pointer_) {
      Window root = 0;
      Window ret_root = 0, ret_child = 0;
      int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
      unsigned int mask = 0;
      int ok = 0;
      int final_x = 0;
      int final_y = 0;
      bool translated = false;
      const Window target_win = static_cast<Window>(target_window_.load(std::memory_order_relaxed));
      {
        std::lock_guard<std::mutex> dpy_lock(dpy_mutex_);
        if ((ptr_grab_active || kbd_grab_active) && fn_xsync_) {
          // Discard grabbed events queued on our connection to avoid unbounded Xlib queue growth.
          fn_xsync_(dpy, 1);
        }
        root = fn_xdefault_root_(dpy);
        ok = fn_xquery_pointer_(dpy, root, &ret_root, &ret_child, &root_x, &root_y, &win_x, &win_y,
                                &mask);
        if (ok) {
          final_x = root_x;
          final_y = root_y;
          if (fn_xtranslate_coordinates_) {
            Window translate_win = target_win;
            if ((translate_win == 0 || translate_win == root) && fn_xget_input_focus_) {
              unsigned long focused_win = 0;
              int revert_to = 0;
              fn_xget_input_focus_(dpy, &focused_win, &revert_to);
              if (focused_win > 1 && focused_win != root) {
                translate_win = static_cast<Window>(focused_win);
              }
            }
            if (translate_win > 1 && translate_win != root) {
              int local_x = 0, local_y = 0;
              unsigned long child = 0;
              if (fn_xtranslate_coordinates_(dpy, root, translate_win, root_x, root_y, &local_x,
                                             &local_y, &child)) {
                final_x = local_x;
                final_y = local_y;
                translated = true;
              }
            }
          }
        }
      }
      if (ok) {
        coords_translated_.store(translated, std::memory_order_relaxed);
        cursor_x_.store(static_cast<float>(final_x), std::memory_order_relaxed);
        cursor_y_.store(static_cast<float>(final_y), std::memory_order_relaxed);
        mouse_down_[0].store((mask & Button1Mask) != 0, std::memory_order_relaxed);
        mouse_down_[1].store((mask & Button3Mask) != 0, std::memory_order_relaxed);
        mouse_down_[2].store((mask & Button2Mask) != 0, std::memory_order_relaxed);
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

void X11Backend::set_grab(bool grab) {
  if (!grab && !x11_display_) {
    is_grabbed_.store(false, std::memory_order_relaxed);
    keyboard_grabbed_.store(false, std::memory_order_relaxed);
    pointer_cursor_hidden_.store(false, std::memory_order_relaxed);
    return;
  }
  init_x11();
  if (!x11_display_ || !x11_lib_) {
    is_grabbed_.store(false, std::memory_order_relaxed);
    keyboard_grabbed_.store(false, std::memory_order_relaxed);
    pointer_cursor_hidden_.store(false, std::memory_order_relaxed);
    return;
  }

  auto* dpy = static_cast<Display*>(x11_display_);
  std::lock_guard<std::mutex> dpy_lock(dpy_mutex_);
  if (grab) {
    Window root = fn_xdefault_root_ ? fn_xdefault_root_(dpy) : 0;
    ensure_blank_cursor_locked(dpy, root);
    const unsigned long cursor_arg = blank_cursor_;

    bool ptr_ok = false;
    if (root != 0 && fn_xgrab_pointer_) {
      // PointerMotionMask (64) | ButtonPressMask (4) | ButtonReleaseMask (8) = 76
      // GrabModeAsync = 1, CurrentTime = 0
      const int res = fn_xgrab_pointer_(dpy, root, False, 76u, 1, 1, 0, cursor_arg, 0);
      ptr_ok = (res == 0 /* GrabSuccess */);
    }

    bool kbd_ok = false;
    if (root != 0 && fn_xgrab_keyboard_) {
      // GrabModeAsync = 1, CurrentTime = 0
      const int kbd_res = fn_xgrab_keyboard_(dpy, root, False, 1, 1, 0);
      kbd_ok = (kbd_res == 0 /* GrabSuccess */);
    }

    is_grabbed_.store(ptr_ok, std::memory_order_relaxed);
    keyboard_grabbed_.store(kbd_ok, std::memory_order_relaxed);
    pointer_cursor_hidden_.store(ptr_ok && (blank_cursor_ != 0), std::memory_order_relaxed);
  } else {
    if (fn_xungrab_pointer_) {
      fn_xungrab_pointer_(dpy, 0);
    }
    if (fn_xungrab_keyboard_) {
      fn_xungrab_keyboard_(dpy, 0);
    }
    is_grabbed_.store(false, std::memory_order_relaxed);
    keyboard_grabbed_.store(false, std::memory_order_relaxed);
    pointer_cursor_hidden_.store(false, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) {
      mouse_down_[i].store(false, std::memory_order_relaxed);
    }
  }
  if (fn_xflush_) {
    fn_xflush_(dpy);
  }
}

void X11Backend::dispatch_mouse_buttons(InputManager& mgr, bool left, bool right, bool middle,
                                        const HudConfig& cfg, ImGuiIO* io) {
  const bool states[3] = {left, right, middle};
  for (size_t i = 0; i < 3; ++i) {
    if (states[i] != prev_mouse_down_[i]) {
      prev_mouse_down_[i] = states[i];
      mgr.handle_x11_mouse_button(i, states[i], cfg, io);
    }
  }
}

void X11Backend::poll(InputManager& mgr) {
  poll(mgr, mgr.active_io(), mgr.active_config(), mgr.display_width(), mgr.display_height());
}

void X11Backend::poll(InputManager& mgr, ImGuiIO* io, HudConfig* cfg, float display_width,
                      float display_height) {
  init_x11();
  if (!worker_running_.load(std::memory_order_relaxed)) return;

  std::vector<PendingKeyEvent> local_events;
  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    if (!pending_key_events_.empty()) {
      local_events.swap(pending_key_events_);
    }
  }

  for (const auto& ev : local_events) {
    if (cfg) {
      mgr.handle_key_event(ev.code, ev.down, *cfg, display_width, display_height, io);
    }
  }

  if (cfg) {
    gui_active_.store(cfg->gui_open, std::memory_order_relaxed);
  }

  const bool track_pointer =
      cfg && (cfg->gui_open || is_grabbed() || keyboard_grabbed_.load(std::memory_order_relaxed) ||
              mgr.is_pending_ungrab());

  if (track_pointer) {
    dispatch_mouse_buttons(mgr, mouse_down_[0].load(std::memory_order_relaxed),
                           mouse_down_[1].load(std::memory_order_relaxed),
                           mouse_down_[2].load(std::memory_order_relaxed), *cfg, io);
  } else {
    for (int i = 0; i < 3; ++i) {
      prev_mouse_down_[i] = false;
    }
  }

  if (cfg && cfg->gui_open && display_width > 0.0f && display_height > 0.0f) {
    const float raw_x = cursor_x_.load(std::memory_order_relaxed);
    const float raw_y = cursor_y_.load(std::memory_order_relaxed);
    const bool translated = coords_translated_.load(std::memory_order_relaxed);
    if (translated || (raw_x >= 0.0f && raw_y >= 0.0f)) {
      const float cx = std::clamp(raw_x, 0.0f, display_width);
      const float cy = std::clamp(raw_y, 0.0f, display_height);
      mgr.set_cursor_pos(cx, cy);
      if (io) {
        io->AddMousePosEvent(cx, cy);
      }
    }
  }
}

}  // namespace fpshud
