#!/usr/bin/env bash
# scripts/install.sh - Developer Installer for Local Builds (Zero-Download / Offline)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

LIB_DIR="$HOME/.local/lib/fpshud"
BIN_DIR="$HOME/.local/bin"
VULKAN_LAYER_DIR="$HOME/.local/share/vulkan/implicit_layer.d"
FONT_DIR="$HOME/.local/share/fpshud/fonts"

echo "=================================================================="
echo "  FPSHUD — Local Developer Build Installer"
echo "=================================================================="

if [ ! -f "$BUILD_DIR/libfpshud.so" ]; then
    echo "[!] ERROR: Local build not found at $BUILD_DIR/libfpshud.so" >&2
    echo "    Please compile the project first via: ./scripts/build_clang.sh" >&2
    exit 1
fi

mkdir -p "$LIB_DIR" "$BIN_DIR" "$VULKAN_LAYER_DIR" "$FONT_DIR"
rm -f "$BIN_DIR/fpshud_test"

echo "[*] Installing local build binaries..."
cp -f "$BUILD_DIR/libfpshud.so" "$LIB_DIR/libfpshud.so"
chmod 755 "$LIB_DIR/libfpshud.so"

if [ -f "$BUILD_DIR/fpshud_test" ]; then
    cp -f "$BUILD_DIR/fpshud_test" "$LIB_DIR/fpshud_test"
    chmod 755 "$LIB_DIR/fpshud_test"
fi

if [ -f "$BUILD_DIR/fpshud_recorder" ]; then
    cp -f "$BUILD_DIR/fpshud_recorder" "$BIN_DIR/fpshud_recorder"
    cp -f "$BUILD_DIR/fpshud_recorder" "$LIB_DIR/fpshud_recorder"
    chmod 755 "$BIN_DIR/fpshud_recorder" "$LIB_DIR/fpshud_recorder"
fi

if [ -f "$BUILD_DIR/fpshud_recorder_bin" ]; then
    cp -f "$BUILD_DIR/fpshud_recorder_bin" "$BIN_DIR/fpshud_recorder_bin"
    cp -f "$BUILD_DIR/fpshud_recorder_bin" "$LIB_DIR/fpshud_recorder_bin"
    chmod 755 "$BIN_DIR/fpshud_recorder_bin" "$LIB_DIR/fpshud_recorder_bin"
fi

if [ -f "$SCRIPT_DIR/uninstall.sh" ]; then
    cp -f "$SCRIPT_DIR/uninstall.sh" "$LIB_DIR/uninstall.sh"
    chmod 755 "$LIB_DIR/uninstall.sh"
fi

sed "s|@FPSHUD_LIB_PATH@|$LIB_DIR/libfpshud.so|g" "$SCRIPT_DIR/fpshud.in" > "$BIN_DIR/fpshud"
chmod 755 "$BIN_DIR/fpshud"

# Copy optional system fonts if available
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

echo "[✓] Local developer build installed successfully to $LIB_DIR and $BIN_DIR"
if [ -x "$BIN_DIR/fpshud" ]; then
    "$BIN_DIR/fpshud" --status || true
fi
