#!/usr/bin/env bash
# scripts/bench_overhead.sh — Benchmark 4 skenario overhead FPSHUD pada vkcube (CPU, RAM, GPU VRAM)
# Penggunaan:
#   ./scripts/bench_overhead.sh          # default 30 detik per skenario
#   ./scripts/bench_overhead.sh 15       # custom durasi (detik)
set -euo pipefail

DURATION="${1:-30}"

if ! command -v vkcube >/dev/null 2>&1; then
    echo "[!] ERROR: vkcube tidak ditemukan di PATH." >&2
    exit 1
fi
if ! command -v ydotool >/dev/null 2>&1; then
    echo "[!] ERROR: ydotool diperlukan untuk membuka/menutup GUI via F12." >&2
    exit 1
fi

echo "=========================================================================="
echo "  FPSHUD Overhead Benchmark (vkcube 1280x720, ${DURATION}s per skenario)"
echo "=========================================================================="

python3 - "$DURATION" <<'PY'
import os, re, shutil, subprocess, sys, tempfile, time

DURATION = float(sys.argv[1])
ALL_31 = (
    "fps,fpsavg,fps1low,fps01low,frametimes,"
    "gpuload,gpuclock,gpuvramclock,gpuvram,gpubw,gputemp,gpufan,gpuwatt,"
    "cpuload,cpuallload,cpuclock,cpuallclock,cpuwatt,cputemp,"
    "ram,gameram,swap,mempsi,"
    "api,gpumodel,cpumodel,clock,battery,disk_io,disk_temp,net_speed"
)
CFG_ALL = f"preset=3\nhud_visible=1\nhud_graph_enabled=1\nmetrics={ALL_31}\n"

def get_proc_stats(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            rss_kb = 0
            for line in f:
                if line.startswith("VmRSS:"):
                    rss_kb = int(line.split()[1])
                    break
        with open(f"/proc/{pid}/stat") as f:
            parts = f.read().split()
        return rss_kb / 1024.0, int(parts[13]) + int(parts[14])
    except Exception:
        return 0.0, 0

def get_gpu_vram_by_pid():
    try:
        out = subprocess.check_output(["nvidia-smi"], text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return {}
    vram = {}
    for line in out.splitlines():
        m = re.search(r'\|\s+\d+\s+N/A\s+N/A\s+(\d+)\s+\S+\s+\S+\s+(\d+)MiB\s*\|', line)
        if m:
            vram[int(m.group(1))] = int(m.group(2))
    return vram

def find_child_ui(parent_pid):
    out = subprocess.check_output(["ps", "-eo", "pid,ppid,comm"], text=True)
    for line in out.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 3 and int(parts[1]) == parent_pid and "fpshud-ui" in parts[2]:
            return int(parts[0])
    return None

def press_f12():
    subprocess.run(["ydotool", "key", "88:1", "88:0"], capture_output=True)

def run_case(idx, label, disable_hud=False, open_gui=False, close_gui=False):
    tmpdir = tempfile.mkdtemp(prefix="fpshud_bench_")
    try:
        env = os.environ.copy()
        if disable_hud:
            env["DISABLE_FPS_HUD"] = "1"
            env.pop("FPS_HUD", None)
        else:
            env["FPS_HUD"] = "1"
            env.pop("DISABLE_FPS_HUD", None)
            cfg_dir = os.path.join(tmpdir, "fpshud")
            os.makedirs(cfg_dir, exist_ok=True)
            with open(os.path.join(cfg_dir, "config.ini"), "w") as f:
                f.write(CFG_ALL)
            env["XDG_CONFIG_HOME"] = tmpdir

        print(f"\n[{idx}/4] Menjalankan: {label} ({DURATION:.0f}s)...", flush=True)
        p = subprocess.Popen(
            ["vkcube", "--wsi", "wayland", "--width", "1280", "--height", "720"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        time.sleep(2.5)
        ui_pid = None if disable_hud else find_child_ui(p.pid)

        if open_gui:
            press_f12()
            time.sleep(2.5 if close_gui else 1.5)
        if close_gui:
            press_f12()
            time.sleep(1.5)

        clk = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
        rss1_v, cpu1_v = get_proc_stats(p.pid)
        rss1_u, cpu1_u = get_proc_stats(ui_pid) if ui_pid else (0.0, 0)
        t0 = time.monotonic()
        time.sleep(DURATION)
        dt = time.monotonic() - t0
        rss2_v, cpu2_v = get_proc_stats(p.pid)
        rss2_u, cpu2_u = get_proc_stats(ui_pid) if ui_pid else (0.0, 0)
        vram = get_gpu_vram_by_pid()

        p.terminate()
        try:
            p.wait(timeout=2)
        except Exception:
            p.kill()
        if ui_pid:
            try:
                os.kill(ui_pid, 15)
            except Exception:
                pass

        cpu_v = ((cpu2_v - cpu1_v) / clk) / dt * 100.0
        cpu_u = ((cpu2_u - cpu1_u) / clk) / dt * 100.0 if ui_pid else 0.0
        vram_v = vram.get(p.pid, 0)
        vram_u = vram.get(ui_pid, 0) if ui_pid else 0

        print(f"  vkcube    : RAM={rss1_v:.1f}->{rss2_v:.1f} MB | CPU={cpu_v:.2f}% | VRAM={vram_v} MiB", flush=True)
        print(f"  fpshud-ui : RAM={rss1_u:.1f}->{rss2_u:.1f} MB | CPU={cpu_u:.2f}% | VRAM={vram_u} MiB", flush=True)
        return (label, rss2_v, rss2_u, cpu_v, cpu_u, vram_v, vram_u)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

results = [
    run_case(1, "Baseline (Tanpa FPSHUD)", disable_hud=True),
    run_case(2, "All 31 Sensors (HUD Only)"),
    run_case(3, "All 31 Sensors + GUI Dibuka", open_gui=True),
    run_case(4, "All 31 Sensors + GUI -> Ditutup", open_gui=True, close_gui=True),
]

print("\n==========================================================================")
print(f"{'Skenario':<34} | {'RAM (MB)':>9} | {'CPU (%)':>8} | {'VRAM (MiB)':>10}")
print("-" * 74)
for label, rv, ru, cv, cu, vv, vu in results:
    print(f"{label:<34} | {rv+ru:>9.1f} | {cv+cu:>8.2f} | {vv+vu:>10d}")
print("==========================================================================")
PY
