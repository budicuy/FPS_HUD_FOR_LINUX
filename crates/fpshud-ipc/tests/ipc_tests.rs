use fpshud_core::HudConfig;
use fpshud_input::{KeybindTarget, UiInputEvent};
use fpshud_ipc::{
    FrameBackendKind, IpcClient, IpcServer, LayerToUiMsg, ShmRingReader, ShmRingWriter,
    ShmSlotMeta, UiToLayerMsg,
    protocol::{IPC_MAGIC, MAX_IPC_PAYLOAD, WireHeader},
    socket::verify_peer_credentials,
};
use fpshud_telemetry::TelemetrySnapshot;
use std::os::fd::AsFd;
use std::os::unix::fs::PermissionsExt;

#[test]
fn protocol_all_variants_and_corrupt_rejection() {
    assert_eq!(std::mem::size_of::<WireHeader>(), 16);
    assert_eq!(std::mem::size_of::<ShmSlotMeta>(), 48);

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
    ];
    for msg in &u2l_cases {
        let bytes = msg.encode().expect("encode UiToLayerMsg");
        assert!(bytes.len() <= MAX_IPC_PAYLOAD);
        let decoded = UiToLayerMsg::decode(&bytes).expect("decode UiToLayerMsg");
        assert_eq!(&decoded, msg);
    }

    // Uji penolakan magic salah, ukuran terpotong, diskriminan tak dikenal, dan payload terlalu besar
    assert!(LayerToUiMsg::decode(b"BAD!").is_err());
    let mut bad_magic = LayerToUiMsg::Shutdown.encode().unwrap();
    bad_magic[0] ^= 0xff;
    assert!(LayerToUiMsg::decode(&bad_magic).is_err());

    let mut bad_disc = UiToLayerMsg::Ready.encode().unwrap();
    bad_disc[4] = 0xfe;
    assert!(UiToLayerMsg::decode(&bad_disc).is_err());

    let mut huge = [0_u8; 20];
    huge[0..4].copy_from_slice(&IPC_MAGIC.to_le_bytes());
    huge[8..12].copy_from_slice(&((MAX_IPC_PAYLOAD as u32) + 1).to_le_bytes());
    assert!(LayerToUiMsg::decode(&huge).is_err());
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
