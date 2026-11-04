#!/usr/bin/env bash
set -e

echo "=================================================================="
echo "  Uninstalling FPSHUD..."
echo "=================================================================="

rm -rf "$HOME/.local/lib/fpshud"
rm -f "$HOME/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json"
rm -f "$HOME/.local/bin/fpshud"
rm -f "$HOME/.local/bin/fpshud_test"
rm -f "$HOME/.local/bin/fpshud_recorder"
rm -f "$HOME/.local/bin/fpshud_recorder_bin"
rm -f "$HOME/.var/app/com.valvesoftware.Steam/data/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json"
rm -rf "$HOME/.local/share/fpshud"
rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/fpshud"
rm -rf "$HOME/.config/fpshud"

# Reset Flatpak Steam overrides if flatpak is available
if command -v flatpak >/dev/null 2>&1; then
    flatpak override --user --nofilesystem="$HOME/.local/lib/fpshud" com.valvesoftware.Steam 2>/dev/null || true
fi

# Remove system-level udev rules & /etc/fpshud if installed
if [ -f /etc/udev/rules.d/99-fpshud-rapl.rules ] || [ -f /etc/udev/rules.d/99-fpshud-input.rules ] || [ -d /etc/fpshud ]; then
    if [ "$EUID" -eq 0 ]; then
        rm -f /etc/udev/rules.d/99-fpshud-rapl.rules /etc/udev/rules.d/99-fpshud-input.rules
        rm -rf /etc/fpshud
        udevadm control --reload-rules 2>/dev/null || true
    elif command -v sudo >/dev/null 2>&1 && [ -z "$FPSHUD_NO_SUDO" ] && [ -z "$CI" ]; then
        if sudo -n true 2>/dev/null; then
            sudo rm -f /etc/udev/rules.d/99-fpshud-rapl.rules /etc/udev/rules.d/99-fpshud-input.rules
            sudo rm -rf /etc/fpshud
            sudo udevadm control --reload-rules 2>/dev/null || true
        elif [ -e /dev/tty ]; then
            echo "[*] Requesting sudo to clean up system udev rules and /etc/fpshud..."
            if sudo </dev/tty -v; then
                sudo rm -f /etc/udev/rules.d/99-fpshud-rapl.rules /etc/udev/rules.d/99-fpshud-input.rules
                sudo rm -rf /etc/fpshud
                sudo udevadm control --reload-rules 2>/dev/null || true
            fi
        fi
    fi
fi

echo "[*] FPSHUD (including all binaries, layers, udev rules, and configs) has been completely removed from your system."
