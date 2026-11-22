use fpshud_telemetry::{CpuPowerSource, TelemetryCollector};
use std::fs;

#[test]
fn sudo_free_cpu_power_model_and_memory_psi() {
    let root = tempfile::tempdir().unwrap();
    let proc_dir = root.path().join("proc");
    let powercap = root.path().join("sys/class/powercap/intel-rapl:0");
    let cpufreq = root.path().join("sys/devices/system/cpu/cpu0/cpufreq");
    let psi = proc_dir.join("pressure");
    fs::create_dir_all(&psi).unwrap();
    fs::create_dir_all(&powercap).unwrap();
    fs::create_dir_all(&cpufreq).unwrap();

    fs::write(
        proc_dir.join("stat"),
        "cpu  1000 0 500 8500 0 0 0 0\ncpu0 500 0 250 4250 0 0 0 0\n",
    )
    .unwrap();
    fs::write(
        proc_dir.join("meminfo"),
        "MemTotal: 16000000 kB\nMemAvailable: 12000000 kB\nSwapTotal: 2097152 kB\nSwapFree: 1048576 kB\n",
    )
    .unwrap();
    fs::write(
        psi.join("memory"),
        "some avg10=1.25 avg60=0.50 avg300=0.10 total=100\n",
    )
    .unwrap();
    fs::write(powercap.join("constraint_0_max_power_uw"), "58000000\n").unwrap();
    fs::write(powercap.join("constraint_1_power_limit_uw"), "125000000\n").unwrap();
    fs::write(cpufreq.join("cpuinfo_max_freq"), "4300000\n").unwrap();
    fs::write(cpufreq.join("scaling_cur_freq"), "3800000\n").unwrap();

    let mut cpu = fpshud_telemetry::cpu::CpuProvider::new(root.path());
    let mut snap = fpshud_telemetry::TelemetrySnapshot::default();
    cpu.poll(&mut snap);
    fs::write(
        proc_dir.join("stat"),
        "cpu  1500 0 500 9000 0 0 0 0\ncpu0 900 0 350 4250 0 0 0 0\n",
    )
    .unwrap();
    cpu.poll(&mut snap);

    assert!(snap.cpu_power_w > 4.0 && snap.cpu_power_w <= 125.0);
    assert_eq!(snap.cpu_power_source, CpuPowerSource::EstimatedDvfs);

    let mut mem = fpshud_telemetry::memory::MemoryProvider::new(root.path());
    mem.poll(&mut snap);
    assert!((snap.mem_psi_some_pct - 1.25).abs() < 0.01);
    assert!(snap.ram_total_gb > 14.0 && snap.ram_used_gb > 3.0);
    assert!(snap.swap_total_gb > 1.9 && snap.swap_used_gb > 0.9);
}

#[test]
fn hwmon_direct_power_takes_precedence_over_estimate() {
    let root = tempfile::tempdir().unwrap();
    let hwmon = root.path().join("sys/class/hwmon/hwmon0");
    fs::create_dir_all(&hwmon).unwrap();
    fs::write(hwmon.join("name"), "zenergy\n").unwrap();
    fs::write(hwmon.join("power1_input"), "42500000\n").unwrap();

    let mut cpu = fpshud_telemetry::cpu::CpuProvider::new(root.path());
    let mut snap = fpshud_telemetry::TelemetrySnapshot::default();
    cpu.poll(&mut snap);
    assert!((snap.cpu_power_w - 42.5).abs() < 0.1);
    assert_eq!(snap.cpu_power_source, CpuPowerSource::DirectHwmon);
}

#[test]
fn drm_gpu_system_io_battery_and_collector_toggle_benchmark() {
    let root = tempfile::tempdir().unwrap();
    let drm = root.path().join("sys/class/drm/card0/device");
    let drm_hwmon = drm.join("hwmon/hwmon1");
    let bat = root.path().join("sys/class/power_supply/BAT0");
    let nvme_hwmon = root.path().join("sys/class/hwmon/hwmon2");
    let proc_self = root.path().join("proc/self");
    let proc_dir = root.path().join("proc");
    fs::create_dir_all(&drm_hwmon).unwrap();
    fs::create_dir_all(&bat).unwrap();
    fs::create_dir_all(&nvme_hwmon).unwrap();
    fs::create_dir_all(&proc_self).unwrap();

    fs::write(drm.join("vendor"), "0x1002\n").unwrap();
    fs::write(drm.join("device"), "0x73bf\n").unwrap();
    fs::write(drm.join("gpu_busy_percent"), "84\n").unwrap();
    fs::write(drm.join("mem_info_vram_used"), "4294967296\n").unwrap();
    fs::write(drm.join("mem_info_vram_total"), "8589934592\n").unwrap();
    fs::write(drm_hwmon.join("temp1_input"), "67000\n").unwrap();
    fs::write(drm_hwmon.join("power1_average"), "145000000\n").unwrap();
    fs::write(drm_hwmon.join("fan1_input"), "1850\n").unwrap();

    fs::write(bat.join("capacity"), "72\n").unwrap();
    fs::write(bat.join("status"), "Discharging\n").unwrap();
    fs::write(bat.join("power_now"), "22500000\n").unwrap();
    fs::write(nvme_hwmon.join("name"), "nvme\n").unwrap();
    fs::write(nvme_hwmon.join("temp1_input"), "46000\n").unwrap();
    fs::write(proc_self.join("statm"), "262144 65536 0 0 0 0 0\n").unwrap();
    fs::write(
        proc_dir.join("diskstats"),
        " 259 0 nvme0n1 100 0 2048 10 50 0 1024 5 0 15 15\n",
    )
    .unwrap();
    fs::create_dir_all(proc_dir.join("net")).unwrap();
    fs::write(
        proc_dir.join("net/dev"),
        "Inter-|\n face |bytes\n  eth0: 1048576 100 0 0 0 0 0 0 524288 50 0 0 0 0 0 0\n",
    )
    .unwrap();

    let collector = TelemetryCollector::with_mock_root(root.path().to_path_buf());
    collector.notify_active_gpu(0x1002, 0x73bf);
    collector.on_frame();
    let snap = collector.get_snapshot();
    assert!((snap.gpu_load - 84.0).abs() < 0.1);
    assert!((snap.gpu_temp_c - 67.0).abs() < 0.1);
    assert!((snap.gpu_power_w - 145.0).abs() < 0.1);
    assert!((snap.battery_pct - 72.0).abs() < 0.1);
    assert!((snap.nvme_temp_c - 46.0).abs() < 0.1);
    assert!(snap.game_ram_gb > 0.2);

    collector.toggle_benchmark();
    collector.on_frame();
    let bench_snap = collector.get_snapshot();
    assert!(bench_snap.benchmark_active);
    collector.toggle_benchmark();
    let finished_snap = collector.get_snapshot();
    assert!(!finished_snap.benchmark_active);
    assert!(finished_snap.last_benchmark.is_some());

    // Uji pemformatan seluruh 31 MetricType & indikator '~' pada EstimatedDvfs
    let mut fmt_snap = finished_snap.clone();
    fmt_snap.cpu_power_w = 55.2;
    fmt_snap.cpu_power_source = CpuPowerSource::EstimatedDvfs;
    assert_eq!(
        fmt_snap.format_metric_value(fpshud_core::MetricType::CpuPower),
        "~55.2 W"
    );
    fmt_snap.cpu_power_source = CpuPowerSource::DirectHwmon;
    assert_eq!(
        fmt_snap.format_metric_value(fpshud_core::MetricType::CpuPower),
        "55.2 W"
    );
    for &m in fpshud_core::MetricType::ALL {
        assert!(!fmt_snap.format_metric_value(m).is_empty());
    }

    assert_eq!(fmt_snap.system_time.len(), 5);
    assert_eq!(fmt_snap.system_time.as_bytes()[2], b':');
    let hh: u32 = fmt_snap.system_time[0..2].parse().unwrap();
    let mm: u32 = fmt_snap.system_time[3..5].parse().unwrap();
    assert!(hh <= 23 && mm <= 59);
}

#[cfg(feature = "vendor-gpu-apis")]
#[test]
fn nvml_d3cold_bandwidth_and_live_nvidia_probe() {
    use fpshud_telemetry::{
        GpuBackendType, TelemetrySnapshot,
        vendor::{NvidiaNvmlProvider, compute_nvml_bandwidth_gbs, is_nvidia_d3cold_suspended},
    };

    // 1. D3cold guard: suspended NVIDIA card detected without waking hardware
    let root_susp = tempfile::tempdir().unwrap();
    let dev = root_susp.path().join("sys/class/drm/card0/device");
    fs::create_dir_all(dev.join("power")).unwrap();
    fs::write(dev.join("vendor"), "0x10de\n").unwrap();
    fs::write(dev.join("power/runtime_status"), "suspended\n").unwrap();
    assert!(is_nvidia_d3cold_suspended(root_susp.path()));

    fs::write(dev.join("power/runtime_status"), "active\n").unwrap();
    assert!(!is_nvidia_d3cold_suspended(root_susp.path()));

    // 2. Bus width * mem clock * utilization bandwidth calculation (192-bit @ 7000 MHz @ 50%)
    let bw = compute_nvml_bandwidth_gbs(7000, 192, 50);
    assert!((bw - 168.0).abs() < 0.1, "expected 168 GB/s, got {bw}");

    // 2b. Non-root path & non-NVIDIA vendor_id wajib mengembalikan None dengan aman tanpa wake/crash
    assert!(NvidiaNvmlProvider::init_for_root(root_susp.path(), 0x10de, 0).is_none());
    assert!(NvidiaNvmlProvider::init_for_root(std::path::Path::new("/"), 0x1002, 0).is_none());

    // 3. Real host NVML probe when libnvidia-ml.so.1 exists
    if let Some(mut nvml) = NvidiaNvmlProvider::init_for_root(std::path::Path::new("/"), 0, 0) {
        let mut snap = TelemetrySnapshot::default();
        assert!(nvml.poll(&mut snap));
        assert!(snap.gpu_available);
        assert_eq!(snap.gpu_backend, GpuBackendType::Nvml);
        assert!(!snap.gpu_model.is_empty() && snap.gpu_model != "NVIDIA GPU");
        assert!(snap.gpu_vram_total_gb > 0.5);
    }
}
