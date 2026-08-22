#!/bin/bash
# Root helper. Invoked by "Install Sister HL2030.sh".
# Usage: _privileged-install.sh ROOT QUEUE USB_URI
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"

ROOT="${1:-}"
QUEUE="${2:-Brother_HL_2030_series}"
USB_URI="${3:-}"
LOG="/tmp/sisterhl2030-install.log"
DEST="/Library/Printers/SisterHL2030"
PLIST_DEST="/Library/LaunchDaemons/org.sisterhl2030.printer.plist"
STATUS_PLIST_DEST="/Library/LaunchDaemons/org.sisterhl2030.status.plist"
LABEL="system/org.sisterhl2030.printer"
STATUS_LABEL="system/org.sisterhl2030.status"

if [[ -z "$ROOT" ]]; then
  echo "Internal use: _privileged-install.sh ROOT QUEUE USB_URI" >&2
  exit 2
fi

set +e
(
  set -euo pipefail
  echo "SisterHL2030 privileged install"
  echo "date: $(date)"
  echo "ROOT=$ROOT"
  echo "QUEUE=$QUEUE"
  echo "USB_URI=$USB_URI"

  FILTER="$ROOT/build/rastertosisterhl2030"
  STATUS_BIN="$ROOT/build/sister-status"
  CMD="$ROOT/src/cups/sister-ipp-command.sh"
  PLIST="$ROOT/launchd/org.sisterhl2030.printer.plist"
  STATUS_PLIST="$ROOT/launchd/org.sisterhl2030.status.plist"

  if [[ ! -x "$FILTER" ]]; then
    echo "Missing the compiled filter: $FILTER" >&2
    exit 1
  fi
  if [[ ! -f "$CMD" ]]; then
    echo "Missing the IPP command: $CMD" >&2
    exit 1
  fi
  if [[ ! -f "$PLIST" ]]; then
    echo "Missing the LaunchDaemon: $PLIST" >&2
    exit 1
  fi
  if [[ ! -x "$STATUS_BIN" ]]; then
    echo "Missing sister-status: $STATUS_BIN" >&2
    exit 1
  fi
  if [[ ! -f "$STATUS_PLIST" ]]; then
    echo "Missing the status LaunchDaemon: $STATUS_PLIST" >&2
    exit 1
  fi

  mkdir -p "$DEST/filter" "$DEST/spool" /Library/Logs
  cp "$FILTER" "$DEST/filter/rastertosisterhl2030"
  cp "$STATUS_BIN" "$DEST/filter/sister-status"
  cp "$CMD" "$DEST/filter/sister-ipp-command"
  chmod 755 "$DEST/filter/rastertosisterhl2030" "$DEST/filter/sister-status" \
            "$DEST/filter/sister-ipp-command"
  touch "$DEST/usb.lock" "$DEST/status-empty"
  chmod 644 "$DEST/usb.lock" "$DEST/status-empty"
  /bin/bash "$ROOT/Scripts/_privileged-icon.sh" "$ROOT"
  /bin/bash "$ROOT/Scripts/_privileged-ipp-attrs.sh" "$ROOT"

  if [[ -n "$USB_URI" && "$USB_URI" != "-" ]]; then
    printf '%s\n' "$USB_URI" > "$DEST/device-uri"
  elif [[ -f "$DEST/device-uri" ]]; then
    echo "Keeping the existing device-uri."
  else
    echo "WARNING: no USB URI; the IPP command cannot talk to the printer until you write $DEST/device-uri"
  fi
  chmod 644 "$DEST/device-uri" 2>/dev/null || true
  rm -f "$DEST/toner-save"

  # Printer Application, not a PPD. Remove any leftover classic PPD we used
  # in earlier SisterHL2030 installs so CUPS cannot pick it by accident.
  rm -f "/Library/Printers/PPDs/Contents/Resources/Sister HL-2030.ppd" \
        "/Library/Printers/PPDs/Contents/Resources/Sister HL-2030.gz" 2>/dev/null || true

  cp "$PLIST" "$PLIST_DEST"
  chmod 644 "$PLIST_DEST"
  cp "$STATUS_PLIST" "$STATUS_PLIST_DEST"
  chmod 644 "$STATUS_PLIST_DEST"

  launchctl bootout "$LABEL" 2>/dev/null || true
  launchctl unload "$PLIST_DEST" 2>/dev/null || true
  if launchctl bootstrap system "$PLIST_DEST" 2>/dev/null; then
    launchctl enable "$LABEL" 2>/dev/null || true
    launchctl kickstart -k "$LABEL" 2>/dev/null || true
    echo "LaunchDaemon $LABEL started."
  elif launchctl load -w "$PLIST_DEST" 2>/dev/null; then
    echo "LaunchDaemon loaded (launchctl load)."
  else
    echo "WARNING: could not start the LaunchDaemon; try: launchctl bootstrap system $PLIST_DEST" >&2
  fi

  launchctl bootout "$STATUS_LABEL" 2>/dev/null || true
  launchctl unload "$STATUS_PLIST_DEST" 2>/dev/null || true
  if launchctl bootstrap system "$STATUS_PLIST_DEST" 2>/dev/null; then
    launchctl enable "$STATUS_LABEL" 2>/dev/null || true
    launchctl kickstart -k "$STATUS_LABEL" 2>/dev/null || true
    echo "LaunchDaemon $STATUS_LABEL started."
  elif launchctl load -w "$STATUS_PLIST_DEST" 2>/dev/null; then
    echo "Status LaunchDaemon loaded (launchctl load)."
  else
    echo "WARNING: could not start the status daemon." >&2
  fi

  echo "Files in $DEST"
  /usr/bin/file "$DEST/filter/rastertosisterhl2030"
  rm -f /etc/cups/ppd/localhost.ppd /etc/cups/ppd/HL_2030.ppd
  launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || true
  echo "SisterHL2030 installed (IPP Printer Application on port 8631)."
) >"$LOG" 2>&1
status=$?
chmod 644 "$LOG" 2>/dev/null || true
cat "$LOG"
exit "$status"
