use fpshud_core::HudConfig;
use fpshud_input::{KeybindTarget, UiInputEvent};
use fpshud_ipc::{
    FrameBackendKind, IpcClient, IpcServer, LayerToUiMsg, ShmRingReader, ShmRingWriter,
    ShmSlotMeta, UiToLayerMsg,
    protocol::{IPC_MAGIC, MAX_IPC_PAYLOAD},
    socket::verify_peer_credentials,
};
use fpshud_telemetry::TelemetrySnapshot;
use std::os::fd::AsFd;
use std::os::unix::fs::PermissionsExt;

#[test]
fn protocol_all_variants_and_corrupt_rejection() {
    let cfg = HudConfig::default();
    let snap = TelemetrySnapshot {
        fps: 144.5,
        gpu_load: 88.0,
        cpu_power_w: 52.3,
        ..TelemetrySnapshot::default()
    };

    let l2u_cases = vec![
        LayerToUiMsg::Init {
            width: 1920,
            height: 1080,
            scale: 1.25,
            config: cfg.clone(),
        },
        LayerToUiMsg::TelemetryUpdate(Box::new(snap)),
        LayerToUiMsg::ConfigSync(cfg.clone()),
        LayerToUiMsg::InputEvents(vec![
            UiInputEvent::MouseMove { x: 100.0, y: 200.0 },
            UiInputEvent::MouseDown { button: 0 },
            UiInputEvent::KeyDown { code: 88, mod_mask: 1 },
        ]),
        LayerToUiMsg::Resize {
            width: 2560,
            height: 1440,
            scale: 1.5,
        },
        LayerToUiMsg::SoftwareCursor(true),
        LayerToUiMsg::SoftwareCursor(false),
        LayerToUiMsg::Shutdown,
    ];
    for msg in &l2u_cases {
        let bytes = msg.encode().expect("encode LayerToUiMsg");
        assert!(bytes.len() <= MAX_IPC_PAYLOAD);
        let decoded = LayerToUiMsg::decode(&bytes).expect("decode LayerToUiMsg");
        assert_eq!(&decoded, msg);
    }

    let u2l_cases = vec![
        UiToLayerMsg::Ready,
        UiToLayerMsg::FrameReady(ShmSlotMeta {
            slot_index: 1,
            dst_x: 0,
            dst_y: 0,
            width: 1920,
            height: 1080,
            stride: 7680,
            fourcc: 0x3432_5241,
            modifier: 0,
            generation: 42,
            backend: FrameBackendKind::DmaBuf,
            byte_len: 1920 * 1080 * 4,
        }),
        UiToLayerMsg::ConfigUpdated(cfg),
        UiToLayerMsg::BenchmarkToggle,
        UiToLayerMsg::ScreenshotRequest { with_hud: true },
        UiToLayerMsg::OpenSaveFolder,
        UiToLayerMsg::KeybindCaptureState {
            capturing: true,
            target: KeybindTarget::ToggleGui,
        },
        UiToLayerMsg::ExternalOverlayActive(true),
        UiToLayerMsg::ExternalOverlayActive(false),
    ];
    for msg in &u2l_cases {
        let bytes = msg.encode().expect("encode UiToLayerMsg");
        assert!(bytes.len() <= MAX_IPC_PAYLOAD);
        let decoded = UiToLayerMsg::decode(&bytes).expect("decode UiToLayerMsg");
        assert_eq!(&decoded, msg);
    }

    // Uji penolakan magic salah, ukuran terpotong, diskriminan tak dikenal, trailing byte, dan overflow batas
    assert!(LayerToUiMsg::decode(b"BAD!").is_err());
    let mut bad_magic = LayerToUiMsg::Shutdown.encode().unwrap();
    assert_eq!(&bad_magic[0..4], &IPC_MAGIC);
    bad_magic[0] ^= 0xff;
    assert!(LayerToUiMsg::decode(&bad_magic).is_err());

    let mut bad_disc = UiToLayerMsg::Ready.encode().unwrap();
    bad_disc[4] = 0xfe;
    assert!(UiToLayerMsg::decode(&bad_disc).is_err());

    let mut with_trailing = LayerToUiMsg::Shutdown.encode().unwrap();
    with_trailing.push(0x00);
    assert!(LayerToUiMsg::decode(&with_trailing).is_err());

    let mut huge = vec![0_u8; MAX_IPC_PAYLOAD + 1];
    huge[0..4].copy_from_slice(&IPC_MAGIC);
    assert!(LayerToUiMsg::decode(&huge).is_err());

    let too_many_hist = LayerToUiMsg::TelemetryUpdate(Box::new(TelemetrySnapshot {
        frametime_history: vec![1.0; 121],
        ..TelemetrySnapshot::default()
    }));
    let mut raw_hist = IPC_MAGIC.to_vec();
    raw_hist.extend_from_slice(&postcard::to_allocvec(&too_many_hist).unwrap());
    assert!(LayerToUiMsg::decode(&raw_hist).is_err());

    let too_many_cores = LayerToUiMsg::TelemetryUpdate(Box::new(TelemetrySnapshot {
        cpu_core_loads: vec![10.0; 65],
        ..TelemetrySnapshot::default()
    }));
    let mut raw_cores = IPC_MAGIC.to_vec();
    raw_cores.extend_from_slice(&postcard::to_allocvec(&too_many_cores).unwrap());
    assert!(LayerToUiMsg::decode(&raw_cores).is_err());

    let too_many_events = LayerToUiMsg::InputEvents(vec![UiInputEvent::MouseDown { button: 0 }; 257]);
    let mut raw_events = IPC_MAGIC.to_vec();
    raw_events.extend_from_slice(&postcard::to_allocvec(&too_many_events).unwrap());
    assert!(LayerToUiMsg::decode(&raw_events).is_err());
}

#[test]
fn socket_perms_so_peercred_and_scm_rights() {
    let dir = tempfile::tempdir().unwrap();
    let sock_path = dir.path().join("ui.sock");
    let server = IpcServer::bind(&sock_path).unwrap();
    let perms = std::fs::metadata(&sock_path).unwrap().permissions().mode() & 0o777;
    assert_eq!(perms, 0o600);

    // Fungsi murni verifikasi SO_PEERCRED
    assert!(verify_peer_credentials(1000, 1000).is_ok());
    assert!(verify_peer_credentials(1001, 1000).is_err());

    let handle = std::thread::spawn(move || {
        let mut conn = IpcClient::connect(&sock_path).unwrap();
        let writer = ShmRingWriter::new(2, 2).unwrap();
        conn.send_msg(b"PING", &[writer.fd().as_fd()]).unwrap();
        let mut reply = [0_u8; 64];
        let (n, _) = conn.recv_msg(&mut reply).unwrap();
        assert_eq!(&reply[..n], b"PONG");
    });

    let mut srv_conn = server.accept_verified().unwrap();
    let mut buf = [0_u8; 64];
    let (n, fds) = srv_conn.recv_msg(&mut buf).unwrap();
    assert_eq!(&buf[..n], b"PING");
    assert_eq!(fds.len(), 1);
    srv_conn.send_msg(b"PONG", &[]).unwrap();

    handle.join().unwrap();
    // Saat client drop, recv_msg berikutnya melaporkan EOF (0 bytes)
    let (eof_n, _) = srv_conn.recv_msg(&mut buf).unwrap();
    assert_eq!(eof_n, 0);
}

#[test]
fn shm_ring_ping_pong_bounds_and_resize() {
    let mut writer = ShmRingWriter::new(2, 2).unwrap();
    let reader = ShmRingReader::from_borrowed_fd(writer.fd(), writer.mapped_byte_len()).unwrap();

    let s0 = writer.write_next_slot(2, 2, &[10_u8; 16]).unwrap();
    assert_eq!(s0.slot_index, 0);
    assert_eq!(s0.generation, 1);
    let (g1, slot1, bytes1) = reader.read_latest().unwrap();
    assert_eq!((g1, slot1), (1, 0));
    assert_eq!(&bytes1[..16], &[10_u8; 16]);

    let s1 = writer.write_next_slot(2, 2, &[20_u8; 16]).unwrap();
    assert_eq!(s1.slot_index, 1);
    assert_eq!(s1.generation, 2);
    let (g2, slot2, bytes2) = reader.read_latest().unwrap();
    assert_eq!((g2, slot2), (2, 1));
    assert_eq!(&bytes2[..16], &[20_u8; 16]);

    let s2 = writer.write_next_slot(2, 2, &[30_u8; 16]).unwrap();
    assert_eq!(s2.slot_index, 0);
    assert_eq!(s2.generation, 3);

    // Payload melebihi kapasitas slot ditolak cleanly
    assert!(writer.write_next_slot(2, 2, &[0_u8; 64]).is_err());

    // Resize memperbarui backing buffer tanpa crash
    writer.resize(4, 4).unwrap();
    let s3 = writer.write_next_slot(4, 4, &[77_u8; 64]).unwrap();
    assert_eq!(s3.width, 4);
    assert_eq!(s3.height, 4);
    let reader2 = ShmRingReader::from_borrowed_fd(writer.fd(), writer.mapped_byte_len()).unwrap();
    let (g4, _, bytes4) = reader2.read_latest().unwrap();
    assert!(g4 >= 4);
    assert_eq!(&bytes4[..64], &[77_u8; 64]);
}

#[test]
fn shm_corrupt_header_truncated_and_oob_rejected() {
    use std::io::Write;
    use std::os::unix::fs::FileExt;

    let mut trunc = tempfile::tempfile().unwrap();
    trunc.write_all(&[0_u8; 32]).unwrap();
    assert!(ShmRingReader::from_borrowed_fd(trunc.as_fd(), 64).is_err());

    let bad_magic = tempfile::tempfile().unwrap();
    bad_magic.set_len(96).unwrap();
    bad_magic.write_all_at(&0xDEAD_BEEF_u32.to_ne_bytes(), 0).unwrap();
    assert!(ShmRingReader::from_borrowed_fd(bad_magic.as_fd(), 96).is_err());

    let writer = ShmRingWriter::new(2, 2).unwrap();
    let reader = ShmRingReader::from_borrowed_fd(writer.fd(), writer.mapped_byte_len()).unwrap();
    assert!(reader.read_latest().is_none());

    // Header valid magic dengan OFF_ACTIVE_SLOT=2 (out-of-range) atau OFF_BYTE_LEN melebihi slot_capacity
    let oob_slot = tempfile::tempfile().unwrap();
    oob_slot.set_len(96).unwrap();
    oob_slot.write_all_at(&0x5249_4E47_u32.to_ne_bytes(), 0).unwrap(); // OFF_MAGIC
    oob_slot.write_all_at(&2_u32.to_ne_bytes(), 8).unwrap(); // OFF_ACTIVE_SLOT = 2
    oob_slot.write_all_at(&1_u64.to_ne_bytes(), 16).unwrap(); // OFF_GENERATION = 1
    oob_slot.write_all_at(&16_u64.to_ne_bytes(), 32).unwrap(); // OFF_SLOT_CAPACITY = 16
    oob_slot.write_all_at(&16_u32.to_ne_bytes(), 40).unwrap(); // OFF_BYTE_LEN = 16
    let oob_reader = ShmRingReader::from_borrowed_fd(oob_slot.as_fd(), 96).unwrap();
    assert!(oob_reader.read_latest().is_none());

    oob_slot.write_all_at(&0_u32.to_ne_bytes(), 8).unwrap(); // slot = 0 (valid)
    oob_slot.write_all_at(&999_u32.to_ne_bytes(), 40).unwrap(); // OFF_BYTE_LEN > map_len & cap
    assert!(oob_reader.read_latest().is_none());

    // Kapasitas slot >= 4 GiB harus ditolak sebelum truncating cast u32
    assert!(ShmRingWriter::validate_slot_capacity(65_536, 16_384).is_err());
}

#[test]
fn shm_seqlock_rejects_odd_in_progress_sequence_and_read_latest_into_reuses_buffer() {
    use std::os::unix::fs::FileExt;

    let mut writer = ShmRingWriter::new(2, 2).unwrap();
    let reader = ShmRingReader::from_borrowed_fd(writer.fd(), writer.mapped_byte_len()).unwrap();
    writer.write_next_slot(2, 2, &[42_u8; 16]).unwrap();

    let mut dst = Vec::with_capacity(64);
    let (g, slot) = reader.read_latest_into(&mut dst).unwrap();
    assert_eq!((g, slot), (1, 0));
    assert_eq!(dst, vec![42_u8; 16]);

    // Simulasikan writer sedang berada di tengah penulisan (OFF_SEQ = 48 bernilai ganjil)
    let raw_file = std::fs::File::from(writer.fd().try_clone_to_owned().unwrap());
    raw_file.write_all_at(&3_u64.to_ne_bytes(), 48).unwrap();
    assert!(reader.read_latest().is_none());
    assert!(reader.read_latest_into(&mut dst).is_none());
}

#[test]
fn protocol_frame_tick_active_gpu_benchmark_loads_and_positioned_shm_slot() {
    use fpshud_ipc::protocol::FrameTickData;

    let tick = FrameTickData {
        fps: 120.0,
        fps_avg: 118.5,
        fps_1pct_low: 95.0,
        fps_01pct_low: 88.0,
        frametime_ms: 8.33,
        frametime_history: vec![8.3, 8.4, 8.2],
        render_api: "VULKAN".to_string(),
        benchmark_active: true,
        benchmark_elapsed_sec: 4.5,
        last_benchmark: None,
    };
    let msg = LayerToUiMsg::FrameTick(Box::new(tick));
    let enc = msg.encode().unwrap();
    assert_eq!(LayerToUiMsg::decode(&enc).unwrap(), msg);

    let gpu_msg = LayerToUiMsg::ActiveGpu {
        vendor_id: 0x10de,
        device_id: 0x21c4,
    };
    assert_eq!(
        LayerToUiMsg::decode(&gpu_msg.encode().unwrap()).unwrap(),
        gpu_msg
    );

    let loads = UiToLayerMsg::BenchmarkLoads {
        cpu_load: 42.5,
        gpu_load: 87.0,
    };
    assert_eq!(
        UiToLayerMsg::decode(&loads.encode().unwrap()).unwrap(),
        loads
    );

    let mut writer = ShmRingWriter::new(16, 16).unwrap();
    let meta = writer.write_next_slot_at(24, 18, 4, 4, &[9_u8; 64]).unwrap();
    assert_eq!(
        (meta.dst_x, meta.dst_y, meta.width, meta.height),
        (24, 18, 4, 4)
    );
}

#[test]
fn test_recorder_ipc_roundtrip() {
    use fpshud_ipc::protocol::RecorderStatusSnapshot;

    let snap = RecorderStatusSnapshot {
        is_recording: true,
        elapsed_sec: 12.5,
        dropped_frames: 2,
        active_backend: "NVENC H.264".to_string(),
        active_output_path: Some("/tmp/fpshud_rec_01.mp4".to_string()),
    };
    let msg = LayerToUiMsg::RecorderStatusUpdate(snap);
    let raw = msg.encode().expect("encode RecorderStatusUpdate");
    let decoded = LayerToUiMsg::decode(&raw).expect("decode RecorderStatusUpdate");
    assert_eq!(decoded, msg);

    let toggle = UiToLayerMsg::ToggleRecord;
    let raw_t = toggle.encode().expect("encode ToggleRecord");
    let decoded_t = UiToLayerMsg::decode(&raw_t).expect("decode ToggleRecord");
    assert_eq!(decoded_t, toggle);
}
