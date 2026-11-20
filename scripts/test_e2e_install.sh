#!/usr/bin/env bash
# scripts/test_e2e_install.sh — Zero-Sudo Install & Uninstall Lifecycle Verification
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=========================================================="
echo "  FPSHUD Zero-Sudo Install & Uninstall Lifecycle Test"
echo "=========================================================="

TEST_HOME="$(mktemp -d /tmp/fpshud_e2e_home.XXXXXX)"
export HOME="${TEST_HOME}"
export CI=1

cleanup() {
    rm -rf "${TEST_HOME}"
}
trap cleanup EXIT

echo "[1/4] Testing root install.sh (release tarball + SHA256 check)..."
(cd "${ROOT_DIR}" && ./install.sh < /dev/null)

test -x "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" || { echo "FAIL: libfpshud.so missing"; exit 1; }
test -x "${TEST_HOME}/.local/lib/fpshud/fpshud-ui" || { echo "FAIL: fpshud-ui missing"; exit 1; }
test -x "${TEST_HOME}/.local/lib/fpshud/uninstall.sh" || { echo "FAIL: uninstall.sh missing"; exit 1; }
test -f "${TEST_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json" || { echo "FAIL: Vulkan manifest missing"; exit 1; }
grep -q "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" "${TEST_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json"

echo "[2/4] Verifying tampered SHA256 rejection..."
BAD_HASH_FILE="$(mktemp /tmp/fpshud_bad_hash.XXXXXX)"
echo "0000000000000000000000000000000000000000000000000000000000000000  fpshud.tar.gz" > "$BAD_HASH_FILE"
SECURITY_FAILED=0
(cd "${ROOT_DIR}" && FPSHUD_RELEASE_SHA_URL="file://${BAD_HASH_FILE}" ./install.sh < /dev/null >/dev/null 2>&1) || SECURITY_FAILED=1
rm -f "$BAD_HASH_FILE"
if [ "$SECURITY_FAILED" -ne 1 ]; then
    echo "FAIL: install.sh did not abort on invalid SHA256 checksum!"
    exit 1
fi

echo "[3/4] Testing local developer installer (scripts/install.sh)..."
rm -rf "${TEST_HOME}/.local"
(cd "${ROOT_DIR}" && ./scripts/install.sh >/dev/null)
test -x "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" || { echo "FAIL: dev libfpshud.so missing"; exit 1; }
test -x "${TEST_HOME}/.local/lib/fpshud/fpshud-ui" || { echo "FAIL: dev fpshud-ui missing"; exit 1; }
grep -q "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" "${TEST_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json"
if grep -q "/target/release/" "${TEST_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json"; then
    echo "FAIL: Vulkan manifest leaked build directory path!"
    exit 1
fi

echo "[4/4] Running uninstall.sh and verifying zero residue..."
"${TEST_HOME}/.local/lib/fpshud/uninstall.sh" >/dev/null
test ! -d "${TEST_HOME}/.local/lib/fpshud" || { echo "FAIL: lib dir residue remains"; exit 1; }
test ! -f "${TEST_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json" || { echo "FAIL: layer manifest residue remains"; exit 1; }

echo "=========================================================="
echo "  [✓] Zero-Sudo Install & Uninstall Lifecycle PASSED!"
echo "=========================================================="
