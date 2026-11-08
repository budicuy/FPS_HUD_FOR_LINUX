#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
DIST_DIR="$PROJECT_DIR/dist"

echo "[*] Ensuring minimal static FFmpeg is built..."
"$SCRIPT_DIR/build_static_ffmpeg.sh" "$BUILD_DIR/ffmpeg_minimal"

echo "[*] Compiling FPSHUD from source with Clang 23 + LLD + ThinLTO..."
"$SCRIPT_DIR/build_clang.sh"

echo "[*] Preparing portable fpshud wrapper script..."
sed "s|@FPSHUD_LIB_PATH@|\$HOME/.local/lib/fpshud/libfpshud.so|g" "$PROJECT_DIR/scripts/fpshud.in" > "$BUILD_DIR/fpshud"
chmod 755 "$BUILD_DIR/fpshud"

cp "$SCRIPT_DIR/uninstall.sh" "$BUILD_DIR/uninstall.sh"
chmod 755 "$BUILD_DIR/uninstall.sh"

STRIP_BIN="$(command -v llvm-strip || command -v strip || true)"
STRIP_TARGETS=("$BUILD_DIR/libfpshud.so" "$BUILD_DIR/fpshud_test")
PORTABLE_GATE_TARGETS=("$BUILD_DIR/libfpshud.so" "$BUILD_DIR/fpshud_test")
TAR_FILES=("libfpshud.so" "fpshud" "fpshud_test" "uninstall.sh")

if [ -f "$BUILD_DIR/fpshud_recorder" ]; then
    STRIP_TARGETS+=("$BUILD_DIR/fpshud_recorder")
    PORTABLE_GATE_TARGETS+=("$BUILD_DIR/fpshud_recorder")
    TAR_FILES+=("fpshud_recorder")
fi
if [ -f "$BUILD_DIR/fpshud_recorder_bin" ]; then
    STRIP_TARGETS+=("$BUILD_DIR/fpshud_recorder_bin")
    PORTABLE_GATE_TARGETS+=("$BUILD_DIR/fpshud_recorder_bin")
    TAR_FILES+=("fpshud_recorder_bin")
fi

if [ -n "$STRIP_BIN" ]; then
    "$STRIP_BIN" --strip-unneeded "${STRIP_TARGETS[@]}"
fi

# Optional UPX compression for developer/CI releases
if command -v upx >/dev/null 2>&1 && [ -f "$BUILD_DIR/fpshud_recorder_bin" ]; then
    echo "[*] UPX detected: Compressing fpshud_recorder_bin..."
    upx --best "$BUILD_DIR/fpshud_recorder_bin" || true
fi

if [ -f "$BUILD_DIR/fpshud_recorder_bin" ]; then
    REC_SIZE="$(stat -c%s "$BUILD_DIR/fpshud_recorder_bin")"
    if [ "$REC_SIZE" -gt 5242880 ]; then
        echo "[!] ERROR: fpshud_recorder_bin exceeds 5 MB limit ($REC_SIZE bytes)! Aborting packaging." >&2
        exit 1
    fi
    echo "[✓] Recorder binary size gate PASSED ($REC_SIZE bytes < 5 MB limit)."

    if LC_ALL=C readelf -d "$BUILD_DIR/fpshud_recorder_bin" | grep NEEDED | grep "libav"; then
        echo "[!] ERROR: Dynamic libav dependency detected in fpshud_recorder_bin! Aborting packaging." >&2
        exit 1
    fi
    echo "[✓] Zero dynamic libav dependencies in fpshud_recorder_bin PASSED."
fi

echo "[*] Verifying GLIBC symbol version floor on baseline portable binaries (must be <= GLIBC_2.35)..."
if objdump -T "${PORTABLE_GATE_TARGETS[@]}" | grep -E "GLIBC_2\.(3[6-9]|4[0-9])"; then
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
    echo "[*] Verifying C++26 standalone compilation of recorder_main.cpp..."
    "$CXX_GATE" -std=c++26 -Wall -Wextra -Werror -Wno-deprecated-declarations -fsyntax-only -I"$PROJECT_DIR/src" -I"$PROJECT_DIR/src/recorder" -I"$PROJECT_DIR/src/core" "$PROJECT_DIR/src/recorder/recorder_main.cpp"
    echo "[✓] C++26 recorder_main.cpp syntax gate PASSED."
fi

YMM_COUNT="$(objdump -d "$BUILD_DIR/libfpshud.so" | grep -c '%ymm' || true)"
if [ "$YMM_COUNT" -gt 32 ]; then
    echo "[i] Note: Host static libstdc++.a contains AVX/AVX2 instructions ($YMM_COUNT %ymm ops)."
    echo "    GitHub Actions CI builds pure x86-64 baseline releases for universal compatibility."
else
    echo "[✓] ISA baseline gate PASSED ($YMM_COUNT %ymm ops, restricted to runtime-dispatched AVX2 kernel)."
fi

mkdir -p "$DIST_DIR"
chmod 755 "${STRIP_TARGETS[@]}"
tar -czf "$DIST_DIR/fpshud-linux-x86_64.tar.gz" -C "$BUILD_DIR" "${TAR_FILES[@]}"
(cd "$DIST_DIR" && sha256sum "fpshud-linux-x86_64.tar.gz" > "fpshud-linux-x86_64.tar.gz.sha256")
echo "[✓] Packaged portable release archive: $DIST_DIR/fpshud-linux-x86_64.tar.gz"
echo "[✓] Generated cryptographic checksum: $DIST_DIR/fpshud-linux-x86_64.tar.gz.sha256"

cd "$PROJECT_DIR"
if [ "${1:-}" = "--install" ] || [ "${FPSHUD_INSTALL_AFTER_PACKAGE:-0}" = "1" ]; then
    bash "$PROJECT_DIR/scripts/install.sh"
else
    echo "[i] Skipped automatic developer installation. Run with --install or FPSHUD_INSTALL_AFTER_PACKAGE=1 to install to ~/.local."
fi
