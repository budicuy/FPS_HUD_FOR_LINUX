#include "backends/evdev_backend.hpp"
#include "config.hpp"
#include "imgui.h"
#include "input_manager.hpp"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace fpshud {

namespace {

bool test_bit(int bit, const unsigned long* array) {
  return (array[bit / (sizeof(unsigned long) * 8)] >> (bit % (sizeof(unsigned long) * 8))) & 1;
}

}  // namespace

EvdevBackend::~EvdevBackend() { shutdown(); }

bool EvdevBackend::init() {
  if (!scanned_) {
    scan_devices();
  }
  return !devices_.empty();
}

void EvdevBackend::rescan_devices() { scan_devices(); }

void EvdevBackend::shutdown() noexcept {
  set_grab(false);
  close_devices();
  scanned_ = false;
}

void EvdevBackend::scan_devices() {
  close_devices();
  scanned_ = true;
  std::error_code ec;
  if (!fs::exists("/dev/input", ec)) return;

  for (const auto& entry : fs::directory_iterator("/dev/input", ec)) {
    std::string fname = entry.path().filename().string();
    if (fname.rfind("event", 0) != 0) continue;

    int fd = open(entry.path().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;

    unsigned long evbit[(EV_MAX + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] = {
        0};
    unsigned long relbit[(REL_MAX + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] =
        {0};
    unsigned long keybit[(KEY_MAX + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] =
        {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0) {
      bool has_key = test_bit(EV_KEY, evbit);
      bool has_rel = test_bit(EV_REL, evbit);
      if (has_rel) ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit);
      if (has_key) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);

      bool is_mouse_dev = has_rel && test_bit(REL_X, relbit) && test_bit(BTN_LEFT, keybit);
      if (has_key || has_rel) {
        devices_.push_back({fd, is_mouse_dev, false});
        continue;
      }
    }
    close(fd);
  }
  scanned_ = true;
}

void EvdevBackend::close_devices() noexcept {
  for (auto& dev : devices_) {
    if (dev.fd >= 0) {
      if (dev.is_grabbed) {
        ioctl(dev.fd, EVIOCGRAB, 0);
        dev.is_grabbed = false;
      }
      close(dev.fd);
    }
  }
  devices_.clear();
}

void EvdevBackend::set_grab(bool grab) {
  for (auto& dev : devices_) {
    if (!dev.is_mouse || dev.fd < 0) continue;
    if (grab && !dev.is_grabbed) {
      if (ioctl(dev.fd, EVIOCGRAB, 1) == 0) {
        dev.is_grabbed = true;
      }
    } else if (!grab && dev.is_grabbed) {
      ioctl(dev.fd, EVIOCGRAB, 0);
      dev.is_grabbed = false;
    }
  }
}

bool EvdevBackend::is_mouse_grabbed() const noexcept {
  for (const auto& dev : devices_) {
    if (dev.is_mouse && dev.is_grabbed) {
      return true;
    }
  }
  return false;
}

void EvdevBackend::poll(InputManager& mgr) {
  poll(mgr, mgr.active_io(), mgr.active_config(), mgr.display_width(), mgr.display_height());
}

void EvdevBackend::poll(InputManager& mgr, ImGuiIO* io, HudConfig* cfg, float display_width,
                        float display_height) {
  if (!cfg) {
    cfg = &get_mutable_config();
  }
  struct input_event ev;
  for (auto& dev : devices_) {
    while (read(dev.fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
      if (ev.type == EV_KEY) {
        bool down = (ev.value != 0);

        if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE) {
          mgr.handle_pointer_button(ev.code, down, *cfg, io);
        } else {
          mgr.handle_key_event(ev.code, down, *cfg, display_width, display_height, io);
        }
      } else if (ev.type == EV_REL && cfg->gui_open) {
        if (ev.code == REL_X) {
          float cur_x =
              std::clamp(mgr.cursor_x() + static_cast<float>(ev.value), 0.0f, display_width);
          mgr.set_cursor_pos(cur_x, mgr.cursor_y());
          if (io) io->AddMousePosEvent(mgr.cursor_x(), mgr.cursor_y());
        } else if (ev.code == REL_Y) {
          float cur_y =
              std::clamp(mgr.cursor_y() + static_cast<float>(ev.value), 0.0f, display_height);
          mgr.set_cursor_pos(mgr.cursor_x(), cur_y);
          if (io) io->AddMousePosEvent(mgr.cursor_x(), mgr.cursor_y());
        } else if (ev.code == REL_WHEEL) {
          if (io) io->AddMouseWheelEvent(0.0f, static_cast<float>(ev.value));
        }
      }
    }
  }

  // If ungrab was deferred until mouse button release, complete it now
  if (mgr.is_pending_ungrab() && !mgr.is_mouse_button_down(0) && !mgr.is_mouse_button_down(1) &&
      !mgr.is_mouse_button_down(2)) {
    set_grab(false);
    mgr.set_pending_ungrab(false);
  }
}

}  // namespace fpshud
