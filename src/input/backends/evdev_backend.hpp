#pragma once

#include <cstdint>
#include <vector>

struct ImGuiIO;

namespace fpshud {

class InputManager;
struct HudConfig;

struct EvdevDevice {
  int fd = -1;
  bool is_mouse = false;
  bool is_grabbed = false;
};

class EvdevBackend {
public:
  EvdevBackend() = default;
  ~EvdevBackend();

  EvdevBackend(const EvdevBackend&) = delete;
  EvdevBackend& operator=(const EvdevBackend&) = delete;
  EvdevBackend(EvdevBackend&&) = delete;
  EvdevBackend& operator=(EvdevBackend&&) = delete;

  bool init();
  void poll(InputManager& mgr);
  void poll(InputManager& mgr, ImGuiIO* io, HudConfig* cfg, float display_width,
            float display_height);
  void set_grab(bool grab);
  void shutdown() noexcept;

  bool is_mouse_grabbed() const noexcept;
  const std::vector<EvdevDevice>& devices() const noexcept { return devices_; }
  void rescan_devices();

private:
  void scan_devices();
  void close_devices() noexcept;

  std::vector<EvdevDevice> devices_;
  bool scanned_ = false;
};

}  // namespace fpshud
