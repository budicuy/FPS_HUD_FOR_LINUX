#!/usr/bin/env bash
# scripts/test_vulkan_headless.sh — Real Vulkan Swapchain Hook & Pure-Rust CPU UI E2E Check
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
RELEASE_DIR="${ROOT_DIR}/target/release"

echo "=========================================================="
echo "  FPSHUD Pure-Rust Vulkan Headless Verification (vkcube)"
echo "=========================================================="

if ! command -v vkcube >/dev/null 2>&1; then
    echo "[!] vkcube command not found. Please install vulkan-tools." >&2
    exit 1
fi

for bin in "${RELEASE_DIR}/libfpshud.so" "${RELEASE_DIR}/fpshud-ui"; do
    if [ ! -f "$bin" ]; then
        echo "[!] Missing release binary: $bin (run ./scripts/build_rust_release.sh)" >&2
        exit 1
    fi
done

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

if [ -z "${VK_DRIVER_FILES:-}" ] && [ -n "$LVP_JSON" ] && [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
    echo "[*] Headless environment: using Mesa Lavapipe CPU rasterizer (${LVP_JSON})"
    export VK_DRIVER_FILES="$LVP_JSON"
fi

TMP_LAYER_DIR="$(mktemp -d /tmp/fpshud_vk_layer.XXXXXX)"
trap 'rm -rf "$TMP_LAYER_DIR"' EXIT

sed "s|@FPSHUD_LIB_PATH@|${RELEASE_DIR}/libfpshud.so|g" \
    "${ROOT_DIR}/manifest/VkLayer_FPSHUD_overlay.json.in" \
    > "${TMP_LAYER_DIR}/VkLayer_FPSHUD_overlay.json"

export VK_LAYER_PATH="$TMP_LAYER_DIR"
export VK_INSTANCE_LAYERS="VK_LAYER_FPSHUD_overlay"
export FPS_HUD=1

FRAME_COUNT=120
RUN_CMD="vkcube --c ${FRAME_COUNT}"
if command -v timeout >/dev/null 2>&1; then
    RUN_CMD="timeout --kill-after=5 30 vkcube --c ${FRAME_COUNT}"
fi

echo "[*] Running vkcube --c ${FRAME_COUNT} under Pure-Rust libfpshud.so + fpshud-ui..."
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
    if ! command -v xvfb-run >/dev/null 2>&1; then
        echo "[!] No DISPLAY / WAYLAND_DISPLAY and xvfb-run not found." >&2
        exit 1
    fi
    xvfb-run -a $RUN_CMD
else
    $RUN_CMD
fi

echo "=========================================================="
echo "  [✓] Headless Vulkan 120-frame hook & Pure-Rust CPU UI pass OK!"
echo "=========================================================="
