#!/usr/bin/env bash
set -euo pipefail

REPO_OWNER="budicuy"
REPO_NAME="FPS_HUD_FOR_LINUX"
RELEASE_URL="${FPSHUD_RELEASE_URL:-https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/latest/download/fpshud-linux-x86_64.tar.gz}"
RELEASE_SHA_URL="${FPSHUD_RELEASE_SHA_URL:-${RELEASE_URL}.sha256}"

LIB_DIR="$HOME/.local/lib/fpshud"
BIN_DIR="$HOME/.local/bin"
VULKAN_LAYER_DIR="$HOME/.local/share/vulkan/implicit_layer.d"
FONT_DIR="$HOME/.local/share/fpshud/fonts"

echo "=================================================================="
echo "  FPSHUD — 1-Command Installer for Linux (Vulkan & OpenGL)"
echo "=================================================================="

# Verify required core host utilities
for tool in curl tar; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[!] ERROR: Required utility '$tool' is not installed." >&2
        exit 1
    fi
done
if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
    echo "[!] ERROR: Neither 'sha256sum' nor 'shasum' is available for checksum verification." >&2
    exit 1
fi

FORCE_OPT=0
for arg in "$@"; do
    case "$arg" in
        --yes|-y) FORCE_OPT=1 ;;
    esac
done

mkdir -p "$LIB_DIR" "$BIN_DIR" "$VULKAN_LAYER_DIR" "$FONT_DIR"
rm -f "$BIN_DIR/fpshud_test"

TMP_DIR="$(mktemp -d /tmp/fpshud-install.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

verify_sha256() {
    local target_file="$1"
    local sha_file="$2"
    local expected_hash actual_hash
    expected_hash="$(grep -oE '[a-fA-F0-9]{64}' "$sha_file" 2>/dev/null | head -n 1 || true)"
    if [ -z "$expected_hash" ]; then
        echo "[!] ERROR: Checksum hash not found in $sha_file" >&2
        return 1
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        actual_hash="$(sha256sum "$target_file" | awk '{print $1}')"
    elif command -v shasum >/dev/null 2>&1; then
        actual_hash="$(shasum -a 256 "$target_file" | awk '{print $1}')"
    fi
    if [ "$expected_hash" != "$actual_hash" ]; then
        echo "[!] Security Error: Cryptographic SHA256 checksum verification failed!" >&2
        echo "    File    : $target_file" >&2
        echo "    Expected: $expected_hash" >&2
        echo "    Actual  : $actual_hash" >&2
        return 1
    fi
    echo "[✓] Cryptographic SHA256 integrity verified ($expected_hash)."
    return 0
}

# 1. Download prebuilt release archive and SHA256 checksum strictly from GitHub Releases
echo "[*] Downloading portable prebuilt FPSHUD package from GitHub Releases..."
if ! curl -fL "$RELEASE_URL" -o "$TMP_DIR/fpshud.tar.gz" 2>/dev/null; then
    echo "[!] ERROR: Failed to download release archive from:" >&2
    echo "    $RELEASE_URL" >&2
    echo "    Please check your network connection or repository releases." >&2
    exit 1
fi

if ! curl -fL "$RELEASE_SHA_URL" -o "$TMP_DIR/fpshud.tar.gz.sha256" 2>/dev/null; then
    echo "[!] ERROR: Failed to download SHA256 checksum from:" >&2
    echo "    $RELEASE_SHA_URL" >&2
    exit 1
fi

verify_sha256 "$TMP_DIR/fpshud.tar.gz" "$TMP_DIR/fpshud.tar.gz.sha256"

# 2. Extract portable binaries
echo "[*] Extracting portable binaries..."
tar -xzf "$TMP_DIR/fpshud.tar.gz" -C "$TMP_DIR"
if [ ! -f "$TMP_DIR/libfpshud.so" ]; then
    echo "[!] ERROR: Downloaded archive is corrupted or missing libfpshud.so." >&2
    exit 1
fi

cp -f "$TMP_DIR/libfpshud.so" "$LIB_DIR/libfpshud.so"
chmod 755 "$LIB_DIR/libfpshud.so"

if [ -f "$TMP_DIR/fpshud_test" ]; then
    cp -f "$TMP_DIR/fpshud_test" "$LIB_DIR/fpshud_test"
    chmod 755 "$LIB_DIR/fpshud_test"
fi

if [ -f "$TMP_DIR/fpshud_recorder" ]; then
    cp -f "$TMP_DIR/fpshud_recorder" "$BIN_DIR/fpshud_recorder"
    cp -f "$TMP_DIR/fpshud_recorder" "$LIB_DIR/fpshud_recorder"
    chmod 755 "$BIN_DIR/fpshud_recorder" "$LIB_DIR/fpshud_recorder"
fi

if [ -f "$TMP_DIR/fpshud_recorder_bin" ]; then
    cp -f "$TMP_DIR/fpshud_recorder_bin" "$BIN_DIR/fpshud_recorder_bin"
    cp -f "$TMP_DIR/fpshud_recorder_bin" "$LIB_DIR/fpshud_recorder_bin"
    chmod 755 "$BIN_DIR/fpshud_recorder_bin" "$LIB_DIR/fpshud_recorder_bin"
fi

# 3. Install CLI wrapper script
if [ -f "$TMP_DIR/fpshud" ]; then
    cp -f "$TMP_DIR/fpshud" "$BIN_DIR/fpshud"
elif [ -f "./scripts/fpshud.in" ]; then
    sed "s|@FPSHUD_LIB_PATH@|$LIB_DIR/libfpshud.so|g" "./scripts/fpshud.in" > "$BIN_DIR/fpshud"
else
    curl -fsSL "https://raw.githubusercontent.com/${REPO_OWNER}/${REPO_NAME}/main/scripts/fpshud.in" 2>/dev/null | \
        sed "s|@FPSHUD_LIB_PATH@|$LIB_DIR/libfpshud.so|g" > "$BIN_DIR/fpshud" || true
fi
chmod 755 "$BIN_DIR/fpshud"

# 4. Copy optional system fonts if available
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

# 5. Generate Vulkan Implicit Layer JSON manifest pointing to ~/.local/lib/fpshud/libfpshud.so
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

# 6. Hardware Telemetry Permissions (Optional RAPL CPU Wattage)
echo ""
echo "=================================================================="
echo "  Hardware Telemetry & Sensor Permissions Setup"
echo "=================================================================="
echo "FPSHUD works out-of-the-box (sudo-free) for FPS, Frametimes, GPU"
echo "sensors, RAM usage/swap/PSI, and in-game GUI/hotkeys on Wayland & X11."
echo "Only privileged CPU Package Power (Watts via RAPL counters) requires"
echo "an optional system udev rule."
echo ""
echo "Security Notice / Perhatian Keamanan:"
echo "  Granting read access to RAPL CPU Wattage (MODE 0444) allows local"
echo "  software to observe CPU power counters (side-channel CVE-2020-8694 Platypus trade-off)."
echo "=================================================================="

USER_CONFIRM=0
if [ "${FORCE_SETUP:-0}" = "1" ] || [ "$FORCE_OPT" -eq 1 ]; then
    USER_CONFIRM=1
elif [ -n "${CI:-}" ]; then
    USER_CONFIRM=0
elif [ -t 0 ]; then
    read -r -p "Proceed with sudo RAPL sensor setup? / Konfigurasi sensor CPU Watt via sudo? [y/N] " confirm || confirm="n"
    case "$confirm" in
        [yY][eE][sS]|[yY]) USER_CONFIRM=1 ;;
        *) echo "[*] Sudo hardware sensor setup skipped by user."; USER_CONFIRM=0 ;;
    esac
elif [ -r /dev/tty ] && [ -w /dev/tty ]; then
    read -r -p "Proceed with sudo RAPL sensor setup? / Konfigurasi sensor CPU Watt via sudo? [y/N] " confirm < /dev/tty || confirm="n"
    case "$confirm" in
        [yY][eE][sS]|[yY]) USER_CONFIRM=1 ;;
        *) echo "[*] Sudo hardware sensor setup skipped by user."; USER_CONFIRM=0 ;;
    esac
else
    echo "[*] Non-interactive environment detected without TTY. Skipping sudo configuration."
    echo "    (Core overlay will install cleanly; to enable RAPL power sensor later, run: ~/.local/bin/fpshud --setup)"
    USER_CONFIRM=0
fi

if [ "$USER_CONFIRM" -eq 1 ]; then
    if [ -x "$BIN_DIR/fpshud" ]; then
        FORCE_SETUP=1 "$BIN_DIR/fpshud" --setup || echo "[!] Sudo configuration incomplete. You can retry anytime via: ~/.local/bin/fpshud --setup"
    fi
else
    echo ""
    echo "[!] Sudo configuration skipped."
    echo "    Core overlay, FPS, GPU sensors, RAM, and Hotkeys function 100% normally."
    echo "    To enable CPU Wattage later, run at any time:"
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
