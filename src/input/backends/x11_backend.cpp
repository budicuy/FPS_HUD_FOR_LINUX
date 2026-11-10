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

void X11Backend::shutdown() noexcept {
  stop_worker();
  is_grabbed_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> dpy_lock(dpy_mutex_);
    if (x11_display_ && fn_xclose_display_) {
      fn_xclose_display_(x11_display_);
      x11_display_ = nullptr;
    }
  }
  if (x11_lib_) {
    dlclose(x11_lib_);
    x11_lib_ = nullptr;
  }
  x11_attempted_ = false;
  std::memset(prev_x11_keymap_, 0, sizeof(prev_x11_keymap_));
  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    pending_key_events_.clear();
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
        bool down = (keys[xkc >> 3] & (1 << (xkc & 7))) != 0;
        bool prev_down = (prev_x11_keymap_[xkc >> 3] & (1 << (xkc & 7))) != 0;
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
              pending_key_events_.begin() + (pending_key_events_.size() - 256));
        }
      }
    }

    // 2. Poll pointer coordinates and mouse buttons if HUD GUI is active
    if (gui_active_.load(std::memory_order_relaxed) && fn_xdefault_root_ && fn_xquery_pointer_) {
      Window root = 0;
      Window ret_root = 0, ret_child = 0;
      int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
      unsigned int mask = 0;
      int ok = 0;
      int final_x = 0;
      int final_y = 0;
      {
        std::lock_guard<std::mutex> dpy_lock(dpy_mutex_);
        root = fn_xdefault_root_(dpy);
        ok = fn_xquery_pointer_(dpy, root, &ret_root, &ret_child, &root_x, &root_y, &win_x, &win_y,
                                &mask);
        if (ok) {
          final_x = root_x;
          final_y = root_y;
          if (fn_xget_input_focus_ && fn_xtranslate_coordinates_) {
            unsigned long focused_win = 0;
            int revert_to = 0;
            fn_xget_input_focus_(dpy, &focused_win, &revert_to);
            if (focused_win > 1) {
              int local_x = 0, local_y = 0;
              unsigned long child = 0;
              if (fn_xtranslate_coordinates_(dpy, root, focused_win, root_x, root_y, &local_x,
                                             &local_y, &child)) {
                final_x = local_x;
                final_y = local_y;
              }
            }
          }
        }
      }
      if (ok) {
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
    return;
  }
  init_x11();
  if (!x11_display_ || !x11_lib_) return;

  using PFN_XDefaultRootWindow = Window (*)(Display*);
  using PFN_XGrabPointer =
      int (*)(Display*, Window, Bool, unsigned int, int, int, Window, Cursor, Time);
  using PFN_XUngrabPointer = int (*)(Display*, Time);
  using PFN_XFlush = int (*)(Display*);

  auto* dpy = static_cast<Display*>(x11_display_);
  auto fnFlush = reinterpret_cast<PFN_XFlush>(dlsym(x11_lib_, "XFlush"));

  std::lock_guard<std::mutex> dpy_lock(dpy_mutex_);
  if (grab) {
    auto fnDefaultRoot =
        reinterpret_cast<PFN_XDefaultRootWindow>(dlsym(x11_lib_, "XDefaultRootWindow"));
    auto fnGrab = reinterpret_cast<PFN_XGrabPointer>(dlsym(x11_lib_, "XGrabPointer"));
    if (fnDefaultRoot && fnGrab) {
      Window root = fnDefaultRoot(dpy);
      // PointerMotionMask (64) | ButtonPressMask (4) | ButtonReleaseMask (8) = 76
      // GrabModeAsync (1)
      int res = fnGrab(dpy, root, False, 76, 1, 1, 0, 0, 0);
      is_grabbed_.store(res == 0 /* GrabSuccess */, std::memory_order_relaxed);
    }
  } else {
    auto fnUngrab = reinterpret_cast<PFN_XUngrabPointer>(dlsym(x11_lib_, "XUngrabPointer"));
    if (fnUngrab) {
      fnUngrab(dpy, 0);
    }
    is_grabbed_.store(false, std::memory_order_relaxed);
  }
  if (fnFlush) {
    fnFlush(dpy);
  }
}

void X11Backend::poll(InputManager& mgr) {
  poll(mgr, mgr.active_io(), mgr.active_config(), mgr.display_width(), mgr.display_height());
}

void X11Backend::poll(InputManager& mgr, ImGuiIO* io, HudConfig* cfg, float display_width,
                      float display_height) {
  init_x11();
  if (!worker_running_.load(std::memory_order_relaxed)) return;

  if (cfg) {
    gui_active_.store(cfg->gui_open, std::memory_order_relaxed);
  }

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

  if (cfg && cfg->gui_open && display_width > 0.0f && display_height > 0.0f) {
    float raw_x = cursor_x_.load(std::memory_order_relaxed);
    float raw_y = cursor_y_.load(std::memory_order_relaxed);
    if (raw_x >= 0.0f && raw_y >= 0.0f) {
      float cx = std::clamp(raw_x, 0.0f, display_width);
      float cy = std::clamp(raw_y, 0.0f, display_height);
      mgr.set_cursor_pos(cx, cy);
      if (io) {
        io->AddMousePosEvent(cx, cy);
        io->AddMouseButtonEvent(0, mouse_down_[0].load(std::memory_order_relaxed));
        io->AddMouseButtonEvent(1, mouse_down_[1].load(std::memory_order_relaxed));
        io->AddMouseButtonEvent(2, mouse_down_[2].load(std::memory_order_relaxed));
      }
    }
  }
}

}  // namespace fpshud
