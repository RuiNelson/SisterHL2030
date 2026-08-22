#!/bin/bash
# Install the Sister printer icon (PNG for ippeveprinter, ICNS for CUPS).
# Usage: _privileged-icon.sh ROOT
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"
set -euo pipefail

ROOT="${1:-}"
DEST="/Library/Printers/SisterHL2030"
SRC="$ROOT/docs/sister.png"
PNG="$DEST/icon.png"

if [[ -z "$ROOT" ]]; then
  echo "Internal use: _privileged-icon.sh ROOT" >&2
  exit 2
fi
if [[ ! -f "$SRC" ]]; then
  echo "WARNING: no $SRC; keeping the default icon." >&2
  exit 0
fi

mkdir -p "$DEST" /Library/Printers/Icons
/usr/bin/sips -z 512 512 -s format png "$SRC" --out "$PNG" >/dev/null
chmod 644 "$PNG"

tmp="$(mktemp -d /tmp/sisterhl2030-icon.XXXXXX)"
cleanup() { rm -rf "$tmp"; }
trap cleanup EXIT

setdir="$tmp/sister.iconset"
mkdir -p "$setdir"
# iconutil wants this exact set of names/sizes.
/usr/bin/sips -z 16 16 "$SRC" --out "$setdir/icon_16x16.png" >/dev/null
/usr/bin/sips -z 32 32 "$SRC" --out "$setdir/icon_16x16@2x.png" >/dev/null
/usr/bin/sips -z 32 32 "$SRC" --out "$setdir/icon_32x32.png" >/dev/null
/usr/bin/sips -z 64 64 "$SRC" --out "$setdir/icon_32x32@2x.png" >/dev/null
/usr/bin/sips -z 128 128 "$SRC" --out "$setdir/icon_128x128.png" >/dev/null
/usr/bin/sips -z 256 256 "$SRC" --out "$setdir/icon_128x128@2x.png" >/dev/null
/usr/bin/sips -z 256 256 "$SRC" --out "$setdir/icon_256x256.png" >/dev/null
/usr/bin/sips -z 512 512 "$SRC" --out "$setdir/icon_256x256@2x.png" >/dev/null
/usr/bin/sips -z 512 512 "$SRC" --out "$setdir/icon_512x512.png" >/dev/null
/usr/bin/sips -z 1024 1024 "$SRC" --out "$setdir/icon_512x512@2x.png" >/dev/null
/usr/bin/iconutil -c icns -o "$tmp/sister.icns" "$setdir"

if [[ -d /etc/cups/ppd ]]; then
  for ppd in /etc/cups/ppd/*.ppd; do
    [[ -f "$ppd" ]] || continue
    if grep -q 'SisterHL2030\|HL-2030 series' "$ppd"; then
      icns="$(awk -F'"' '/^\*APPrinterIconPath:/{print $2; exit}' "$ppd")"
      if [[ -n "$icns" ]]; then
        mkdir -p "$(dirname "$icns")"
        cp "$tmp/sister.icns" "$icns"
        chmod 644 "$icns"
        echo "CUPS icon: $icns"
      fi
    fi
  done
fi
echo "IPP icon: $PNG"
