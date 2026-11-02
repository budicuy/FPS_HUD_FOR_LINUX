#!/usr/bin/env bash
set -e

echo "[*] Uninstalling FPSHUD user files..."
rm -f "$HOME/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json"
rm -f "$HOME/.local/bin/fpshud"
rm -rf "$HOME/.local/share/fpshud"

echo "[*] FPSHUD has been uninstalled."
echo "    (Optional) To remove saved user configurations, run: rm -rf ~/.config/fpshud"
