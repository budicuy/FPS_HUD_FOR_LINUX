#!/usr/bin/env bash
# scripts/install.sh — Zero-sudo Local Developer Installer (Pure-Rust)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
RELEASE_DIR="${PROJECT_DIR}/target/release"

LIB_DIR="${HOME}/.local/lib/fpshud"
VULKAN_LAYER_DIR="${HOME}/.local/share/vulkan/implicit_layer.d"

echo "=================================================================="
echo "  FPSHUD — Zero-Sudo Local Developer Installer (Pure-Rust)"
echo "=================================================================="

if [ ! -f "${RELEASE_DIR}/libfpshud.so" ] || [ ! -f "${RELEASE_DIR}/fpshud-ui" ]; then
    echo "[*] Release binaries not found; building via scripts/build_rust_release.sh..."
    "${SCRIPT_DIR}/build_rust_release.sh"
fi

mkdir -p "$LIB_DIR" "$VULKAN_LAYER_DIR"

echo "[*] Installing Pure-Rust overlay library and CPU UI daemon..."
cp -f "${RELEASE_DIR}/libfpshud.so" "${LIB_DIR}/libfpshud.so"
cp -f "${RELEASE_DIR}/fpshud-ui" "${LIB_DIR}/fpshud-ui"
cp -f "${SCRIPT_DIR}/uninstall.sh" "${LIB_DIR}/uninstall.sh"
chmod 755 "${LIB_DIR}/libfpshud.so" "${LIB_DIR}/fpshud-ui" "${LIB_DIR}/uninstall.sh"

sed "s|@FPSHUD_LIB_PATH@|${LIB_DIR}/libfpshud.so|g" \
    "${PROJECT_DIR}/manifest/VkLayer_FPSHUD_overlay.json.in" \
    > "${VULKAN_LAYER_DIR}/VkLayer_FPSHUD_overlay.json"

echo "[✓] Installed cleanly (zero sudo, zero root daemons):"
echo "    - Layer Library   : ${LIB_DIR}/libfpshud.so"
echo "    - CPU UI Daemon   : ${LIB_DIR}/fpshud-ui"
echo "    - Vulkan Manifest : ${VULKAN_LAYER_DIR}/VkLayer_FPSHUD_overlay.json"
echo "    - Launch (Vulkan) : FPS_HUD=1 %command%"
echo "    - Launch (OpenGL) : FPS_HUD=1 LD_PRELOAD=${LIB_DIR}/libfpshud.so %command%"
echo "    - Uninstall       : ${LIB_DIR}/uninstall.sh"
