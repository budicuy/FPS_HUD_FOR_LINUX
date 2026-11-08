#!/usr/bin/env bash
# scripts/build_static_ffmpeg.sh - Compiles ultra-minimal static FFmpeg (<4MB) for single-binary FPSHUD recorder
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
OUT_ARG="${1:-$PROJECT_DIR/build/ffmpeg_minimal}"
mkdir -p "$OUT_ARG"
OUT_DIR="$(cd "$OUT_ARG" && pwd)"
BUILD_CACHE="${FFMPEG_BUILD_CACHE:-/tmp/fpshud_ffmpeg_src_$(id -u)}"

if [ -f "$OUT_DIR/lib/libavformat.a" ] && [ -f "$OUT_DIR/lib/libavcodec.a" ] && \
   [ -f "$OUT_DIR/lib/libswscale.a" ] && [ -f "$OUT_DIR/lib/libswresample.a" ] && \
   [ -f "$OUT_DIR/lib/libavutil.a" ]; then
    echo "[✓] Minimal static FFmpeg already built at: $OUT_DIR"
    exit 0
fi

echo "=================================================================="
echo "  Building Minimal Hardware-Only Static FFmpeg (< 4 MB)"
echo "  Target Output: $OUT_DIR"
echo "=================================================================="

mkdir -p "$BUILD_CACHE"
chmod 700 "$BUILD_CACHE"
mkdir -p "$OUT_DIR"
export PKG_CONFIG_PATH="$OUT_DIR/lib/pkgconfig:$OUT_DIR/share/pkgconfig:${PKG_CONFIG_PATH:-}"

# 1. Install ffnvcodec headers if not present in system
if ! pkg-config --exists ffnvcodec 2>/dev/null; then
    echo "[*] Installing ffnvcodec (NVIDIA NVENC) headers to $OUT_DIR..."
    NV_CACHE="$BUILD_CACHE/nv_headers"
    rm -rf "$NV_CACHE"
    git clone --depth 1 https://git.videolan.org/git/ffmpeg/nv-codec-headers.git "$NV_CACHE"
    make -C "$NV_CACHE" install PREFIX="$OUT_DIR"
fi

cd "$BUILD_CACHE"

FFMPEG_VERSION="7.1"
FFMPEG_TAR="ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/${FFMPEG_TAR}"
FFMPEG_SHA256="40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6"

if [ ! -d "ffmpeg-${FFMPEG_VERSION}" ]; then
    if [ ! -f "$FFMPEG_TAR" ]; then
        echo "[*] Downloading FFmpeg ${FFMPEG_VERSION} source archive..."
        curl -fsSL "$FFMPEG_URL" -o "$FFMPEG_TAR" || {
            echo "[*] Fallback: Cloning shallow git release branch..."
            git clone --depth 1 -b "release/${FFMPEG_VERSION}" https://github.com/FFmpeg/FFmpeg.git "ffmpeg-${FFMPEG_VERSION}"
        }
    fi
    if [ -f "$FFMPEG_TAR" ] && [ ! -d "ffmpeg-${FFMPEG_VERSION}" ]; then
        echo "[*] Verifying SHA256 checksum for ${FFMPEG_TAR}..."
        echo "${FFMPEG_SHA256}  ${FFMPEG_TAR}" | sha256sum -c - || {
            echo "[!] ERROR: SHA256 verification failed for ${FFMPEG_TAR}!" >&2
            rm -f "$FFMPEG_TAR"
            exit 1
        }
        echo "[*] Extracting ${FFMPEG_TAR}..."
        tar -xf "$FFMPEG_TAR"
    fi
fi

cd "ffmpeg-${FFMPEG_VERSION}"

echo "[*] Configuring ultra-minimal static FFmpeg (Hardware NVENC + VAAPI + MP4 only)..."
./configure \
    --prefix="$OUT_DIR" \
    --enable-static \
    --disable-shared \
    --disable-all \
    --enable-avcodec \
    --enable-avformat \
    --enable-avutil \
    --enable-swscale \
    --enable-swresample \
    --enable-ffnvcodec \
    --enable-nvenc \
    --enable-vaapi \
    --enable-encoder=h264_nvenc,hevc_nvenc,h264_vaapi,hevc_vaapi,aac \
    --enable-muxer=mp4,matroska,webm \
    --enable-protocol=file \
    --enable-hwaccel=h264_vaapi,hevc_vaapi \
    --disable-x86asm \
    --disable-programs \
    --disable-doc \
    --disable-network \
    --disable-xlib \
    --disable-vdpau \
    --disable-sdl2 \
    --disable-alsa \
    --disable-sndio \
    --disable-libxcb \
    --extra-cflags="-O3 -fPIC -ffunction-sections -fdata-sections" \
    --extra-cxxflags="-O3 -fPIC -ffunction-sections -fdata-sections"

echo "[*] Compiling minimal static FFmpeg with $(nproc) parallel threads..."
make -j"$(nproc)"
make install

echo "=================================================================="
echo "  Minimal Static FFmpeg Built Successfully!"
ls -lh "$OUT_DIR/lib"/*.a
echo "=================================================================="
