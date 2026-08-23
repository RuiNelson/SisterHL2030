#!/bin/bash
# Reload the SisterHL2030 printer application after a change.
# Usage: _privileged-restart-ipp.sh ROOT
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"
set -euo pipefail

ROOT="${1:-}"
DEST="/Library/Printers/SisterHL2030"
PLIST_SRC="${ROOT}/launchd/org.sisterhl2030.printer.plist"
PLIST_DEST="/Library/LaunchDaemons/org.sisterhl2030.printer.plist"
LABEL="system/org.sisterhl2030.printer"

if [[ -z "$ROOT" ]]; then
  echo "Internal use: _privileged-restart-ipp.sh ROOT" >&2
  exit 2
fi

/bin/bash "${ROOT}/Scripts/_privileged-icon.sh" "$ROOT"
if [[ -x "${ROOT}/build/sister-printer-app" ]]; then
  cp "${ROOT}/build/sister-printer-app" "$DEST/sister-printer-app"
  chmod 755 "$DEST/sister-printer-app"
  codesign --force --sign - "$DEST/sister-printer-app"
fi
if [[ -x "${ROOT}/build/sister-status" ]]; then
  cp "${ROOT}/build/sister-status" "$DEST/sister-status"
  chmod 755 "$DEST/sister-status"
  codesign --force --sign - "$DEST/sister-status"
fi
cp "$PLIST_SRC" "$PLIST_DEST"
chmod 644 "$PLIST_DEST"

launchctl bootout "$LABEL" 2>/dev/null || true
launchctl unload "$PLIST_DEST" 2>/dev/null || true
if ! launchctl bootstrap system "$PLIST_DEST"; then
  launchctl load -w "$PLIST_DEST"
fi
launchctl kickstart -k "$LABEL" 2>/dev/null || true
echo "Printer application reloaded."
sleep 2
nc -z localhost 8631 && echo "localhost:8631 is listening" || echo "WARNING: 8631 still closed"
launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || true
echo "cupsd reloaded."
