# FPSHUD — Overlay In-Game GPUI-CE 100% Pure Rust untuk Linux (Vulkan & OpenGL)

FPSHUD adalah overlay performa dan menu konfigurator in-game yang dibangun **100% dalam Pure Rust**, **bebas `sudo` sepenuhnya**, untuk game Vulkan dan OpenGL di Linux (termasuk Steam Proton / DXVK / VKD3D).

## Arsitektur (`rust-rewrite`)

- **`libfpshud.so` (`fpshud-layer`)**: Shared library Vulkan Implicit Layer (`ash`) dan interceptor OpenGL (`glX` / `egl` lewat `dlsym(RTLD_NEXT)`) yang hanya mengekspor tepat 8 simbol C-ABI (`manifest/libfpshud.version`), tanpa dependensi `DT_NEEDED` ke `libGL*`, `libX11*`, `libwayland-client*`, `libfontconfig*`, `libfreetype*`, `libxkbcommon*`, `libnvidia-ml*`, `libze_loader*`, maupun `libstdc++`.
- **`fpshud-ui` (`fpshud-ui-bin` + `fpshud-ui` + `fpshud-gpui-platform`)**: Daemon GPUI-CE headless yang diluncurkan di namespace yang sama dengan game (`DISABLE_FPS_HUD=1`, `LD_PRELOAD` dibersihkan), merender UI memakai font TTF tertanam (`OpenSans-Bold.ttf` & `VeraMono-Bold.ttf` tanpa `fontconfig` sistem) melalui `gpui_ce_wgpu` ke cincin `memfd` atomik 2-slot (`Unix SEQPACKET` + `SO_PEERCRED`).
- **31 Sensor Telemetri Bebas `sudo` (`fpshud-telemetry`)**: Membaca `/proc`, `/sys`, `hwmon`, DRM sysfs, baterai, disk I/O & suhu NVMe, tekanan memori PSI, dan daya CPU menggunakan `hwmon` world-readable serta model fisika Multi-Core DVFS + PL1/PL2 tanpa `udev` rule root apa pun.
- **Input & Screenshot Pure Rust (`fpshud-input`, `fpshud-screenshot`, `fpshud-analytics`)**: Isolasi pointer & keyboard X11/XWayland lewat `x11rb`, dukungan opsional keyboard/mouse `evdev` non-root di Wayland (grup `input`), encoder PNG `png` + `miniz_oxide` (mode *Clean Game* vs *With HUD*), serta benchmark CSV aman terhadap symlink (`O_NOFOLLOW`).

## Instalasi & Pemakaian Cepat (Tanpa `sudo`)

### Instalasi Rilis Portabel

```bash
curl -fsSL https://raw.githubusercontent.com/budicuy/FPS_HUD_FOR_LINUX/main/install.sh | bash
```

### Build & Install Lokal dari Source (Rust Edition 2024)

```bash
./scripts/package_release.sh --install
```

### Opsi Peluncuran Game

- **Steam / Proton (Vulkan):**
  ```text
  FPS_HUD=1 %command%
  ```
- **OpenGL (`LD_PRELOAD`):**
  ```bash
  FPS_HUD=1 LD_PRELOAD="$HOME/.local/lib/fpshud/libfpshud.so" ./game_anda
  ```

### Pintasan Keyboard Bawaan (4 Keybind Kustom)

| Tombol | Fungsi |
|---|---|
| `F12` / `Insert` | Buka / Tutup Dashboard Konfigurator GPUI-CE |
| `F11` | Tampilkan / Sembunyikan HUD |
| `F9` | Mulai / Hentikan Rekaman Benchmark CSV |
| `F10` | Ambil Screenshot PNG (Clean Game atau With HUD) |

### Uninstall

```bash
~/.local/lib/fpshud/uninstall.sh
```

## Pengujian & Gate Kualitas

```bash
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
cargo clippy -p gpui_ce_wgpu --lib -- -D warnings
./scripts/package_release.sh
env -u WAYLAND_DISPLAY -u DISPLAY ./scripts/test_vulkan_headless.sh
```
