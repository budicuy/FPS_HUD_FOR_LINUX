use fpshud_core::{HudConfig, Keybind};
use fpshud_input::{
    InputAction, InputManager, KeybindTarget, SurfaceBackendType, UiInputEvent,
    evdev::EvdevBackend,
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

    // 1. Default key_toggle_gui = 0 menerima Insert (110) & F12 (88); repeat ditahan
    assert_eq!(
        mgr.handle_key_event(110, true, &mut cfg),
        vec![InputAction::ToggleGui]
    );
    assert!(cfg.gui_open);
    assert!(mgr.handle_key_event(110, true, &mut cfg).is_empty());
    assert!(mgr.handle_key_event(110, false, &mut cfg).is_empty());

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

    let mut evdev = EvdevBackend::probe_directory(std::path::Path::new("/nonexistent_dev_input"));
    assert_eq!(evdev.device_count(), 0);
    assert!(evdev.poll_events().is_empty());
}

#[test]
fn evdev_caps_filtering_and_stationary_cursor_dedup() {
    use fpshud_input::evdev::{EvdevDeviceRole, EvdevEvent, classify_device_caps, translate_input_event};

    // Keyboard: EV_KEY + KEY_A tanpa EV_REL / EV_ABS
    let mut kb_ev = evdev::AttributeSet::<evdev::EventType>::new();
    kb_ev.insert(evdev::EventType::KEY);
    let mut kb_keys = evdev::AttributeSet::<evdev::KeyCode>::new();
    kb_keys.insert(evdev::KeyCode::KEY_A);
    assert_eq!(
        classify_device_caps(&kb_ev, Some(&kb_keys), None),
        EvdevDeviceRole::Keyboard
    );

    // Mouse: EV_REL (REL_X | REL_Y)
    let mut mouse_ev = evdev::AttributeSet::<evdev::EventType>::new();
    mouse_ev.insert(evdev::EventType::KEY);
    mouse_ev.insert(evdev::EventType::RELATIVE);
    let mut mouse_rel = evdev::AttributeSet::<evdev::RelativeAxisCode>::new();
    mouse_rel.insert(evdev::RelativeAxisCode::REL_X);
    mouse_rel.insert(evdev::RelativeAxisCode::REL_Y);
    assert_eq!(
        classify_device_caps(&mouse_ev, None, Some(&mouse_rel)),
        EvdevDeviceRole::Mouse
    );

    // Gamepad/misc: EV_ABS tanpa tombol keyboard / mouse rel -> diabaikan
    let mut pad_ev = evdev::AttributeSet::<evdev::EventType>::new();
    pad_ev.insert(evdev::EventType::KEY);
    pad_ev.insert(evdev::EventType::ABSOLUTE);
    assert_eq!(classify_device_caps(&pad_ev, None, None), EvdevDeviceRole::Ignore);

    // Translasi InputEvent: gerak mouse, wheel, klik mouse, hotkey, dan tolak tombol gamepad 0x120
    assert_eq!(
        translate_input_event(evdev::InputEvent::new(evdev::EventType::RELATIVE.0, evdev::RelativeAxisCode::REL_X.0, 12)),
        Some(EvdevEvent::RelMotion { dx: 12, dy: 0 })
    );
    assert_eq!(
        translate_input_event(evdev::InputEvent::new(evdev::EventType::RELATIVE.0, evdev::RelativeAxisCode::REL_WHEEL.0, -1)),
        Some(EvdevEvent::Scroll { delta: -1 })
    );
    assert_eq!(
        translate_input_event(evdev::InputEvent::new(evdev::EventType::KEY.0, evdev::KeyCode::BTN_LEFT.0, 1)),
        Some(EvdevEvent::MouseBtn { button: 0, down: true })
    );
    assert_eq!(
        translate_input_event(evdev::InputEvent::new(evdev::EventType::KEY.0, evdev::KeyCode::KEY_ESC.0, 1)),
        Some(EvdevEvent::Key { code: 1, down: true })
    );
    assert_eq!(
        translate_input_event(evdev::InputEvent::new(evdev::EventType::KEY.0, 0x120, 1)),
        None
    );

    // Stationary cursor deduplication & X11 vs Wayland evdev grab arbitration
    let mut mgr = InputManager::new_headless();
    let mut cfg = HudConfig { gui_open: true, ..HudConfig::default() };
    mgr.set_cursor_pos(640.0, 360.0);
    let (_, first_evs, _) = mgr.poll_frame(&mut cfg, 1920.0, 1080.0);
    assert!(first_evs.iter().any(|e| matches!(e, UiInputEvent::MouseMove { .. })));

    // Calling poll_frame again with unchanged cursor MUST NOT emit redundant MouseMove
    let (_, second_evs, _) = mgr.poll_frame(&mut cfg, 1920.0, 1080.0);
    assert!(second_evs.is_empty());

    // Evdev grab disabled when X11 active, enabled on pure Wayland
    mgr.notify_x11_swapchain(0x42);
    assert!(!mgr.should_grab_evdev(&cfg));
    mgr.notify_wayland_swapchain();
    assert!(mgr.should_grab_evdev(&cfg));
}

#[test]
fn evdev_drain_event_batches_reads_until_wouldblock_and_bounds_batches() {
    use fpshud_input::evdev::{EvdevEvent, MAX_FETCH_BATCHES_PER_POLL, drain_event_batches};
    use std::io;

    let mut step = 0usize;
    let drained = drain_event_batches(&mut || {
        step += 1;
        match step {
            1 => Ok(vec![
                evdev::InputEvent::new(evdev::EventType::RELATIVE.0, evdev::RelativeAxisCode::REL_X.0, 5),
            ]
            .into_iter()),
            2 => Ok(vec![
                evdev::InputEvent::new(evdev::EventType::RELATIVE.0, evdev::RelativeAxisCode::REL_Y.0, -3),
            ]
            .into_iter()),
            _ => Err(io::Error::from(io::ErrorKind::WouldBlock)),
        }
    });
    assert_eq!(
        drained,
        vec![
            EvdevEvent::RelMotion { dx: 5, dy: 0 },
            EvdevEvent::RelMotion { dx: 0, dy: -3 },
        ]
    );

    let mut calls = 0usize;
    let bounded = drain_event_batches(&mut || {
        calls += 1;
        Ok(vec![evdev::InputEvent::new(
            evdev::EventType::RELATIVE.0,
            evdev::RelativeAxisCode::REL_X.0,
            1,
        )]
        .into_iter())
    });
    assert_eq!(calls, MAX_FETCH_BATCHES_PER_POLL);
    assert_eq!(bounded.len(), MAX_FETCH_BATCHES_PER_POLL);
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
fn wayland_backend_translates_pointer_events_and_centers_cursor_on_gui_open() {
    use fpshud_input::wayland::{WaylandBackend, WaylandEvent, map_wl_button};

    assert_eq!(map_wl_button(0x110), Some(0)); // BTN_LEFT
    assert_eq!(map_wl_button(0x111), Some(1)); // BTN_RIGHT
    assert_eq!(map_wl_button(0x112), Some(2)); // BTN_MIDDLE
    assert_eq!(map_wl_button(0x120), None);

    let mut wl = WaylandBackend::new_for_test();
    wl.push_test_event(WaylandEvent::CursorPos { x: 320.5, y: 180.25 });
    wl.push_test_event(WaylandEvent::MouseBtn { button: 0, down: true });
    wl.push_test_event(WaylandEvent::MouseBtn { button: 0, down: false });
    let drained = wl.poll_events();
    assert_eq!(drained.len(), 3);

    // Saat GUI terbuka pertama kali tanpa gerakan mouse, kursor harus otomatis ditengah layar
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
}
