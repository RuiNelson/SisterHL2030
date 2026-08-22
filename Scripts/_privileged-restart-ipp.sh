#!/bin/bash
# Reload the SisterHL2030 IPP daemon after a plist change.
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"
set -euo pipefail
ROOT="${1:-}"
PLIST_SRC="${ROOT}/launchd/org.sisterhl2030.printer.plist"
PLIST_DEST="/Library/LaunchDaemons/org.sisterhl2030.printer.plist"
LABEL="system/org.sisterhl2030.printer"
/bin/bash "${ROOT}/Scripts/_privileged-icon.sh" "$ROOT"
/bin/bash "${ROOT}/Scripts/_privileged-ipp-attrs.sh" "$ROOT"
cp "$PLIST_SRC" "$PLIST_DEST"
chmod 644 "$PLIST_DEST"
launchctl bootout "$LABEL" 2>/dev/null || true
launchctl unload "$PLIST_DEST" 2>/dev/null || true
if ! launchctl bootstrap system "$PLIST_DEST"; then
  launchctl load -w "$PLIST_DEST"
fi
launchctl kickstart -k "$LABEL" 2>/dev/null || true
echo "Daemon recarregado."
sleep 1
nc -z localhost 8631 && echo "localhost:8631 a escuta" || echo "AVISO: 8631 ainda fechada"
# Drop leftover AirPrint PPDs from the Bonjour duplicate; keep the CUPS queue.
rm -f /etc/cups/ppd/localhost.ppd /etc/cups/ppd/HL_2030.ppd
launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || true
echo "cupsd recarregado."
