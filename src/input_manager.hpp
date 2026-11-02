#pragma once

#include "config.hpp"
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
    InputManager();
    ~InputManager();

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

    std::vector<EvdevDevice> evdev_devices_;
    bool evdev_scanned_ = false;
    bool prev_gui_open_ = false;
    bool pending_ungrab_ = false;

    float cursor_x_ = -1.0f;
    float cursor_y_ = -1.0f;
    bool mouse_buttons_[3] = {false, false, false};

    // Dynamic X11 fallback state
    void* x11_lib_ = nullptr;
    void* x11_display_ = nullptr;
    bool x11_attempted_ = false;
    bool prev_x11_f12_ = false;
    bool prev_x11_insert_ = false;
};

InputManager& get_input_manager();

} // namespace fpshud
