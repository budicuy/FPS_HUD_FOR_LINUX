#!/usr/bin/env bash
# test_e2e_install.sh - Automated End-to-End Installation & Uninstallation Lifecycle Test
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=========================================================="
echo "  FPSHUD End-to-End Installation & Uninstallation Test"
echo "=========================================================="

TEST_HOME="$(mktemp -d /tmp/fpshud_e2e_home.XXXXXX)"
export HOME="${TEST_HOME}"
export CI=1

cleanup() {
    rm -rf "${TEST_HOME}"
}
trap cleanup EXIT

echo "[1/4] Running install.sh in sandbox home (${TEST_HOME})..."
(cd "${ROOT_DIR}" && ./install.sh < /dev/null)

echo "[2/4] Verifying installed artifacts and permissions..."
test -d "${TEST_HOME}/.local/lib/fpshud" || { echo "FAIL: lib dir missing"; exit 1; }
test -f "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" || { echo "FAIL: libfpshud.so missing"; exit 1; }
test -x "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" || { echo "FAIL: libfpshud.so not executable"; exit 1; }
test -f "${TEST_HOME}/.local/bin/fpshud" || { echo "FAIL: fpshud binary missing"; exit 1; }
test -x "${TEST_HOME}/.local/bin/fpshud" || { echo "FAIL: fpshud binary not executable"; exit 1; }
test ! -f "${TEST_HOME}/.local/bin/fpshud_test" || { echo "FAIL: fpshud_test should not be placed in ~/.local/bin"; exit 1; }
test -f "${TEST_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json" || { echo "FAIL: Vulkan layer manifest missing"; exit 1; }

# Verify layer manifest references the sandbox libfpshud.so
grep -q "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" "${TEST_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json" || {
    echo "FAIL: Vulkan manifest does not reference sandbox libfpshud.so"
    exit 1
}

# Verify wrapper execution
echo "[3/4] Testing wrapper execution..."
"${TEST_HOME}/.local/bin/fpshud" --help >/dev/null || { echo "FAIL: fpshud --help failed"; exit 1; }

echo "[4/4] Running uninstall.sh in sandbox home..."
(cd "${ROOT_DIR}" && ./uninstall.sh)

# Verify clean removal
test ! -d "${TEST_HOME}/.local/lib/fpshud" || { echo "FAIL: lib dir was not removed"; exit 1; }
test ! -f "${TEST_HOME}/.local/bin/fpshud" || { echo "FAIL: fpshud wrapper was not removed"; exit 1; }
test ! -f "${TEST_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json" || { echo "FAIL: layer manifest was not removed"; exit 1; }

echo "=========================================================="
echo "  [✓] E2E Install & Uninstall lifecycle passed with zero residue!"
echo "=========================================================="
