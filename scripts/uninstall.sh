#!/usr/bin/env bash
# Forwarder to root uninstall.sh to eliminate code duplication
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/../uninstall.sh" "$@"
