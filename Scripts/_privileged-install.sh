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
# The IPP printer name inside the printer application. PAPPL shows a friendlier
# name over Bonjour; see kDnsSdName in src/pappl/sister_app.cpp.
PRINTER="Brother_HL_2030"

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

  APP="$ROOT/build/sister-printer-app"
  STATUS_BIN="$ROOT/build/sister-status"
  PLIST="$ROOT/launchd/org.sisterhl2030.printer.plist"

  if [[ ! -x "$APP" ]]; then
    echo "Missing the printer application: $APP" >&2
    exit 1
  fi
  if [[ ! -f "$PLIST" ]]; then
    echo "Missing the LaunchDaemon: $PLIST" >&2
    exit 1
  fi

  mkdir -p "$DEST/spool" /Library/Logs
  ver="$("$APP" --version 2>/dev/null || true)"
  cp "$APP" "$DEST/sister-printer-app"
  chmod 755 "$DEST/sister-printer-app"
  codesign --force --sign - "$DEST/sister-printer-app"
  if [[ -x "$STATUS_BIN" ]]; then
    cp "$STATUS_BIN" "$DEST/sister-status"
    chmod 755 "$DEST/sister-status"
    codesign --force --sign - "$DEST/sister-status"
  fi
  if [[ -n "$ver" ]]; then
    printf '%s\n' "$ver" > "$DEST/VERSION"
    echo "Installed driver $ver"
  fi
  chmod 700 "$DEST/spool"
  /bin/bash "$ROOT/Scripts/_privileged-icon.sh" "$ROOT"

  # Retire the ippeveprinter façade. The printer application owns the USB
  # device, its own IPP attributes and its own supply polling, so the command
  # script, the attributes file, the device-uri file, the USB lock and the
  # no-op status job daemon all go away.
  launchctl bootout "$STATUS_LABEL" 2>/dev/null || true
  rm -f "$STATUS_PLIST_DEST"
  rm -rf "$DEST/filter"
  rm -f "$DEST/printer-attrs.conf" "$DEST/device-uri" "$DEST/usb.lock" \
        "$DEST/status-empty" "$DEST/toner-save"

  # Printer Application, not a PPD. Remove any classic PPD earlier versions
  # installed so CUPS cannot pick it by accident.
  rm -f "/Library/Printers/PPDs/Contents/Resources/Sister HL-2030.ppd" \
        "/Library/Printers/PPDs/Contents/Resources/Sister HL-2030.gz" 2>/dev/null || true

  cp "$PLIST" "$PLIST_DEST"
  chmod 644 "$PLIST_DEST"

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

  ready=0
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do
    if nc -z localhost 8631 2>/dev/null; then
      ready=1
      break
    fi
    sleep 1
  done
  if [[ "$ready" -ne 1 ]]; then
    echo "WARNING: the printer application is not listening on 8631 yet." >&2
  fi

  # Add the printer once; the application remembers it in its state file.
  if "$DEST/sister-printer-app" -u ipp://localhost:8631/ 2>/dev/null | grep -q "$PRINTER"; then
    echo "Printer '$PRINTER' already registered."
    if [[ -n "$USB_URI" && "$USB_URI" != "-" ]]; then
      "$DEST/sister-printer-app" modify -u ipp://localhost:8631/ -d "$PRINTER" \
        -v "$USB_URI" 2>/dev/null || true
    fi
  elif [[ -n "$USB_URI" && "$USB_URI" != "-" ]]; then
    if "$DEST/sister-printer-app" add -u ipp://localhost:8631/ -d "$PRINTER" \
        -m sister-hl2030 -v "$USB_URI"; then
      echo "Printer '$PRINTER' added for $USB_URI."
    else
      echo "WARNING: could not add the printer; is the HL-2030 connected?" >&2
    fi
  else
    echo "WARNING: no USB URI. Connect the HL-2030 and run the installer again."
  fi

  echo "Files in $DEST"
  /usr/bin/file "$DEST/sister-printer-app"
  rm -f /etc/cups/ppd/localhost.ppd /etc/cups/ppd/HL_2030.ppd
  launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || true

  # Apple's ipp2ppd, not `lpadmin -m everywhere`: the everywhere PPD maps
  # Normal quality to 300 dpi and the page prints at half size.
  /bin/bash "$ROOT/Scripts/_privileged-create-queue.sh" "$QUEUE"

  echo "SisterHL2030 installed (PAPPL printer application on port 8631)."
) >"$LOG" 2>&1
status=$?
chmod 644 "$LOG" 2>/dev/null || true
cat "$LOG"
exit "$status"
