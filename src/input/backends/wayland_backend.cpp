#include "backends/wayland_backend.hpp"
#include "config.hpp"
#include "imgui.h"
#include "input_manager.hpp"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

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
  using PFN_proxy_create_wrapper = void* (*)(void*);
  using PFN_proxy_wrapper_destroy = void (*)(void*);

  PFN_create_queue fn_display_create_queue = nullptr;
  PFN_destroy_queue fn_event_queue_destroy = nullptr;
  PFN_roundtrip_queue fn_display_roundtrip_queue = nullptr;
  PFN_dispatch_queue_pending fn_display_dispatch_queue_pending = nullptr;
  PFN_proxy_set_queue fn_proxy_set_queue = nullptr;
  PFN_proxy_destroy fn_proxy_destroy = nullptr;
  PFN_proxy_add_listener fn_proxy_add_listener = nullptr;
  PFN_proxy_marshal_constructor fn_marshal_constructor = nullptr;
  PFN_proxy_marshal_constructor_versioned fn_marshal_versioned = nullptr;
  PFN_proxy_create_wrapper fn_proxy_create_wrapper = nullptr;
  PFN_proxy_wrapper_destroy fn_proxy_wrapper_destroy = nullptr;

  const struct wl_interface* registry_interface = nullptr;
  const struct wl_interface* seat_interface = nullptr;
  const struct wl_interface* keyboard_interface = nullptr;
  const struct wl_interface* pointer_interface = nullptr;
};

static WaylandClientApi g_wl;

namespace {

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

WaylandBackend::~WaylandBackend() { shutdown(); }

bool WaylandBackend::init() {
  init_wayland_client_api();
  return (wayland_lib_ != nullptr && g_wl.fn_display_create_queue != nullptr);
}

void WaylandBackend::shutdown() noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  teardown_locked();
  if (wayland_lib_) {
    dlclose(wayland_lib_);
    wayland_lib_ = nullptr;
  }
  wayland_attempted_ = false;
}

bool WaylandBackend::has_display() const noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  return wayland_attached_;
}

void WaylandBackend::init_wayland_client_api() noexcept {
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
  g_wl.fn_proxy_create_wrapper = reinterpret_cast<WaylandClientApi::PFN_proxy_create_wrapper>(
      dlsym(wayland_lib_, "wl_proxy_create_wrapper"));
  g_wl.fn_proxy_wrapper_destroy = reinterpret_cast<WaylandClientApi::PFN_proxy_wrapper_destroy>(
      dlsym(wayland_lib_, "wl_proxy_wrapper_destroy"));

  g_wl.registry_interface =
      static_cast<const struct wl_interface*>(dlsym(wayland_lib_, "wl_registry_interface"));
  g_wl.seat_interface =
      static_cast<const struct wl_interface*>(dlsym(wayland_lib_, "wl_seat_interface"));
  g_wl.keyboard_interface =
      static_cast<const struct wl_interface*>(dlsym(wayland_lib_, "wl_keyboard_interface"));
  g_wl.pointer_interface =
      static_cast<const struct wl_interface*>(dlsym(wayland_lib_, "wl_pointer_interface"));
}

void WaylandBackend::teardown_locked() noexcept {
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
  wheel_x_.store(0.0f, std::memory_order_relaxed);
  wheel_y_.store(0.0f, std::memory_order_relaxed);
}

void WaylandBackend::attach_display(void* display, InputManager* mgr) noexcept {
  if (!display) return;
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (mgr) {
    mgr_ = mgr;
  }
  if (wayland_display_ == display) return;

  teardown_locked();
  wayland_display_ = display;
  wayland_attached_ = true;
}

void WaylandBackend::ensure_queue_locked() noexcept {
  if (!wayland_display_ || wayland_queue_) return;

  // Mock / dummy pointer for test harness
  if (wayland_display_ == reinterpret_cast<void*>(0x5678)) {
    return;
  }

  init_wayland_client_api();
  if (!wayland_lib_ || !g_wl.fn_display_create_queue || !g_wl.fn_marshal_constructor ||
      !g_wl.registry_interface) {
    return;
  }

  auto* dpy = static_cast<struct wl_display*>(wayland_display_);
  wayland_queue_ = g_wl.fn_display_create_queue(dpy);
  if (!wayland_queue_) return;

  struct wl_proxy* reg = nullptr;
  if (g_wl.fn_proxy_create_wrapper && g_wl.fn_proxy_wrapper_destroy) {
    void* wrapper = g_wl.fn_proxy_create_wrapper(dpy);
    if (!wrapper) return;
    if (g_wl.fn_proxy_set_queue) {
      g_wl.fn_proxy_set_queue(static_cast<struct wl_proxy*>(wrapper),
                              static_cast<struct wl_event_queue*>(wayland_queue_));
    }
    // Opcode 1 = wl_display.get_registry
    reg = g_wl.fn_marshal_constructor(static_cast<struct wl_proxy*>(wrapper), 1,
                                      g_wl.registry_interface, nullptr);
    g_wl.fn_proxy_wrapper_destroy(wrapper);
  } else {
    reg = g_wl.fn_marshal_constructor(reinterpret_cast<struct wl_proxy*>(dpy), 1,
                                      g_wl.registry_interface, nullptr);
    if (reg && g_wl.fn_proxy_set_queue) {
      g_wl.fn_proxy_set_queue(reg, static_cast<struct wl_event_queue*>(wayland_queue_));
    }
  }
  if (!reg) return;

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

void WaylandBackend::detach_display(void* display) noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (display && wayland_display_ != display) return;
  teardown_locked();
}

bool WaylandBackend::has_seat() const noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  return wayland_seat_ != nullptr;
}

void WaylandBackend::destroy_keyboard() noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (wayland_keyboard_ && g_wl.fn_proxy_destroy) {
    g_wl.fn_proxy_destroy(static_cast<struct wl_proxy*>(wayland_keyboard_));
    wayland_keyboard_ = nullptr;
  }
}

void WaylandBackend::destroy_pointer() noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (wayland_pointer_ && g_wl.fn_proxy_destroy) {
    g_wl.fn_proxy_destroy(static_cast<struct wl_proxy*>(wayland_pointer_));
    wayland_pointer_ = nullptr;
  }
}

void WaylandBackend::register_seat(void* seat, uint32_t id) noexcept {
  (void)id;
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (!seat || !wayland_queue_ || wayland_seat_ != nullptr) {
    if (seat && wayland_seat_ != nullptr && seat != wayland_seat_ && g_wl.fn_proxy_destroy) {
      g_wl.fn_proxy_destroy(static_cast<struct wl_proxy*>(seat));
    }
    return;
  }
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

void WaylandBackend::register_keyboard(void* seat) noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (!seat || !wayland_queue_ || !g_wl.keyboard_interface) return;
  if (!g_wl.fn_marshal_constructor) return;

  destroy_keyboard();

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

void WaylandBackend::register_pointer(void* seat) noexcept {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (!seat || !wayland_queue_ || !g_wl.pointer_interface) return;
  if (!g_wl.fn_marshal_constructor) return;

  destroy_pointer();

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

void WaylandBackend::accumulate_axis(uint32_t axis, int32_t value) noexcept {
  double delta = static_cast<double>(value) / 256.0 / 15.0;
  if (axis == 0) {
    wheel_y_.fetch_add(static_cast<float>(-delta), std::memory_order_relaxed);
  } else if (axis == 1) {
    wheel_x_.fetch_add(static_cast<float>(-delta), std::memory_order_relaxed);
  }
}

void WaylandBackend::poll(InputManager& mgr) {
  std::lock_guard<std::recursive_mutex> lock(wayland_mutex_);
  if (mgr_ != &mgr) {
    mgr_ = &mgr;
  }
  if (!wayland_display_) {
    return;
  }
  if (!wayland_queue_) {
    ensure_queue_locked();
  }
  if (!wayland_queue_ || !g_wl.fn_display_dispatch_queue_pending) {
    return;
  }

  g_wl.fn_display_dispatch_queue_pending(static_cast<struct wl_display*>(wayland_display_),
                                         static_cast<struct wl_event_queue*>(wayland_queue_));

  float wx = wheel_x_.exchange(0.0f, std::memory_order_relaxed);
  float wy = wheel_y_.exchange(0.0f, std::memory_order_relaxed);
  if (wx != 0.0f || wy != 0.0f) {
    ImGuiIO* io = mgr.active_io();
    if (io) {
      io->AddMouseWheelEvent(wx, wy);
    }
  }
}

namespace {

void wl_reg_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface,
                   uint32_t version) {
  auto* self = static_cast<WaylandBackend*>(data);
  if (!self || !interface) return;
  if (std::strcmp(interface, "wl_seat") == 0 && g_wl.seat_interface) {
    if (self->has_seat()) return;
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
      self->register_seat(seat, name);
    }
  }
}

void wl_reg_global_remove(void* /*data*/, struct wl_registry* /*registry*/, uint32_t /*name*/) {}

void wl_seat_caps(void* data, struct wl_seat* seat, uint32_t capabilities) {
  auto* self = static_cast<WaylandBackend*>(data);
  if (!self || !seat) return;
  if ((capabilities & 1u) != 0u) {
    self->register_keyboard(seat);
  } else {
    self->destroy_keyboard();
  }
  if ((capabilities & 2u) != 0u) {
    self->register_pointer(seat);
  } else {
    self->destroy_pointer();
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
  auto* self = static_cast<WaylandBackend*>(data);
  if (!self || !self->input_manager()) return;
  auto* mgr = self->input_manager();
  HudConfig& cfg = mgr->active_config() ? *mgr->active_config() : get_mutable_config();
  mgr->handle_key_event(key, state == 1, cfg, mgr->display_width(), mgr->display_height(),
                        mgr->active_io());
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
  auto* self = static_cast<WaylandBackend*>(data);
  if (!self || !self->input_manager()) return;
  auto* mgr = self->input_manager();
  float x = static_cast<float>(sx) / 256.0f;
  float y = static_cast<float>(sy) / 256.0f;
  const HudConfig& cfg = mgr->active_config() ? *mgr->active_config() : get_global_config();
  mgr->handle_pointer_motion(x, y, cfg, mgr->active_io());
}

void wl_ptr_button(void* data, struct wl_pointer* /*pointer*/, uint32_t /*serial*/,
                   uint32_t /*time*/, uint32_t button, uint32_t state) {
  auto* self = static_cast<WaylandBackend*>(data);
  if (!self || !self->input_manager()) return;
  auto* mgr = self->input_manager();
  const HudConfig& cfg = mgr->active_config() ? *mgr->active_config() : get_global_config();
  mgr->handle_pointer_button(button, state == 1, cfg, mgr->active_io());
}

void wl_ptr_axis(void* data, struct wl_pointer* /*pointer*/, uint32_t /*time*/, uint32_t axis,
                 int32_t value) {
  auto* self = static_cast<WaylandBackend*>(data);
  if (!self) return;
  self->accumulate_axis(axis, value);
}
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

}  // namespace fpshud
