#!/usr/bin/env bash
set -e

REPO_OWNER="budicuy"
REPO_NAME="FPS_HUD_FOR_LINUX"
RELEASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/latest/download/fpshud-linux-x86_64.tar.gz"
RAW_DIST_URL="https://raw.githubusercontent.com/${REPO_OWNER}/${REPO_NAME}/main/dist/fpshud-linux-x86_64.tar.gz"

LIB_DIR="$HOME/.local/lib/fpshud"
BIN_DIR="$HOME/.local/bin"
VULKAN_LAYER_DIR="$HOME/.local/share/vulkan/implicit_layer.d"
FONT_DIR="$HOME/.local/share/fpshud/fonts"

echo "=================================================================="
echo "  FPSHUD — 1-Command Installer for Linux (Vulkan & OpenGL)"
echo "=================================================================="

mkdir -p "$LIB_DIR" "$BIN_DIR" "$VULKAN_LAYER_DIR" "$FONT_DIR"

TMP_DIR="$(mktemp -d /tmp/fpshud-install.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

INSTALLED_BIN=0
INSTALLED_FROM_SOURCE=0

select_cpp_toolchain() {
    CXX_BIN=""
    CC_BIN=""
    if command -v g++-16 >/dev/null 2>&1; then
        CXX_BIN="g++-16"; CC_BIN="gcc-16"
    elif command -v g++-15 >/dev/null 2>&1; then
        CXX_BIN="g++-15"; CC_BIN="gcc-15"
    elif command -v clang++ >/dev/null 2>&1; then
        CXX_BIN="clang++"; CC_BIN="clang"
    elif command -v g++ >/dev/null 2>&1; then
        CXX_BIN="g++"; CC_BIN="gcc"
    fi
}

build_and_install_from_source() {
    select_cpp_toolchain
    if ! command -v cmake >/dev/null 2>&1 || [ -z "$CXX_BIN" ]; then
        echo "[!] Cannot build from source: cmake or C++26 compiler (g++-15+ / clang++) not installed."
        return 1
    fi
    local src_dir="$TMP_DIR/src"
    if [ -f "./CMakeLists.txt" ] && [ -d "./src" ]; then
        src_dir="."
    elif [ ! -d "$src_dir" ]; then
        git clone --depth 1 "https://github.com/${REPO_OWNER}/${REPO_NAME}.git" "$src_dir"
    fi
    local cmake_args=(-B "$TMP_DIR/build" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=$CXX_BIN")
    if [ -n "$CC_BIN" ] && command -v "$CC_BIN" >/dev/null 2>&1; then
        cmake_args+=("-DCMAKE_C_COMPILER=$CC_BIN")
    fi
    if command -v ninja >/dev/null 2>&1; then
        cmake_args+=(-G Ninja)
    fi
    cmake "${cmake_args[@]}" "$src_dir"
    cmake --build "$TMP_DIR/build" --parallel
    cp -f "$TMP_DIR/build/libfpshud.so" "$LIB_DIR/libfpshud.so"
    [ -f "$TMP_DIR/build/fpshud_test" ] && cp -f "$TMP_DIR/build/fpshud_test" "$BIN_DIR/fpshud_test"
    if [ -f "$TMP_DIR/build/fpshud_recorder" ]; then
        cp -f "$TMP_DIR/build/fpshud_recorder" "$BIN_DIR/fpshud_recorder"
        cp -f "$TMP_DIR/build/fpshud_recorder" "$LIB_DIR/fpshud_recorder"
    fi
    if [ -f "$TMP_DIR/build/fpshud_recorder_bin" ]; then
        cp -f "$TMP_DIR/build/fpshud_recorder_bin" "$BIN_DIR/fpshud_recorder_bin"
        cp -f "$TMP_DIR/build/fpshud_recorder_bin" "$LIB_DIR/fpshud_recorder_bin"
    fi
    INSTALLED_BIN=1
    INSTALLED_FROM_SOURCE=1
}

# 1. If executed from a local checkout with build/libfpshud.so or dist/ tarball
if [ -f "./build/libfpshud.so" ]; then
    echo "[*] Installing from local build directory..."
    cp -f "./build/libfpshud.so" "$LIB_DIR/libfpshud.so"
    [ -f "./build/fpshud_test" ] && cp -f "./build/fpshud_test" "$BIN_DIR/fpshud_test"
    if [ -f "./build/fpshud_recorder" ]; then
        cp -f "./build/fpshud_recorder" "$BIN_DIR/fpshud_recorder"
        cp -f "./build/fpshud_recorder" "$LIB_DIR/fpshud_recorder"
    fi
    if [ -f "./build/fpshud_recorder_bin" ]; then
        cp -f "./build/fpshud_recorder_bin" "$BIN_DIR/fpshud_recorder_bin"
        cp -f "./build/fpshud_recorder_bin" "$LIB_DIR/fpshud_recorder_bin"
    fi
    INSTALLED_BIN=1
elif [ -f "./dist/fpshud-linux-x86_64.tar.gz" ]; then
    echo "[*] Extracting from local prebuilt package..."
    tar -xzf "./dist/fpshud-linux-x86_64.tar.gz" -C "$TMP_DIR"
    cp -f "$TMP_DIR/libfpshud.so" "$LIB_DIR/libfpshud.so"
    [ -f "$TMP_DIR/fpshud_test" ] && cp -f "$TMP_DIR/fpshud_test" "$BIN_DIR/fpshud_test"
    if [ -f "$TMP_DIR/fpshud_recorder" ]; then
        cp -f "$TMP_DIR/fpshud_recorder" "$BIN_DIR/fpshud_recorder"
        cp -f "$TMP_DIR/fpshud_recorder" "$LIB_DIR/fpshud_recorder"
    fi
    if [ -f "$TMP_DIR/fpshud_recorder_bin" ]; then
        cp -f "$TMP_DIR/fpshud_recorder_bin" "$BIN_DIR/fpshud_recorder_bin"
        cp -f "$TMP_DIR/fpshud_recorder_bin" "$LIB_DIR/fpshud_recorder_bin"
    fi
    INSTALLED_BIN=1
fi

# 2. Download prebuilt portable binary via curl (Zero build packages required!)
if [ "$INSTALLED_BIN" -eq 0 ]; then
    echo "[*] Downloading prebuilt portable FPSHUD binary (no compilation needed)..."
    if curl -fsSL "$RELEASE_URL" -o "$TMP_DIR/fpshud.tar.gz" 2>/dev/null || \
       curl -fsSL "$RAW_DIST_URL" -o "$TMP_DIR/fpshud.tar.gz"; then
        tar -xzf "$TMP_DIR/fpshud.tar.gz" -C "$TMP_DIR"
        cp -f "$TMP_DIR/libfpshud.so" "$LIB_DIR/libfpshud.so"
        if [ -f "$TMP_DIR/fpshud_test" ]; then
            cp -f "$TMP_DIR/fpshud_test" "$BIN_DIR/fpshud_test"
            chmod +x "$BIN_DIR/fpshud_test"
        fi
        if [ -f "$TMP_DIR/fpshud_recorder" ]; then
            cp -f "$TMP_DIR/fpshud_recorder" "$BIN_DIR/fpshud_recorder"
            cp -f "$TMP_DIR/fpshud_recorder" "$LIB_DIR/fpshud_recorder"
            chmod +x "$BIN_DIR/fpshud_recorder" "$LIB_DIR/fpshud_recorder"
        fi
        if [ -f "$TMP_DIR/fpshud_recorder_bin" ]; then
            cp -f "$TMP_DIR/fpshud_recorder_bin" "$BIN_DIR/fpshud_recorder_bin"
            cp -f "$TMP_DIR/fpshud_recorder_bin" "$LIB_DIR/fpshud_recorder_bin"
            chmod +x "$BIN_DIR/fpshud_recorder_bin" "$LIB_DIR/fpshud_recorder_bin"
        fi
        INSTALLED_BIN=1
    else
        echo "[!] Failed to download prebuilt binary. Falling back to source build..."
        build_and_install_from_source
    fi
fi

chmod +x "$LIB_DIR/libfpshud.so"
[ -f "$BIN_DIR/fpshud_test" ] && chmod +x "$BIN_DIR/fpshud_test"
[ -f "$BIN_DIR/fpshud_recorder" ] && chmod +x "$BIN_DIR/fpshud_recorder"
[ -f "$LIB_DIR/fpshud_recorder" ] && chmod +x "$LIB_DIR/fpshud_recorder"
[ -f "$BIN_DIR/fpshud_recorder_bin" ] && chmod +x "$BIN_DIR/fpshud_recorder_bin"
[ -f "$LIB_DIR/fpshud_recorder_bin" ] && chmod +x "$LIB_DIR/fpshud_recorder_bin"

# Verify prebuilt libfpshud.so & fpshud_test compatibility (GLIBC & pre-AVX2 CPU safety)
if [ "$INSTALLED_FROM_SOURCE" -eq 0 ]; then
    NEED_REBUILD=0
    if ldd "$LIB_DIR/libfpshud.so" 2>&1 | grep -q "not found"; then
        echo "[*] Notice: Prebuilt libfpshud.so requires newer system libraries than installed on this distro."
        NEED_REBUILD=1
    elif ! grep -qw "avx2" /proc/cpuinfo 2>/dev/null && [ -x "$BIN_DIR/fpshud_test" ]; then
        TEST_RC=0
        "$BIN_DIR/fpshud_test" >/dev/null 2>&1 || TEST_RC=$?
        if [ "$TEST_RC" -eq 132 ] || [ "$TEST_RC" -eq 127 ]; then
            echo "[*] Notice: Prebuilt binary uses CPU instructions unsupported by this processor (exit $TEST_RC)."
            NEED_REBUILD=1
        fi
    fi
    if [ "$NEED_REBUILD" -eq 1 ]; then
        echo "    Rebuilding native FPSHUD binaries from source for this host..."
        build_and_install_from_source || echo "[!] Warning: Native source rebuild skipped (install cmake & g++-15/clang++ to build locally)."
        chmod +x "$LIB_DIR/libfpshud.so"
        [ -f "$BIN_DIR/fpshud_test" ] && chmod +x "$BIN_DIR/fpshud_test"
    fi
fi

# Verify if prebuilt fpshud_recorder works on this distro
REC_CHECK_TARGET="$BIN_DIR/fpshud_recorder_bin"
[ ! -f "$REC_CHECK_TARGET" ] && REC_CHECK_TARGET="$BIN_DIR/fpshud_recorder"
if [ -f "$REC_CHECK_TARGET" ] && ldd "$REC_CHECK_TARGET" 2>&1 | grep -q "not found"; then
    echo "[*] Notice: Host FFmpeg library version differs from prebuilt binary."
    echo "    Compiling native fpshud_recorder for this system..."
    select_cpp_toolchain
    if [ -n "$CXX_BIN" ] && command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libavformat libavcodec libavutil libswscale libswresample libpulse-simple 2>/dev/null; then
        REC_TMP="$(mktemp -d /tmp/fpshud-rec-build.XXXXXX)"
        REC_SRC_DIR="$REC_TMP/src"
        if [ -f "./src/recorder_main.cpp" ]; then
            REC_SRC_DIR="."
        elif command -v git >/dev/null 2>&1; then
            git clone --depth 1 "https://github.com/${REPO_OWNER}/${REPO_NAME}.git" "$REC_SRC_DIR" 2>/dev/null || true
        fi
        if [ -f "$REC_SRC_DIR/src/recorder_main.cpp" ]; then
            REC_STD_FLAG="-std=c++20"
            if "$CXX_BIN" -std=c++26 -x c++ -E /dev/null >/dev/null 2>&1; then
                REC_STD_FLAG="-std=c++26"
            fi
            if "$CXX_BIN" -O3 "$REC_STD_FLAG" "$REC_SRC_DIR/src/recorder_main.cpp" \
                -I"$REC_SRC_DIR/src" \
                $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale libswresample libpulse-simple) \
                -pthread -ldl \
                -Wl,-rpath,'$ORIGIN:$ORIGIN/../lib:/run/host/usr/lib:/run/host/usr/lib/pulseaudio:/usr/lib' \
                -Wl,--disable-new-dtags \
                -o "$LIB_DIR/fpshud_recorder_bin" 2>"$REC_TMP/err.log"; then
                chmod +x "$LIB_DIR/fpshud_recorder_bin"
                cp -f "$LIB_DIR/fpshud_recorder_bin" "$BIN_DIR/fpshud_recorder_bin"
                if [ -n "$CC_BIN" ] && command -v "$CC_BIN" >/dev/null 2>&1 && [ -f "$REC_SRC_DIR/src/recorder_launcher.c" ]; then
                    "$CC_BIN" -O3 "$REC_SRC_DIR/src/recorder_launcher.c" -ldl -o "$BIN_DIR/fpshud_recorder" 2>/dev/null && \
                        chmod +x "$BIN_DIR/fpshud_recorder" && \
                        cp -f "$BIN_DIR/fpshud_recorder" "$LIB_DIR/fpshud_recorder" || true
                fi
                if ! ldd "$REC_CHECK_TARGET" 2>&1 | grep -q "not found"; then
                    echo "[✓] Native fpshud_recorder compiled successfully ($CXX_BIN $REC_STD_FLAG)!"
                fi
            else
                echo "[!] Warning: Failed to compile native fpshud_recorder_bin ($(head -n 1 "$REC_TMP/err.log" 2>/dev/null))."
            fi
        fi
        rm -rf "$REC_TMP"
    else
        echo "[!] Warning: Cannot recompile native fpshud_recorder_bin (missing C++ compiler or FFmpeg/PulseAudio dev headers)."
    fi
fi

# 3. Copy optional system fonts if available on the host distro
for f in \
    /usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf \
    /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf \
    /usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-Bold.ttf \
    /usr/share/fonts/TTF/OpenSans-Bold.ttf \
    /usr/share/fonts/truetype/open-sans/OpenSans-Bold.ttf \
    /usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf \
    /usr/share/fonts/TTF/VeraMoBd.ttf \
    /usr/share/fonts/truetype/ttf-bitstream-vera/VeraMoBd.ttf; do
    if [ -f "$f" ]; then
        cp -f "$f" "$FONT_DIR/" 2>/dev/null || true
    fi
done

# 4. Generate Vulkan Implicit Layer JSON manifest pointing to ~/.local/lib/fpshud/libfpshud.so
cat > "$VULKAN_LAYER_DIR/VkLayer_FPSHUD_overlay.json" <<EOF
{
    "file_format_version": "1.2.1",
    "layer": {
        "name": "VK_LAYER_FPSHUD_overlay",
        "type": "GLOBAL",
        "library_path": "$LIB_DIR/libfpshud.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "FPSHUD Minimalist Performance Overlay Layer",
        "functions": {
            "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion",
            "vkGetInstanceProcAddr": "vkGetInstanceProcAddr",
            "vkGetDeviceProcAddr": "vkGetDeviceProcAddr"
        },
        "disable_environment": {
            "DISABLE_FPS_HUD": "1"
        }
    }
}
EOF

# 5. Generate OpenGL Wrapper Script (~/.local/bin/fpshud)
cat > "$BIN_DIR/fpshud" << EOF
#!/usr/bin/env bash
# ====================================================================
# FPSHUD — Linux Gaming Performance Overlay & Management CLI
# ====================================================================

LIB_SO="$LIB_DIR/libfpshud.so"
RAM_CACHE_USER="\${XDG_DATA_HOME:-\$HOME/.local/share}/fpshud/ram_info"
RAM_CACHE_SYS="/etc/fpshud/ram_info"

show_help() {
    cat <<'HELP_EOF'
====================================================================
  FPSHUD — Linux Gaming Performance Overlay & Management CLI
====================================================================
Penggunaan / Usage:
  fpshud <perintah/game> [args...] Launch OpenGL / Vulkan game with overlay
  fpshud --status, -c              Periksa status izin sensor hardware
  fpshud --setup, -s               Konfigurasi izin sensor hardware (Sudo)
  fpshud --help, -h                Tampilkan bantuan ini

Contoh / Examples:
  fpshud glxgears
  fpshud %command%                 (Steam Launch Options untuk game OpenGL)
  fpshud --status                  (Periksa apakah CPU Watt & RAM terbaca)
  fpshud --setup                   (Beri izin sensor kapan saja tanpa instal ulang)
====================================================================
HELP_EOF
}

check_status() {
    echo "===================================================================="
    echo "  FPSHUD — Status Izin & Akses Sensor Perangkat Keras"
    echo "===================================================================="

    local rapl_ok=0
    local input_ok=0
    local ram_ok=0

    # 1. RAPL CPU Wattage
    for r in /sys/class/powercap/*rapl*/*/energy_uj /sys/class/powercap/*rapl*/energy_uj; do
        if [ -r "\$r" ] 2>/dev/null; then
            rapl_ok=1
            break
        fi
    done

    # 2. Wayland Input / Evdev
    if groups 2>/dev/null | grep -qw "input" || [ -r /dev/input/event0 ] 2>/dev/null; then
        input_ok=1
    fi

    # 3. RAM Cache
    if [ -f "\$RAM_CACHE_USER" ] || [ -f "\$RAM_CACHE_SYS" ]; then
        ram_ok=1
    fi

    echo -n "  [+] CPU Package Power (RAPL / Watt)   : "
    if [ "\$rapl_ok" -eq 1 ]; then
        echo -e "\033[32mAKTIF (Dapat dibaca)\033[0m"
    else
        echo -e "\033[33mTERBATAS (Perlu izin udev / sudo)\033[0m"
    fi

    echo -n "  [+] RAM Speed (MT/s) & DDR Type       : "
    if [ "\$ram_ok" -eq 1 ]; then
        echo -e "\033[32mAKTIF (Cache tersedia)\033[0m"
    else
        echo -e "\033[33mTERBATAS (Cache belum dibuat)\033[0m"
    fi

    echo -n "  [+] Wayland Hotkeys & Mouse Capture   : "
    if [ "\$input_ok" -eq 1 ]; then
        echo -e "\033[32mAKTIF (Grup input terdeteksi)\033[0m"
    else
        echo -e "\033[33mTERBATAS (User belum di grup input)\033[0m"
    fi

    echo "--------------------------------------------------------------------"
    if [ "\$rapl_ok" -eq 1 ] && [ "\$ram_ok" -eq 1 ] && [ "\$input_ok" -eq 1 ]; then
        echo -e "\033[32mSemua izin sensor hardware lengkap dan siap digunakan!\033[0m"
    else
        echo -e "\033[33mSebagian sensor belum aktif. Untuk mengaktifkannya, jalankan:\033[0m"
        echo "    fpshud --setup"
    fi
    echo "===================================================================="
}

run_setup() {
    echo "===================================================================="
    echo "  FPSHUD — Konfigurasi Izin Sensor Hardware (CPU, RAM & Wayland)"
    echo "===================================================================="
    echo "Mengonfigurasi aturan udev standar Linux & cache SMBIOS agar sensor"
    echo "dapat dibaca oleh user biasa tanpa hak root saat bermain game."
    echo ""

    local SUDO_CMD="sudo"
    if [ "\$EUID" -eq 0 ]; then
        SUDO_CMD=""
    else
        if ! command -v sudo >/dev/null 2>&1; then
            echo "[!] Perintah 'sudo' tidak ditemukan pada sistem ini."
            exit 1
        fi
        echo "[*] Meminta izin administrator (sudo)..."
        if ! sudo -v; then
            echo "[!] Gagal mendapatkan izin sudo. Konfigurasi dibatalkan."
            exit 1
        fi
    fi

    # 1. RAPL CPU Wattage udev rule
    echo "[*] Memasang udev rule untuk CPU Wattage (/etc/udev/rules.d/99-fpshud-rapl.rules)..."
    \$SUDO_CMD bash -c 'cat > /etc/udev/rules.d/99-fpshud-rapl.rules << "UDEV_EOF"
SUBSYSTEM=="powercap", ACTION=="add|change", KERNEL=="*rapl*", ATTR{energy_uj}!="", MODE="0444"
SUBSYSTEM=="powercap", ACTION=="add|change", KERNEL=="*rapl:*", ATTR{energy_uj}!="", MODE="0444"
UDEV_EOF'
    \$SUDO_CMD chmod 644 /etc/udev/rules.d/99-fpshud-rapl.rules 2>/dev/null || true

    # 2. Wayland Input udev rule & Group
    echo "[*] Memasang udev rule untuk Wayland Hotkeys (/etc/udev/rules.d/99-fpshud-input.rules)..."
    \$SUDO_CMD bash -c 'cat > /etc/udev/rules.d/99-fpshud-input.rules << "UDEV_EOF"
KERNEL=="event*", SUBSYSTEM=="input", MODE="0660", GROUP="input"
UDEV_EOF'
    \$SUDO_CMD chmod 644 /etc/udev/rules.d/99-fpshud-input.rules 2>/dev/null || true

    # Add user to input group
    TARGET_USER="\${SUDO_USER:-\$USER}"
    if [ -n "\$TARGET_USER" ] && id -u "\$TARGET_USER" >/dev/null 2>&1; then
        echo "[*] Menambahkan pengguna '\$TARGET_USER' ke grup input..."
        \$SUDO_CMD usermod -a -G input "\$TARGET_USER" 2>/dev/null || true
    fi

    # 3. Generate RAM Info Cache
    echo "[*] Mendeteksi informasi RAM (Kecepatan MT/s & Tipe DDR via SMBIOS)..."
    TARGET_HOME="\${HOME}"
    if [ -n "\$SUDO_USER" ]; then
        USER_HOME="\$(getent passwd "\$SUDO_USER" 2>/dev/null | cut -d: -f6)"
        [ -n "\$USER_HOME" ] && TARGET_HOME="\$USER_HOME"
    fi
    TARGET_RAM_FILE="\${XDG_DATA_HOME:-\$TARGET_HOME/.local/share}/fpshud/ram_info"
    mkdir -p "\$(dirname "\$TARGET_RAM_FILE")"

    if command -v python3 >/dev/null 2>&1; then
        \$SUDO_CMD python3 -c '
import glob, struct, os, re, subprocess

types = {
    0x12: "DDR", 0x13: "DDR2", 0x18: "DDR3", 0x1A: "DDR4",
    0x1B: "LPDDR", 0x1C: "LPDDR2", 0x1D: "LPDDR3", 0x1E: "LPDDR4",
    0x22: "DDR5", 0x23: "LPDDR5"
}

speed = 0
ram_type = "RAM"
modules = 0

for raw_path in glob.glob("/sys/firmware/dmi/entries/17-*/raw"):
    try:
        with open(raw_path, "rb") as f:
            b = f.read()
        if len(b) < 0x17 or b[0] != 17:
            continue
        length = min(b[1], len(b))
        size = struct.unpack_from("<H", b, 0x0C)[0]
        if size == 0 or size == 0xFFFF:
            continue
        modules += 1
        if length > 0x12 and ram_type == "RAM":
            t = types.get(b[0x12])
            if t: ram_type = t
        rated = struct.unpack_from("<H", b, 0x15)[0] if length > 0x16 else 0
        conf = struct.unpack_from("<H", b, 0x20)[0] if length > 0x21 else 0
        if rated == 0xFFFF and length >= 0x58:
            rated = struct.unpack_from("<I", b, 0x54)[0]
        if conf == 0xFFFF and length >= 0x5C:
            conf = struct.unpack_from("<I", b, 0x58)[0]
        eff = conf if conf > 0 else rated
        if eff > speed:
            speed = eff
    except Exception:
        pass

if speed == 0:
    for cmd in ["/usr/sbin/dmidecode", "/usr/bin/dmidecode", "dmidecode"]:
        try:
            out = subprocess.check_output([cmd, "-t", "memory"], text=True, stderr=subprocess.DEVNULL)
            for block in out.split("Memory Device"):
                m_size = re.search(r"Size:\s+(\d+)\s+([MG]B)", block)
                if not m_size or "No Module Installed" in block:
                    continue
                modules += 1
                m_type = re.search(r"Type:\s+([A-Za-z0-9\-]+)", block)
                if m_type and ram_type == "RAM":
                    ram_type = m_type.group(1).replace("-", "")
                m_conf = re.search(r"Configured Memory Speed:\s+(\d+)\s+MT/s", block) or \
                         re.search(r"Configured Clock Speed:\s+(\d+)\s+MHz", block)
                m_speed = re.search(r"Speed:\s+(\d+)\s+(?:MT/s|MHz)", block)
                sp = int(m_conf.group(1)) if m_conf else (int(m_speed.group(1)) if m_speed else 0)
                if sp > speed:
                    speed = sp
            break
        except Exception:
            pass

target_file = "'"\$TARGET_RAM_FILE"'"
if speed > 0:
    with open(target_file, "w") as f:
        f.write(f"type={ram_type}\nspeed_mts={speed}\nmodules={modules}\n")
    os.chmod(target_file, 0o644)
    print(f"    [+] Deteksi RAM Sukses: {ram_type} {speed} MT/s ({modules} modul)")
' 2>/dev/null || true
    else
        echo "    [!] python3 tidak ditemukan, pembacaan SMBIOS RAM dilewati."
    fi

    # Fix ownership if created under sudo
    if [ -n "\$TARGET_USER" ] && [ -f "\$TARGET_RAM_FILE" ]; then
        \$SUDO_CMD chown -R "\$TARGET_USER:" "\$(dirname "\$TARGET_RAM_FILE")" 2>/dev/null || true
    fi

    # System-wide copy
    \$SUDO_CMD mkdir -p /etc/fpshud 2>/dev/null || true
    if [ -f "\$TARGET_RAM_FILE" ]; then
        \$SUDO_CMD cp "\$TARGET_RAM_FILE" /etc/fpshud/ram_info 2>/dev/null || true
        \$SUDO_CMD chmod 644 /etc/fpshud/ram_info 2>/dev/null || true
    fi

    # 4. Reload udev rules
    echo "[*] Me-reload aturan udev sistem..."
    \$SUDO_CMD udevadm control --reload-rules 2>/dev/null || true
    \$SUDO_CMD udevadm trigger --subsystem-match=powercap 2>/dev/null || true
    \$SUDO_CMD udevadm trigger --subsystem-match=input 2>/dev/null || true

    echo ""
    echo "===================================================================="
    echo "  [✓] Konfigurasi Izin Sensor Berhasil Selesai!"
    echo "===================================================================="
    echo "Catatan: Jika Anda baru ditambahkan ke grup 'input', silakan relogin"
    echo "atau jalankan 'newgrp input' agar hotkey Wayland langsung aktif."
    echo "===================================================================="
}

case "\$1" in
    --help|-h|help)
        show_help
        exit 0
        ;;
    --status|-c|status|check)
        check_status
        exit 0
        ;;
    --setup|-s|setup)
        run_setup
        exit 0
        ;;
    *)
        if [ \$# -eq 0 ]; then
            show_help
            exit 1
        fi
        ;;
esac

# Launch game with LD_PRELOAD
if [ -z "\$FPS_HUD" ]; then
    export FPS_HUD=1
fi
export LD_PRELOAD="\$LIB_SO\${LD_PRELOAD:+:\$LD_PRELOAD}"
exec "\$@"
EOF
chmod +x "$BIN_DIR/fpshud"

# ====================================================================
# 6. Hardware Telemetry Permissions (RAM Speed, CPU Wattage & Wayland Hotkeys)
# ====================================================================
HAVE_SUDO=0
SUDO_CMD="sudo"

echo ""
echo "=================================================================="
echo "  Hardware Telemetry & Sensor Permissions Setup"
echo "=================================================================="
echo "FPSHUD works out-of-the-box for FPS, Frametimes, and GPU sensors."
echo "However, Linux kernel security policies restrict:"
echo "  • CPU Package Power (Watts via RAPL energy counters)"
echo "  • RAM Speed (MT/s) & Memory Generation (DDR4 / DDR5 via SMBIOS)"
echo "  • In-game GUI menu & hotkeys on Wayland compositors"
echo ""
echo "Sudo access is STRONGLY RECOMMENDED to enable these sensors"
echo "via lightweight, standard system udev rules."
echo "=================================================================="

if [ "$EUID" -eq 0 ]; then
    HAVE_SUDO=1
    SUDO_CMD=""
elif command -v sudo >/dev/null 2>&1 && [ -z "$FPSHUD_NO_SUDO" ] && [ -z "$CI" ]; then
    if sudo -n true 2>/dev/null; then
        HAVE_SUDO=1
        SUDO_CMD="sudo"
    elif [ -e /dev/tty ]; then
        echo ""
        echo "[*] Requesting sudo permission to configure hardware sensors permanently..."
        if sudo </dev/tty -v; then
            HAVE_SUDO=1
            SUDO_CMD="sudo"
        else
            echo "[!] Sudo permission skipped or failed. Non-privileged sensors will continue to function."
        fi
    fi
fi

if [ "$HAVE_SUDO" -eq 0 ]; then
    echo ""
    echo "[!] Sudo permission skipped."
    echo "    Core overlay, FPS, and GPU sensors will function normally."
    echo "    To enable CPU Wattage, RAM Speed, and Wayland hotkeys later,"
    echo "    simply run at any time:"
    echo ""
    echo "        fpshud --setup"
    echo ""
fi

RAM_INFO_FILE="${XDG_DATA_HOME:-$HOME/.local/share}/fpshud/ram_info"
RAM_PY='
import glob, struct, os, re, subprocess

types = {
    0x12: "DDR", 0x13: "DDR2", 0x18: "DDR3", 0x1A: "DDR4",
    0x1B: "LPDDR", 0x1C: "LPDDR2", 0x1D: "LPDDR3", 0x1E: "LPDDR4",
    0x22: "DDR5", 0x23: "LPDDR5"
}

speed = 0
ram_type = "RAM"
modules = 0

for raw_path in glob.glob("/sys/firmware/dmi/entries/17-*/raw"):
    try:
        with open(raw_path, "rb") as f:
            b = f.read()
        if len(b) < 0x17 or b[0] != 17:
            continue
        length = min(b[1], len(b))
        size = struct.unpack_from("<H", b, 0x0C)[0]
        if size == 0 or size == 0xFFFF:
            continue
        modules += 1
        if length > 0x12 and ram_type == "RAM":
            t = types.get(b[0x12])
            if t: ram_type = t
        rated = struct.unpack_from("<H", b, 0x15)[0] if length > 0x16 else 0
        conf = struct.unpack_from("<H", b, 0x20)[0] if length > 0x21 else 0
        if rated == 0xFFFF and length >= 0x58:
            rated = struct.unpack_from("<I", b, 0x54)[0]
        if conf == 0xFFFF and length >= 0x5C:
            conf = struct.unpack_from("<I", b, 0x58)[0]
        eff = conf if conf > 0 else rated
        if eff > speed:
            speed = eff
    except Exception:
        pass

if speed == 0:
    for cmd in ["/usr/sbin/dmidecode", "/usr/bin/dmidecode", "dmidecode"]:
        try:
            out = subprocess.check_output([cmd, "-t", "memory"], text=True, stderr=subprocess.DEVNULL)
            for block in out.split("Memory Device"):
                m_size = re.search(r"Size:\s+(\d+)\s+([MG]B)", block)
                if not m_size or "No Module Installed" in block:
                    continue
                modules += 1
                m_type = re.search(r"Type:\s+([A-Za-z0-9\-]+)", block)
                if m_type and ram_type == "RAM":
                    ram_type = m_type.group(1).replace("-", "")
                m_conf = re.search(r"Configured Memory Speed:\s+(\d+)\s+MT/s", block) or \
                         re.search(r"Configured Clock Speed:\s+(\d+)\s+MHz", block)
                m_speed = re.search(r"Speed:\s+(\d+)\s+(?:MT/s|MHz)", block)
                sp = int(m_conf.group(1)) if m_conf else (int(m_speed.group(1)) if m_speed else 0)
                if sp > speed:
                    speed = sp
            break
        except Exception:
            pass

print(f"type={ram_type}")
print(f"speed_mts={speed}")
print(f"rated_mts={speed}")
print(f"modules={modules}")
'

if command -v python3 >/dev/null 2>&1; then
    TMP_RAM="$TMP_DIR/ram_info"
    if [ "$HAVE_SUDO" -eq 1 ]; then
        $SUDO_CMD python3 -c "$RAM_PY" > "$TMP_RAM" 2>/dev/null || true
    else
        python3 -c "$RAM_PY" > "$TMP_RAM" 2>/dev/null || true
    fi
    if [ -s "$TMP_RAM" ]; then
        cp -f "$TMP_RAM" "$RAM_INFO_FILE"
        chmod 644 "$RAM_INFO_FILE" 2>/dev/null || true
        if [ "$HAVE_SUDO" -eq 1 ]; then
            $SUDO_CMD mkdir -p /etc/fpshud 2>/dev/null || true
            $SUDO_CMD cp -f "$TMP_RAM" /etc/fpshud/ram_info 2>/dev/null || true
            $SUDO_CMD chmod 644 /etc/fpshud/ram_info 2>/dev/null || true
        fi
    fi
fi

if [ "$HAVE_SUDO" -eq 1 ]; then
    echo "[*] Configuring system udev rules for full sensor access..."
    # 1. RAPL CPU Wattage Rule
    $SUDO_CMD mkdir -p /etc/udev/rules.d 2>/dev/null || true
    $SUDO_CMD bash -c 'cat > /etc/udev/rules.d/99-fpshud-rapl.rules << "UDEV_EOF"
SUBSYSTEM=="powercap", ACTION=="add|change", KERNEL=="*rapl*", ATTR{energy_uj}!="", MODE="0444"
SUBSYSTEM=="powercap", ACTION=="add|change", KERNEL=="*rapl:*", ATTR{energy_uj}!="", MODE="0444"
UDEV_EOF'
    $SUDO_CMD chmod 644 /etc/udev/rules.d/99-fpshud-rapl.rules 2>/dev/null || true
    $SUDO_CMD chmod a+r /sys/class/powercap/*rapl*/energy_uj /sys/class/powercap/*rapl*/*/energy_uj 2>/dev/null || true

    # 2. Wayland Hotkeys & Mouse Input Rule
    $SUDO_CMD bash -c 'cat > /etc/udev/rules.d/99-fpshud-input.rules << "UDEV_EOF"
KERNEL=="event*", SUBSYSTEM=="input", MODE="0660", GROUP="input"
UDEV_EOF'
    $SUDO_CMD chmod 644 /etc/udev/rules.d/99-fpshud-input.rules 2>/dev/null || true
    TARGET_USER="${SUDO_USER:-$USER}"
    if [ -n "$TARGET_USER" ] && id -u "$TARGET_USER" >/dev/null 2>&1; then
        $SUDO_CMD usermod -a -G input "$TARGET_USER" 2>/dev/null || true
    fi

    # Reload udev rules
    $SUDO_CMD udevadm control --reload-rules 2>/dev/null || true
    $SUDO_CMD udevadm trigger --subsystem-match=powercap 2>/dev/null || true
    $SUDO_CMD udevadm trigger --subsystem-match=input 2>/dev/null || true
    echo "[✓] Full hardware permissions configured successfully!"
fi

echo ""
echo "=================================================================="
echo "  FPSHUD Successfully Installed!"
echo "=================================================================="
echo "  - Overlay Library       : $LIB_DIR/libfpshud.so"
echo "  - Vulkan Implicit Layer : $VULKAN_LAYER_DIR/VkLayer_FPSHUD_overlay.json"
echo "  - OpenGL Wrapper Script : $BIN_DIR/fpshud"
echo "  - Diagnostic CLI Tool   : $BIN_DIR/fpshud_test"
echo "  - Screen Recorder Helper: $BIN_DIR/fpshud_recorder"
echo ""
echo "  Quick Start:"
echo "  - Steam Launch Options  : FPS_HUD=1 %command%"
echo "  - Lutris / Heroic       : Add Environment Variable FPS_HUD=1"
echo "  - Terminal (Vulkan)     : FPS_HUD=1 ./your_game"
echo "  - Terminal (OpenGL)     : ~/.local/bin/fpshud ./your_game"
echo "  - Open In-Game GUI      : Press [Insert], [F12], or [F10] in-game"
echo "  - Screen Recording      : Press [F7] (Start/Stop Recording)"
echo ""
echo "  Hardware Sensors & Permissions:"
echo "  - Check permission state: ~/.local/bin/fpshud --status"
echo "  - Tip: If CPU Wattage or RAM Speed shows 0, run:"
echo "        ~/.local/bin/fpshud --setup"
echo "=================================================================="
