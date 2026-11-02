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

# 1. If executed from a local checkout with build/libfpshud.so or dist/ tarball
if [ -f "./build/libfpshud.so" ]; then
    echo "[*] Installing from local build directory..."
    cp -f "./build/libfpshud.so" "$LIB_DIR/libfpshud.so"
    [ -f "./build/fpshud_test" ] && cp -f "./build/fpshud_test" "$BIN_DIR/fpshud_test"
    INSTALLED_BIN=1
elif [ -f "./dist/fpshud-linux-x86_64.tar.gz" ]; then
    echo "[*] Extracting from local prebuilt package..."
    tar -xzf "./dist/fpshud-linux-x86_64.tar.gz" -C "$TMP_DIR"
    cp -f "$TMP_DIR/libfpshud.so" "$LIB_DIR/libfpshud.so"
    [ -f "$TMP_DIR/fpshud_test" ] && cp -f "$TMP_DIR/fpshud_test" "$BIN_DIR/fpshud_test"
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
        INSTALLED_BIN=1
    else
        echo "[!] Failed to download prebuilt binary. Falling back to source build..."
        git clone --depth 1 "https://github.com/${REPO_OWNER}/${REPO_NAME}.git" "$TMP_DIR/src"
        if command -v ninja >/dev/null 2>&1; then
            cmake -B "$TMP_DIR/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "$TMP_DIR/src"
        else
            cmake -B "$TMP_DIR/build" -DCMAKE_BUILD_TYPE=Release "$TMP_DIR/src"
        fi
        cmake --build "$TMP_DIR/build" --parallel
        cp -f "$TMP_DIR/build/libfpshud.so" "$LIB_DIR/libfpshud.so"
        cp -f "$TMP_DIR/build/fpshud_test" "$BIN_DIR/fpshud_test"
        INSTALLED_BIN=1
    fi
fi

chmod +x "$LIB_DIR/libfpshud.so"
[ -f "$BIN_DIR/fpshud_test" ] && chmod +x "$BIN_DIR/fpshud_test"

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
cat > "$BIN_DIR/fpshud" <<EOF
#!/usr/bin/env bash
# Wrapper script for FPSHUD (enables OpenGL LD_PRELOAD + Vulkan Layer automatically)

if [ -z "\$FPS_HUD" ]; then
    export FPS_HUD=1
fi

export LD_PRELOAD="$LIB_DIR/libfpshud.so\${LD_PRELOAD:+:\$LD_PRELOAD}"
exec "\$@"
EOF
chmod +x "$BIN_DIR/fpshud"

echo ""
echo "=================================================================="
echo "  FPSHUD Successfully Installed!"
echo "=================================================================="
echo "  - Overlay Library       : $LIB_DIR/libfpshud.so"
echo "  - Vulkan Implicit Layer : $VULKAN_LAYER_DIR/VkLayer_FPSHUD_overlay.json"
echo "  - OpenGL Wrapper Script : $BIN_DIR/fpshud"
echo "  - Diagnostic CLI Tool   : $BIN_DIR/fpshud_test"
echo ""
echo "  Quick Start:"
echo "  - Steam Launch Options  : FPS_HUD=1 %command%"
echo "  - Lutris / Heroic       : Add Environment Variable FPS_HUD=1"
echo "  - Terminal (Vulkan)     : FPS_HUD=1 ./your_game"
echo "  - Terminal (OpenGL)     : ~/.local/bin/fpshud ./your_game"
echo "  - Open In-Game GUI      : Press [Insert], [F12], or [F10] in-game"
echo "=================================================================="
