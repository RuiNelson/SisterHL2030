#!/bin/bash
# Show what the PAPPL printer application would put on paper, without
# printing it. Runs a throwaway PAPPL server whose "printer" is a local
# socket, captures the HL-2030 job, decodes it back to an image and opens it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

require_macos
banner "Preview a PAPPL render (screen only)"
echo "This does NOT print and never touches the printer."
echo "The job goes to a local socket instead, and is decoded back into"
echo "an image of the actual page the HL-2030 would produce."
echo

INPUT="${1:-$ROOT/test_fixtures/flag.png}"
QUALITY="${2:-normal}"
if [[ ! -f "$INPUT" ]]; then
  die "No such file: $INPUT"
fi
case "$QUALITY" in
  draft) IPP_QUALITY=3 ;;
  normal) IPP_QUALITY=4 ;;
  high|best) IPP_QUALITY=5 ;;
  *) die "Quality must be draft, normal or high (got '$QUALITY')." ;;
esac

APP="$ROOT/build/sister-printer-app"
if [[ ! -x "$APP" ]]; then
  echo "Building the PAPPL application (this also fetches and builds PAPPL)…"
  cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DSISTER_WITH_PAPPL=ON
  cmake --build "$ROOT/build" --target sister-printer-app -j
fi

WORK="$(mktemp -d /tmp/sister-preview-pappl.XXXXXX)"
SINK_PORT=9101
IPP_PORT=8633
while /usr/bin/nc -z -G 1 127.0.0.1 "$SINK_PORT" 2>/dev/null; do SINK_PORT=$((SINK_PORT + 1)); done
while /usr/bin/nc -z -G 1 127.0.0.1 "$IPP_PORT" 2>/dev/null; do IPP_PORT=$((IPP_PORT + 1)); done

cleanup() {
  [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null || true
  [[ -n "${SINK_PID:-}" ]] && kill "$SINK_PID" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

echo "Capturing on port ${SINK_PORT}, PAPPL server on ${IPP_PORT}."
python3 "$SCRIPT_DIR/_socket_sink.py" "$SINK_PORT" "$WORK/job.prn" >"$WORK/sink.log" 2>&1 &
SINK_PID=$!
sleep 1

# A private state file: never disturb a real installed printer. PAPPL derives
# the path from $HOME (mainloop-subcommands.c), so overriding HOME is what
# actually isolates it -- there is no PAPPL_STATE variable.
HOME="$WORK" "$APP" server \
  -o server-port="$IPP_PORT" \
  -o spool-directory="$WORK/spool" \
  -o log-file="$WORK/server.log" \
  -o log-level=info >"$WORK/stdout.log" 2>&1 &
SERVER_PID=$!

for _ in 1 2 3 4 5 6 7 8; do
  /usr/bin/nc -z -G 1 127.0.0.1 "$IPP_PORT" 2>/dev/null && break
  sleep 1
done

"$APP" add -u "ipp://127.0.0.1:${IPP_PORT}/" -d preview -m sister-hl2030 \
  -v "socket://127.0.0.1:${SINK_PORT}" >/dev/null 2>&1 || true

echo "Rendering $(basename "$INPUT") at ${QUALITY} quality…"
"$APP" submit -u "ipp://127.0.0.1:${IPP_PORT}/" -d preview \
  -o print-quality="$IPP_QUALITY" "$INPUT" >/dev/null

for _ in $(seq 1 30); do
  sleep 2
  [[ -s "$WORK/job.prn" ]] && sleep 3 && break
done

if [[ ! -s "$WORK/job.prn" ]]; then
  echo "${c_yellow}Nothing was captured.${c_reset} Server log:"
  tail -5 "$WORK/server.log" 2>/dev/null || true
  pause
  exit 1
fi

OUT="$HOME/Desktop/sister-pappl-render.png"
echo
python3 "$SCRIPT_DIR/decode_job.py" "$WORK/job.prn" "$OUT" --detail
echo
echo "${c_bold}mixed bytes${c_reset} is the halftone signal: bytes that are neither blank"
echo "nor solid. A dithered photo runs around 90%. Near 0% means the page"
echo "lost its tone and would print as flat black and white."
echo
echo "File: $OUT"
echo "The full page looks grey on screen only because it is shrunk to fit;"
echo "the -detail file is 1:1, where the halftone dots are actual dots."
open -a Preview "${OUT%.png}-detail.png" "$OUT" 2>/dev/null || open "$OUT"
pause
