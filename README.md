# FPSHUD

**Lightweight In-Game Performance Overlay & Live GUI Configurator for Linux Gaming (Vulkan & OpenGL)**

[![CI Build & Release](https://github.com/budicuy/FPS_HUD_FOR_LINUX/actions/workflows/ci.yml/badge.svg)](https://github.com/budicuy/FPS_HUD_FOR_LINUX/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Language: C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?logo=c%2B%2B)](CMakeLists.txt)
[![Graphics API: Vulkan & OpenGL](https://img.shields.io/badge/Graphics-Vulkan%20%7C%20OpenGL-orange.svg)](src/)
[![GPU Support: NVIDIA | AMD | Intel](https://img.shields.io/badge/GPU-NVIDIA%20%7C%20AMD%20%7C%20Intel-success.svg)](src/telemetry.cpp)

> 🌐 **Language / Bahasa Dokumentasi:** **[English (Default)](README.md)** | **[Bahasa Indonesia](README.id.md)**

---

## Quick Install (1 Command — No Compiling Required!)

Install **FPSHUD** in seconds on any Linux distribution (Arch, Ubuntu, Debian, Fedora, Linux Mint, Pop!_OS, Nobara, Bazzite, or SteamOS) without installing compilers or build tools:

```bash
curl -fsSL https://raw.githubusercontent.com/budicuy/FPS_HUD_FOR_LINUX/main/install.sh | bash
```

To uninstall at any time:
```bash
curl -fsSL https://raw.githubusercontent.com/budicuy/FPS_HUD_FOR_LINUX/main/uninstall.sh | bash
```

---

## Overview

### What is FPSHUD for?
When playing games on Linux (whether through Steam Proton, Lutris, Heroic, or native Linux ports), you often want to check your **FPS**, see if your game is stuttering (**1% Low / Frametime**), or keep an eye on your **GPU and CPU temperatures, clock speeds, power draw, and RAM usage**.

With traditional Linux overlays, customizing what appears on screen usually means closing the game, opening a text editor, and manually typing long configuration lines or environment variables. **FPSHUD solves this by giving you a live, interactive control panel directly inside your game.** You simply turn it on with `FPS_HUD=1`, press **`Insert`** or **`F12`** while playing, and click what you want to see—just like modern overlays on Windows, but built natively and lightweight for Linux.

### What is it built with?
FPSHUD is written in **modern C++20** for maximum speed and minimal memory usage, uses **Dear ImGui** to draw both the on-screen HUD and the in-game settings menu, and hooks directly into **Vulkan** (as a standard Vulkan Implicit Layer) and **OpenGL** (`LD_PRELOAD`). Fonts are embedded directly inside the library so text always looks sharp, even inside containerized environments like Steam Linux Runtime and Proton.

### What features are inside?
- **3-Column Split View In-Game Dashboard (`Insert` / `F12` / `F10`)**: A clean, widescreen control panel divided into 3 modular sections (Left: HUD Style, Colors & Typography | Center: 23 Live Hardware Sensors & Presets | Right: Hardware-Accelerated Screen Recorder & Benchmark Hub).
- **Hardware-Accelerated Screen Recording (`F7`)**: Built-in zero-stutter video capture daemon supporting HEVC (H.265) and H.264 with Adaptive CQP (16–30), prominent 1-click recording, and PulseAudio/PipeWire audio mixing (Game, Mic, or Both).
- **4 Ready-to-Use Presets**: Switch with one click between **Minimal** (FPS + Frametime only), **Standard** (FPS, GPU, CPU, RAM essentials), **Advance** (all 23 sensors), or save your own **Custom** preset.
- **Works Across All Hardware (NVIDIA, AMD, Intel)**: Automatically detects and reads sensors from **NVIDIA GPUs**, **AMD Radeon GPUs & APUs**, **Intel Arc & Iris GPUs**, and **Intel Core & AMD Ryzen CPUs**—including dual-GPU gaming laptops and handhelds like the Steam Deck.
- **Easy-to-Read Two-Tone Design & Thermal Warnings**: Numbers are displayed in bright white while units (`MHz`, `GB`, `°C`, `W`) are softly dimmed so values never blend together, with automatic yellow/red color warnings when your GPU or CPU runs hot.
- **Accurate 1% & 0.1% Low FPS + CSV Benchmark Recorder (`F9`)**: Measures real frame smoothness over a rolling 3.5-second window (automatically ignoring loading-screen freezes) and lets you record benchmark results directly to a `.csv` file.

### How does it work?
1. **Install once** using the 1-line `curl` command above (it places the portable `libfpshud.so` library in `~/.local/lib/fpshud/` and registers the Vulkan layer in your user folder).
2. **Enable in your game** by adding `FPS_HUD=1 %command%` in Steam Launch Options (or running `FPS_HUD=1 your_game` in terminal).
3. **Zero FPS drop while gaming**: As your game renders each frame, FPSHUD draws the overlay on top of the final image before it hits your monitor. Meanwhile, a separate background thread quietly reads your CPU/GPU temperatures and power usage once every 0.5 seconds—so the overlay never slows down your game.

---

## Table of Contents

1. [Key Features](#1-key-features)
2. [Installation & Sensor Permissions](#2-installation--sensor-permissions)
3. [How to Use in Steam, Lutris, Heroic & Terminal](#3-how-to-use-in-steam-lutris-heroic--terminal)
4. [In-Game GUI Guide & Keyboard Shortcuts](#4-in-game-gui-guide--keyboard-shortcuts)
5. [Hardware-Accelerated Screen Recording](#5-hardware-accelerated-screen-recording)
6. [Supported Hardware & All 23 Sensors](#6-supported-hardware--all-23-sensors)
7. [Tech Stack & System Architecture](#7-tech-stack--system-architecture)
8. [Developer Guide (Building from Source, Sanitizers & Testing)](#8-developer-guide-building-from-source-sanitizers--testing)
9. [Troubleshooting & FAQ](#9-troubleshooting--faq)
10. [Contributing, Credits & License](#10-contributing-credits--license)

---

## 1. Key Features

- **Simple Activation (`FPS_HUD=1` / `FPS_HUD=0`)**:
  - `FPS_HUD=1`: Turns on the HUD overlay, Screen Recorder, and the In-Game GUI Configurator.
  - `FPS_HUD=0` (or unset): Completely disabled with zero background impact.
- **3-Column Split View In-Game GUI (`FPSHUD Configuration Tool`)**:
  - Press **`Insert`**, **`F12`**, or **`F10`** in-game to open a clean 3-column widescreen dashboard (Left: Position, Layout, Font, Colors, Shortcuts | Center: 23 Live Hardware Sensors & Quick Filters | Right: Screen Recorder & Benchmark Hub).
  - Automatically captures your mouse cursor while the menu is open and returns control to the game the moment you close it.
  - Automatically saves all your changes to `~/.config/fpshud/config.ini`.
- **Hardware-Accelerated Screen Recording Hub**:
  - Dedicated background recording daemon (`fpshud_recorder`) with zero frame stutter, using triple-buffered POSIX Shared Memory IPC.
  - Supports industry-standard **HEVC (H.265 / `hvc1`)** and **H.264** with **Adaptive CQP (CQ 16–30)** for crisp, artifact-free gameplay video.
  - Multi-source audio recording (Game Audio, Microphone, Mixed, or Mute) via PulseAudio/PipeWire saved directly to `~/Videos/FPSHUD`.
- **Universal Multi-Vendor Hardware Support**:
  - **NVIDIA**: Uses `libnvidia-ml.so.1` (NVML) dynamically.
  - **AMD Radeon & APUs (`amdgpu`)**: Reads Linux DRM & `hwmon` sensors, supports `pp_dpm_sclk` fallback, and combines shared `GTT` memory on APUs/Steam Deck.
  - **Intel Arc & Integrated Graphics (`i915` / `xe`)**: Reads Linux DRM & `hwmon` sensors, `lmem_total_bytes`, and Intel RAPL power counters.
  - **Hybrid Gaming Laptops**: Automatically prioritizes the dedicated gaming GPU (`card1`) over the integrated GPU (`card0`) and identifies exact GPU model names via `/usr/share/hwdata/pci.ids`.
- **High-Contrast Readability & Dynamic Alerts**:
  - Category color badges (`FPS` Gold, `GPU` Green, `CPU` Cyan, `RAM` Purple, `SYS` Silver), middle-dot (`·`) separators, and automatic thermal alerts (**Amber at `>= 72°C`**, **Red at `>= 82°C`**).
- **Mini Frametime Graph & CSV Benchmark Logging (`F9`)**:
  - Optional live frametime waveform graph right on the HUD, plus 1-key CSV benchmarking (`~/Documents/fpshud_benchmark.csv`) that logs Avg FPS, 1% Low, 0.1% Low, Min/Max FPS, and peak GPU/CPU temperatures and wattage.
- **Extreme LLVM/Clang 23 Optimization & Memory Hardening**:
  - Compiled with Clang 23.1.4, LLD linker, ThinLTO (`-flto=thin`), and `-O3` producing an ultra-compact **1.3 MB** library with zero runtime overhead, verified by AddressSanitizer and UndefinedBehaviorSanitizer.

---

## 2. Installation & Sensor Permissions

### 1-Command Installation (Recommended for All Users)
```bash
curl -fsSL https://raw.githubusercontent.com/budicuy/FPS_HUD_FOR_LINUX/main/install.sh | bash
```
> [!NOTE]
> During installation, the script will prompt for optional `sudo` permission. Granting it will automatically configure:
> 1. **RAM Speed & Generation** (`DDR4 / DDR5 / LPDDR5` in `MT/s`).
> 2. **CPU Power Consumption** (`intel-rapl` / `amd-rapl` in Watts).
> 3. **Wayland Hotkeys & Mouse Input** (`/dev/input/event*` for `Insert` / `F12` in-game GUI).
>
> If you decline or run non-interactively without sudo, the installation continues cleanly and non-privileged sensors will function normally.

### 1-Command Uninstallation (Clean Removal)
```bash
curl -fsSL https://raw.githubusercontent.com/budicuy/FPS_HUD_FOR_LINUX/main/uninstall.sh | bash
```

---

## 3. How to Use in Steam, Lutris, Heroic & Terminal

### 1. Steam (Proton / DXVK / Vulkan Games)
Right-click your game in Steam -> **Properties** -> **General** -> **Launch Options**:
```bash
FPS_HUD=1 %command%
```
*(To disable temporarily, change it to `FPS_HUD=0 %command%`)*

### 2. Steam (Native Linux OpenGL Games)
```bash
FPS_HUD=1 ~/.local/bin/fpshud %command%
```

### 3. Lutris & Heroic Games Launcher
Open **Game Settings** -> **System / Environment Variables** -> Add:
- **Key**: `FPS_HUD`
- **Value**: `1`

### 4. Terminal / Command Line
```bash
# Run any Vulkan game or application:
FPS_HUD=1 ./your_game

# Run any OpenGL game or application:
~/.local/bin/fpshud ./your_game
```

---

## 4. In-Game GUI Guide & Keyboard Shortcuts

Once inside a game with `FPS_HUD=1`, use these shortcuts (all customizable inside the GUI):

| Default Key | Action |
| :--- | :--- |
| **`Insert` / `F12` / `F10`** | Open or Close the **`FPSHUD Configuration Tool`** menu |
| **`F11`** | Quickly hide or show the HUD overlay without opening the menu |
| **`F7`** | Start or Stop **Hardware-Accelerated Screen Recording** (`~/Videos/FPSHUD`) |
| **`F9`** | Start or Stop **CSV Benchmark Recording** (`~/Documents/fpshud_benchmark.csv`) |
| **`ESC`** | Close the GUI menu and return mouse control to the game |

### The 4 Built-In Presets
- **`Minimal`**: Shows only `FPS` and `Frametime (ms)`.
- **`Standard`**: Shows `FPS`, `Frametime`, `GPU (Load, Clock, Temp)`, `CPU (Load, Clock, Temp)`, and `RAM`.
- **`Advance`**: Enables all **23 sensors** (`FPS`, `AVG`, `1% Low`, `0.1% Low`, `Frametime`, all 7 `GPU` metrics, all 4 `CPU` metrics, all 5 `Memory/RAM` metrics, `Render API`, and `Local Clock`).
- **`Custom` / `Save Custom`**: Save your favorite combination of sensors and layout to `~/.config/fpshud/custom_preset.ini`.

---

## 5. Hardware-Accelerated Screen Recording

FPSHUD includes a dedicated, lightweight background screen recording daemon (`fpshud_recorder`) that runs decoupled from the game process. Frames are submitted via triple-buffered POSIX Shared Memory (`/dev/shm`), ensuring that hardware video encoding never introduces frame stutter or micro-freezes to your gameplay.

### Key Capabilities & Codec Options

| Feature | Description | Default / Recommended |
| :--- | :--- | :--- |
| **Video Codecs** | **HEVC (H.265 / `hvc1`)** or **H.264 (AVC)** | **HEVC (`hvc1`)** for 50% smaller files and universal compatibility in Discord, Chrome, QuickTime, and Premiere. |
| **Rate Control** | **Adaptive CQP (Constant Quantization Parameter)** or **Fixed VBR** | **Adaptive CQP (Level 22)**: dynamically allocates bitrates based on scene complexity so fast action never pixelates. |
| **CQP Levels** | Fine-tune visual quality from **CQ 16** (Near-lossless, higher bitrate) to **CQ 30** (Maximum compression). | **CQ 20 - 22** |
| **Framerate & Resolution** | Native (Matches game resolution), 1080p, 720p, or 1440p at **30, 60, 120, or 144 FPS**. | **Native @ 60 FPS** |
| **Audio Capture** | Low-latency audio recording via PulseAudio / PipeWire: **Game Only**, **Microphone Only**, **Game + Microphone (Stereo Downmixed)**, or **Mute**. | **Game Audio Only** |
| **Storage Destination** | Clips automatically saved to `~/Videos/FPSHUD/` with timestamped filenames (`FPSHUD_REC_YYYYMMDD_HHMMSS.mp4`). | `~/Videos/FPSHUD` |

### How to Use Screen Recorder

1. **Start / Stop Recording (`F7`)**:
   - Press **`F7`** in-game (or click **`[REC] Start Record`** in Column 3 of the GUI).
   - An in-game status toast confirms recording has started. Press **`F7`** again to finalize the MP4/MKV video file.
2. **Open System Folder**:
   - Click the **`Open System Folder`** button in the in-game GUI menu to open your recordings directory directly in your file manager (Dolphin, Nautilus, Nemo, Thunar, etc.).

---

## 6. Supported Hardware & All 23 Sensors

### Hardware Compatibility Matrix

| Component | Supported Vendors & Drivers | Linux Data Sources |
| :--- | :--- | :--- |
| **NVIDIA GPUs** | NVIDIA Proprietary / Open Kernel (`nvidia`) | `libnvidia-ml.so.1` (NVML runtime `dlopen`) |
| **AMD GPUs & APUs** | AMD Radeon Discrete & APU (`amdgpu`) | `/sys/class/drm/card*/device` + `hwmon` + `mem_info_gtt_*` + `pci.ids` |
| **Intel GPUs** | Intel Arc Discrete & Iris/UHD (`xe`, `i915`) | `/sys/class/drm/card*` + `hwmon` + `intel-rapl:0:1` + `pci.ids` |
| **Intel CPUs** | Intel Core (All Generations) | `/proc/cpuinfo`, `/proc/stat`, `cpufreq`, `coretemp`, `intel-rapl` |
| **AMD CPUs** | AMD Ryzen / Threadripper / Custom APU | `/proc/cpuinfo`, `/proc/stat`, `cpufreq`, `k10temp`/`zenpower`, `amd-rapl` |
| **System Memory** | DDR3, DDR4, DDR5, LPDDR4, LPDDR5 | `/proc/meminfo`, `/proc/self/status`, `/proc/pressure/memory`, SMBIOS 17 |

### All 23 Available Sensors

| # | Sensor Name | Group | What It Shows | Example |
| :--- | :--- | :--- | :--- | :--- |
| `01` | **FPS Counter** | `FPS` | Current smooth frames per second | `144` |
| `02` | **FPS Average** | `FPS` | Average FPS over the last 3.5 seconds | `AVG: 138` |
| `03` | **FPS 1% Low** | `FPS` | Time-weighted 1% slowest frames (stutter indicator) | `1%: 112` |
| `04` | **FPS 0.1% Low** | `FPS` | Time-weighted 0.1% worst micro-stutter frames | `0.1%: 95` |
| `05` | **Frame Time** | `FPS` | Time taken to render a frame in milliseconds | `6.9 ms` |
| `06` | **GPU Load** | `GPU` | GPU core usage percentage | `98%` |
| `07` | **GPU Clock** | `GPU` | Current GPU graphics core clock speed | `2430 MHz` |
| `08` | **GPU VRAM** | `GPU` | Used video memory in GB (includes shared GTT on APUs) | `6.4 GB` |
| `09` | **GPU Bandwidth** | `GPU` | Active VRAM + PCIe memory bandwidth throughput | `165.4 GB/s` |
| `10` | **GPU Temp** | `GPU` | GPU temperature in Celsius | `65 °C` |
| `11` | **GPU Fan** | `GPU` | GPU cooling fan speed percentage | `FAN: 55%` |
| `12` | **GPU Power** | `GPU` | GPU power draw in Watts | `185 W` |
| `13` | **CPU Load** | `CPU` | Total CPU utilization across all threads | `42%` |
| `14` | **CPU Clock** | `CPU` | Highest active core clock speed | `5050 MHz` |
| `15` | **CPU Power** | `CPU` | CPU package power draw in Watts | `58 W` |
| `16` | **CPU Temp** | `CPU` | CPU package temperature in Celsius | `62 °C` |
| `17` | **System RAM** | `RAM` | Used system memory in GB and percentage with warning alerts | `9.2 GB 60%` |
| `18` | **Game RAM** | `RAM` | Physical resident memory (VmRSS) allocated by this game | `GAME: 4.8 GB` |
| `19` | **Swap / ZRAM** | `RAM` | Active swap / ZRAM consumption in GB | `SWAP: 1.2 GB` |
| `20` | **Memory Pressure** | `RAM` | Linux PSI metric (last 10s memory stall percentage) | `PSI: 2.1%` |
| `21` | **RAM Speed** | `RAM` | Effective memory transfer speed and generation | `DDR5: 6000 MT/s` |
| `22` | **Render API** | `SYS` | Active graphics engine (`VULKAN` or `OPENGL`) | `VULKAN` |
| `23` | **Local Clock** | `SYS` | Current local system time (`HH:MM`) | `21:45` |

---

## 7. Tech Stack & System Architecture

| Layer | Technology | Highlights |
| :--- | :--- | :--- |
| **Language & Build** | **C++20**, **Clang/LLVM 23 + LLD**, **ThinLTO (`-O3`)** | Extreme optimization with ThinLTO, `-fno-plt`, stack canaries, ASan/UBSan diagnostics & static libstdc++. |
| **Graphics Hooks** | **Vulkan Layer (`1.3`)** + **GLX/EGL Hook** | Hooks `vkQueuePresentKHR`, `glXSwapBuffers`, and `eglSwapBuffers`. |
| **UI Framework** | **Dear ImGui (`v1.91.6`)** | Custom vertex-shaded outline/drop-shadow text & 3-Column Split View dashboard. |
| **Input Capture** | **Linux `evdev` + `X11` Fallback** | Non-blocking `/dev/input/event*` polling with `EVIOCGRAB` cursor lock. |
| **Telemetry Engine** | **Async 500ms Worker Thread** | Zero heap allocations per frame; lock-free atomic snapshot reads. |
| **Screen Recorder** | **POSIX SHM IPC + FFmpeg (libavcodec)** | Triple-buffered hardware-accelerated HEVC/H.264 capture & PulseAudio/PipeWire audio. |

```mermaid
flowchart TD
    Env["Launch Option: FPS_HUD=1"] --> Init["Config Manager (~/.config/fpshud/config.ini)"]
    subgraph GameProcess["Game Process (Vulkan / Proton / DXVK / OpenGL)"]
        VK["Vulkan Loader (VkLayer_FPSHUD_overlay)"] --> HUD["3-Column Split View HUD & In-Game GUI"]
        GL["OpenGL Wrapper (glXSwapBuffers / eglSwapBuffers)"] --> HUD
        Input["Linux evdev + X11 Input Manager"] --> HUD
        HUD <--> Init
        subgraph Worker["Background Telemetry Thread (Every 500ms)"]
            NV["NVIDIA: libnvidia-ml.so.1"]
            DRM["AMD & Intel: /sys/class/drm + hwmon + pci.ids"]
            CPU["CPU & RAM: /proc/stat + cpufreq + RAPL + hwmon"]
        end
        Worker -->|Lock-Free Snapshot| HUD
        HUD -->|Triple-Buffered POSIX SHM| RecDaemon["Screen Recorder Daemon (fpshud_recorder)"]
    end
```

---

## 8. Developer Guide (Building from Source, Sanitizers & Testing)

If you are a developer or want to compile **FPSHUD** from source instead of using the prebuilt binary:

### 1. Install Build Dependencies
- **Arch Linux**: `sudo pacman -S --needed base-devel cmake ninja git pkgconf clang lld llvm vulkan-headers vulkan-icd-loader mesa libglvnd libx11 wayland hwdata ffmpeg libpulse`
- **Ubuntu / Debian**: `sudo apt install -y build-essential cmake ninja-build git pkg-config clang lld llvm libvulkan-dev libgl1-mesa-dev libegl1-mesa-dev libx11-dev pci.ids libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libpulse-dev`
- **Fedora**: `sudo dnf install -y gcc-c++ clang lld llvm cmake ninja-build git pkgconf-pkg-config vulkan-headers vulkan-loader-devel mesa-libGL-devel mesa-libEGL-devel libX11-devel hwdata ffmpeg-free-devel pulseaudio-libs-devel`

### 2. Clone, Compile & Run Multi-Device Verification Suite
```bash
git clone https://github.com/budicuy/FPS_HUD_FOR_LINUX.git
cd FPS_HUD_FOR_LINUX

# 1. High-Performance Build with Clang 23 + LLD + ThinLTO (-O3 Extreme)
./scripts/build_clang.sh

# 2. Run the automated 37-check verification suite across 5 simulated device profiles + memory safety:
./build/fpshud_test 1

# 3. (Optional) Run AddressSanitizer + LeakSanitizer + UBSan diagnostic bug detection:
./scripts/run_sanitizers.sh

# 4. (Optional) Run automated Clang-Tidy static code analysis:
ninja -C build clang-tidy
```

### Project Structure
```text
FPS_HUD_FOR_LINUX/
├── .github/workflows/ci.yml             # Automated CI build, 5-device test & GitHub Release publisher
├── .clang-tidy                          # Automated static code analysis rules for C++ memory safety
├── CMakeLists.txt                       # C++20 build config with ThinLTO, LLD, ASan/UBSan & static libstdc++
├── dist/fpshud-linux-x86_64.tar.gz      # Prebuilt portable binary package for 1-command curl install
├── install.sh / uninstall.sh            # 1-command curl installer & uninstaller
├── README.md / README.id.md             # English & Indonesian documentation
├── scripts/
│   ├── build_clang.sh                   # Clang 23 + LLD + ThinLTO build script
│   ├── run_sanitizers.sh                # AddressSanitizer, LeakSanitizer & UBSan test runner
│   ├── install.sh / uninstall.sh        # Local build installer & uninstaller
│   └── gen_embedded_fonts.py            # Converts TTF fonts into C++ byte arrays
└── src/
    ├── config.hpp / config.cpp          # Preset manager & INI configuration persistence
    ├── embedded_fonts.hpp               # Embedded OpenSans-Bold & VeraMono-Bold TTF byte arrays
    ├── telemetry.hpp / telemetry.cpp    # Multi-vendor GPU/CPU telemetry & CSV benchmark recorder
    ├── input_manager.hpp / .cpp         # Linux evdev + X11 input polling & EVIOCGRAB mouse lock
    ├── gui_menu.hpp / gui_menu.cpp      # 3-Column Split View Dear ImGui in-game configurator
    ├── overlay_ui.hpp / overlay_ui.cpp  # Two-tone HUD renderer, outline/shadow & mini FT graph
    ├── vulkan_layer.cpp                 # Vulkan Implicit Layer dispatch & vkQueuePresentKHR hook
    ├── opengl_hook.cpp                  # OpenGL GLX/EGL buffer swap hooks
    ├── recorder_ipc.hpp / .cpp          # Zero-overhead IPC controller for screen recorder
    ├── recorder_main.cpp                # Companion screen recorder daemon (HEVC/H.264 & PulseAudio capture)
    ├── recorder_launcher.c              # Lightweight container-aware companion launcher
    └── test_cli.cpp                     # Automated 5-Device Sysfs Simulation & Memory Safety Suite
```

---

## 9. Troubleshooting & FAQ

1. **Pressing `Insert` / `F12` does not open the GUI or mouse clicks pass through to the game:**
   - Run `sudo chmod a+r /dev/input/event*` (or permanently add your user to the `input` group with `sudo usermod -aG input $USER` and log out/in).
2. **CPU Power shows `0 W`:**
   - Run `sudo chmod a+r /sys/class/powercap/*rapl*/energy_uj` (see [Section 2](#2-installation--sensor-permissions) to make it permanent across reboots).
3. **How do I reset all settings back to default?**
   - Open the In-Game GUI (`Insert` / `F12`) and click **`Reset`** in the top-right corner, or delete `~/.config/fpshud/config.ini`.

---

## 10. Contributing, Credits & License

### Contributing
Pull requests, issue reports, and hardware test feedback are always welcome! Before opening a PR, please run `./build/fpshud_test 1` to ensure all 37 multi-vendor checks and memory safety tests pass.

### Credits & Acknowledgments
- **[Dear ImGui](https://github.com/ocornut/imgui)** by Omar Cornut — Immediate Mode GUI library powering the overlay and live configurator.
- **Khronos Group** — Vulkan & OpenGL headers and implicit layer architecture.
- **Linux Kernel Community** — `drm`, `amdgpu`, `i915`/`xe`, `hwmon`, and `powercap` driver maintainers.
- **Open Sans & Bitstream Vera** — Open-source TrueType fonts embedded for crisp in-game typography.

### License
Distributed under the **[MIT License](LICENSE)**.
