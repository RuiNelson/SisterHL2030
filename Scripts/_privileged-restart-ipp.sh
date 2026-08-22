#!/bin/bash
# Reload the SisterHL2030 IPP daemon after a plist change.
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"
set -euo pipefail
ROOT="${1:-}"
PLIST_SRC="${ROOT}/launchd/org.sisterhl2030.printer.plist"
PLIST_DEST="/Library/LaunchDaemons/org.sisterhl2030.printer.plist"
STATUS_PLIST_SRC="${ROOT}/launchd/org.sisterhl2030.status.plist"
STATUS_PLIST_DEST="/Library/LaunchDaemons/org.sisterhl2030.status.plist"
LABEL="system/org.sisterhl2030.printer"
STATUS_LABEL="system/org.sisterhl2030.status"
/bin/bash "${ROOT}/Scripts/_privileged-icon.sh" "$ROOT"
/bin/bash "${ROOT}/Scripts/_privileged-ipp-attrs.sh" "$ROOT"
if [[ -x "${ROOT}/build/sister-status" ]]; then
  mkdir -p /Library/Printers/SisterHL2030/filter
  cp "${ROOT}/build/sister-status" /Library/Printers/SisterHL2030/filter/sister-status
  chmod 755 /Library/Printers/SisterHL2030/filter/sister-status
fi
if [[ -f "${ROOT}/src/cups/sister-ipp-command.sh" ]]; then
  cp "${ROOT}/src/cups/sister-ipp-command.sh" \
     /Library/Printers/SisterHL2030/filter/sister-ipp-command
  chmod 755 /Library/Printers/SisterHL2030/filter/sister-ipp-command
fi
cp "$PLIST_SRC" "$PLIST_DEST"
chmod 644 "$PLIST_DEST"
cp "$STATUS_PLIST_SRC" "$STATUS_PLIST_DEST"
chmod 644 "$STATUS_PLIST_DEST"
launchctl bootout "$LABEL" 2>/dev/null || true
launchctl unload "$PLIST_DEST" 2>/dev/null || true
if ! launchctl bootstrap system "$PLIST_DEST"; then
  launchctl load -w "$PLIST_DEST"
fi
launchctl kickstart -k "$LABEL" 2>/dev/null || true
echo "Daemon reloaded."
launchctl bootout "$STATUS_LABEL" 2>/dev/null || true
launchctl unload "$STATUS_PLIST_DEST" 2>/dev/null || true
if ! launchctl bootstrap system "$STATUS_PLIST_DEST"; then
  launchctl load -w "$STATUS_PLIST_DEST"
fi
launchctl kickstart -k "$STATUS_LABEL" 2>/dev/null || true
echo "Status daemon reloaded."
sleep 1
nc -z localhost 8631 && echo "localhost:8631 is listening" || echo "WARNING: 8631 still closed"
# Drop leftover AirPrint PPDs from the Bonjour duplicate; keep the CUPS queue.
rm -f /etc/cups/ppd/localhost.ppd /etc/cups/ppd/HL_2030.ppd
launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || true
echo "cupsd reloaded."
