#!/bin/bash
# Root helper. Invoked by "Uninstall Sister HL2030.sh".
# Usage: _privileged-sister-uninstall.sh QUEUE
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"

QUEUE="${1:-Brother_HL_2030_series}"
DEST="/Library/Printers/SisterHL2030"
LOG="/tmp/sisterhl2030-uninstall.log"
LABEL="system/org.sisterhl2030.printer"
STATUS_LABEL="system/org.sisterhl2030.status"

set +e
(
  set -uo pipefail
  echo "SisterHL2030 uninstall"
  echo "date: $(date)"

  # Stop the printer application, and the status daemon older installs left.
  for label in "$LABEL" "$STATUS_LABEL"; do
    launchctl bootout "$label" 2>/dev/null
    launchctl disable "$label" 2>/dev/null
  done
  rm -f /Library/LaunchDaemons/org.sisterhl2030.printer.plist \
        /Library/LaunchDaemons/org.sisterhl2030.status.plist
  echo "LaunchDaemons removed."

  # Drop every CUPS queue that pointed at us, however it was added.
  while IFS= read -r line; do
    [[ "$line" == "device for "* ]] || continue
    name="${line#device for }"
    uri="${name#*: }"
    name="${name%%:*}"
    if [[ "$uri" == *":8631"* || "$uri" == *"HL-2030._ipp"* ]]; then
      echo "Removing queue $name"
      lpadmin -x "$name" 2>/dev/null
    fi
  done < <(LC_ALL=C lpstat -v 2>/dev/null)
  lpadmin -x "$QUEUE" 2>/dev/null

  rm -rf "$DEST"
  rm -f /private/var/lib/sister-printer-app.state
  rm -f "/Library/Printers/PPDs/Contents/Resources/Sister HL-2030.ppd" \
        "/Library/Printers/PPDs/Contents/Resources/Sister HL-2030.gz"
  rm -f /etc/cups/ppd/localhost.ppd /etc/cups/ppd/HL_2030.ppd
  launchctl kickstart -k system/org.cups.cupsd 2>/dev/null

  echo "Removed $DEST and its printer state."
  echo "The log at /Library/Logs/SisterHL2030.log was left in place."
) >"$LOG" 2>&1
status=$?
chmod 644 "$LOG" 2>/dev/null
cat "$LOG"
exit "$status"
