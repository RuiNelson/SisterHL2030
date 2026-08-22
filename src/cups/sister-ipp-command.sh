#!/bin/bash
# Print command for ippeveprinter(1).
# Reads Apple/PWG/CUPS raster from $1 or stdin, writes a HL-2030 job
# to the USB printer via the CUPS usb backend.
set -euo pipefail
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"

ROOT="/Library/Printers/SisterHL2030"
FILTER="$ROOT/filter/rastertosisterhl2030"
STATUS="$ROOT/filter/sister-status"
URI_FILE="$ROOT/device-uri"
LOCK="$ROOT/usb.lock"
BACKEND="/usr/libexec/cups/backend/usb"

if [[ ! -x "$FILTER" ]]; then
  echo "ERROR: missing $FILTER" >&2
  exit 1
fi
if [[ ! -f "$URI_FILE" ]]; then
  echo "ERROR: missing $URI_FILE (USB printer URI)" >&2
  exit 1
fi

DEVICE_URI="$(tr -d '[:space:]' < "$URI_FILE")"
export DEVICE_URI

job_file=""
if [[ $# -ge 1 && -f "$1" ]]; then
  job_file="$1"
fi

tmp="$(mktemp -t sisterhl2030-job)"
trap 'rm -f "$tmp"' EXIT

options=""
append_opt() {
  local kv="$1"
  if [[ -n "$options" ]]; then
    options+=" ${kv}"
  else
    options="${kv}"
  fi
}

if [[ -n "${IPP_MEDIA:-}" ]]; then
  append_opt "media=${IPP_MEDIA}"
fi
if [[ -n "${IPP_MEDIA_TYPE:-}" ]]; then
  append_opt "media-type=${IPP_MEDIA_TYPE}"
fi
if [[ -n "${IPP_PRINT_QUALITY:-}" ]]; then
  append_opt "print-quality=${IPP_PRINT_QUALITY}"
fi
if [[ -n "${IPP_PRINTER_RESOLUTION:-}" ]]; then
  append_opt "printer-resolution=${IPP_PRINTER_RESOLUTION}"
fi

if [[ "${IPP_JOB_NAME:-}" == ".sister-status" ]]; then
  if [[ -x "$STATUS" ]]; then
    "$STATUS" --ipp >&2 || true
  fi
  exit 0
fi

echo "INFO: SisterHL2030 encoding ${CONTENT_TYPE:-unknown} -> HL-2030 USB options=${options:-<none>}" >&2

if [[ -n "$job_file" ]]; then
  "$FILTER" 1 "${USER:-root}" "${IPP_JOB_NAME:-SisterHL2030}" 1 "$options" "$job_file" >"$tmp"
else
  "$FILTER" 1 "${USER:-root}" "${IPP_JOB_NAME:-SisterHL2030}" 1 "$options" >"$tmp"
fi

if [[ ! -s "$tmp" ]]; then
  echo "ERROR: encoder produced an empty job" >&2
  exit 1
fi

echo "INFO: sending $(wc -c < "$tmp" | tr -d ' ') bytes to $DEVICE_URI" >&2
exec 9>"$LOCK"
flock 9
export SISTER_USB_LOCKED=1
"$BACKEND" 1 "${USER:-root}" "${IPP_JOB_NAME:-SisterHL2030}" 1 "" "$tmp"
echo "INFO: USB backend finished" >&2
if [[ -x "$STATUS" ]]; then
  "$STATUS" --ipp >&2 || true
fi
flock -u 9
