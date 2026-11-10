#include "doctest.h"
#include "test_common.hpp"

#include "backends/evdev_backend.hpp"
#include "backends/wayland_backend.hpp"
#include "backends/x11_backend.hpp"
#include "config.hpp"
#include "imgui.h"
#include "input_manager.hpp"
#include "recorder_ipc.hpp"
#include "telemetry.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <linux/input.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fpshud;

TEST_CASE("Input Manager") {
  SUBCASE("Wayland Hook_DestroySurfaceKHR Display Preservation") {
    fs::path rroot = find_repo_root();
    if (!rroot.empty()) {
      std::string hooks_src = read_text_file(rroot / "src" / "layer" / "vulkan_hooks.cpp");
      size_t destroy_surface_pos = hooks_src.find("Hook_DestroySurfaceKHR");
      REQUIRE(destroy_surface_pos != std::string::npos);
      size_t destroy_surface_end = hooks_src.find("}", destroy_surface_pos);
      REQUIRE(destroy_surface_end != std::string::npos);
      std::string destroy_surface_body =
          hooks_src.substr(destroy_surface_pos, destroy_surface_end - destroy_surface_pos);

      bool no_detach_in_surface =
          (destroy_surface_body.find("detach_wayland_display") == std::string::npos);
      CHECK_MESSAGE(no_detach_in_surface,
                    "Hook_DestroySurfaceKHR must not call detach_wayland_display");

      size_t destroy_inst_pos = hooks_src.find("Hook_DestroyInstance");
      REQUIRE(destroy_inst_pos != std::string::npos);
      size_t destroy_inst_end = hooks_src.find("Hook_EnumeratePhysicalDevices", destroy_inst_pos);
      REQUIRE(destroy_inst_end != std::string::npos);
      std::string destroy_inst_body =
          hooks_src.substr(destroy_inst_pos, destroy_inst_end - destroy_inst_pos);
      bool detach_in_inst = (destroy_inst_body.find("detach_wayland_display") != std::string::npos);
      CHECK_MESSAGE(detach_in_inst, "Hook_DestroyInstance must call detach_wayland_display");
    }
  }

  SUBCASE("Conditional Grab: X11 Grab Suppressed When Wayland or Evdev Active") {
    ImGuiContext* prev_ctx = ImGui::GetCurrentContext();
    ImGuiContext* test_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(test_ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);

    InputManager im;
    HudConfig cfg{};
    cfg.enabled = true;
    cfg.gui_open = false;

    // When Wayland display is attached, sync_grab_state must not activate X11 grab
    void* mock_display = reinterpret_cast<void*>(0x5678);
    im.attach_wayland_display(mock_display);
    REQUIRE(im.has_wayland_display());

    cfg.gui_open = true;
    im.sync_grab_state(io, cfg);
    CHECK_MESSAGE(!im.is_mouse_grabbed(),
                  "X11 grab not initiated when Wayland display is attached");

    cfg.gui_open = false;
    im.sync_grab_state(io, cfg);
    im.detach_wayland_display(mock_display);

    if (test_ctx) {
      ImGui::DestroyContext(test_ctx);
    }
    ImGui::SetCurrentContext(prev_ctx);
  }

  SUBCASE("Evdev Backend Non-Blocking Grab & Device Inspection") {
    EvdevBackend evdev;
    bool init_res = evdev.init();
    (void)init_res;  // Dependent on /dev/input permissions
    bool not_grabbed_initially = !evdev.is_mouse_grabbed();
    evdev.set_grab(false);
    bool still_not_grabbed = !evdev.is_mouse_grabbed();
    evdev.shutdown();
    bool empty_after_shutdown = evdev.devices().empty() && !evdev.is_mouse_grabbed();

    CHECK_MESSAGE(not_grabbed_initially, "17.1 EvdevBackend initially mouse not grabbed");
    CHECK_MESSAGE(still_not_grabbed,
                  "17.1 EvdevBackend mouse remains ungrabbed after set_grab(false)");
    CHECK_MESSAGE(empty_after_shutdown,
                  "17.1 EvdevBackend devices empty and not grabbed after shutdown");
  }

  SUBCASE("Wayland Backend Mock Attachment & State Tracking") {
    WaylandBackend wayland;
    bool init_res = wayland.init();
    (void)init_res;
    bool init_no_display = !wayland.has_display();
    void* mock_display = reinterpret_cast<void*>(0x5678);
    wayland.attach_display(mock_display);
    bool attached = wayland.has_display();
    wayland.detach_display(mock_display);
    bool detached = !wayland.has_display();
    wayland.shutdown();
    bool shutdown_ok = !wayland.has_display();

    CHECK_MESSAGE(init_no_display, "17.2 WaylandBackend initially has no display");
    CHECK_MESSAGE(attached, "17.2 WaylandBackend tracks attached display");
    CHECK_MESSAGE(detached, "17.2 WaylandBackend tracks detached display");
    CHECK_MESSAGE(shutdown_ok, "17.2 WaylandBackend shutdown clears display reference");

    // In-process Wayland display attachment on InputManager
    InputManager im;
    im.attach_wayland_display(mock_display);
    CHECK_MESSAGE(im.has_wayland_display(), "InputManager registers attached Wayland display");
    im.detach_wayland_display(mock_display);
    CHECK_MESSAGE(!im.has_wayland_display(), "InputManager clears detached Wayland display");
  }

  SUBCASE("X11 Backend Decoupled Worker & Idempotent Shutdown") {
    X11Backend xb;
    bool init_res = xb.init();
    (void)init_res;
    xb.shutdown();  // Must safely join worker thread without deadlock
    xb.shutdown();  // Idempotent shutdown check

    CHECK(!xb.is_grabbed());
  }

  SUBCASE("InputManager Hotkey Routing & Keybind Capture") {
    InputManager im;
    HudConfig cfg{};
    cfg.enabled = true;
    cfg.gui_open = false;
    cfg.hud_visible = true;
    cfg.bind_toggle_gui = Keybind{0, KEY_F12};
    cfg.bind_benchmark = Keybind{KEY_LEFTSHIFT, KEY_F9};

    // F12 toggles GUI
    im.handle_key_event(KEY_F12, true, cfg, 1920.0f, 1080.0f);
    bool f12_opened_gui = cfg.gui_open;
    im.handle_key_event(KEY_F12, false, cfg, 1920.0f, 1080.0f);
    CHECK_MESSAGE(f12_opened_gui, "F12 key press toggles in-game GUI open");

    // Escape closes GUI
    im.handle_key_event(KEY_ESC, true, cfg, 1920.0f, 1080.0f);
    bool esc_closed_gui = !cfg.gui_open;
    im.handle_key_event(KEY_ESC, false, cfg, 1920.0f, 1080.0f);
    CHECK_MESSAGE(esc_closed_gui, "Escape key press closes in-game GUI");

    // Shift+F9 combo trigger
    im.handle_key_event(KEY_LEFTSHIFT, true, cfg, 1920.0f, 1080.0f);
    im.handle_key_event(KEY_F9, true, cfg, 1920.0f, 1080.0f);
    im.handle_key_event(KEY_F9, false, cfg, 1920.0f, 1080.0f);
    im.handle_key_event(KEY_LEFTSHIFT, false, cfg, 1920.0f, 1080.0f);
    CHECK(fpshud::get_telemetry().is_benchmark_active());

    // Stop benchmark cleanly
    fpshud::get_telemetry().toggle_benchmark();
    CHECK(!fpshud::get_telemetry().is_benchmark_active());

    // 1-key capture
    im.start_keybind_capture(KeybindTarget::ToggleHud);
    CHECK_MESSAGE(im.is_capturing_keybind(), "Keybind capture active for ToggleHud");
    im.handle_key_event(KEY_F8, true, cfg, 1920.0f, 1080.0f);
    im.handle_key_event(KEY_F8, false, cfg, 1920.0f, 1080.0f);
    bool one_key_captured = (cfg.bind_toggle_hud == Keybind{0, KEY_F8});
    CHECK_MESSAGE(!im.is_capturing_keybind(), "1-key capture finished");
    CHECK_MESSAGE(one_key_captured, "1-key capture mapped KEY_F8 to ToggleHud");

    // 2-key capture
    im.start_keybind_capture(KeybindTarget::ToggleBenchmark);
    CHECK_MESSAGE(im.is_capturing_keybind(), "Keybind capture active for ToggleBenchmark");
    im.handle_key_event(KEY_LEFTALT, true, cfg, 1920.0f, 1080.0f);
    im.handle_key_event(KEY_B, true, cfg, 1920.0f, 1080.0f);
    im.handle_key_event(KEY_B, false, cfg, 1920.0f, 1080.0f);
    im.handle_key_event(KEY_LEFTALT, false, cfg, 1920.0f, 1080.0f);
    bool two_key_captured = (cfg.bind_benchmark == Keybind{KEY_LEFTALT, KEY_B});
    CHECK_MESSAGE(!im.is_capturing_keybind(), "2-key combo capture finished");
    CHECK_MESSAGE(two_key_captured, "2-key capture mapped LeftAlt+B to ToggleBenchmark");

    // Cancel capture via KEY_ESC
    im.start_keybind_capture(KeybindTarget::ToggleHud);
    CHECK(im.is_capturing_keybind());
    im.handle_key_event(KEY_ESC, true, cfg, 1920.0f, 1080.0f);
    im.handle_key_event(KEY_ESC, false, cfg, 1920.0f, 1080.0f);
    CHECK(!im.is_capturing_keybind());
    CHECK(cfg.bind_toggle_hud == Keybind{0, KEY_F8});

    // F10 hotkey triggers screenshot request
    RecorderController::instance().consume_screenshot_request();
    im.handle_key_event(KEY_F10, true, cfg, 1920.0f, 1080.0f);
    im.handle_key_event(KEY_F10, false, cfg, 1920.0f, 1080.0f);
    CHECK(RecorderController::instance().consume_screenshot_request());
  }

  SUBCASE("InputManager Mouse Isolation & Double-Cursor Safety") {
    fs::path so_path = fpshud::test::find_libfpshud_path();
    if (!so_path.empty()) {
      void* handle = dlopen(so_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
      if (handle) {
        bool sym_poll = (dlsym(handle, "SDL_PollEvent") != nullptr);
        bool sym_peep = (dlsym(handle, "SDL_PeepEvents") != nullptr);
        bool sym_x11 = (dlsym(handle, "XNextEvent") != nullptr);
        bool sym_qptr = (dlsym(handle, "XQueryPointer") != nullptr);
        CHECK_MESSAGE(!sym_poll, "SDL_PollEvent not exported by libfpshud.so");
        CHECK_MESSAGE(!sym_peep, "SDL_PeepEvents not exported by libfpshud.so");
        CHECK_MESSAGE(!sym_x11, "XNextEvent not exported by libfpshud.so");
        CHECK_MESSAGE(!sym_qptr, "XQueryPointer not exported by libfpshud.so");
        dlclose(handle);
      }
    }

    fpshud::InputManager& im = fpshud::get_input_manager();
    ImGuiContext* prev_ctx = ImGui::GetCurrentContext();
    ImGuiContext* test_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(test_ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* font_pixels = nullptr;
    int font_w = 0, font_h = 0;
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_w, &font_h);

    fpshud::HudConfig test_cfg;
    test_cfg.enabled = true;

    // Reset baseline state from prior test suites
    test_cfg.gui_open = false;
    im.sync_grab_state(io, test_cfg);

    // GUI opened: verify double cursor protection!
    test_cfg.gui_open = true;
    im.sync_grab_state(io, test_cfg);
    bool double_cursor_prevented = (io.MouseDrawCursor == im.is_mouse_grabbed());
    CHECK_MESSAGE(double_cursor_prevented,
                  "io.MouseDrawCursor matches mouse grabbed state (double cursor prevented)");

    // Mouse motion forwarded
    im.handle_pointer_motion(800.0f, 600.0f, test_cfg, &io);
    bool motion_im_ok = (im.cursor_x() == 800.0f && im.cursor_y() == 600.0f);
    CHECK_MESSAGE(motion_im_ok, "InputManager cursor position updated to (800, 600)");

    // Mouse wheel forwarded
    im.handle_pointer_wheel(0.0f, 3.0f, test_cfg, &io);

    // Mouse button pressed
    im.handle_pointer_button(BTN_LEFT, true, test_cfg, &io);
    bool btn_down_im_ok = im.is_mouse_button_down(0);
    CHECK_MESSAGE(btn_down_im_ok, "Mouse button 0 is down in InputManager");

    // Run ImGui::NewFrame() to process queued events into Dear ImGui internal state
    ImGui::NewFrame();
    bool motion_imgui_ok = (io.MousePos.x == 800.0f && io.MousePos.y == 600.0f);
    bool wheel_imgui_ok = (io.MouseWheel == 3.0f);
    bool btn_imgui_ok = io.MouseDown[0];
    ImGui::EndFrame();

    CHECK_MESSAGE(motion_imgui_ok, "Dear ImGui MousePos matches forwarded pointer position");
    CHECK_MESSAGE(wheel_imgui_ok, "Dear ImGui MouseWheel matches forwarded wheel event");
    CHECK_MESSAGE(btn_imgui_ok, "Dear ImGui MouseDown[0] is true on mouse button click");

    // Close GUI while mouse button is still pressed: deferred ungrab
    test_cfg.gui_open = false;
    im.sync_grab_state(io, test_cfg);
    bool pending_ungrab_active = im.is_pending_ungrab();
    CHECK_MESSAGE(pending_ungrab_active, "Deferred ungrab active while mouse button is held");

    // Release mouse button: pending ungrab completes
    im.handle_pointer_button(BTN_LEFT, false, test_cfg, &io);
    bool fully_ungrabbed = !im.is_pending_ungrab();
    CHECK_MESSAGE(fully_ungrabbed, "Releasing mouse button completes deferred ungrab");

    test_cfg.gui_open = true;
    im.sync_grab_state(io, test_cfg);
    im.handle_pointer_motion(720.0f, 480.0f, test_cfg, &io);
    im.handle_pointer_button(BTN_LEFT, true, test_cfg, &io);
    im.handle_pointer_wheel(0.0f, 4.0f, test_cfg, &io);

    ImGui::NewFrame();
    bool direct_motion_ok = (io.MousePos.x == 720.0f && io.MousePos.y == 480.0f);
    bool direct_wheel_ok = (io.MouseWheel == 4.0f);
    bool direct_click_ok = io.MouseDown[0];
    ImGui::EndFrame();

    CHECK_MESSAGE(direct_motion_ok, "Direct pointer motion updated ImGui MousePos");
    CHECK_MESSAGE(direct_wheel_ok, "Direct wheel motion updated ImGui MouseWheel");
    CHECK_MESSAGE(direct_click_ok, "Direct click updated ImGui MouseDown[0]");

    // Clean up test context and state
    test_cfg.gui_open = false;
    im.sync_grab_state(io, test_cfg);
    im.handle_pointer_button(BTN_LEFT, false, test_cfg, &io);

    if (test_ctx) {
      ImGui::DestroyContext(test_ctx);
    }
    ImGui::SetCurrentContext(prev_ctx);
  }

  SUBCASE("Clean Architecture & Proton Input Documentation") {
    fs::path rroot = find_repo_root();
    bool im_queue_cleaned = rroot.empty();
    if (!rroot.empty()) {
      std::string im_hpp = read_text_file(rroot / "src" / "input" / "input_manager.hpp");
      im_queue_cleaned = (im_hpp.find("enqueue_intercepted_motion") == std::string::npos &&
                          im_hpp.find("pending_intercepted_inputs_") == std::string::npos);
    }
    CHECK_MESSAGE(im_queue_cleaned,
                  "11.1 InputManager removes redundant intercepted queue and dead mutexes");

    bool clean_version_script = rroot.empty();
    if (!rroot.empty()) {
      std::string ver_src = read_text_file(rroot / "manifest" / "libfpshud.version");
      clean_version_script =
          (ver_src.find("SDL_") == std::string::npos &&
           ver_src.find("XNextEvent") == std::string::npos &&
           ver_src.find("XQueryPointer") == std::string::npos &&
           ver_src.find("vkNegotiateLoaderLayerInterfaceVersion") != std::string::npos &&
           ver_src.find("glXSwapBuffers") != std::string::npos &&
           ver_src.find("eglSwapBuffers") != std::string::npos);
    }
    CHECK_MESSAGE(clean_version_script,
                  "11.2 Clean DSO version script without polluting SDL or X11 intercept hooks");

    bool launch_guidance_present = rroot.empty();
    if (!rroot.empty()) {
      std::string fpshud_in = read_text_file(rroot / "scripts" / "fpshud.in");
      std::string readme_en = read_text_file(rroot / "README.md");
      std::string readme_id = read_text_file(rroot / "README.id.md");
      launch_guidance_present =
          (fpshud_in.find("fpshud %command%") != std::string::npos &&
           fpshud_in.find("Steam Launch Options for Proton & Native games") != std::string::npos &&
           readme_en.find("~/.local/bin/fpshud %command%") != std::string::npos &&
           readme_id.find("~/.local/bin/fpshud %command%") != std::string::npos);
    }
    CHECK_MESSAGE(launch_guidance_present,
                  "11.3 Universal fpshud wrapper and Steam Proton input isolation documented");
  }
}
