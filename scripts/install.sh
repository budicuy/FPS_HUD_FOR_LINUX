#!/usr/bin/env bash
# Forwarder to root install.sh for consistent installation
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/../install.sh" "$@"
