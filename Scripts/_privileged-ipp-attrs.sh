#!/bin/bash
# Install IPP attributes (0 hardware margins) and rewrite generated PPDs
# so ImageableArea matches PaperDimension.
# Usage: _privileged-ipp-attrs.sh ROOT
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"
set -euo pipefail

ROOT="${1:-}"
DEST="/Library/Printers/SisterHL2030"
SRC="$ROOT/ipp/printer-attrs.conf"

if [[ -z "$ROOT" ]]; then
  echo "Uso interno: _privileged-ipp-attrs.sh ROOT" >&2
  exit 2
fi
if [[ ! -f "$SRC" ]]; then
  echo "Falta $SRC" >&2
  exit 1
fi

mkdir -p "$DEST"
cp "$SRC" "$DEST/printer-attrs.conf"
chmod 644 "$DEST/printer-attrs.conf"
echo "IPP attrs: $DEST/printer-attrs.conf"
