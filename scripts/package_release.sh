#!/usr/bin/env bash
# scripts/package_release.sh — Builds, verifies ABI gates, and packages zero-sudo release tarball
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
RELEASE_DIR="${PROJECT_DIR}/target/release"
DIST_DIR="${PROJECT_DIR}/dist"
STAGE_DIR="${PROJECT_DIR}/target/package_stage"

"${SCRIPT_DIR}/build_rust_release.sh"

STRIP_BIN="$(command -v llvm-strip || command -v strip || true)"
if [ -n "$STRIP_BIN" ]; then
    "$STRIP_BIN" --strip-unneeded "${RELEASE_DIR}/libfpshud.so" "${RELEASE_DIR}/fpshud-ui"
fi

"${SCRIPT_DIR}/verify_rust_abi.sh" "${RELEASE_DIR}/libfpshud.so" "${RELEASE_DIR}/fpshud-ui"

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR" "$DIST_DIR"

cp -f "${RELEASE_DIR}/libfpshud.so" "${STAGE_DIR}/libfpshud.so"
cp -f "${RELEASE_DIR}/fpshud-ui" "${STAGE_DIR}/fpshud-ui"
cp -f "${PROJECT_DIR}/manifest/VkLayer_FPSHUD_overlay.json.in" "${STAGE_DIR}/VkLayer_FPSHUD_overlay.json.in"
cp -f "${SCRIPT_DIR}/uninstall.sh" "${STAGE_DIR}/uninstall.sh"
chmod 755 "${STAGE_DIR}/libfpshud.so" "${STAGE_DIR}/fpshud-ui" "${STAGE_DIR}/uninstall.sh"

tar -czf "${DIST_DIR}/fpshud-linux-x86_64.tar.gz" -C "$STAGE_DIR" \
    libfpshud.so fpshud-ui VkLayer_FPSHUD_overlay.json.in uninstall.sh
(
    cd "$DIST_DIR"
    sha256sum "fpshud-linux-x86_64.tar.gz" > "fpshud-linux-x86_64.tar.gz.sha256"
)

echo "[✓] Packaged portable release archive: ${DIST_DIR}/fpshud-linux-x86_64.tar.gz"
echo "[✓] Generated SHA-256 checksum      : ${DIST_DIR}/fpshud-linux-x86_64.tar.gz.sha256"

if [ "${1:-}" = "--install" ] || [ "${FPSHUD_INSTALL_AFTER_PACKAGE:-0}" = "1" ]; then
    bash "${SCRIPT_DIR}/install.sh"
fi
