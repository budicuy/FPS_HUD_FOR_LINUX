#!/usr/bin/env bash
set -e

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

mkdir -p "$DIST_DIR"
tar -czf "$DIST_DIR/fpshud-linux-x86_64.tar.gz" -C "$BUILD_DIR" "${TAR_FILES[@]}"

cd "$PROJECT_DIR"
bash "$PROJECT_DIR/install.sh"
