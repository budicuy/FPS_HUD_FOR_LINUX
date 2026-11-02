#!/usr/bin/env bash
set -e

echo "[*] Uninstalling FPSHUD from your user account..."
rm -rf "$HOME/.local/lib/fpshud"
rm -f  "$HOME/.local/share/vulkan/implicit_layer.d/VkLayer_FPSHUD_overlay.json"
rm -f  "$HOME/.local/bin/fpshud"
rm -rf "$HOME/.local/share/fpshud"

echo "[+] FPSHUD has been cleanly uninstalled!"
echo "    (Optional) To also delete your saved presets/config: rm -rf ~/.config/fpshud"
