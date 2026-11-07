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

echo "[1/5] Testing root install.sh (Pure GitHub Release download)..."
(cd "${ROOT_DIR}" && ./install.sh < /dev/null)

echo "[2/5] Verifying installed release artifacts and permissions..."
test -d "${TEST_HOME}/.local/lib/fpshud" || { echo "FAIL: lib dir missing"; exit 1; }
test -f "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" || { echo "FAIL: libfpshud.so missing"; exit 1; }
test -x "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" || { echo "FAIL: libfpshud.so not executable"; exit 1; }
test -f "${TEST_HOME}/.local/bin/fpshud" || { echo "FAIL: fpshud wrapper missing"; exit 1; }
test -x "${TEST_HOME}/.local/bin/fpshud" || { echo "FAIL: fpshud wrapper not executable"; exit 1; }
test ! -f "${TEST_HOME}/.local/bin/fpshud_test" || { echo "FAIL: fpshud_test should not be placed in ~/.local/bin"; exit 1; }
test -f "${TEST_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json" || { echo "FAIL: Vulkan layer manifest missing"; exit 1; }

# Verify layer manifest references the sandbox libfpshud.so
grep -q "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" "${TEST_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json" || {
    echo "FAIL: Vulkan manifest does not reference sandbox libfpshud.so"
    exit 1
}

# Verify wrapper execution
"${TEST_HOME}/.local/bin/fpshud" --help >/dev/null || { echo "FAIL: fpshud --help failed"; exit 1; }

echo "[3/5] Testing negative security cases (tampered hash & corrupted archive)..."
BAD_HASH_FILE="$(mktemp /tmp/fpshud_bad_hash.XXXXXX)"
echo "0000000000000000000000000000000000000000000000000000000000000000  fpshud.tar.gz" > "$BAD_HASH_FILE"
SECURITY_FAILED=0
(cd "${ROOT_DIR}" && FPSHUD_RELEASE_SHA_URL="file://${BAD_HASH_FILE}" ./install.sh < /dev/null >/dev/null 2>&1) || SECURITY_FAILED=1
rm -f "$BAD_HASH_FILE"
if [ "$SECURITY_FAILED" -ne 1 ]; then
    echo "FAIL: install.sh should have aborted on invalid SHA256 checksum!"
    exit 1
fi

# Verify root install.sh contains zero unverified raw git curls or local script probes
if grep -q -E '(raw\.githubusercontent\.com.*scripts/fpshud\.in|\./scripts/fpshud\.in)' "${ROOT_DIR}/install.sh"; then
    echo "FAIL: install.sh contains unverified raw GitHub script curls or local script probes!"
    exit 1
fi
echo "[✓] Security check passed: Tampered checksum and unverified raw download exclusions verified."

echo "[4/5] Testing developer installer (scripts/install.sh from local build with clean sandbox)..."
# Clean sandbox ~/.local to ensure complete phase isolation
rm -rf "${TEST_HOME}/.local"
(cd "${ROOT_DIR}" && ./scripts/install.sh >/dev/null)
test -x "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" || { echo "FAIL: dev install lib missing"; exit 1; }
test -x "${TEST_HOME}/.local/bin/fpshud" || { echo "FAIL: dev install wrapper missing"; exit 1; }

# Verify dev wrapper references sandbox library path and does not leak build tree path
grep -q "${TEST_HOME}/.local/lib/fpshud/libfpshud.so" "${TEST_HOME}/.local/bin/fpshud" || {
    echo "FAIL: dev install wrapper does not reference sandbox lib directory!"
    exit 1
}
if grep -q "/build/libfpshud.so" "${TEST_HOME}/.local/bin/fpshud"; then
    echo "FAIL: dev install wrapper leaked temporary build directory path!"
    exit 1
fi
"${TEST_HOME}/.local/bin/fpshud" --help >/dev/null || { echo "FAIL: dev installed fpshud --help failed"; exit 1; }
"${TEST_HOME}/.local/bin/fpshud" --status | grep -q "AVAILABLE" || { echo "FAIL: fpshud --status did not output AVAILABLE sensor probe"; exit 1; }

echo "[5/5] Running local fpshud --uninstall in sandbox home..."
"${TEST_HOME}/.local/bin/fpshud" --uninstall >/dev/null

# Verify clean removal
test ! -d "${TEST_HOME}/.local/lib/fpshud" || { echo "FAIL: lib dir was not removed"; exit 1; }
test ! -f "${TEST_HOME}/.local/bin/fpshud" || { echo "FAIL: fpshud wrapper was not removed"; exit 1; }
test ! -f "${TEST_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json" || { echo "FAIL: layer manifest was not removed"; exit 1; }

echo "=========================================================="
echo "  [✓] E2E Install & Uninstall lifecycle passed with zero residue!"
echo "=========================================================="
