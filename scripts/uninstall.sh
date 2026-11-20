#!/usr/bin/env bash
# scripts/uninstall.sh — Zero-sudo clean uninstaller
set -euo pipefail

echo "=================================================================="
echo "  Uninstalling FPSHUD (Zero-Sudo Clean Removal)..."
echo "=================================================================="

rm -rf "${HOME}/.local/lib/fpshud"
rm -f "${HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json"
rm -f "${HOME}/.var/app/com.valvesoftware.Steam/data/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json"
rm -rf "${HOME}/.local/share/fpshud"
rm -rf "${XDG_CONFIG_HOME:-${HOME}/.config}/fpshud"
rm -rf "${HOME}/.config/fpshud"

# Clean any legacy CLI remnants from older versions without requiring sudo
rm -f "${HOME}/.local/bin/fpshud" "${HOME}/.local/bin/fpshud_test" \
      "${HOME}/.local/bin/fpshud_recorder" "${HOME}/.local/bin/fpshud_recorder_bin"

if command -v flatpak >/dev/null 2>&1; then
    flatpak override --user --nofilesystem="${HOME}/.local/lib/fpshud" com.valvesoftware.Steam 2>/dev/null || true
fi

echo "[✓] FPSHUD removed cleanly from user space."
