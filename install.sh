#!/usr/bin/env bash
set -e

REPO_OWNER="budicuy"
REPO_NAME="FPS_HUD_FOR_LINUX"
RELEASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/latest/download/fpshud-linux-x86_64.tar.gz"
RAW_DIST_URL="https://raw.githubusercontent.com/${REPO_OWNER}/${REPO_NAME}/main/dist/fpshud-linux-x86_64.tar.gz"

LIB_DIR="$HOME/.local/lib/fpshud"
BIN_DIR="$HOME/.local/bin"
VULKAN_LAYER_DIR="$HOME/.local/share/vulkan/implicit_layer.d"
FONT_DIR="$HOME/.local/share/fpshud/fonts"

echo "=================================================================="
echo "  FPSHUD — 1-Command Installer for Linux (Vulkan & OpenGL)"
echo "=================================================================="

mkdir -p "$LIB_DIR" "$BIN_DIR" "$VULKAN_LAYER_DIR" "$FONT_DIR"

TMP_DIR="$(mktemp -d /tmp/fpshud-install.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

INSTALLED_BIN=0

# 1. If executed from a local checkout with build/libfpshud.so or dist/ tarball
if [ -f "./build/libfpshud.so" ]; then
    echo "[*] Installing from local build directory..."
    cp -f "./build/libfpshud.so" "$LIB_DIR/libfpshud.so"
    [ -f "./build/fpshud_test" ] && cp -f "./build/fpshud_test" "$BIN_DIR/fpshud_test"
    INSTALLED_BIN=1
elif [ -f "./dist/fpshud-linux-x86_64.tar.gz" ]; then
    echo "[*] Extracting from local prebuilt package..."
    tar -xzf "./dist/fpshud-linux-x86_64.tar.gz" -C "$TMP_DIR"
    cp -f "$TMP_DIR/libfpshud.so" "$LIB_DIR/libfpshud.so"
    [ -f "$TMP_DIR/fpshud_test" ] && cp -f "$TMP_DIR/fpshud_test" "$BIN_DIR/fpshud_test"
    INSTALLED_BIN=1
fi

# 2. Download prebuilt portable binary via curl (Zero build packages required!)
if [ "$INSTALLED_BIN" -eq 0 ]; then
    echo "[*] Downloading prebuilt portable FPSHUD binary (no compilation needed)..."
    if curl -fsSL "$RELEASE_URL" -o "$TMP_DIR/fpshud.tar.gz" 2>/dev/null || \
       curl -fsSL "$RAW_DIST_URL" -o "$TMP_DIR/fpshud.tar.gz"; then
        tar -xzf "$TMP_DIR/fpshud.tar.gz" -C "$TMP_DIR"
        cp -f "$TMP_DIR/libfpshud.so" "$LIB_DIR/libfpshud.so"
        if [ -f "$TMP_DIR/fpshud_test" ]; then
            cp -f "$TMP_DIR/fpshud_test" "$BIN_DIR/fpshud_test"
            chmod +x "$BIN_DIR/fpshud_test"
        fi
        INSTALLED_BIN=1
    else
        echo "[!] Failed to download prebuilt binary. Falling back to source build..."
        git clone --depth 1 "https://github.com/${REPO_OWNER}/${REPO_NAME}.git" "$TMP_DIR/src"
        if command -v ninja >/dev/null 2>&1; then
            cmake -B "$TMP_DIR/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "$TMP_DIR/src"
        else
            cmake -B "$TMP_DIR/build" -DCMAKE_BUILD_TYPE=Release "$TMP_DIR/src"
        fi
        cmake --build "$TMP_DIR/build" --parallel
        cp -f "$TMP_DIR/build/libfpshud.so" "$LIB_DIR/libfpshud.so"
        cp -f "$TMP_DIR/build/fpshud_test" "$BIN_DIR/fpshud_test"
        INSTALLED_BIN=1
    fi
fi

chmod +x "$LIB_DIR/libfpshud.so"
[ -f "$BIN_DIR/fpshud_test" ] && chmod +x "$BIN_DIR/fpshud_test"

# 3. Copy optional system fonts if available on the host distro
for f in \
    /usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf \
    /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf \
    /usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-Bold.ttf \
    /usr/share/fonts/TTF/OpenSans-Bold.ttf \
    /usr/share/fonts/truetype/open-sans/OpenSans-Bold.ttf \
    /usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf \
    /usr/share/fonts/TTF/VeraMoBd.ttf \
    /usr/share/fonts/truetype/ttf-bitstream-vera/VeraMoBd.ttf; do
    if [ -f "$f" ]; then
        cp -f "$f" "$FONT_DIR/" 2>/dev/null || true
    fi
done

# 4. Generate Vulkan Implicit Layer JSON manifest pointing to ~/.local/lib/fpshud/libfpshud.so
cat > "$VULKAN_LAYER_DIR/VkLayer_FPSHUD_overlay.json" <<EOF
{
    "file_format_version": "1.2.1",
    "layer": {
        "name": "VK_LAYER_FPSHUD_overlay",
        "type": "GLOBAL",
        "library_path": "$LIB_DIR/libfpshud.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "FPSHUD Minimalist Performance Overlay Layer",
        "functions": {
            "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion",
            "vkGetInstanceProcAddr": "vkGetInstanceProcAddr",
            "vkGetDeviceProcAddr": "vkGetDeviceProcAddr"
        },
        "disable_environment": {
            "DISABLE_FPS_HUD": "1"
        }
    }
}
EOF

# 5. Generate OpenGL Wrapper Script (~/.local/bin/fpshud)
cat > "$BIN_DIR/fpshud" <<EOF
#!/usr/bin/env bash
# Wrapper script for FPSHUD (enables OpenGL LD_PRELOAD + Vulkan Layer automatically)

if [ -z "\$FPS_HUD" ]; then
    export FPS_HUD=1
fi

export LD_PRELOAD="$LIB_DIR/libfpshud.so\${LD_PRELOAD:+:\$LD_PRELOAD}"
exec "\$@"
EOF
chmod +x "$BIN_DIR/fpshud"

# ====================================================================
# 6. Hardware Telemetry Permissions (RAM Speed, CPU Wattage & Wayland Hotkeys)
# ====================================================================
HAVE_SUDO=0
SUDO_CMD="sudo"

if [ "$EUID" -eq 0 ]; then
    HAVE_SUDO=1
    SUDO_CMD=""
elif command -v sudo >/dev/null 2>&1 && [ -z "$FPSHUD_NO_SUDO" ] && [ -z "$CI" ]; then
    if sudo -n true 2>/dev/null; then
        HAVE_SUDO=1
        SUDO_CMD="sudo"
    elif [ -e /dev/tty ]; then
        echo ""
        echo "[*] Requesting sudo permission to configure hardware sensors permanently:"
        echo "    • RAM Speed (MT/s) & Memory Generation (DDR4 / DDR5 / LPDDR5)"
        echo "    • CPU Package Power (Watts via intel-rapl / amd-rapl)"
        echo "    • In-game GUI hotkeys (Insert/F12) & mouse capture on Wayland"
        echo ""
        if sudo </dev/tty -v; then
            HAVE_SUDO=1
            SUDO_CMD="sudo"
        else
            echo "[!] Sudo permission skipped or failed. Non-privileged sensors will continue to function."
        fi
    fi
fi

RAM_INFO_FILE="$HOME/.local/share/fpshud/ram_info"
RAM_PY='
import glob, struct, os, re, subprocess

types = {
    0x12: "DDR", 0x13: "DDR2", 0x18: "DDR3", 0x1A: "DDR4",
    0x1B: "LPDDR", 0x1C: "LPDDR2", 0x1D: "LPDDR3", 0x1E: "LPDDR4",
    0x22: "DDR5", 0x23: "LPDDR5"
}

speed = 0
ram_type = "RAM"
modules = 0

for raw_path in glob.glob("/sys/firmware/dmi/entries/17-*/raw"):
    try:
        with open(raw_path, "rb") as f:
            b = f.read()
        if len(b) < 0x17 or b[0] != 17:
            continue
        length = min(b[1], len(b))
        size = struct.unpack_from("<H", b, 0x0C)[0]
        if size == 0 or size == 0xFFFF:
            continue
        modules += 1
        if length > 0x12 and ram_type == "RAM":
            t = types.get(b[0x12])
            if t: ram_type = t
        rated = struct.unpack_from("<H", b, 0x15)[0] if length > 0x16 else 0
        conf = struct.unpack_from("<H", b, 0x20)[0] if length > 0x21 else 0
        if rated == 0xFFFF and length >= 0x58:
            rated = struct.unpack_from("<I", b, 0x54)[0]
        if conf == 0xFFFF and length >= 0x5C:
            conf = struct.unpack_from("<I", b, 0x58)[0]
        eff = conf if conf > 0 else rated
        if eff > speed:
            speed = eff
    except Exception:
        pass

if speed == 0:
    for cmd in ["/usr/sbin/dmidecode", "/usr/bin/dmidecode", "dmidecode"]:
        try:
            out = subprocess.check_output([cmd, "-t", "memory"], text=True, stderr=subprocess.DEVNULL)
            for block in out.split("Memory Device"):
                m_size = re.search(r"Size:\s+(\d+)\s+([MG]B)", block)
                if not m_size or "No Module Installed" in block:
                    continue
                modules += 1
                m_type = re.search(r"Type:\s+([A-Za-z0-9\-]+)", block)
                if m_type and ram_type == "RAM":
                    ram_type = m_type.group(1).replace("-", "")
                m_conf = re.search(r"Configured Memory Speed:\s+(\d+)\s+MT/s", block) or \
                         re.search(r"Configured Clock Speed:\s+(\d+)\s+MHz", block)
                m_speed = re.search(r"Speed:\s+(\d+)\s+(?:MT/s|MHz)", block)
                sp = int(m_conf.group(1)) if m_conf else (int(m_speed.group(1)) if m_speed else 0)
                if sp > speed:
                    speed = sp
            break
        except Exception:
            pass

print(f"type={ram_type}")
print(f"speed_mts={speed}")
print(f"rated_mts={speed}")
print(f"modules={modules}")
'

if command -v python3 >/dev/null 2>&1; then
    TMP_RAM="$TMP_DIR/ram_info"
    if [ "$HAVE_SUDO" -eq 1 ]; then
        $SUDO_CMD python3 -c "$RAM_PY" > "$TMP_RAM" 2>/dev/null || true
    else
        python3 -c "$RAM_PY" > "$TMP_RAM" 2>/dev/null || true
    fi
    if [ -s "$TMP_RAM" ]; then
        cp -f "$TMP_RAM" "$RAM_INFO_FILE"
        chmod 644 "$RAM_INFO_FILE" 2>/dev/null || true
        if [ "$HAVE_SUDO" -eq 1 ]; then
            $SUDO_CMD mkdir -p /etc/fpshud 2>/dev/null || true
            $SUDO_CMD cp -f "$TMP_RAM" /etc/fpshud/ram_info 2>/dev/null || true
            $SUDO_CMD chmod 644 /etc/fpshud/ram_info 2>/dev/null || true
        fi
    fi
fi

if [ "$HAVE_SUDO" -eq 1 ]; then
    echo "[*] Configuring system udev rules for full sensor access..."
    # 1. RAPL CPU Wattage Rule
    $SUDO_CMD mkdir -p /etc/udev/rules.d 2>/dev/null || true
    echo 'SUBSYSTEM=="powercap", KERNEL=="*rapl:*", RUN+="/bin/chmod a+r /sys/class/powercap/%k/energy_uj"' | \
        $SUDO_CMD tee /etc/udev/rules.d/99-fpshud-rapl.rules >/dev/null 2>&1 || true
    $SUDO_CMD chmod a+r /sys/class/powercap/*rapl*/energy_uj 2>/dev/null || true

    # 2. Wayland Hotkeys & Mouse Input Rule
    echo 'KERNEL=="event*", SUBSYSTEM=="input", MODE="0664", GROUP="input"' | \
        $SUDO_CMD tee /etc/udev/rules.d/99-fpshud-input.rules >/dev/null 2>&1 || true
    $SUDO_CMD usermod -aG input "$USER" 2>/dev/null || true
    $SUDO_CMD chmod a+r /dev/input/event* 2>/dev/null || true

    # Reload udev rules
    $SUDO_CMD udevadm control --reload-rules 2>/dev/null || true
    $SUDO_CMD udevadm trigger 2>/dev/null || true
    echo "[✓] Full hardware permissions configured successfully!"
fi

echo ""
echo "=================================================================="
echo "  FPSHUD Successfully Installed!"
echo "=================================================================="
echo "  - Overlay Library       : $LIB_DIR/libfpshud.so"
echo "  - Vulkan Implicit Layer : $VULKAN_LAYER_DIR/VkLayer_FPSHUD_overlay.json"
echo "  - OpenGL Wrapper Script : $BIN_DIR/fpshud"
echo "  - Diagnostic CLI Tool   : $BIN_DIR/fpshud_test"
echo ""
echo "  Quick Start:"
echo "  - Steam Launch Options  : FPS_HUD=1 %command%"
echo "  - Lutris / Heroic       : Add Environment Variable FPS_HUD=1"
echo "  - Terminal (Vulkan)     : FPS_HUD=1 ./your_game"
echo "  - Terminal (OpenGL)     : ~/.local/bin/fpshud ./your_game"
echo "  - Open In-Game GUI      : Press [Insert], [F12], or [F10] in-game"
echo "=================================================================="
