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

echo "[*] FPSHUD (including all binaries, layers, and user configs) has been completely removed from your system."
