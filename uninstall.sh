#!/usr/bin/env bash
set -e

echo "=================================================================="
echo "  Uninstalling FPSHUD..."
echo "=================================================================="

rm -rf "$HOME/.local/lib/fpshud"
rm -f "$HOME/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json"
rm -f "$HOME/.local/bin/fpshud"
rm -f "$HOME/.local/bin/fpshud_test"
rm -rf "$HOME/.local/share/fpshud"
rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/fpshud"
rm -rf "$HOME/.config/fpshud"

# Remove system-level udev rules & /etc/fpshud if installed
if [ -f /etc/udev/rules.d/99-fpshud-rapl.rules ] || [ -f /etc/udev/rules.d/99-fpshud-input.rules ] || [ -d /etc/fpshud ]; then
    if [ "$EUID" -eq 0 ]; then
        rm -f /etc/udev/rules.d/99-fpshud-rapl.rules /etc/udev/rules.d/99-fpshud-input.rules
        rm -rf /etc/fpshud
        udevadm control --reload-rules 2>/dev/null || true
    elif command -v sudo >/dev/null 2>&1 && [ -z "$FPSHUD_NO_SUDO" ] && [ -z "$CI" ]; then
        if sudo -n true 2>/dev/null; then
            sudo -n rm -f /etc/udev/rules.d/99-fpshud-rapl.rules /etc/udev/rules.d/99-fpshud-input.rules
            sudo -n rm -rf /etc/fpshud
            sudo -n udevadm control --reload-rules 2>/dev/null || true
        elif [ -t 0 ] || [ -e /dev/tty ]; then
            read -r -t 10 -p "[?] Remove system udev rules with sudo? [Y/n] " U_ANSWER </dev/tty 2>/dev/null || U_ANSWER="n"
            case "$U_ANSWER" in
                [yY][eE][sS]|[yY]|"")
                    if sudo </dev/tty -v 2>/dev/null; then
                        sudo </dev/tty rm -f /etc/udev/rules.d/99-fpshud-rapl.rules /etc/udev/rules.d/99-fpshud-input.rules
                        sudo </dev/tty rm -rf /etc/fpshud
                        sudo </dev/tty udevadm control --reload-rules 2>/dev/null || true
                    fi
                    ;;
            esac
        fi
    fi
fi

echo "[*] FPSHUD (including all binaries, layers, udev rules, and configs) has been completely removed from your system."
