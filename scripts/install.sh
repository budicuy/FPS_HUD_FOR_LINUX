#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
DIST_DIR="$PROJECT_DIR/dist"

echo "[*] Compiling FPSHUD from source..."
if command -v ninja >/dev/null 2>&1; then
    cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release "$PROJECT_DIR"
else
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release "$PROJECT_DIR"
fi
cmake --build "$BUILD_DIR" --parallel

mkdir -p "$DIST_DIR"
tar -czf "$DIST_DIR/fpshud-linux-x86_64.tar.gz" -C "$BUILD_DIR" libfpshud.so fpshud_test

cd "$PROJECT_DIR"
bash "$PROJECT_DIR/install.sh"
