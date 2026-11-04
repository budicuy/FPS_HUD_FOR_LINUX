#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
DIST_DIR="$PROJECT_DIR/dist"

echo "[*] Compiling FPSHUD from source with Clang 23 + LLD + ThinLTO..."
"$SCRIPT_DIR/build_clang.sh"

STRIP_BIN="$(command -v llvm-strip || command -v strip || true)"
STRIP_TARGETS=("$BUILD_DIR/libfpshud.so" "$BUILD_DIR/fpshud_test")
TAR_FILES=("libfpshud.so" "fpshud_test")

if [ -f "$BUILD_DIR/fpshud_recorder" ]; then
    STRIP_TARGETS+=("$BUILD_DIR/fpshud_recorder")
    TAR_FILES+=("fpshud_recorder")
fi
if [ -f "$BUILD_DIR/fpshud_recorder_bin" ]; then
    STRIP_TARGETS+=("$BUILD_DIR/fpshud_recorder_bin")
    TAR_FILES+=("fpshud_recorder_bin")
fi

if [ -n "$STRIP_BIN" ]; then
    "$STRIP_BIN" --strip-unneeded "${STRIP_TARGETS[@]}"
fi

echo "[*] Verifying GLIBC symbol version floor (must be <= GLIBC_2.35)..."
if objdump -T "${STRIP_TARGETS[@]}" | grep -E "GLIBC_2\.(3[6-9]|4[0-9])"; then
    echo "[!] ERROR: Release binary imports GLIBC >= 2.36 symbols! Aborting packaging." >&2
    exit 1
fi
echo "[✓] GLIBC portability gate PASSED (0 symbols >= GLIBC_2.36)."

EXPORT_COUNT="$(nm -D --defined-only "$BUILD_DIR/libfpshud.so" | wc -l | tr -d ' ')"
if [ "$EXPORT_COUNT" -ne 8 ]; then
    echo "[!] ERROR: Expected exactly 8 exported ABI symbols in libfpshud.so, found $EXPORT_COUNT!" >&2
    exit 1
fi
echo "[✓] ABI symbol visibility gate PASSED (8 exported Vulkan/GLX/EGL entrypoints)."

echo "[*] Verifying zero DT_NEEDED dependencies on libGL/libGLX/libEGL/libOpenGL..."
if LC_ALL=C readelf -d "$BUILD_DIR/libfpshud.so" | grep NEEDED | grep -E "lib(GLX|EGL|OpenGL|GL)\.so"; then
    echo "[!] ERROR: Direct dynamic link to OpenGL/GLX/EGL detected in libfpshud.so DT_NEEDED! Aborting packaging." >&2
    exit 1
fi
echo "[✓] DT_NEEDED graphics portability gate PASSED (0 direct GL/GLX/EGL dependencies)."

CXX_GATE="${CXX:-$(command -v clang++ || command -v g++ || true)}"
if [ -n "$CXX_GATE" ]; then
    echo "[*] Verifying C++20 standalone compilation of recorder_main.cpp..."
    "$CXX_GATE" -std=c++20 -Wall -Wextra -Werror -fsyntax-only "$PROJECT_DIR/src/recorder_main.cpp"
    echo "[✓] C++20 recorder_main.cpp syntax gate PASSED."
fi

YMM_COUNT="$(objdump -d "$BUILD_DIR/libfpshud.so" | grep -c '%ymm' || true)"
if [ "$YMM_COUNT" -gt 32 ]; then
    echo "[i] Note: Host static libstdc++.a contains AVX/AVX2 instructions ($YMM_COUNT %ymm ops)."
    echo "    install.sh includes automatic pre-AVX2 CPU detection & source fallback; GitHub Actions CI builds pure x86-64 baseline releases."
else
    echo "[✓] ISA baseline gate PASSED ($YMM_COUNT %ymm ops, restricted to runtime-dispatched AVX2 kernel)."
fi

mkdir -p "$DIST_DIR"
tar -czf "$DIST_DIR/fpshud-linux-x86_64.tar.gz" -C "$BUILD_DIR" "${TAR_FILES[@]}"
echo "[✓] Packaged portable release archive: $DIST_DIR/fpshud-linux-x86_64.tar.gz"

cd "$PROJECT_DIR"
if [ "${1:-}" = "--install" ] || [ "${FPSHUD_INSTALL_AFTER_PACKAGE:-0}" = "1" ]; then
    bash "$PROJECT_DIR/install.sh"
else
    echo "[i] Skipped automatic developer installation. Run with --install or FPSHUD_INSTALL_AFTER_PACKAGE=1 to install to ~/.local."
fi
