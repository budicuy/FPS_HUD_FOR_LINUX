
#[test]
fn test_runtime_toggle_recorder_nonblocking_and_rec_persists_when_hud_hidden() {
    use fpshud::runtime::LayerRuntime;
    use fpshud_input::InputAction;

    let mut rt = LayerRuntime::new_for_test();
    rt.overlay_sync.latest.rgba = std::sync::Arc::from([1_u8, 2, 3, 4]);
    rt.set_recorder_active_for_test(true);

    // Hiding HUD while recording must keep overlay_sync.latest.rgba intact and should_blit_overlay() true
    rt.apply_input_actions(&[InputAction::ToggleHud]);
    assert!(!rt.config.hud_visible);
    assert!(!rt.overlay_sync.latest.rgba.is_empty());
    assert!(rt.should_blit_overlay());

    // When recording stops and no toast is active, hiding HUD clears overlay
    rt.set_recorder_active_for_test(false);
    assert!(!rt.should_blit_overlay());
}

#[test]
fn test_vulkan_capture_ring_drain_in_flight_slots_without_stopping_session() {
    use fpshud::vulkan::capture::{RecorderSlotState, wait_for_slot_states_free};
    use std::sync::Arc;
    use std::sync::atomic::{AtomicU8, Ordering};
    use std::time::{Duration, Instant};

    let s0 = Arc::new(AtomicU8::new(RecorderSlotState::ReadingByWorker as u8));
    let s1 = Arc::new(AtomicU8::new(RecorderSlotState::Free as u8));
    let s0_bg = Arc::clone(&s0);
    let worker = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(15));
        s0_bg.store(RecorderSlotState::Free as u8, Ordering::Release);
    });

    let t0 = Instant::now();
    let drained = wait_for_slot_states_free(&[Arc::clone(&s0), s1], Duration::from_millis(250));
    worker.join().unwrap();
    assert!(drained, "drain barrier must wait until worker releases slot");
    assert!(t0.elapsed() >= Duration::from_millis(10));
}

#[test]
fn test_vulkan_format_mapping_includes_fp16_and_color_space() {
    use fpshud::vulkan::capture::{map_vk_color_transfer, map_vk_format_to_recorder_format};
    use fpshud_recorder::capture::{ColorTransfer, RecorderPixelFormat};

    assert_eq!(
        map_vk_format_to_recorder_format(97),
        Some(RecorderPixelFormat::R16G16B16A16Sfloat)
    );
    assert_eq!(
        map_vk_format_to_recorder_format(44),
        Some(RecorderPixelFormat::Bgra8)
    );
    assert_eq!(
        map_vk_color_transfer(1000104008, RecorderPixelFormat::A2B10G10R10),
        ColorTransfer::PqHdr10
    );
    assert_eq!(
        map_vk_color_transfer(0, RecorderPixelFormat::A2B10G10R10),
        ColorTransfer::Sdr
    );
}
