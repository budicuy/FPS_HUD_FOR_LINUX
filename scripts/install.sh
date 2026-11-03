#!/usr/bin/env bash
# Forwarder: compile, package release, and install locally
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/package_release.sh" "$@"
