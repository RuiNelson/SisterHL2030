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
# Read the version from the build tree: macOS kills an unsigned copy under
# /Library/Printers (OS_REASON_CODESIGNING) until we ad-hoc sign it.
ver="$("$APP" --version 2>/dev/null || true)"
cp "$APP" "$DEST/sister-printer-app"
chmod 755 "$DEST/sister-printer-app"
codesign --force --sign - "$DEST/sister-printer-app"
if [[ -x "$ROOT/build/sister-status" ]]; then
  cp "$ROOT/build/sister-status" "$DEST/sister-status"
  chmod 755 "$DEST/sister-status"
  codesign --force --sign - "$DEST/sister-status"
fi
if [[ -n "$ver" ]]; then
  printf '%s\n' "$ver" > "$DEST/VERSION"
fi
launchctl kickstart -k "$LABEL" 2>/dev/null || true
echo "Printer application updated:"
/usr/bin/file "$DEST/sister-printer-app"
if [[ -n "$ver" ]]; then
  echo "Installed driver $ver"
fi

# Rebuild a stale everywhere PPD (Normal quality mapped to 300 dpi).
PPD="/etc/cups/ppd/Brother_HL_2030_series.ppd"
if [[ -f "$ROOT/Scripts/_privileged-create-queue.sh" ]] &&
   { [[ ! -f "$PPD" ]] ||
     ! grep -q 'cupsInteger1 4 /HWResolution\[600 600\]' "$PPD"; }; then
  for _ in 1 2 3 4 5 6 7 8; do
    nc -z localhost 8631 2>/dev/null && break
    sleep 1
  done
  /bin/bash "$ROOT/Scripts/_privileged-create-queue.sh" \
    "Brother_HL_2030_series" || true
fi
