#!/usr/bin/env bash
# scripts/build_rust_release.sh — Pure-Rust release builder for libfpshud.so & fpshud-ui
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

if command -v glslc >/dev/null 2>&1; then
    for shader in shaders/quad.vert shaders/quad.frag; do
        spv="${shader}.spv"
        if [ ! -f "$spv" ] || [ "$shader" -nt "$spv" ]; then
            glslc -O "$shader" -o "$spv"
        fi
    done
fi

USE_ZIGBUILD=0
if [ "${FPSHUD_ZIGBUILD:-0}" = "1" ]; then
    USE_ZIGBUILD=1
elif command -v cargo-zigbuild >/dev/null 2>&1 && command -v zig >/dev/null 2>&1; then
    HOST_GLIBC="$(ldd --version 2>/dev/null | head -n 1 | grep -oE '[0-9]+\.[0-9]+$' || true)"
    if [ -n "$HOST_GLIBC" ] && printf '%s\n2.36\n' "$HOST_GLIBC" | sort -VC 2>/dev/null; then
        USE_ZIGBUILD=0
    elif [ -n "$HOST_GLIBC" ]; then
        USE_ZIGBUILD=1
    fi
fi

if [ "$USE_ZIGBUILD" -eq 1 ]; then
    echo "[*] Building release artifacts via cargo-zigbuild (GLIBC 2.35 floor)..."
    cargo zigbuild --release --target x86_64-unknown-linux-gnu.2.35 -p fpshud-layer -p fpshud-ui-bin
    mkdir -p target/release
    cp -f target/x86_64-unknown-linux-gnu/release/libfpshud.so target/release/libfpshud.so
    cp -f target/x86_64-unknown-linux-gnu/release/fpshud-ui target/release/fpshud-ui
else
    echo "[*] Building release artifacts via cargo build --release..."
    cargo build --release -p fpshud-layer -p fpshud-ui-bin
fi

echo "[✓] Built target/release/libfpshud.so and target/release/fpshud-ui"
