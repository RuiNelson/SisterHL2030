#!/bin/bash
# Copy a newly built encoder + IPP command into the live install.
# Does not restart the LaunchDaemon: both files are exec'd per job.
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"
set -euo pipefail

ROOT="${1:-}"
DEST="/Library/Printers/SisterHL2030"
if [[ -z "$ROOT" ]]; then
  echo "Uso interno: _privileged-update-filter.sh ROOT" >&2
  exit 2
fi

FILTER="$ROOT/build/rastertosisterhl2030"
CMD="$ROOT/src/cups/sister-ipp-command.sh"
if [[ ! -x "$FILTER" ]]; then
  echo "Falta o filtro compilado: $FILTER" >&2
  exit 1
fi
if [[ ! -f "$CMD" ]]; then
  echo "Falta o comando IPP: $CMD" >&2
  exit 1
fi

mkdir -p "$DEST/filter"
cp "$FILTER" "$DEST/filter/rastertosisterhl2030"
cp "$CMD" "$DEST/filter/sister-ipp-command"
chmod 755 "$DEST/filter/rastertosisterhl2030" "$DEST/filter/sister-ipp-command"
rm -f "$DEST/toner-save"
echo "Filtro actualizado:"
/usr/bin/file "$DEST/filter/rastertosisterhl2030"
echo "Comando IPP actualizado: $DEST/filter/sister-ipp-command"
