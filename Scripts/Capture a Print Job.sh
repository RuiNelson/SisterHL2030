#!/bin/bash
# Capture the exact job stream the driver sends for one print, and decode it
# back into an image, without using any paper.
#
# "Preview PAPPL Render.sh" renders a file you hand it through a throwaway
# server. That is not enough to reproduce an app-specific fault: what makes
# Word differ from Preview happens in the app -> CUPS half, before this
# driver sees anything. So this stands a capture daemon up on the installed
# daemon's port instead, and you print from the app, through the real queue,
# with the real PPD and the real print dialog.
#
# The installed daemon is stopped for the duration and started again on the
# way out, including on Ctrl-C. Its state file is never touched: the capture
# daemon keeps its own in a temporary HOME (PAPPL derives the path from $HOME,
# see mainloop-subcommands.c -- there is no state-file option).
#
# Nothing is ever sent to the printer, and no printer or queue is modified.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

# The port the installed daemon owns, and the queue that feeds it. Overridable
# so this can be rehearsed against a throwaway server.
SISTER_IPP_PORT="${SISTER_IPP_PORT:-8631}"
DAEMON_LABEL="${SISTER_DAEMON_LABEL:-system/com.ruinelson.sisterhl2030.printer}"
DAEMON_PLIST="${SISTER_DAEMON_PLIST:-/Library/LaunchDaemons/com.ruinelson.sisterhl2030.printer.plist}"

port_open() { /usr/bin/nc -z -G 1 127.0.0.1 "$1" 2>/dev/null; }

wait_for_port() {
  local port="$1" tries="${2:-15}"
  for _ in $(seq 1 "$tries"); do
    port_open "$port" && return 0
    sleep 1
  done
  return 1
}

require_macos
banner "Capture a print job"
echo "This captures the raw HL-2030 job stream for one print and decodes it"
echo "back into an image. ${c_bold}Nothing is printed and no paper is used.${c_reset}"
echo
echo "For the duration, a capture daemon stands in for the printer daemon so"
echo "that a job printed from any app -- through your normal queue, PPD and"
echo "print dialog -- is written to a file instead of sent to the printer."
echo "Your printer, your queue and the daemon's settings are left alone, and"
echo "the daemon is started again when this exits, even on Ctrl-C."
echo

APP="$ROOT/build/sister-printer-app"
[[ -x "$APP" ]] || die "Build the printer application first:
  ${c_bold}cmake --build '$ROOT/build' -j${c_reset}"
[[ -f "$DAEMON_PLIST" ]] || die "SisterHL2030 does not look installed
($DAEMON_PLIST is missing)."

# The queue whose jobs we want: the one whose device URI actually points at
# this driver. Taking whatever queue lpstat happens to list first would send
# you to another printer on this Mac and the capture would quietly get
# nothing.
#
# lpstat is localized and LC_ALL=C does not change that on macOS, so parse by
# shape, never by the prefix text: every line is "<prefix> NAME: URI". The
# two URI tests are the ones remove_duplicate_sister_queues() in _common.sh
# uses; "HL-2030._ipp" matches both _ipp._tcp and the _ipps._tcp TLS variant,
# which is the one macOS discovers and adds first.
QUEUE=""
while IFS= read -r line; do
  [[ "$line" == *": "* ]] || continue
  uri="${line#*: }"
  name="${line%%:*}"
  name="${name##* }"
  [[ -n "$name" ]] || continue
  [[ "$uri" == *":8631"* || "$uri" == *"HL-2030._ipp"* ]] || continue
  # Prefer the name the installer creates, whatever order lpstat lists in;
  # any other matching queue will do if that one is not there.
  if [[ "$name" == "$DEFAULT_QUEUE" ]]; then
    QUEUE="$name"
    break
  fi
  [[ -n "$QUEUE" ]] || QUEUE="$name"
done < <(LC_ALL=C lpstat -v 2>/dev/null || true)
if [[ -n "$QUEUE" ]]; then
  echo "Queue to print to:  ${c_bold}${QUEUE}${c_reset}"
else
  echo "${c_yellow}Could not read the queue name from lpstat.${c_reset}"
  echo "Print to your usual Sister HL-2030 queue when asked."
fi

# ---------------------------------------------------------------------------
# Where it goes, and a free socket port.
# ---------------------------------------------------------------------------
STAMP="$(date +%Y%m%d-%H%M%S)"
OUTDIR="${HOME}/Desktop/sister-capture-$STAMP"
[[ -d "$HOME/Desktop" ]] || OUTDIR="/tmp/sister-capture-$STAMP"
mkdir -p "$OUTDIR"

WORK="$(mktemp -d /tmp/sister-capture-work.XXXXXX)"
SINK_PORT=9200
while port_open "$SINK_PORT"; do SINK_PORT=$((SINK_PORT + 1)); done

echo "Capturing to:       ${OUTDIR}"
echo

echo "Stopping and starting the printer daemon needs your password."
sudo -v || die "Could not get administrator rights. Nothing was changed."

# ---------------------------------------------------------------------------
# Everything from here must be undone on the way out.
# ---------------------------------------------------------------------------
DAEMON_STOPPED=0
cleanup() {
  local status=$?
  [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null || true
  [[ -n "${LISTENER_PID:-}" ]] && kill "$LISTENER_PID" 2>/dev/null || true
  if [[ "$DAEMON_STOPPED" == "1" ]]; then
    echo
    echo "Starting the printer daemon again…"
    sleep 1
    sudo launchctl bootstrap system "$DAEMON_PLIST" 2>/dev/null || true
    if wait_for_port "$SISTER_IPP_PORT" 15; then
      echo "${c_green}The printer daemon is back.${c_reset}"
    else
      echo
      echo "${c_red}${c_bold}The printer daemon did not come back.${c_reset}"
      echo "Printing will not work until you run:"
      echo
      echo "  sudo launchctl bootstrap system '$DAEMON_PLIST'"
      echo
    fi
    DAEMON_STOPPED=0
  fi
  rm -rf "$WORK"
  return $status
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Capture listener, then the stand-in daemon on the real port.
# ---------------------------------------------------------------------------
python3 "$SCRIPT_DIR/_capture_listener.py" "$SINK_PORT" "$OUTDIR" \
  >"$OUTDIR/listener.log" 2>&1 &
LISTENER_PID=$!
sleep 1
kill -0 "$LISTENER_PID" 2>/dev/null ||
  die "The capture listener did not start. See $OUTDIR/listener.log"

echo "Stopping the printer daemon…"
sudo launchctl bootout "$DAEMON_LABEL" 2>/dev/null || true
DAEMON_STOPPED=1
for _ in $(seq 1 15); do
  port_open "$SISTER_IPP_PORT" || break
  sleep 1
done
if port_open "$SISTER_IPP_PORT"; then
  die "The printer daemon is still holding port $SISTER_IPP_PORT."
fi

echo "Starting the capture daemon on port ${SISTER_IPP_PORT}…"
HOME="$WORK" "$APP" server \
  -o server-port="$SISTER_IPP_PORT" \
  -o spool-directory="$WORK/spool" \
  -o log-file="$OUTDIR/capture-daemon.log" \
  -o log-level=debug >"$WORK/stdout.log" 2>&1 &
SERVER_PID=$!
wait_for_port "$SISTER_IPP_PORT" 15 ||
  die "The capture daemon did not start. See $OUTDIR/capture-daemon.log"

# Same name and driver as the real one, so the queue's resource path and the
# advertised Bonjour service still resolve to something that answers.
HOME="$WORK" "$APP" add -u "ipp://localhost:${SISTER_IPP_PORT}/" \
  -d Brother_HL_2030 -m sister-hl2030 \
  -v "socket://127.0.0.1:${SINK_PORT}" >/dev/null 2>&1 || true
sleep 2
HOME="$WORK" "$APP" printers -u "ipp://localhost:${SISTER_IPP_PORT}/" 2>/dev/null |
  grep -q . || die "The capture daemon has no printer. See $OUTDIR/capture-daemon.log"

# ---------------------------------------------------------------------------
# The pause.
# ---------------------------------------------------------------------------
banner "Ready -- print now"
echo "Print the document that misbehaves, from the app that misbehaves"
echo "(Word), to ${c_bold}${QUEUE:-your Sister HL-2030 queue}${c_reset}, exactly as you normally would."
echo
echo "${c_bold}Nothing will come out of the printer.${c_reset} Captured files appear in:"
echo "  $OUTDIR"
echo
echo "For comparison, print the same document from Preview afterwards -- then"
echo "we have both, from one run, and can decode them side by side."
echo
read -r -p "Press Enter when the job (or both jobs) have left the queue. " _ || true

sleep 3

shopt -s nullglob
while :; do
  JOBS=("$OUTDIR"/*-job.prn)
  (( ${#JOBS[@]} )) && break
  echo
  echo "${c_yellow}No print job captured yet.${c_reset}"
  echo "Connections seen so far:"
  { sed 's/^/  /' "$OUTDIR/listener.log" 2>/dev/null | tail -5; } || true
  echo
  if ! ask_yes "Wait 15 more seconds and check again? (type YES)"; then
    die "Nothing was captured. Check that the job really went to
'${QUEUE:-the Sister queue}' and that it is not still held in the queue."
  fi
  sleep 15
done

# ---------------------------------------------------------------------------
# Decode. The daemon comes back in the trap right after this.
# ---------------------------------------------------------------------------
banner "Captured ${#JOBS[@]} job(s)"
for job in "${JOBS[@]}"; do
  echo "${c_bold}$(basename "$job")${c_reset} -- $(stat -f%z "$job") bytes"
  python3 "$SCRIPT_DIR/decode_job.py" "$job" "${job%.prn}.png" 2>&1 | sed 's/^/  /' || true
  echo
done

echo "${c_bold}What to look for${c_reset}"
echo "  The .png files are the pages as the printer would have rendered them."
echo "  If a page shows the same gaps and half-lines you get on paper, the"
echo "  fault is already in the job this driver produced. If it looks clean,"
echo "  the page leaves the encoder intact and the bytes are being lost on"
echo "  the way to the printer over USB."
echo
echo "  Compare sizes too: a Word page much larger than the same page from"
echo "  Preview points at the printer running out of band memory."
echo
echo "Everything is in: ${c_bold}${OUTDIR}${c_reset}"
open "$OUTDIR" 2>/dev/null || true
pause
