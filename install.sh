#!/usr/bin/env bash
set -e

REPO_OWNER="budicuy"
REPO_NAME="FPS_HUD_FOR_LINUX"
RELEASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/latest/download/fpshud-linux-x86_64.tar.gz"
RELEASE_SHA_URL="${RELEASE_URL}.sha256"
RAW_DIST_URL="https://raw.githubusercontent.com/${REPO_OWNER}/${REPO_NAME}/main/dist/fpshud-linux-x86_64.tar.gz"
RAW_DIST_SHA_URL="${RAW_DIST_URL}.sha256"

LIB_DIR="$HOME/.local/lib/fpshud"
BIN_DIR="$HOME/.local/bin"
VULKAN_LAYER_DIR="$HOME/.local/share/vulkan/implicit_layer.d"
FONT_DIR="$HOME/.local/share/fpshud/fonts"

echo "=================================================================="
echo "  FPSHUD — 1-Command Installer for Linux (Vulkan & OpenGL)"
echo "=================================================================="

mkdir -p "$LIB_DIR" "$BIN_DIR" "$VULKAN_LAYER_DIR" "$FONT_DIR"
rm -f "$BIN_DIR/fpshud_test"

TMP_DIR="$(mktemp -d /tmp/fpshud-install.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

INSTALLED_BIN=0
INSTALLED_FROM_SOURCE=0

# Parse CLI options & isolate working directory
ALLOW_LOCAL=0
FORCE_OPT=0
for arg in "$@"; do
    case "$arg" in
        --local|-l) ALLOW_LOCAL=1 ;;
        --yes|-y) FORCE_OPT=1 ;;
    esac
done

# If executed directly as a script file (not piped into bash) from inside the verified repo:
if [ "$ALLOW_LOCAL" -eq 0 ] && [ "$0" != "bash" ] && [ "$0" != "-bash" ] && [ "$0" != "sh" ]; then
    if [ -f "./CMakeLists.txt" ] && grep -q 'project(fpshud' "./CMakeLists.txt" 2>/dev/null; then
        ALLOW_LOCAL=1
    fi
fi

get_clang_version() {
    local bin="$1"
    "$bin" --version 2>/dev/null | grep -oE 'clang version [0-9]+' | awk '{print $3}'
}

get_gcc_version() {
    local bin="$1"
    "$bin" -dumpversion 2>/dev/null | cut -d. -f1
}

select_cpp_toolchain() {
    CXX_BIN=""
    CC_BIN=""
    # 1. Probe Clang candidate compilers (Clang 19+)
    for cand in clang++-23 clang++-22 clang++-21 clang++-20 clang++-19 clang++; do
        if command -v "$cand" >/dev/null 2>&1; then
            local ver
            ver="$(get_clang_version "$cand")"
            if [ -n "$ver" ] && [ "$ver" -ge 19 ] 2>/dev/null; then
                CXX_BIN="$cand"
                local cc_cand="${cand/clang++/clang}"
                command -v "$cc_cand" >/dev/null 2>&1 && CC_BIN="$cc_cand" || CC_BIN="clang"
                return 0
            fi
        fi
    done

    # 2. Probe GCC candidate compilers (GCC 15+)
    for cand in g++-16 g++-15 g++; do
        if command -v "$cand" >/dev/null 2>&1; then
            local ver
            ver="$(get_gcc_version "$cand")"
            if [ -n "$ver" ] && [ "$ver" -ge 15 ] 2>/dev/null; then
                CXX_BIN="$cand"
                local cc_cand="${cand/g++/gcc}"
                command -v "$cc_cand" >/dev/null 2>&1 && CC_BIN="$cc_cand" || CC_BIN="gcc"
                return 0
            fi
        fi
    done

    return 1
}

build_and_install_from_source() {
    if ! command -v cmake >/dev/null 2>&1 || ! select_cpp_toolchain; then
        echo "[!] Cannot build from source: CMake and a pure native C++26 compiler (Clang 19+ or GCC 15+) are required."
        return 1
    fi
    echo "[*] Compiling FPSHUD from source using $CXX_BIN..."
    local src_dir="$TMP_DIR/src"
    if [ "$ALLOW_LOCAL" -eq 1 ] && [ -f "./CMakeLists.txt" ] && [ -d "./src" ]; then
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
    [ -f "$TMP_DIR/build/fpshud_test" ] && cp -f "$TMP_DIR/build/fpshud_test" "$LIB_DIR/fpshud_test"
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

# 1. Install from local build or dist only when explicitly authorized
if [ "$ALLOW_LOCAL" -eq 1 ]; then
    if [ -f "./build/libfpshud.so" ]; then
        echo "[*] Installing from local build directory..."
        cp -f "./build/libfpshud.so" "$LIB_DIR/libfpshud.so"
        [ -f "./build/fpshud_test" ] && cp -f "./build/fpshud_test" "$LIB_DIR/fpshud_test"
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
        if [ ! -f "$TMP_DIR/libfpshud.so" ]; then
            echo "[!] ERROR: Local archive ./dist/fpshud-linux-x86_64.tar.gz is corrupted or missing libfpshud.so."
            exit 1
        fi
        cp -f "$TMP_DIR/libfpshud.so" "$LIB_DIR/libfpshud.so"
        [ -f "$TMP_DIR/fpshud_test" ] && cp -f "$TMP_DIR/fpshud_test" "$LIB_DIR/fpshud_test"
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
fi

verify_sha256() {
    local target_file="$1"
    local sha_file="$2"
    local expected_hash=""
    expected_hash="$(grep -oE '[a-fA-F0-9]{64}' "$sha_file" 2>/dev/null | head -n 1 || true)"
    if [ -z "$expected_hash" ]; then
        echo "[!] ERROR: Checksum hash not found in $sha_file"
        return 1
    fi
    local actual_hash=""
    if command -v sha256sum >/dev/null 2>&1; then
        actual_hash="$(sha256sum "$target_file" | awk '{print $1}')"
    elif command -v shasum >/dev/null 2>&1; then
        actual_hash="$(shasum -a 256 "$target_file" | awk '{print $1}')"
    else
        echo "[!] WARNING: Neither sha256sum nor shasum is installed; cannot verify archive integrity."
        return 1
    fi
    if [ "$expected_hash" != "$actual_hash" ]; then
        echo "[!] ERROR: Cryptographic SHA256 checksum verification failed!"
        echo "    File    : $target_file"
        echo "    Expected: $expected_hash"
        echo "    Actual  : $actual_hash"
        return 1
    fi
    echo "[✓] Cryptographic SHA256 integrity verified ($expected_hash)."
    return 0
}

# 2. Download prebuilt portable binary with SHA256 verification
if [ "$INSTALLED_BIN" -eq 0 ]; then
    echo "[*] Downloading prebuilt portable FPSHUD binary..."
    DOWNLOAD_OK=0
    if curl -fL "$RELEASE_URL" -o "$TMP_DIR/fpshud.tar.gz" 2>/dev/null && \
       curl -fL "$RELEASE_SHA_URL" -o "$TMP_DIR/fpshud.tar.gz.sha256" 2>/dev/null; then
        DOWNLOAD_OK=1
    elif curl -fL "$RAW_DIST_URL" -o "$TMP_DIR/fpshud.tar.gz" 2>/dev/null && \
         curl -fL "$RAW_DIST_SHA_URL" -o "$TMP_DIR/fpshud.tar.gz.sha256" 2>/dev/null; then
        DOWNLOAD_OK=1
    fi

    if [ "$DOWNLOAD_OK" -eq 1 ]; then
        if ! verify_sha256 "$TMP_DIR/fpshud.tar.gz" "$TMP_DIR/fpshud.tar.gz.sha256"; then
            echo "[!] Security Error: Downloaded release archive does not match published hash! Aborting."
            exit 1
        fi
        tar -xzf "$TMP_DIR/fpshud.tar.gz" -C "$TMP_DIR"
        if [ ! -f "$TMP_DIR/libfpshud.so" ]; then
            echo "[!] ERROR: Downloaded archive is invalid or missing libfpshud.so."
            exit 1
        fi
        cp -f "$TMP_DIR/libfpshud.so" "$LIB_DIR/libfpshud.so"
        if [ -f "$TMP_DIR/fpshud_test" ]; then
            cp -f "$TMP_DIR/fpshud_test" "$LIB_DIR/fpshud_test"
            chmod +x "$LIB_DIR/fpshud_test"
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
[ -f "$LIB_DIR/fpshud_test" ] && chmod +x "$LIB_DIR/fpshud_test"
[ -f "$BIN_DIR/fpshud_recorder" ] && chmod +x "$BIN_DIR/fpshud_recorder"
[ -f "$LIB_DIR/fpshud_recorder" ] && chmod +x "$LIB_DIR/fpshud_recorder"
[ -f "$BIN_DIR/fpshud_recorder_bin" ] && chmod +x "$BIN_DIR/fpshud_recorder_bin"
[ -f "$LIB_DIR/fpshud_recorder_bin" ] && chmod +x "$LIB_DIR/fpshud_recorder_bin"

# Verify prebuilt libfpshud.so compatibility (GLIBC & pre-AVX2 CPU safety)
if [ "$INSTALLED_FROM_SOURCE" -eq 0 ]; then
    NEED_REBUILD=0
    if ldd "$LIB_DIR/libfpshud.so" 2>&1 | grep -q "not found"; then
        echo "[*] Notice: Prebuilt libfpshud.so requires newer system libraries than installed on this distro."
        NEED_REBUILD=1
    elif ! grep -qw "avx2" /proc/cpuinfo 2>/dev/null && [ -x "$LIB_DIR/fpshud_test" ]; then
        TEST_RC=0
        "$LIB_DIR/fpshud_test" >/dev/null 2>&1 || TEST_RC=$?
        if [ "$TEST_RC" -eq 132 ] || [ "$TEST_RC" -eq 127 ]; then
            echo "[*] Notice: Prebuilt binary uses CPU instructions unsupported by this processor (exit $TEST_RC)."
            NEED_REBUILD=1
        fi
    fi
    if [ "$NEED_REBUILD" -eq 1 ]; then
        echo "    Rebuilding native FPSHUD binaries from source for this host..."
        build_and_install_from_source || echo "[!] Warning: Native source rebuild skipped (install cmake & Clang 19+ or GCC 15+ to build locally)."
        chmod +x "$LIB_DIR/libfpshud.so"
        [ -f "$LIB_DIR/fpshud_test" ] && chmod +x "$LIB_DIR/fpshud_test"
    fi
fi

# Verify if prebuilt fpshud_recorder works on this distro
REC_CHECK_TARGET="$BIN_DIR/fpshud_recorder_bin"
[ ! -f "$REC_CHECK_TARGET" ] && REC_CHECK_TARGET="$BIN_DIR/fpshud_recorder"
if [ -f "$REC_CHECK_TARGET" ] && ldd "$REC_CHECK_TARGET" 2>&1 | grep -q "not found"; then
    echo "[*] Notice: Host FFmpeg library version differs from prebuilt binary."
    echo "    Compiling native fpshud_recorder for this system..."
    select_cpp_toolchain
    if [ -n "$CXX_BIN" ] && command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libavformat libavcodec libavutil libswscale libswresample libpulse 2>/dev/null; then
        REC_TMP="$(mktemp -d /tmp/fpshud-rec-build.XXXXXX)"
        REC_SRC_DIR="$REC_TMP/src"
        if [ "$ALLOW_LOCAL" -eq 1 ] && [ -f "./src/recorder/recorder_main.cpp" ]; then
            REC_SRC_DIR="."
        elif command -v git >/dev/null 2>&1; then
            git clone --depth 1 "https://github.com/${REPO_OWNER}/${REPO_NAME}.git" "$REC_SRC_DIR" 2>/dev/null || true
        fi
        REC_MAIN_SRC=""
        if [ -f "$REC_SRC_DIR/src/recorder/recorder_main.cpp" ]; then
            REC_MAIN_SRC="$REC_SRC_DIR/src/recorder/recorder_main.cpp"
        elif [ -f "$REC_SRC_DIR/src/recorder_main.cpp" ]; then
            REC_MAIN_SRC="$REC_SRC_DIR/src/recorder_main.cpp"
        fi
        REC_LAUNCHER_SRC=""
        if [ -f "$REC_SRC_DIR/src/recorder/recorder_launcher.c" ]; then
            REC_LAUNCHER_SRC="$REC_SRC_DIR/src/recorder/recorder_launcher.c"
        elif [ -f "$REC_SRC_DIR/src/recorder_launcher.c" ]; then
            REC_LAUNCHER_SRC="$REC_SRC_DIR/src/recorder_launcher.c"
        fi
        if [ -n "$REC_MAIN_SRC" ]; then
            REC_STD_FLAG="-std=c++26"
            if "$CXX_BIN" -O3 "$REC_STD_FLAG" "$REC_MAIN_SRC" \
                -I"$REC_SRC_DIR/src" -I"$REC_SRC_DIR/src/recorder" -I"$REC_SRC_DIR/src/core" \
                $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale libswresample libpulse) \
                -pthread -ldl \
                -Wl,-rpath,'$ORIGIN:$ORIGIN/../lib:/run/host/usr/lib/x86_64-linux-gnu:/run/host/usr/lib/x86_64-linux-gnu/pulseaudio:/run/host/usr/lib64:/run/host/usr/lib:/run/host/usr/lib/pulseaudio:/usr/lib' \
                -Wl,--disable-new-dtags \
                -o "$LIB_DIR/fpshud_recorder_bin" 2>"$REC_TMP/err.log"; then
                chmod +x "$LIB_DIR/fpshud_recorder_bin"
                cp -f "$LIB_DIR/fpshud_recorder_bin" "$BIN_DIR/fpshud_recorder_bin"
                if [ -n "$CC_BIN" ] && command -v "$CC_BIN" >/dev/null 2>&1 && [ -n "$REC_LAUNCHER_SRC" ]; then
                    "$CC_BIN" -O3 "$REC_LAUNCHER_SRC" -ldl -o "$BIN_DIR/fpshud_recorder" 2>/dev/null && \
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
        "enable_environment": {
            "FPS_HUD": "1"
        },
        "disable_environment": {
            "DISABLE_FPS_HUD": "1"
        }
    }
}
EOF

# 5. Install OpenGL & CLI Management Wrapper Script (~/.local/bin/fpshud)
if [ -f "$TMP_DIR/fpshud" ]; then
    cp -f "$TMP_DIR/fpshud" "$BIN_DIR/fpshud"
elif [ -f "./scripts/fpshud.in" ]; then
    sed "s|@FPSHUD_LIB_PATH@|$LIB_DIR/libfpshud.so|g" "./scripts/fpshud.in" > "$BIN_DIR/fpshud"
elif [ -f "$TMP_DIR/src/scripts/fpshud.in" ]; then
    sed "s|@FPSHUD_LIB_PATH@|$LIB_DIR/libfpshud.so|g" "$TMP_DIR/src/scripts/fpshud.in" > "$BIN_DIR/fpshud"
elif [ -f "$TMP_DIR/scripts/fpshud.in" ]; then
    sed "s|@FPSHUD_LIB_PATH@|$LIB_DIR/libfpshud.so|g" "$TMP_DIR/scripts/fpshud.in" > "$BIN_DIR/fpshud"
else
    curl -fsSL "https://raw.githubusercontent.com/${REPO_OWNER}/${REPO_NAME}/main/scripts/fpshud.in" 2>/dev/null | \
        sed "s|@FPSHUD_LIB_PATH@|$LIB_DIR/libfpshud.so|g" > "$BIN_DIR/fpshud" || true
fi
chmod +x "$BIN_DIR/fpshud"

# ====================================================================
# 6. Hardware Telemetry Permissions (RAM Speed, CPU Wattage & Wayland Hotkeys)
# ====================================================================
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
echo ""
echo "Security Notice / Perhatian Keamanan:"
echo "  1. Granting read access to RAPL CPU Wattage (MODE 0444) allows local"
echo "     software to observe CPU power counters (side-channel CVE-2020-8694 Platypus trade-off)."
echo "  2. Adding user to 'input' group grants access to raw input event nodes"
echo "     (/dev/input/event*), which can observe system keystrokes."
echo "=================================================================="

USER_CONFIRM=0
if [ "${FORCE_SETUP:-0}" = "1" ] || [ "$FORCE_OPT" -eq 1 ]; then
    USER_CONFIRM=1
elif [ -n "${CI:-}" ]; then
    USER_CONFIRM=0
elif [ -t 0 ]; then
    read -r -p "Proceed with sudo hardware sensor setup? / Lanjutkan konfigurasi sensor via sudo? [y/N] " confirm || confirm="n"
    case "$confirm" in
        [yY][eE][sS]|[yY]) USER_CONFIRM=1 ;;
        *) echo "[*] Sudo hardware sensor setup skipped by user."; USER_CONFIRM=0 ;;
    esac
elif [ -r /dev/tty ] && [ -w /dev/tty ]; then
    read -r -p "Proceed with sudo hardware sensor setup? / Lanjutkan konfigurasi sensor via sudo? [y/N] " confirm < /dev/tty || confirm="n"
    case "$confirm" in
        [yY][eE][sS]|[yY]) USER_CONFIRM=1 ;;
        *) echo "[*] Sudo hardware sensor setup skipped by user."; USER_CONFIRM=0 ;;
    esac
else
    echo "[*] Non-interactive environment detected without TTY. Skipping sudo configuration."
    echo "    (Core overlay will install cleanly; to enable privileged sensors later, run: ~/.local/bin/fpshud --setup)"
    USER_CONFIRM=0
fi

if [ "$USER_CONFIRM" -eq 1 ]; then
    if [ -x "$BIN_DIR/fpshud" ]; then
        FORCE_SETUP=1 "$BIN_DIR/fpshud" --setup || echo "[!] Sudo configuration incomplete. You can retry anytime via: ~/.local/bin/fpshud --setup"
    fi
else
    echo ""
    echo "[!] Sudo configuration skipped."
    echo "    Core overlay, FPS, and GPU sensors will function normally."
    echo "    To enable CPU Wattage, RAM Speed, and Wayland hotkeys later, run at any time:"
    echo "        ~/.local/bin/fpshud --setup"
    echo ""
fi

echo ""
echo "=================================================================="
echo "  FPSHUD Successfully Installed!"
echo "=================================================================="
echo "  - Overlay Library       : $LIB_DIR/libfpshud.so"
echo "  - Vulkan Implicit Layer : $VULKAN_LAYER_DIR/VkLayer_FPSHUD_overlay.json"
echo "  - OpenGL & CLI Wrapper  : $BIN_DIR/fpshud"
if [ -f "$BIN_DIR/fpshud_recorder" ]; then
echo "  - Screen Recorder Daemon: $BIN_DIR/fpshud_recorder"
fi
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
echo "  - Configure permissions : ~/.local/bin/fpshud --setup"
echo "=================================================================="
