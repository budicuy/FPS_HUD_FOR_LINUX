use fpshud_ipc::IpcServer;
use std::process::Command;
use std::time::{Duration, Instant};

#[test]
fn daemon_parent_exit_watchdog_and_cli_integration() {
    let dir = tempfile::tempdir().unwrap();
    let sock_path = dir.path().join("daemon.sock");
    let server = IpcServer::bind(&sock_path).unwrap();

    // Uji exit non-zero bila argumen CLI hilang
    let bad = Command::new(env!("CARGO_BIN_EXE_fpshud-ui"))
        .output()
        .unwrap();
    assert!(!bad.status.success());

    // Gunakan PID yang tidak ada (dead parent) -> daemon wajib keluar bersih < 500ms!
    let dead_pid = "999999";
    let start = Instant::now();
    let mut child = Command::new(env!("CARGO_BIN_EXE_fpshud-ui"))
        .arg("--socket")
        .arg(&sock_path)
        .arg("--parent-pid")
        .arg(dead_pid)
        .spawn()
        .unwrap();

    // Terima koneksi agar client tidak gagal connect, lalu tutup server connection
    let _conn = server.accept_verified();
    drop(_conn);
    drop(server);

    let status = child.wait().unwrap();
    let elapsed = start.elapsed();
    assert!(status.success(), "daemon should exit cleanly on EOF/dead parent, got {status}");
    assert!(elapsed < Duration::from_millis(500), "watchdog took {elapsed:?}");
}
