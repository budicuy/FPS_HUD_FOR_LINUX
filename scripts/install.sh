#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
DIST_DIR="$PROJECT_DIR/dist"

echo "[*] Compiling FPSHUD from source with Clang 23 + LLD + ThinLTO..."
"$SCRIPT_DIR/build_clang.sh"
llvm-strip --strip-unneeded "$BUILD_DIR/libfpshud.so" "$BUILD_DIR/fpshud_recorder" "$BUILD_DIR/fpshud_recorder_bin" "$BUILD_DIR/fpshud_test"

mkdir -p "$DIST_DIR"
tar -czf "$DIST_DIR/fpshud-linux-x86_64.tar.gz" -C "$BUILD_DIR" libfpshud.so fpshud_test fpshud_recorder fpshud_recorder_bin

cd "$PROJECT_DIR"
bash "$PROJECT_DIR/install.sh"
