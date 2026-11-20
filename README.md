# FPSHUD — Pure-Rust GPUI-CE In-Game Overlay for Linux (Vulkan & OpenGL)

FPSHUD is a **100% Pure-Rust**, **zero-`sudo`**, lightweight in-game performance overlay and live configurator for Linux Vulkan and OpenGL games (including Steam Proton / DXVK / VKD3D).

## Architecture (`rust-rewrite`)

- **`libfpshud.so` (`fpshud-layer`)**: Thin `cdylib` Vulkan Implicit Layer (`ash`) and runtime-loaded OpenGL (`glX` / `egl` via `dlsym(RTLD_NEXT)`) interceptor exporting only the exact 8 Vulkan/GLX/EGL entrypoints (`manifest/libfpshud.version`), with **0 banned `DT_NEEDED` dependencies** (`libGL*`, `libX11*`, `libwayland-client*`, `libfontconfig*`, `libfreetype*`, `libxkbcommon*`, `libnvidia-ml*`, `libze_loader*`, `libstdc++`).
- **`fpshud-ui` (`fpshud-ui-bin` + `fpshud-ui` + `fpshud-gpui-platform`)**: Headless GPUI-CE daemon lazily launched inside the game's namespace (`DISABLE_FPS_HUD=1`, `LD_PRELOAD` stripped), rendering with embedded TTF fonts (`OpenSans-Bold.ttf` & `VeraMono-Bold.ttf`, no system `fontconfig`) over `gpui_ce_wgpu` into a dual-slot atomic `memfd` shared-memory ring (`SO_PEERCRED` Unix `SEQPACKET`).
- **31 Sudo-Free Sensors (`fpshud-telemetry`)**: Reads `/proc`, `/sys`, `hwmon`, DRM sysfs, PCI, battery, NVMe/disk I/O, PSI memory pressure, and CPU wattage via world-readable `hwmon` / passive counters plus a Multi-Core DVFS & PL1/PL2 power model — **zero root privileges or `/etc/udev/rules.d/` required**.
- **Pure-Rust Input & Screenshots (`fpshud-input`, `fpshud-screenshot`, `fpshud-analytics`)**: `x11rb` X11/XWayland pointer & keyboard isolation, optional non-privileged keyboard/mouse `evdev` support on Wayland (`input` group), `png` + `miniz_oxide` Clean-Game vs With-HUD screenshots, and symlink-safe (`O_NOFOLLOW`) CSV benchmark recording.

## Zero-Sudo Quick Start

### Install Latest Release

```bash
curl -fsSL https://raw.githubusercontent.com/budicuy/FPS_HUD_FOR_LINUX/main/install.sh | bash
```

### Build & Install From Source (Rust Edition 2024)

```bash
./scripts/package_release.sh --install
```

### Game Launch Options

- **Steam / Proton (Vulkan):**
  ```text
  FPS_HUD=1 %command%
  ```
- **OpenGL (`LD_PRELOAD`):**
  ```bash
  FPS_HUD=1 LD_PRELOAD="$HOME/.local/lib/fpshud/libfpshud.so" ./your_game
  ```

### Default Hotkeys (4 Customizable Keybinds)

| Key | Action |
|---|---|
| `F12` / `Insert` | Toggle In-Game GPUI-CE Configurator Dashboard |
| `F11` | Toggle Minimalist HUD Visibility |
| `F9` | Start / Stop CSV Frametime Benchmark |
| `F10` | Capture PNG Screenshot (Clean Game or With HUD) |

### Uninstall

```bash
~/.local/lib/fpshud/uninstall.sh
```

## Verification & Quality Gates

```bash
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
cargo clippy -p gpui_ce_wgpu --lib -- -D warnings
./scripts/package_release.sh
env -u WAYLAND_DISPLAY -u DISPLAY ./scripts/test_vulkan_headless.sh
```
