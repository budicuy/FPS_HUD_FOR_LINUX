use fpshud_core::{HudConfig, Keybind};
use fpshud_input::{
    InputAction, InputManager, KeybindTarget, SurfaceBackendType, UiInputEvent,
    x11::{X11Backend, diff_x11_keymap, should_retry_grab},
};
use std::time::Duration;

#[test]
fn x11_swapchain_overrides_wayland_surface_hint_and_clears_matching_window() {
    let mut mgr = InputManager::new_headless();
    mgr.attach_wayland_display_hint(true);
    assert_eq!(mgr.active_backend(), SurfaceBackendType::Wayland);
    assert!(!mgr.should_use_x11_grab(true));

    // Vulkan swapchain Xlib/XCB di Steam Proton membatalkan stale probe Wayland
    mgr.notify_x11_swapchain(0xCAFE);
    assert_eq!(mgr.active_backend(), SurfaceBackendType::X11);
    assert_eq!(mgr.x11_target_window(), Some(0xCAFE));
    assert!(mgr.should_use_x11_grab(true));

    // Clear window yang tidak cocok tidak menghapus target aktif
    mgr.clear_x11_target_window_if_matches(0xDEAD);
    assert_eq!(mgr.x11_target_window(), Some(0xCAFE));

    // Clear window yang cocok menghapus target window
    mgr.clear_x11_target_window_if_matches(0xCAFE);
    assert_eq!(mgr.x11_target_window(), None);

    // Wayland swapchain aktif menonaktifkan override X11
    mgr.notify_wayland_swapchain();
    assert_eq!(mgr.active_backend(), SurfaceBackendType::Wayland);
    assert!(!mgr.should_use_x11_grab(true));

    mgr.reset_surface_hints();
    assert_eq!(mgr.active_backend(), SurfaceBackendType::Unknown);
}

#[test]
fn all_four_keybinds_legacy_fallback_repeat_filter_and_capture_mode() {
    let mut mgr = InputManager::new_headless();
    let mut cfg = HudConfig::default();

    // 1. Default key_toggle_gui = 0 hanya menerima F12 (88), bukan Insert (110); repeat ditahan
    assert!(mgr.handle_key_event(110, true, &mut cfg).is_empty());
    assert!(mgr.handle_key_event(110, false, &mut cfg).is_empty());
    assert_eq!(
        mgr.handle_key_event(88, true, &mut cfg),
        vec![InputAction::ToggleGui]
    );
    assert!(cfg.gui_open);
    assert!(mgr.handle_key_event(88, true, &mut cfg).is_empty());
    assert!(mgr.handle_key_event(88, false, &mut cfg).is_empty());

    assert_eq!(
        mgr.handle_key_event(88, true, &mut cfg),
        vec![InputAction::ToggleGui]
    );
    assert!(!cfg.gui_open);
    mgr.handle_key_event(88, false, &mut cfg);

    // 2. ToggleHud (F11=87), ToggleBenchmark (F9=67), TakeScreenshot (F10=68)
    assert_eq!(
        mgr.handle_key_event(87, true, &mut cfg),
        vec![InputAction::ToggleHud]
    );
    assert!(!cfg.hud_visible);
    mgr.handle_key_event(87, false, &mut cfg);

    assert_eq!(
        mgr.handle_key_event(67, true, &mut cfg),
        vec![InputAction::ToggleBenchmark]
    );
    mgr.handle_key_event(67, false, &mut cfg);

    assert_eq!(
        mgr.handle_key_event(68, true, &mut cfg),
        vec![InputAction::TakeScreenshot]
    );
    mgr.handle_key_event(68, false, &mut cfg);

    // 3. Interactive capture: ESC cancel tanpa mengubah binding
    mgr.start_keybind_capture(KeybindTarget::TakeScreenshot);
    assert!(mgr.is_capturing_keybind());
    mgr.handle_key_event(1 /* KEY_ESC */, true, &mut cfg);
    mgr.handle_key_event(1, false, &mut cfg);
    assert!(!mgr.is_capturing_keybind());
    assert_eq!(cfg.bind_screenshot, Keybind::new(0, 68));

    // 4. Interactive capture: modifier-only hold + main key saves bitmask combo (LeftCtrl=29 + F10=68)
    mgr.start_keybind_capture(KeybindTarget::TakeScreenshot);
    assert!(mgr.handle_key_event(29 /* KEY_LEFTCTRL */, true, &mut cfg).is_empty());
    assert!(mgr.is_capturing_keybind());
    assert!(mgr.handle_key_event(68 /* KEY_F10 */, true, &mut cfg).is_empty());
    assert!(!mgr.is_capturing_keybind());
    assert_eq!(cfg.bind_screenshot, Keybind::new(1 /* MOD_CTRL */, 68));
    mgr.handle_key_event(68, false, &mut cfg);
    mgr.handle_key_event(29, false, &mut cfg);

    // Bare F10 tidak boleh cocok lagi bila combo Ctrl+F10 terpasang, sedangkan Ctrl+F10 cocok!
    assert!(mgr.handle_key_event(68, true, &mut cfg).is_empty());
    mgr.handle_key_event(68, false, &mut cfg);

    mgr.handle_key_event(29, true, &mut cfg);
    assert_eq!(
        mgr.handle_key_event(68, true, &mut cfg),
        vec![InputAction::TakeScreenshot]
    );
}

#[test]
fn deferred_ungrab_waits_for_mouse_release_mouse_scaling_and_headless_safe() {
    let mut mgr = InputManager::new_headless();
    let mut cfg = HudConfig { gui_open: true, ..HudConfig::default() };

    let ev_down = mgr.dispatch_x11_mouse_buttons([true, false, false], &cfg);
    assert!(ev_down.contains(&UiInputEvent::MouseDown { button: 0 }));

    // Tutup GUI saat tombol kiri masih ditekan -> pending_ungrab = true sampai release
    cfg.gui_open = false;
    mgr.sync_grab_state(&cfg);
    assert!(mgr.is_pending_ungrab());

    let ev_up = mgr.dispatch_x11_mouse_buttons([false, false, false], &cfg);
    assert!(ev_up.contains(&UiInputEvent::MouseUp { button: 0 }));
    mgr.sync_grab_state(&cfg);
    assert!(!mgr.is_pending_ungrab());

    // poll_frame hanya menghasilkan mouse events ke UI saat gui_open == true
    mgr.set_cursor_pos(400.0, 200.0);
    let (_, ui_closed, _) = mgr.poll_frame(&mut cfg, 1920.0, 1080.0);
    assert!(ui_closed.is_empty());

    cfg.gui_open = true;
    mgr.set_cursor_pos(640.0, 360.0);
    let (_, ui_open, _) = mgr.poll_frame(&mut cfg, 1920.0, 1080.0);
    assert!(ui_open.iter().any(|e| matches!(e, UiInputEvent::MouseMove { x, y } if (*x - 640.0).abs() < 0.1 && (*y - 360.0).abs() < 0.1)));
}

#[test]
fn x11_keymap_diff_retry_interval_headless_and_evdev_fallback() {
    let prev = [0_u8; 32];
    let mut next = [0_u8; 32];
    // X11 keycode 96 -> Linux keycode 96 - 8 = 88 (KEY_F12)
    next[96 / 8] |= 1 << (96 % 8);
    let pressed = diff_x11_keymap(&prev, &next);
    assert_eq!(pressed, vec![(88, true)]);
    let released = diff_x11_keymap(&next, &prev);
    assert_eq!(released, vec![(88, false)]);

    assert!(!should_retry_grab(Duration::from_millis(20)));
    assert!(should_retry_grab(Duration::from_millis(50)));

    // Headless & non-privileged safety (zero panic)
    let x11 = X11Backend::connect_with_display(Some(""));
    assert!(!x11.is_connected());
}

#[test]
fn stationary_cursor_dedup_prevents_redundant_mouse_moves() {
    let mut mgr = InputManager::new_headless();
    let mut cfg = HudConfig { gui_open: true, ..HudConfig::default() };
    mgr.set_cursor_pos(640.0, 360.0);
    let (_, first_evs, _) = mgr.poll_frame(&mut cfg, 1920.0, 1080.0);
    assert!(first_evs.iter().any(|e| matches!(e, UiInputEvent::MouseMove { .. })));

    // Calling poll_frame again with unchanged cursor MUST NOT emit redundant MouseMove
    let (_, second_evs, _) = mgr.poll_frame(&mut cfg, 1920.0, 1080.0);
    assert!(second_evs.is_empty());
}

#[test]
fn x11_button_mask_decodes_left_right_middle_correctly() {
    use fpshud_input::x11::{decode_x11_button_mask, x11_detail_to_button_idx};

    assert_eq!(decode_x11_button_mask(1 << 8), [true, false, false]); // Button1 = Left
    assert_eq!(decode_x11_button_mask(1 << 10), [false, true, false]); // Button3 = Right
    assert_eq!(decode_x11_button_mask(1 << 9), [false, false, true]); // Button2 = Middle
    assert_eq!(x11_detail_to_button_idx(1), Some(0));
    assert_eq!(x11_detail_to_button_idx(3), Some(1));
    assert_eq!(x11_detail_to_button_idx(2), Some(2));
    assert_eq!(x11_detail_to_button_idx(4), None);
}

#[test]
fn gui_open_centers_cursor_and_wayland_hint_disables_x11_grab() {
    let mut mgr = InputManager::new_headless();
    let mut cfg = HudConfig {
        gui_open: true,
        ..HudConfig::default()
    };
    let (_, ui_evs, _) = mgr.poll_frame(&mut cfg, 1280.0, 720.0);
    assert!(ui_evs.iter().any(|e| matches!(
        e,
        UiInputEvent::MouseMove { x, y } if (*x - 640.0).abs() < 0.1 && (*y - 360.0).abs() < 0.1
    )));

    mgr.attach_wayland_display_hint(true);
    assert_eq!(mgr.active_backend(), SurfaceBackendType::Wayland);
    assert!(!mgr.should_use_x11_grab(true));
    assert!(!mgr.should_draw_software_cursor(&cfg));

    mgr.reset_surface_hints();
    assert_eq!(mgr.active_backend(), SurfaceBackendType::Unknown);
}

#[test]
fn software_cursor_disabled_on_wayland_and_when_os_cursor_visible() {
    let mut mgr = InputManager::new_headless();
    let closed_cfg = HudConfig {
        gui_open: false,
        ..HudConfig::default()
    };
    assert!(!mgr.should_draw_software_cursor(&closed_cfg));

    let open_wayland_cfg = HudConfig {
        gui_open: true,
        ..HudConfig::default()
    };
    mgr.notify_wayland_swapchain();
    assert!(
        !mgr.should_draw_software_cursor(&open_wayland_cfg),
        "Wayland compositor draws the native OS cursor; software cursor must stay disabled to prevent double cursor"
    );
}

#[test]
fn x11_fp1616_translation_and_xinput2_button_mapping() {
    use fpshud_input::x11::{fp1616_to_i16, x11_detail_to_button_idx};

    assert_eq!(fp1616_to_i16(123 << 16), 123);
    assert_eq!(fp1616_to_i16((450 << 16) + 32768), 450);
    assert_eq!(fp1616_to_i16(-15 << 16), -15);
    assert_eq!(x11_detail_to_button_idx(1), Some(0));
    assert_eq!(x11_detail_to_button_idx(3), Some(1));
    assert_eq!(x11_detail_to_button_idx(2), Some(2));
}

#[test]
fn external_overlay_suppresses_in_process_grabs_and_seeds_held_keys() {
    let mut mgr = InputManager::new_headless();
    let mut cfg = HudConfig {
        gui_open: true,
        ..HudConfig::default()
    };

    mgr.notify_x11_swapchain(0x1111);
    assert!(mgr.should_use_x11_grab(true));
    mgr.set_external_overlay_active(true);
    assert!(mgr.is_external_overlay_active());
    assert!(!mgr.should_use_x11_grab(true));
    assert!(!mgr.should_draw_software_cursor(&cfg));

    mgr.notify_wayland_swapchain();

    // While external overlay owns compositor focus, poll_frame must not forward stale cursor moves
    mgr.set_cursor_pos(300.0, 200.0);
    let (actions, ui_evs, grabbed) = mgr.poll_frame(&mut cfg, 1280.0, 720.0);
    assert!(actions.is_empty());
    assert!(ui_evs.is_empty());
    assert!(!grabbed);

    // Seeding held keys (e.g. wl_keyboard::enter carrying F12 while still pressed) prevents bounce
    mgr.set_external_overlay_active(false);
    mgr.seed_keys_down(&[88]);
    assert!(mgr.handle_key_event(88, true, &mut cfg).is_empty());
    assert!(mgr.handle_key_event(88, false, &mut cfg).is_empty());
    assert_eq!(
        mgr.handle_key_event(88, true, &mut cfg),
        vec![InputAction::ToggleGui]
    );
}

#[test]
fn test_record_keybind_trigger_and_capture() {
    use fpshud_input::keycodes::KEY_F8;

    let mut mgr = InputManager::new_headless();
    let mut cfg = HudConfig::default();

    // Default F8 triggers ToggleRecord once (edge-trigger anti-repeat)
    let a1 = mgr.handle_key_event(KEY_F8, true, &mut cfg);
    assert_eq!(a1, vec![InputAction::ToggleRecord]);
    let a_repeat = mgr.handle_key_event(KEY_F8, true, &mut cfg);
    assert!(a_repeat.is_empty());
    let _ = mgr.handle_key_event(KEY_F8, false, &mut cfg);

    // Interactive capture for KeybindTarget::Record with Ctrl+F8
    mgr.start_keybind_capture(KeybindTarget::Record);
    let _ = mgr.handle_key_event(29, true, &mut cfg); // Left Ctrl
    let _ = mgr.handle_key_event(KEY_F8, true, &mut cfg);
    assert!(!mgr.is_capturing_keybind());
    assert_eq!(cfg.bind_record, Keybind::new(1, KEY_F8));
}
