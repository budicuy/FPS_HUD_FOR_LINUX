#!/usr/bin/env bash
# install.sh — 1-Command Zero-Sudo Portable Installer for FPSHUD (Pure-Rust)
set -euo pipefail

REPO_OWNER="budicuy"
REPO_NAME="FPS_HUD_FOR_LINUX"
RELEASE_URL="${FPSHUD_RELEASE_URL:-https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/latest/download/fpshud-linux-x86_64.tar.gz}"
RELEASE_SHA_URL="${FPSHUD_RELEASE_SHA_URL:-${RELEASE_URL}.sha256}"

LIB_DIR="${HOME}/.local/lib/fpshud"
VULKAN_LAYER_DIR="${HOME}/.local/share/vulkan/implicit_layer.d"

echo "=================================================================="
echo "  FPSHUD — 1-Command Zero-Sudo Installer (Vulkan & OpenGL)"
echo "=================================================================="

for tool in curl tar; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[!] ERROR: Required utility '$tool' is not installed." >&2
        exit 1
    fi
done
if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
    echo "[!] ERROR: Neither 'sha256sum' nor 'shasum' is available." >&2
    exit 1
fi

TMP_DIR="$(mktemp -d /tmp/fpshud-install.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

verify_sha256() {
    local target_file="$1"
    local sha_file="$2"
    local expected_hash actual_hash
    expected_hash="$(grep -oE '[a-fA-F0-9]{64}' "$sha_file" 2>/dev/null | head -n 1 | tr '[:upper:]' '[:lower:]' || true)"
    if [ -z "$expected_hash" ]; then
        echo "[!] ERROR: Checksum hash not found in $sha_file" >&2
        return 1
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        actual_hash="$(sha256sum "$target_file" | awk '{print $1}')"
    else
        actual_hash="$(shasum -a 256 "$target_file" | awk '{print $1}')"
    fi
    if [ "$expected_hash" != "$actual_hash" ]; then
        echo "[!] Security Error: Cryptographic SHA256 checksum verification failed!" >&2
        return 1
    fi
    echo "[✓] Cryptographic SHA256 integrity verified ($expected_hash)."
}

echo "[*] Downloading portable prebuilt FPSHUD package..."
curl -fL "$RELEASE_URL" -o "${TMP_DIR}/fpshud.tar.gz"
curl -fL "$RELEASE_SHA_URL" -o "${TMP_DIR}/fpshud.tar.gz.sha256"
verify_sha256 "${TMP_DIR}/fpshud.tar.gz" "${TMP_DIR}/fpshud.tar.gz.sha256"

tar -xzf "${TMP_DIR}/fpshud.tar.gz" -C "$TMP_DIR"
for required in libfpshud.so fpshud-ui fpshud-ffmpeg VkLayer_FPSHUD_overlay.json.in; do
    if [ ! -f "${TMP_DIR}/${required}" ]; then
        echo "[!] ERROR: Release archive missing ${required}." >&2
        exit 1
    fi
done

mkdir -p "$LIB_DIR" "$VULKAN_LAYER_DIR"
cp -f "${TMP_DIR}/libfpshud.so" "${LIB_DIR}/libfpshud.so"
cp -f "${TMP_DIR}/fpshud-ui" "${LIB_DIR}/fpshud-ui"
cp -f "${TMP_DIR}/fpshud-ffmpeg" "${LIB_DIR}/fpshud-ffmpeg"
chmod 755 "${LIB_DIR}/libfpshud.so" "${LIB_DIR}/fpshud-ui" "${LIB_DIR}/fpshud-ffmpeg"

if [ -f "${TMP_DIR}/LICENSE.ffmpeg-gpl" ]; then
    cp -f "${TMP_DIR}/LICENSE.ffmpeg-gpl" "${LIB_DIR}/LICENSE.ffmpeg-gpl"
fi

if [ -f "${TMP_DIR}/uninstall.sh" ]; then
    cp -f "${TMP_DIR}/uninstall.sh" "${LIB_DIR}/uninstall.sh"
    chmod 755 "${LIB_DIR}/uninstall.sh"
fi

sed "s|@FPSHUD_LIB_PATH@|${LIB_DIR}/libfpshud.so|g" \
    "${TMP_DIR}/VkLayer_FPSHUD_overlay.json.in" \
    > "${VULKAN_LAYER_DIR}/VkLayer_FPSHUD_overlay.json"

echo ""
echo "=================================================================="
echo "  FPSHUD Installed Cleanly (Zero-Sudo)"
echo "=================================================================="
echo "  - Overlay Library       : ${LIB_DIR}/libfpshud.so"
echo "  - GPUI-CE Daemon        : ${LIB_DIR}/fpshud-ui"
echo "  - Vulkan Implicit Layer : ${VULKAN_LAYER_DIR}/VkLayer_FPSHUD_overlay.json"
echo "  - Uninstall             : ${LIB_DIR}/uninstall.sh"
echo ""
echo "  Quick Start:"
echo "  - Steam Launch Options  : FPS_HUD=1 %command%"
echo "  - OpenGL (LD_PRELOAD)   : FPS_HUD=1 LD_PRELOAD=${LIB_DIR}/libfpshud.so ./your_game"
echo "=================================================================="
