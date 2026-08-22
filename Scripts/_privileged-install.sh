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
LABEL="system/org.sisterhl2030.printer"

if [[ -z "$ROOT" ]]; then
  echo "Uso interno: _privileged-install.sh ROOT QUEUE USB_URI" >&2
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
  CMD="$ROOT/src/cups/sister-ipp-command.sh"
  PLIST="$ROOT/launchd/org.sisterhl2030.printer.plist"

  if [[ ! -x "$FILTER" ]]; then
    echo "Falta o filtro compilado: $FILTER" >&2
    exit 1
  fi
  if [[ ! -f "$CMD" ]]; then
    echo "Falta o comando IPP: $CMD" >&2
    exit 1
  fi
  if [[ ! -f "$PLIST" ]]; then
    echo "Falta o LaunchDaemon: $PLIST" >&2
    exit 1
  fi

  mkdir -p "$DEST/filter" "$DEST/spool" /Library/Logs
  cp "$FILTER" "$DEST/filter/rastertosisterhl2030"
  cp "$CMD" "$DEST/filter/sister-ipp-command"
  chmod 755 "$DEST/filter/rastertosisterhl2030" "$DEST/filter/sister-ipp-command"

  if [[ -n "$USB_URI" && "$USB_URI" != "-" ]]; then
    printf '%s\n' "$USB_URI" > "$DEST/device-uri"
  elif [[ -f "$DEST/device-uri" ]]; then
    echo "A manter device-uri existente."
  else
    echo "AVISO: sem URI USB; o comando IPP nao consegue falar com a impressora ate la rescreveres $DEST/device-uri"
  fi
  chmod 644 "$DEST/device-uri" 2>/dev/null || true
  rm -f "$DEST/toner-save"

  # Printer Application, not a PPD. Remove any leftover classic PPD we used
  # in earlier SisterHL2030 installs so CUPS cannot pick it by accident.
  rm -f "/Library/Printers/PPDs/Contents/Resources/Sister HL-2030.ppd" \
        "/Library/Printers/PPDs/Contents/Resources/Sister HL-2030.gz" 2>/dev/null || true

  cp "$PLIST" "$PLIST_DEST"
  chmod 644 "$PLIST_DEST"

  launchctl bootout "$LABEL" 2>/dev/null || true
  launchctl unload "$PLIST_DEST" 2>/dev/null || true
  if launchctl bootstrap system "$PLIST_DEST" 2>/dev/null; then
    launchctl enable "$LABEL" 2>/dev/null || true
    launchctl kickstart -k "$LABEL" 2>/dev/null || true
    echo "LaunchDaemon $LABEL iniciado."
  elif launchctl load -w "$PLIST_DEST" 2>/dev/null; then
    echo "LaunchDaemon carregado (launchctl load)."
  else
    echo "AVISO: nao consegui arrancar o LaunchDaemon; tenta: launchctl bootstrap system $PLIST_DEST" >&2
  fi

  echo "Ficheiros em $DEST"
  /usr/bin/file "$DEST/filter/rastertosisterhl2030"
  echo "SisterHL2030 instalado (Printer Application IPP na porta 8631)."
) >"$LOG" 2>&1
status=$?
chmod 644 "$LOG" 2>/dev/null || true
cat "$LOG"
exit "$status"
