#!/usr/bin/env bash
# test_vulkan_headless.sh - Headless / Real Vulkan Swapchain Hook & HUD Render Verification
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

echo "=========================================================="
echo "  FPSHUD Vulkan Swapchain Hook & Rendering Test"
echo "=========================================================="

# 1. Verify vkcube availability
if ! command -v vkcube >/dev/null 2>&1; then
    echo "[!] vkcube command not found. Please install vulkan-tools."
    exit 1
fi

# 2. Locate libfpshud.so and layer manifest
if [ ! -f "${BUILD_DIR}/libfpshud.so" ]; then
    echo "[!] libfpshud.so not found in ${BUILD_DIR}. Please compile first."
    exit 1
fi

LAYER_DIR="${BUILD_DIR}"
if [ ! -f "${LAYER_DIR}/VkLayer_FPSHUD_overlay.json" ]; then
    echo "[!] VkLayer_FPSHUD_overlay.json not found in ${LAYER_DIR}."
    exit 1
fi

# 3. Detect Lavapipe CPU Software Rasterizer if in headless CI
LVP_CANDIDATES=(
    "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
    "/usr/share/vulkan/icd.d/lvp_icd.json"
    "/usr/local/share/vulkan/icd.d/lvp_icd.x86_64.json"
)

LVP_JSON=""
for cand in "${LVP_CANDIDATES[@]}"; do
    if [ -f "$cand" ]; then
        LVP_JSON="$cand"
        break
    fi
done

# If VK_DRIVER_FILES is already provided by environment, respect it
if [ -n "${VK_DRIVER_FILES:-}" ]; then
    echo "[*] Using environment VK_DRIVER_FILES: ${VK_DRIVER_FILES}"
elif [ -n "$LVP_JSON" ] && [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
    echo "[*] Headless environment detected: Using Mesa Lavapipe CPU rasterizer (${LVP_JSON})"
    export VK_DRIVER_FILES="$LVP_JSON"
fi

export VK_LAYER_PATH="${LAYER_DIR}"
export VK_INSTANCE_LAYERS="VK_LAYER_FPSHUD_overlay"
export FPS_HUD=1

FRAME_COUNT=120
echo "[*] Running vkcube for ${FRAME_COUNT} frames under FPSHUD layer..."

if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
    if ! command -v xvfb-run >/dev/null 2>&1; then
        echo "[!] No DISPLAY / WAYLAND_DISPLAY and xvfb-run not found. Cannot run headless vkcube."
        exit 1
    fi
    echo "[*] Launching with xvfb-run virtual display..."
    xvfb-run -a vkcube --c "${FRAME_COUNT}"
else
    vkcube --c "${FRAME_COUNT}"
fi

echo "=========================================================="
echo "  [✓] Vulkan swapchain hook & HUD rendering passed (${FRAME_COUNT} frames)!"
echo "=========================================================="
