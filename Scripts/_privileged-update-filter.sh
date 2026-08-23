#!/bin/bash
# Copy a freshly built printer application into the live install and restart
# it. The daemon holds the USB device, so it has to come back for the new
# binary to take effect.
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"
set -euo pipefail

ROOT="${1:-}"
DEST="/Library/Printers/SisterHL2030"
LABEL="system/org.sisterhl2030.printer"
if [[ -z "$ROOT" ]]; then
  echo "Internal use: _privileged-update-filter.sh ROOT" >&2
  exit 2
fi

APP="$ROOT/build/sister-printer-app"
if [[ ! -x "$APP" ]]; then
  echo "Missing the printer application: $APP" >&2
  echo "Build it with: cmake -S . -B build -DSISTER_WITH_PAPPL=ON" >&2
  exit 1
fi

mkdir -p "$DEST"
cp "$APP" "$DEST/sister-printer-app"
chmod 755 "$DEST/sister-printer-app"
if [[ -x "$ROOT/build/sister-status" ]]; then
  cp "$ROOT/build/sister-status" "$DEST/sister-status"
  chmod 755 "$DEST/sister-status"
fi
launchctl kickstart -k "$LABEL" 2>/dev/null || true
echo "Printer application updated:"
/usr/bin/file "$DEST/sister-printer-app"
