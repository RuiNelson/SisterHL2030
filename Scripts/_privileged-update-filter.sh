#!/bin/bash
# Copy a newly built encoder + IPP command into the live install.
# Does not restart the LaunchDaemon: both files are exec'd per job.
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"
set -euo pipefail

ROOT="${1:-}"
DEST="/Library/Printers/SisterHL2030"
if [[ -z "$ROOT" ]]; then
  echo "Internal use: _privileged-update-filter.sh ROOT" >&2
  exit 2
fi

FILTER="$ROOT/build/rastertosisterhl2030"
STATUS_BIN="$ROOT/build/sister-status"
CMD="$ROOT/src/cups/sister-ipp-command.sh"
if [[ ! -x "$FILTER" ]]; then
  echo "Missing the compiled filter: $FILTER" >&2
  exit 1
fi
if [[ ! -x "$STATUS_BIN" ]]; then
  echo "Missing sister-status: $STATUS_BIN" >&2
  exit 1
fi
if [[ ! -f "$CMD" ]]; then
  echo "Missing the IPP command: $CMD" >&2
  exit 1
fi

mkdir -p "$DEST/filter"
cp "$FILTER" "$DEST/filter/rastertosisterhl2030"
cp "$STATUS_BIN" "$DEST/filter/sister-status"
cp "$CMD" "$DEST/filter/sister-ipp-command"
chmod 755 "$DEST/filter/rastertosisterhl2030" "$DEST/filter/sister-status" \
          "$DEST/filter/sister-ipp-command"
rm -f "$DEST/toner-save"
touch "$DEST/status-empty" "$DEST/usb.lock"
chmod 644 "$DEST/status-empty" "$DEST/usb.lock"
launchctl kickstart -k system/org.sisterhl2030.status 2>/dev/null || true
echo "Filter updated:"
/usr/bin/file "$DEST/filter/rastertosisterhl2030"
echo "USB status: $DEST/filter/sister-status"
echo "IPP command updated: $DEST/filter/sister-ipp-command"
