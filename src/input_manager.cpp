#include "input_manager.hpp"
#include "recorder_ipc.hpp"
#include "telemetry.hpp"

#include "imgui.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace fpshud {

namespace {

bool test_bit(int bit, const unsigned long* array) {
    return (array[bit / (sizeof(unsigned long) * 8)] >> (bit % (sizeof(unsigned long) * 8))) & 1;
}

void release_imgui_inputs(ImGuiIO& io) {
    io.MouseDrawCursor = false;
    io.AddMouseButtonEvent(0, false);
    io.AddMouseButtonEvent(1, false);
    io.AddMouseButtonEvent(2, false);
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    io.ClearInputKeys();
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
}

bool is_gui_toggle_key(unsigned int code, int mode) {
    switch (mode) {
        case 0: return code == KEY_INSERT || code == KEY_F12 || code == KEY_F10;
        case 1: return code == KEY_INSERT;
        case 2: return code == KEY_F10;
        case 3: return code == KEY_HOME;
        case 4: return code == KEY_F12;
        default: return code == KEY_INSERT || code == KEY_F12;
    }
}

bool is_hud_toggle_key(unsigned int code, int mode) {
    switch (mode) {
        case 0: return code == KEY_F11;
        case 1: return code == KEY_END;
        default: return code == KEY_F11;
    }
}

bool is_bench_toggle_key(unsigned int code, int mode) {
    switch (mode) {
        case 0: return code == KEY_F9;
        case 1: return code == KEY_PAGEUP;
        default: return code == KEY_F9;
    }
}

bool is_record_toggle_key(unsigned int code, int mode, bool alt_down) {
    switch (mode) {
        case 0: return code == KEY_F7;
        case 1: return (code == KEY_F9 && alt_down);
        case 2: return code == KEY_SCROLLLOCK;
        default: return code == KEY_F7;
    }
}

bool is_replay_save_key(unsigned int code, int mode, bool alt_down) {
    switch (mode) {
        case 0: return code == KEY_F8;
        case 1: return (code == KEY_F10 && alt_down);
        case 2: return code == KEY_PAUSE;
        default: return code == KEY_F8;
    }
}

} // namespace

InputManager::InputManager() = default;

InputManager::~InputManager() {
    set_mouse_grab(false);
    close_evdev_devices();
    if (x11_display_ && x11_lib_) {
        using PFN_XCloseDisplay = int (*)(Display*);
        auto fn = reinterpret_cast<PFN_XCloseDisplay>(dlsym(x11_lib_, "XCloseDisplay"));
        if (fn) fn(static_cast<Display*>(x11_display_));
        x11_display_ = nullptr;
    }
    if (x11_lib_) {
        dlclose(x11_lib_);
        x11_lib_ = nullptr;
    }
}

void InputManager::scan_evdev_devices() {
    close_evdev_devices();
    std::error_code ec;
    if (!fs::exists("/dev/input", ec)) return;

    for (const auto& entry : fs::directory_iterator("/dev/input", ec)) {
        std::string fname = entry.path().filename().string();
        if (fname.rfind("event", 0) != 0) continue;

        int fd = open(entry.path().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        unsigned long evbit[(EV_MAX + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] = {0};
        unsigned long relbit[(REL_MAX + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] = {0};
        unsigned long keybit[(KEY_MAX + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] = {0};

        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0) {
            bool has_key = test_bit(EV_KEY, evbit);
            bool has_rel = test_bit(EV_REL, evbit);
            if (has_rel) ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit);
            if (has_key) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);

            bool is_mouse_dev = has_rel && test_bit(REL_X, relbit) && test_bit(BTN_LEFT, keybit);
            if (has_key || has_rel) {
                evdev_devices_.push_back({fd, is_mouse_dev, false});
                continue;
            }
        }
        close(fd);
    }
}

void InputManager::close_evdev_devices() {
    for (auto& dev : evdev_devices_) {
        if (dev.fd >= 0) {
            if (dev.is_grabbed) {
                ioctl(dev.fd, EVIOCGRAB, 0);
                dev.is_grabbed = false;
            }
            close(dev.fd);
        }
    }
    evdev_devices_.clear();
}

void InputManager::set_mouse_grab(bool grab) {
    for (auto& dev : evdev_devices_) {
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

void InputManager::poll_evdev(ImGuiIO& io, HudConfig& cfg, float display_width, float display_height) {
    struct input_event ev;
    for (auto& dev : evdev_devices_) {
        while (read(dev.fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
            if (ev.type == EV_KEY) {
                bool down = (ev.value != 0);

                // Always track mouse button state so button releases are never missed when closing GUI
                if (ev.code == BTN_LEFT) {
                    mouse_buttons_[0] = down;
                    if (cfg.gui_open || !down) io.AddMouseButtonEvent(0, down);
                } else if (ev.code == BTN_RIGHT) {
                    mouse_buttons_[1] = down;
                    if (cfg.gui_open || !down) io.AddMouseButtonEvent(1, down);
                } else if (ev.code == BTN_MIDDLE) {
                    mouse_buttons_[2] = down;
                    if (cfg.gui_open || !down) io.AddMouseButtonEvent(2, down);
                }

                if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) {
                    alt_pressed_ = down;
                }

                // Toggle shortcuts on key press (value == 1)
                if (ev.value == 1) {
                    if (is_gui_toggle_key(ev.code, cfg.key_toggle_gui)) {
                        cfg.gui_open = !cfg.gui_open;
                    } else if (is_hud_toggle_key(ev.code, cfg.key_toggle_hud)) {
                        cfg.hud_visible = !cfg.hud_visible;
                    } else if (is_bench_toggle_key(ev.code, cfg.key_benchmark)) {
                        get_telemetry().toggle_benchmark();
                    } else if (is_record_toggle_key(ev.code, cfg.key_record, alt_pressed_)) {
                        RecorderController::instance().toggle_recording(cfg, display_width, display_height);
                    } else if (is_replay_save_key(ev.code, cfg.key_replay, alt_pressed_)) {
                        RecorderController::instance().save_instant_replay(cfg);
                    } else if (ev.code == KEY_ESC && cfg.gui_open) {
                        cfg.gui_open = false;
                    }
                }

                if (cfg.gui_open) {
                    switch (ev.code) {
                        case KEY_UP:
                            io.AddKeyEvent(ImGuiKey_UpArrow, down);
                            break;
                        case KEY_DOWN:
                            io.AddKeyEvent(ImGuiKey_DownArrow, down);
                            break;
                        case KEY_LEFT:
                            io.AddKeyEvent(ImGuiKey_LeftArrow, down);
                            break;
                        case KEY_RIGHT:
                            io.AddKeyEvent(ImGuiKey_RightArrow, down);
                            break;
                        case KEY_TAB:
                            io.AddKeyEvent(ImGuiKey_Tab, down);
                            break;
                        case KEY_ENTER:
                        case KEY_KPENTER:
                            io.AddKeyEvent(ImGuiKey_Enter, down);
                            break;
                        case KEY_SPACE:
                            io.AddKeyEvent(ImGuiKey_Space, down);
                            break;
                        case KEY_BACKSPACE:
                            io.AddKeyEvent(ImGuiKey_Backspace, down);
                            break;
                        default:
                            break;
                    }
                }
            } else if (ev.type == EV_REL && cfg.gui_open) {
                if (ev.code == REL_X) {
                    cursor_x_ = std::clamp(cursor_x_ + static_cast<float>(ev.value), 0.0f, display_width);
                    io.AddMousePosEvent(cursor_x_, cursor_y_);
                } else if (ev.code == REL_Y) {
                    cursor_y_ = std::clamp(cursor_y_ + static_cast<float>(ev.value), 0.0f, display_height);
                    io.AddMousePosEvent(cursor_x_, cursor_y_);
                } else if (ev.code == REL_WHEEL) {
                    io.AddMouseWheelEvent(0.0f, static_cast<float>(ev.value));
                }
            }
        }
    }

    // If ungrab was deferred until mouse button release, complete it now
    if (pending_ungrab_ && !mouse_buttons_[0] && !mouse_buttons_[1] && !mouse_buttons_[2]) {
        set_mouse_grab(false);
        pending_ungrab_ = false;
    }
}

void InputManager::init_x11_fallback() {
    if (x11_attempted_) return;
    x11_attempted_ = true;

    x11_lib_ = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!x11_lib_) return;

    using PFN_XOpenDisplay = Display* (*)(const char*);
    auto fnOpen = reinterpret_cast<PFN_XOpenDisplay>(dlsym(x11_lib_, "XOpenDisplay"));
    if (fnOpen) {
        x11_display_ = fnOpen(nullptr);
    }
}

void InputManager::poll_x11_fallback(ImGuiIO& io, HudConfig& cfg, float display_width, float display_height) {
    init_x11_fallback();
    if (!x11_display_ || !x11_lib_) return;

    using PFN_XKeysymToKeycode = KeyCode (*)(Display*, KeySym);
    using PFN_XQueryKeymap = int (*)(Display*, char[32]);
    using PFN_XDefaultRootWindow = Window (*)(Display*);
    using PFN_XQueryPointer = Bool (*)(Display*, Window, Window*, Window*, int*, int*, int*, int*, unsigned int*);

    auto fnKeysymToKeycode = reinterpret_cast<PFN_XKeysymToKeycode>(dlsym(x11_lib_, "XKeysymToKeycode"));
    auto fnQueryKeymap = reinterpret_cast<PFN_XQueryKeymap>(dlsym(x11_lib_, "XQueryKeymap"));
    auto fnDefaultRoot = reinterpret_cast<PFN_XDefaultRootWindow>(dlsym(x11_lib_, "XDefaultRootWindow"));
    auto fnQueryPointer = reinterpret_cast<PFN_XQueryPointer>(dlsym(x11_lib_, "XQueryPointer"));

    auto* dpy = static_cast<Display*>(x11_display_);
    if (fnKeysymToKeycode && fnQueryKeymap && evdev_devices_.empty()) {
        char keys[32] = {0};
        fnQueryKeymap(dpy, keys);

        KeyCode kc_f12 = fnKeysymToKeycode(dpy, XK_F12);
        KeyCode kc_ins = fnKeysymToKeycode(dpy, XK_Insert);
        KeyCode kc_f7  = fnKeysymToKeycode(dpy, XK_F7);
        KeyCode kc_f8  = fnKeysymToKeycode(dpy, XK_F8);

        bool f12_down = kc_f12 && (keys[kc_f12 >> 3] & (1 << (kc_f12 & 7)));
        bool ins_down = kc_ins && (keys[kc_ins >> 3] & (1 << (kc_ins & 7)));
        bool f7_down  = kc_f7  && (keys[kc_f7  >> 3] & (1 << (kc_f7  & 7)));
        bool f8_down  = kc_f8  && (keys[kc_f8  >> 3] & (1 << (kc_f8  & 7)));

        if ((f12_down && !prev_x11_f12_) || (ins_down && !prev_x11_insert_)) {
            cfg.gui_open = !cfg.gui_open;
        }
        if (f7_down && !prev_x11_f7_) {
            RecorderController::instance().toggle_recording(cfg, display_width, display_height);
        }
        if (f8_down && !prev_x11_f8_) {
            RecorderController::instance().save_instant_replay(cfg);
        }
        prev_x11_f12_ = f12_down;
        prev_x11_insert_ = ins_down;
        prev_x11_f7_ = f7_down;
        prev_x11_f8_ = f8_down;
    }

    if (cfg.gui_open && fnDefaultRoot && fnQueryPointer && evdev_devices_.empty() && display_width > 0.0f && display_height > 0.0f) {
        Window root = fnDefaultRoot(dpy);
        Window ret_root, ret_child;
        int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
        unsigned int mask = 0;
        if (fnQueryPointer(dpy, root, &ret_root, &ret_child, &root_x, &root_y, &win_x, &win_y, &mask)) {
            int dw = static_cast<int>(display_width);
            int dh = static_cast<int>(display_height);
            cursor_x_ = std::clamp(static_cast<float>(dw > 0 ? (root_x % dw) : 0), 0.0f, display_width);
            cursor_y_ = std::clamp(static_cast<float>(dh > 0 ? (root_y % dh) : 0), 0.0f, display_height);
            io.AddMousePosEvent(cursor_x_, cursor_y_);
            io.AddMouseButtonEvent(0, (mask & Button1Mask) != 0);
            io.AddMouseButtonEvent(1, (mask & Button3Mask) != 0);
            io.AddMouseButtonEvent(2, (mask & Button2Mask) != 0);
        }
    }
}

void InputManager::sync_grab_state(ImGuiIO& io, const HudConfig& cfg) {
    if (cfg.gui_open && !prev_gui_open_) {
        // GUI just opened: center ImGui cursor and grab mouse device so clicks don't leak into game
        if (cursor_x_ < 0.0f || cursor_y_ < 0.0f) {
            cursor_x_ = io.DisplaySize.x * 0.5f;
            cursor_y_ = io.DisplaySize.y * 0.5f;
        }
        io.AddMousePosEvent(cursor_x_, cursor_y_);
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.MouseDrawCursor = true;
        pending_ungrab_ = false;
        set_mouse_grab(true);
    } else if (!cfg.gui_open && prev_gui_open_) {
        // GUI just closed: auto-save current config, release ImGui cursor/inputs, and ungrab mouse back to game
        save_config_to_disk(cfg);
        release_imgui_inputs(io);
        if (!mouse_buttons_[0] && !mouse_buttons_[1] && !mouse_buttons_[2]) {
            set_mouse_grab(false);
            pending_ungrab_ = false;
        } else {
            // Wait for physical mouse button release before ungrabbing so game doesn't receive stray release
            pending_ungrab_ = true;
        }
    }
    prev_gui_open_ = cfg.gui_open;
}

void InputManager::update_for_frame(ImGuiIO& io, HudConfig& cfg, float display_width, float display_height) {
    if (!cfg.enabled) {
        if (prev_gui_open_) {
            release_imgui_inputs(io);
            set_mouse_grab(false);
            prev_gui_open_ = false;
        }
        return;
    }

    if (!evdev_scanned_) {
        evdev_scanned_ = true;
        scan_evdev_devices();
    }

    if (cursor_x_ < 0.0f || cursor_y_ < 0.0f) {
        cursor_x_ = display_width * 0.5f;
        cursor_y_ = display_height * 0.5f;
    }

    if (!evdev_devices_.empty()) {
        poll_evdev(io, cfg, display_width, display_height);
    } else {
        poll_x11_fallback(io, cfg, display_width, display_height);
    }

    sync_grab_state(io, cfg);
}

InputManager& get_input_manager() {
    static InputManager instance;
    return instance;
}

} // namespace fpshud
