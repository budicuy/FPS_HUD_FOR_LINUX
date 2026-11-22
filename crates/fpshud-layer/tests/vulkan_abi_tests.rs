use std::collections::HashMap;
use std::path::{Path, PathBuf};

#[test]
fn launcher_path_resolution_env_sanitization_and_container_host_bridge() {
    let ui_bin =
        fpshud::launcher::resolve_ui_binary_path(Path::new("/opt/fpshud/libfpshud.so"));
    assert_eq!(ui_bin, Path::new("/opt/fpshud/fpshud-ui"));

    let mut env_map = HashMap::from([
        ("FPS_HUD".to_string(), "1".to_string()),
        (
            "VK_INSTANCE_LAYERS".to_string(),
            "VK_LAYER_FPSHUD_overlay:VK_LAYER_OTHER".to_string(),
        ),
        (
            "LD_PRELOAD".to_string(),
            "/tmp/libfpshud.so:/other.so".to_string(),
        ),
    ]);
    fpshud::launcher::sanitize_ui_child_env(&mut env_map);
    assert_eq!(
        env_map.get("DISABLE_FPS_HUD").map(String::as_str),
        Some("1")
    );
    assert_eq!(
        env_map.get("VK_INSTANCE_LAYERS").map(String::as_str),
        Some("VK_LAYER_OTHER")
    );
    assert!(!env_map.contains_key("FPS_HUD"));
    assert!(!env_map.contains_key("LD_PRELOAD"));

    // Uji helper konstruksi LD_LIBRARY_PATH container host bridge
    let paths =
        fpshud::launcher::build_container_host_lib_paths(Path::new("/nonexistent_host_marker"));
    assert!(paths.is_none());
}

#[test]
fn vulkan_layer_negotiation_and_proc_addr_dispatch_tables() {
    // Uji vkGetInstanceProcAddr mengekspor seluruh instance/surface hooks & tolak simbol tak dikenal
    for sym in [
        c"vkCreateInstance",
        c"vkDestroyInstance",
        c"vkGetInstanceProcAddr",
        c"vkCreateXlibSurfaceKHR",
        c"vkCreateXcbSurfaceKHR",
        c"vkCreateWaylandSurfaceKHR",
        c"vkDestroySurfaceKHR",
        c"vkCreateDevice",
        c"vkDestroyDevice",
        c"vkCreateSwapchainKHR",
        c"vkDestroySwapchainKHR",
        c"vkQueuePresentKHR",
    ] {
        assert!(
            unsafe { fpshud::vk_get_instance_proc_addr(std::ptr::null_mut(), sym.as_ptr()) }
                .is_some()
        );
    }
    assert!(
        unsafe {
            fpshud::vk_get_instance_proc_addr(
                std::ptr::null_mut(),
                c"vkNonExistentFpshudFn".as_ptr(),
            )
        }
        .is_none()
    );

    // Uji vkGetDeviceProcAddr mengekspor device hooks & TIDAK mengekspor surface instance hooks
    assert!(
        unsafe {
            fpshud::vk_get_device_proc_addr(std::ptr::null_mut(), c"vkCreateSwapchainKHR".as_ptr())
        }
        .is_some()
    );
    assert!(
        unsafe {
            fpshud::vk_get_device_proc_addr(std::ptr::null_mut(), c"vkQueuePresentKHR".as_ptr())
        }
        .is_some()
    );
    assert!(
        unsafe {
            fpshud::vk_get_device_proc_addr(
                std::ptr::null_mut(),
                c"vkCreateXlibSurfaceKHR".as_ptr(),
            )
        }
        .is_none()
    );
    assert!(
        unsafe {
            fpshud::vk_get_device_proc_addr(
                std::ptr::null_mut(),
                c"vkCreateWaylandSurfaceKHR".as_ptr(),
            )
        }
        .is_none()
    );
}

#[test]
fn spirv_magic_runtime_actions_resize_generation_and_capture_format_mapping() {
    // 1. Verifikasi embedded SPIR-V quad shaders memiliki magic word little-endian 0x07230203
    assert!(fpshud::vulkan::blit::verify_spirv_shaders_valid());

    // 2. Uji vkNegotiateLoaderLayerInterfaceVersion (struct v2 diterima, v1/null ditolak)
    //    & mapping VkFormat capture ke PixelFormat
    assert!(fpshud::vulkan::hooks::verify_loader_negotiation_contract());
    assert!(
        fpshud::vulkan::capture::map_vk_format_to_pixel_format(44 /* B8G8R8A8_UNORM */)
            .is_some()
    );
    assert!(
        fpshud::vulkan::capture::map_vk_format_to_pixel_format(37 /* R8G8B8A8_UNORM */)
            .is_some()
    );
    assert!(fpshud::vulkan::capture::map_vk_format_to_pixel_format(0).is_none());

    // 3. Uji LayerRuntime memproses ke-4 InputAction & menolak frame slot stale generation saat resize (Review Focus #5)
    let mut rt = fpshud::runtime::LayerRuntime::new_for_test();
    assert!(rt.test_actions_and_generation_resize_guard());
}

#[test]
fn runtime_config_sync_only_on_real_change_and_vulkan_slot_isolation() {
    let mut rt = fpshud::runtime::LayerRuntime::new_for_test();
    // Simulate IPC pair so LayerToUiMsg emission can be inspected directly
    let dir = tempfile::tempdir().unwrap();
    let sock = dir.path().join("layer_sync.sock");
    let server = fpshud_ipc::IpcServer::bind(&sock).unwrap();
    let mut client = fpshud_ipc::IpcClient::connect(&sock).unwrap();
    client.set_nonblocking(true).unwrap();
    let conn = server.accept_verified().unwrap();
    conn.set_nonblocking(true).unwrap();
    rt.attach_ipc_for_test(conn);

    // Pump initial frame -> sends Init (1 msg), no extra ConfigSync even if grabbed/stationary
    rt.pump_frame(1280, 720, "VULKAN");
    let mut buf = vec![0_u8; 65_600];
    let mut kinds = Vec::new();
    while let Ok((n, _)) = client.recv_msg(&mut buf) {
        if n == 0 {
            break;
        }
        kinds.push(fpshud_ipc::LayerToUiMsg::decode(&buf[..n]).unwrap());
    }
    assert!(matches!(kinds.first(), Some(fpshud_ipc::LayerToUiMsg::Init { .. })));
    assert!(!kinds.iter().any(|m| matches!(m, fpshud_ipc::LayerToUiMsg::ConfigSync(_))));

    // Simulate hotkey toggle inside input manager (grabbed == false!) -> MUST emit ConfigSync once
    assert!(rt.test_key_event_triggers_config_sync_once(88 /* KEY_F12 */));
    let mut synced = 0usize;
    while let Ok((n, _)) = client.recv_msg(&mut buf) {
        if n == 0 {
            break;
        }
        if matches!(fpshud_ipc::LayerToUiMsg::decode(&buf[..n]).unwrap(), fpshud_ipc::LayerToUiMsg::ConfigSync(_)) {
            synced += 1;
        }
    }
    assert_eq!(synced, 1);

    // Per-swapchain-slot overlay upload tracker must keep slot 0/1/2 isolated cleanly
    let mut tracker = fpshud::vulkan::blit::OverlaySlotSyncTracker::new(3);
    assert!(tracker.needs_upload(0, 1280, 720, 1));
    tracker.mark_uploaded(0, 1280, 720, 1);
    assert!(!tracker.needs_upload(0, 1280, 720, 1));
    // Slot 1 still hasn't received gen=1 on the GPU!
    assert!(tracker.needs_upload(1, 1280, 720, 1));
    assert!(tracker.needs_upload(2, 1280, 720, 1));
}

#[test]
fn proc_self_maps_parser_handles_spaces_anon_and_resolves_self() {
    let sample = "\
00400000-00401000 r-xp 00000000 08:01 123 /tmp/my game/libfpshud.so\n\
00500000-00502000 rw-p 00000000 00:00 0 [heap]\n\
00600000-00604000 r-xp 00000000 08:01 456 /opt/deleted/libx.so (deleted)\n";
    assert_eq!(
        fpshud::launcher::parse_maps_path_for_addr(sample, 0x0040_0500),
        Some(PathBuf::from("/tmp/my game/libfpshud.so"))
    );
    assert_eq!(
        fpshud::launcher::parse_maps_path_for_addr(sample, 0x0050_0100),
        None
    );
    assert_eq!(
        fpshud::launcher::parse_maps_path_for_addr(sample, 0x0060_0100),
        Some(PathBuf::from("/opt/deleted/libx.so"))
    );
    let resolved = fpshud::launcher::resolve_self_so_path().expect("resolve self executable/so path");
    assert!(resolved.exists());
}

#[test]
fn runtime_chunks_ui_input_events_above_256_without_dropping() {
    let mut rt = fpshud::runtime::LayerRuntime::new_for_test();
    let dir = tempfile::tempdir().unwrap();
    let sock = dir.path().join("chunk_events.sock");
    let server = fpshud_ipc::IpcServer::bind(&sock).unwrap();
    let mut client = fpshud_ipc::IpcClient::connect(&sock).unwrap();
    client.set_nonblocking(true).unwrap();
    let conn = server.accept_verified().unwrap();
    conn.set_nonblocking(true).unwrap();
    rt.attach_ipc_for_test(conn);

    let burst: Vec<fpshud_input::UiInputEvent> = (0..300)
        .map(|i| fpshud_input::UiInputEvent::MouseMove {
            x: i as f32,
            y: i as f32,
        })
        .collect();
    let sent = rt.push_ui_events(&burst);
    assert_eq!(sent, 300);

    let mut buf = vec![0_u8; 65_600];
    let mut chunk_sizes = Vec::new();
    while let Ok((n, _)) = client.recv_msg(&mut buf) {
        if n == 0 {
            break;
        }
        if let fpshud_ipc::LayerToUiMsg::InputEvents(evs) =
            fpshud_ipc::LayerToUiMsg::decode(&buf[..n]).unwrap()
        {
            chunk_sizes.push(evs.len());
        }
    }
    assert_eq!(chunk_sizes, vec![256, 44]);
}

#[test]
fn external_overlay_ipc_disables_grabs_and_debounces_f12_reopen() {
    let mut rt = fpshud::runtime::LayerRuntime::new_for_test();
    rt.notify_surface_swapchain(fpshud_input::SurfaceBackendType::X11, 0x1234);
    let dir = tempfile::tempdir().unwrap();
    let sock = dir.path().join("ext_overlay.sock");
    let server = fpshud_ipc::IpcServer::bind(&sock).unwrap();
    let mut client = fpshud_ipc::IpcClient::connect(&sock).unwrap();
    client.set_nonblocking(true).unwrap();
    let conn = server.accept_verified().unwrap();
    conn.set_nonblocking(true).unwrap();
    rt.attach_ipc_for_test(conn);

    // Open GUI via F12
    assert!(rt.test_key_event_triggers_config_sync_once(88 /* KEY_F12 */));
    assert!(rt.config.gui_open);
    assert!(rt.input.should_use_x11_grab(true));

    // UI daemon activates external Wayland overlay -> Layer must disable in-process grabs
    let on_msg = fpshud_ipc::UiToLayerMsg::ExternalOverlayActive(true)
        .encode()
        .unwrap();
    client.send_msg(&on_msg, &[]).unwrap();
    rt.pump_frame(1280, 720, "VULKAN");
    assert!(rt.input.is_external_overlay_active());
    assert!(!rt.input.should_use_x11_grab(true));

    // UI daemon closes GUI & releases external overlay -> Layer closes GUI and arms debounce
    let mut closed_cfg = rt.config.clone();
    closed_cfg.gui_open = false;
    let off_msg = fpshud_ipc::UiToLayerMsg::ExternalOverlayActive(false)
        .encode()
        .unwrap();
    let cfg_msg = fpshud_ipc::UiToLayerMsg::ConfigUpdated(closed_cfg)
        .encode()
        .unwrap();
    client.send_msg(&off_msg, &[]).unwrap();
    client.send_msg(&cfg_msg, &[]).unwrap();
    rt.pump_frame(1280, 720, "VULKAN");
    assert!(!rt.input.is_external_overlay_active());
    assert!(!rt.config.gui_open);
    assert!(rt.is_gui_reopen_debounced());

    // Phantom F12-still-held edge right after focus return must be suppressed by debounce!
    assert!(!rt.test_key_event_triggers_config_sync_once(88));
    assert!(!rt.config.gui_open);

    // After debounce expires, intentional F12 re-opens GUI cleanly
    rt.clear_gui_reopen_debounce_for_test();
    assert!(!rt.is_gui_reopen_debounced());
    assert!(rt.test_key_event_triggers_config_sync_once(88));
    assert!(rt.config.gui_open);
}

#[test]
fn vulkan_hooks_clamp_group_count_and_guard_null_dispatch_key() {
    use fpshud::vulkan::hooks::clamp_device_group_count;
    use fpshud::vulkan::swapchain::dispatch_key_from_raw;

    assert_eq!(clamp_device_group_count(2, 32), 2);
    assert_eq!(clamp_device_group_count(99, 32), 32);
    assert_eq!(unsafe { dispatch_key_from_raw(0) }, 0);
}

#[test]
fn vulkan_positioned_overlay_viewport_and_subrect_shm_sync() {
    use ash::vk;
    use fpshud::runtime::OverlayFrameSync;
    use fpshud::vulkan::blit::compute_vk_overlay_viewport;
    use fpshud_ipc::protocol::{FrameBackendKind, ShmSlotMeta};

    let (vp, scissor) = compute_vk_overlay_viewport(
        vk::Extent2D {
            width: 1280,
            height: 720,
        },
        16,
        20,
        320,
        140,
    );
    assert_eq!(
        (vp.x, vp.y, vp.width, vp.height),
        (16.0, 20.0, 320.0, 140.0)
    );
    assert_eq!(
        (
            scissor.offset.x,
            scissor.offset.y,
            scissor.extent.width,
            scissor.extent.height
        ),
        (16, 20, 320, 140)
    );

    let mut sync = OverlayFrameSync::default();
    sync.reset_for_resize(1280, 720);
    let meta = ShmSlotMeta {
        modifier: 0,
        generation: 1,
        slot_index: 0,
        dst_x: 16,
        dst_y: 20,
        width: 4,
        height: 4,
        stride: 16,
        fourcc: 0x3432_5241,
        byte_len: 64,
        backend: FrameBackendKind::MemfdRgba,
    };
    assert!(sync.accept_metadata_only(&meta, &[255_u8; 64]));
    assert_eq!(
        (
            sync.latest.dst_x,
            sync.latest.dst_y,
            sync.latest.width,
            sync.latest.height
        ),
        (16, 20, 4, 4)
    );

    // Regression: switching presets (e.g. Minimal -> Advance) can reallocate SHM FD;
    // reset_generation() ensures generation=1 from the new ring is accepted immediately.
    let mut high_meta = meta;
    high_meta.generation = 42;
    assert!(sync.accept_metadata_only(&high_meta, &[255_u8; 64]));
    assert!(!sync.accept_metadata_only(&meta, &[255_u8; 64]));
    sync.reset_generation();
    let mut resized_meta = meta;
    resized_meta.dst_x = 500;
    resized_meta.generation = 1;
    assert!(sync.accept_metadata_only(&resized_meta, &[255_u8; 64]));
    assert_eq!(sync.latest.dst_x, 500);
}
