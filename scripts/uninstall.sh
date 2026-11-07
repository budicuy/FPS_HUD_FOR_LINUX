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

# Remove system-level udev rules, /etc/fpshud, and input group membership if installed
TARGET_USER="${SUDO_USER:-$USER}"
if [ -f /etc/udev/rules.d/99-fpshud-rapl.rules ] || [ -f /etc/udev/rules.d/99-fpshud-input.rules ] || [ -d /etc/fpshud ]; then
    if [ "$EUID" -eq 0 ]; then
        rm -f /etc/udev/rules.d/99-fpshud-rapl.rules /etc/udev/rules.d/99-fpshud-input.rules
        rm -rf /etc/fpshud
        udevadm control --reload-rules 2>/dev/null || true
        if [ -n "$TARGET_USER" ] && groups "$TARGET_USER" 2>/dev/null | grep -qw "input"; then
            if command -v gpasswd >/dev/null 2>&1; then
                gpasswd -d "$TARGET_USER" input 2>/dev/null && echo "[✓] Removed '$TARGET_USER' from 'input' group." || true
            elif command -v deluser >/dev/null 2>&1; then
                deluser "$TARGET_USER" input 2>/dev/null && echo "[✓] Removed '$TARGET_USER' from 'input' group." || true
            fi
        fi
    elif command -v sudo >/dev/null 2>&1 && [ -z "$FPSHUD_NO_SUDO" ] && [ -z "$CI" ]; then
        if sudo -n true 2>/dev/null; then
            sudo rm -f /etc/udev/rules.d/99-fpshud-rapl.rules /etc/udev/rules.d/99-fpshud-input.rules
            sudo rm -rf /etc/fpshud
            sudo udevadm control --reload-rules 2>/dev/null || true
            if [ -n "$TARGET_USER" ] && groups "$TARGET_USER" 2>/dev/null | grep -qw "input"; then
                if command -v gpasswd >/dev/null 2>&1; then
                    sudo gpasswd -d "$TARGET_USER" input 2>/dev/null && echo "[✓] Removed '$TARGET_USER' from 'input' group." || true
                elif command -v deluser >/dev/null 2>&1; then
                    sudo deluser "$TARGET_USER" input 2>/dev/null && echo "[✓] Removed '$TARGET_USER' from 'input' group." || true
                fi
            fi
        elif [ -t 0 ] || ( : < /dev/tty ) 2>/dev/null; then
            echo "[*] Optional system udev rule detected (/etc/udev/rules.d/99-fpshud-rapl.rules)."
            UNINSTALL_SUDO_CONFIRM=0
            if [ "${FORCE_UNINSTALL_SUDO:-0}" = "1" ] || [ "${1:-}" = "-y" ] || [ "${1:-}" = "--yes" ]; then
                UNINSTALL_SUDO_CONFIRM=1
            elif [ -t 0 ]; then
                read -r -p "Remove system udev rule via sudo? / Hapus aturan udev sistem via sudo? [y/N] " confirm || confirm="n"
                case "$confirm" in
                    [yY][eE][sS]|[yY]) UNINSTALL_SUDO_CONFIRM=1 ;;
                esac
            else
                read -r -p "Remove system udev rule via sudo? / Hapus aturan udev sistem via sudo? [y/N] " confirm < /dev/tty || confirm="n"
                case "$confirm" in
                    [yY][eE][sS]|[yY]) UNINSTALL_SUDO_CONFIRM=1 ;;
                esac
            fi
            if [ "$UNINSTALL_SUDO_CONFIRM" -eq 1 ]; then
                if sudo </dev/tty -v 2>/dev/null || sudo -v; then
                    sudo rm -f /etc/udev/rules.d/99-fpshud-rapl.rules /etc/udev/rules.d/99-fpshud-input.rules
                    sudo rm -rf /etc/fpshud
                    sudo udevadm control --reload-rules 2>/dev/null || true
                    if [ -n "$TARGET_USER" ] && groups "$TARGET_USER" 2>/dev/null | grep -qw "input"; then
                        if command -v gpasswd >/dev/null 2>&1; then
                            sudo gpasswd -d "$TARGET_USER" input 2>/dev/null && echo "[✓] Removed '$TARGET_USER' from 'input' group." || true
                        elif command -v deluser >/dev/null 2>&1; then
                            sudo deluser "$TARGET_USER" input 2>/dev/null && echo "[✓] Removed '$TARGET_USER' from 'input' group." || true
                        fi
                    fi
                fi
            else
                echo "[*] Skipped sudo udev rule removal. All user-space FPSHUD files have been removed."
            fi
        fi
    fi
fi

echo "[*] FPSHUD (including all binaries, layers, udev rules, and configs) has been completely removed from your system."
