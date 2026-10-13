#!/bin/bash
# Compatibility entry point for existing Intel build instructions.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec /bin/bash "$SCRIPT_DIR/build_static_deps.sh" x86_64 "$@"
