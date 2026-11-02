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

echo "[*] FPSHUD has been cleanly removed from your system."
echo "    (Optional) To remove saved user presets/settings: rm -rf ~/.config/fpshud"
