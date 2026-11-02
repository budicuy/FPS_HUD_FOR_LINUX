#!/usr/bin/env bash
set -e

REPO_OWNER="budicuy"
REPO_NAME="FPS_HUD_FOR_LINUX"
RELEASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/latest/download/fpshud-linux-x86_64.tar.gz"
GIT_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}.git"

INSTALL_LIB_DIR="$HOME/.local/lib/fpshud"
INSTALLED_SO="$INSTALL_LIB_DIR/libfpshud.so"
VULKAN_LAYER_DIR="$HOME/.local/share/vulkan/implicit_layer.d"
BIN_DIR="$HOME/.local/bin"
FONT_DIR="$HOME/.local/share/fpshud/fonts"

mkdir -p "$INSTALL_LIB_DIR" "$VULKAN_LAYER_DIR" "$BIN_DIR" "$FONT_DIR"

echo "=========================================================="
echo "  FPSHUD Installer for Linux (Vulkan & OpenGL Overlay)"
echo "=========================================================="

# Detect if running inside a local checkout of the repository
SCRIPT_PATH="${BASH_SOURCE[0]:-}"
LOCAL_PROJECT_DIR=""
if [ -n "$SCRIPT_PATH" ] && [ -f "$SCRIPT_PATH" ]; then
    CANDIDATE_DIR="$(cd "$(dirname "$SCRIPT_PATH")" && pwd)"
    if [ -f "$CANDIDATE_DIR/CMakeLists.txt" ] && [ -d "$CANDIDATE_DIR/src" ]; then
        LOCAL_PROJECT_DIR="$CANDIDATE_DIR"
    elif [ -f "$(dirname "$CANDIDATE_DIR")/CMakeLists.txt" ]; then
        LOCAL_PROJECT_DIR="$(dirname "$CANDIDATE_DIR")"
    fi
fi

build_from_source() {
    local src_dir="$1"
    local bld_dir="$2"
    echo "[*] Compiling FPSHUD from source..."
    if command -v ninja >/dev/null 2>&1; then
        cmake -B "$bld_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release "$src_dir"
    else
        cmake -B "$bld_dir" -DCMAKE_BUILD_TYPE=Release "$src_dir"
    fi
    cmake --build "$bld_dir" --parallel
    cp -f "$bld_dir/libfpshud.so" "$INSTALLED_SO"
}

if [ -n "$LOCAL_PROJECT_DIR" ] && [ "${1:-}" != "--download" ]; then
    build_from_source "$LOCAL_PROJECT_DIR" "$LOCAL_PROJECT_DIR/build"
else
    TMP_DIR="$(mktemp -d /tmp/fpshud-install.XXXXXX)"
    trap 'rm -rf "$TMP_DIR"' EXIT

    echo "[*] Downloading prebuilt portable binary from GitHub Releases..."
    if curl -fsSL "$RELEASE_URL" -o "$TMP_DIR/fpshud.tar.gz" 2>/dev/null; then
        tar -xzf "$TMP_DIR/fpshud.tar.gz" -C "$TMP_DIR"
        if [ -f "$TMP_DIR/libfpshud.so" ]; then
            cp -f "$TMP_DIR/libfpshud.so" "$INSTALLED_SO"
            echo "[+] Prebuilt portable binary downloaded and extracted!"
        fi
    fi

    # Fallback: If prebuilt release binary is not uploaded yet, build automatically in /tmp
    if [ ! -f "$INSTALLED_SO" ]; then
        echo "[*] Prebuilt release not found yet; building automatically in temporary directory..."
        if ! command -v cmake >/dev/null 2>&1 || ! command -v g++ >/dev/null 2>&1; then
            echo "[*] Installing required build tools for your Linux distribution..."
            if command -v pacman >/dev/null 2>&1; then
                sudo pacman -S --needed --noconfirm base-devel cmake ninja git pkgconf vulkan-headers vulkan-icd-loader mesa libglvnd libx11 wayland hwdata
            elif command -v apt-get >/dev/null 2>&1; then
                sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build git pkg-config libvulkan-dev libgl1-mesa-dev libegl1-mesa-dev libx11-dev pci.ids
            elif command -v dnf >/dev/null 2>&1; then
                sudo dnf install -y gcc-c++ cmake ninja-build git pkgconf-pkg-config vulkan-headers vulkan-loader-devel mesa-libGL-devel mesa-libEGL-devel libX11-devel hwdata
            fi
        fi
        git clone --depth 1 "$GIT_URL" "$TMP_DIR/src"
        build_from_source "$TMP_DIR/src" "$TMP_DIR/build"
    fi
fi

chmod 755 "$INSTALLED_SO"

# Copy optional system TrueType fonts if present on host (embedded fonts are already built into libfpshud.so)
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

# Generate Vulkan Implicit Layer JSON manifest pointing to ~/.local/lib/fpshud/libfpshud.so
cat > "$VULKAN_LAYER_DIR/VkLayer_FPSHUD_overlay.json" <<EOF
{
    "file_format_version": "1.2.1",
    "layer": {
        "name": "VK_LAYER_FPSHUD_overlay",
        "type": "GLOBAL",
        "library_path": "$INSTALLED_SO",
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

# Generate OpenGL wrapper script pointing to ~/.local/lib/fpshud/libfpshud.so
cat > "$BIN_DIR/fpshud" <<EOF
#!/usr/bin/env bash
# Wrapper script for FPSHUD (enables OpenGL LD_PRELOAD + Vulkan Layer automatically)

if [ -z "\$FPS_HUD" ]; then
    export FPS_HUD=1
fi

export LD_PRELOAD="$INSTALLED_SO\${LD_PRELOAD:+:\$LD_PRELOAD}"
exec "\$@"
EOF
chmod +x "$BIN_DIR/fpshud"

# Try to enable non-root read permissions for CPU Watt (RAPL) & input events if readable or skip silently
chmod a+r /sys/class/powercap/*rapl*/energy_uj 2>/dev/null || true

echo "=========================================================="
echo " [SUCCESS] FPSHUD is now installed!"
echo " - Shared Library        : $INSTALLED_SO"
echo " - Vulkan Implicit Layer : $VULKAN_LAYER_DIR/VkLayer_FPSHUD_overlay.json"
echo " - OpenGL Wrapper Script : $BIN_DIR/fpshud"
echo ""
echo " Quick Start:"
echo " - Steam Launch Options  : FPS_HUD=1 %command%"
echo " - Terminal (Vulkan)     : FPS_HUD=1 vkcube"
echo " - In-Game GUI Shortcut  : Press Insert, F12, or F10 inside your game"
echo "=========================================================="
