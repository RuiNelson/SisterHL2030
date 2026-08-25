#!/bin/bash
# Build and install the native Apple Silicon SisterHL2030 CUPS filter.
# Refuses to run while official Intel Brother drivers are still present.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

trap 'echo; echo "${c_red}The program stopped with an error.${c_reset}"; echo "Read the message above — most of the time you just need to install what is missing and try again."; pause; exit 1' ERR

require_macos

banner "Install Sister HL-2030"
echo "This program installs a ${c_bold}native Apple Silicon${c_reset} driver"
echo "for the Brother HL-2030 (series) printer."
echo
echo "It does not use Brother's Intel software, and it does not use Rosetta."
echo

echo "${c_bold}1/4  Checking for leftover official Intel drivers…${c_reset}"
echo
reasons="$(official_brother_reasons || true)"
if [[ -n "$reasons" ]]; then
  echo "${c_red}${c_bold}The official Brother drivers are still installed.${c_reset}"
  echo
  echo "Found:"
  echo "$reasons"
  echo
  echo "SisterHL2030 ${c_bold}will not install on top${c_reset} of the Intel package."
  echo "That avoids a half-configured print queue that still calls the old"
  echo "filter."
  echo
  echo "What to do:"
  echo "  1. Run ${c_bold}Uninstall Official Brother Drivers.sh${c_reset}"
  echo "     (it is in the same folder as this program)."
  echo "  2. Run ${c_bold}Install Sister HL2030.sh${c_reset} again."
  echo
  pause
  exit 1
fi
echo "${c_green}No official drivers found. We can continue.${c_reset}"
echo

echo "${c_bold}    Checking for a previous SisterHL2030 install…${c_reset}"
echo
sister_old="$(previous_sister_reasons || true)"
if [[ -n "$sister_old" ]]; then
  echo "Found the old version (CUPS PPD), the one macOS reports as deprecated:"
  echo "$sister_old"
  echo
  echo "You do not need to remove it by hand. This program ${c_bold}replaces it${c_reset}:"
  echo "it deletes the PPD, installs the IPP Everywhere service, and points the"
  echo "same queue at it."
else
  echo "No previous SisterHL2030 install."
fi
echo

arch="$(uname -m)"
if [[ "$arch" != "arm64" ]]; then
  echo "${c_yellow}Warning:${c_reset} this Mac reports architecture \"${arch}\", not Apple Silicon."
  echo "SisterHL2030 was made for M1/M2/M3/M4 Macs."
  if ! ask_yes "Continue anyway? Type YES:"; then
    echo "Cancelled."
    pause
    exit 0
  fi
  echo
fi

echo "${c_bold}2/4  Preparing the build tools…${c_reset}"
if ! xcode-select -p >/dev/null 2>&1; then
  die "Could not find Apple's Command Line Tools.
In Terminal, run:

    xcode-select --install

When that finishes, come back to this program."
fi
if ! command -v cmake >/dev/null 2>&1; then
  die "Could not find CMake (the program that builds the driver).

If you have Homebrew, run this in Terminal:

    brew install cmake

No Homebrew? Open https://brew.sh and follow the instructions,
then install cmake and come back here."
fi
echo "CMake and compiler found."
echo

echo "${c_bold}    Choosing a halftone screen…${c_reset}"
echo
echo "SisterHL2030 can dither photos and shading two ways:"
echo "  ${c_bold}FM${c_reset}  \"Pencil style\" — Atkinson error diffusion, dots scattered"
echo "        individually. Soft and sketchy. The shipping default, and the"
echo "        one whose darkness is calibrated against printed test sheets."
echo "  ${c_bold}AM${c_reset}  \"Newspaper style\" — 45° clustered-dot ordered dither, dots"
echo "        grouped into an even grid. Crisper on line art and small text,"
echo "        with a more visible dot pattern on photos. Calibrated by eye on"
echo "        paper as needing no darkness correction, so it is left exactly"
echo "        as the screen renders it."
echo
halftone_cmake=""
halftone_label=""
while [[ -z "$halftone_cmake" ]]; do
  read -r -p "Install which one? [FM/am] (default FM): " halftone_answer || true
  halftone_answer="$(printf '%s' "$halftone_answer" | tr '[:lower:]' '[:upper:]')"
  case "$halftone_answer" in
    ""|FM) halftone_cmake="ATKINSON"; halftone_label="FM (Atkinson, Pencil style)" ;;
    AM)    halftone_cmake="AM45";     halftone_label="AM (45° clustered-dot, Newspaper style)" ;;
    *)     echo "Please type FM or AM." ;;
  esac
done
echo "${c_green}Using the ${halftone_label} screen.${c_reset}"
echo

echo "${c_bold}3/4  Compiling the printer application for this Mac…${c_reset}"
echo "${c_dim}(the first build also downloads and builds PAPPL, a few minutes)${c_reset}"
echo
cd "$ROOT"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSISTER_WITH_PAPPL=ON \
  -DSISTER_HALFTONE_SCREEN="$halftone_cmake"
# --clean-first: switching SISTER_HALFTONE_SCREEN on an existing build/ only
# changes a compile *definition*, and re-running configure immediately
# before build can regenerate flags.make in the same filesystem-mtime tick
# as the stale object file, so make sees nothing to rebuild and silently
# keeps the previous screen. Forcing a clean of just these two targets costs
# a few seconds (their dependencies are already built) and makes sure the
# choice above always takes effect.
cmake --build build --target sister-printer-app sister-status --clean-first -j
echo
if ! file "$ROOT/build/sister-printer-app" | grep -q 'arm64\|x86_64\|executable'; then
  die "The build did not produce the printer application. See the messages above."
fi
echo "${c_green}Printer application compiled.${c_reset}  $(file -b "$ROOT/build/sister-printer-app")"
echo

uri="$(detect_hl2030_uri || true)"
queue="$DEFAULT_QUEUE"
if [[ -n "$uri" ]]; then
  echo "Found the HL-2030 on USB:"
  echo "  $uri"
else
  echo "${c_yellow}No HL-2030 on USB right now.${c_reset}"
  echo "The IPP service will still be installed. Connect the printer,"
  echo "write the USB URI to /Library/Printers/SisterHL2030/device-uri"
  echo "and add the printer in System Settings."
fi
echo
echo "${c_yellow}You will be asked for this Mac's administrator password.${c_reset}"
if ! ask_yes "Type YES and press Enter to install:"; then
  echo
  echo "Cancelled. The driver was built but not installed."
  pause
  exit 0
fi

echo
echo "${c_bold}4/4  Installing…${c_reset}"
run_as_admin "$SCRIPT_DIR/_privileged-install.sh" \
  "SisterHL2030 needs permission to install the native HL-2030 driver." \
  "$ROOT" "$queue" "${uri:--}"

echo
if [[ ! -x "$SISTER_APP" ]]; then
  die "The install did not leave the printer application at $SISTER_APP."
fi

echo "Waiting for the printer application on localhost:8631…"
ipp_ready=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do
  if /usr/bin/nc -z localhost 8631 2>/dev/null; then
    ipp_ready=1
    break
  fi
  sleep 0.5
done
if [[ "$ipp_ready" -ne 1 ]]; then
  echo "${c_yellow}The printer application is not listening yet. Setting up the queue anyway.${c_reset}"
  echo "If the printer does not show up, run this installer again."
fi

echo "Creating the CUPS queue…"
# IPP Everywhere so System Settings lists it, then the PPD is rewritten
# with Apple's Quality mapping (Normal = 600 dpi, not 300). The privileged
# installer already ran this; running it again is idempotent.
if [[ ! -f "/etc/cups/ppd/${queue}.ppd" ]] ||
   ! grep -q 'cupsInteger1 4 /HWResolution\[600 600\]' \
        "/etc/cups/ppd/${queue}.ppd" 2>/dev/null; then
  run_as_admin "$SCRIPT_DIR/_privileged-create-queue.sh" \
    "SisterHL2030 needs permission to create the print queue." \
    "$queue" || true
fi
if [[ -f "/etc/cups/ppd/${queue}.ppd" ]]; then
  /usr/sbin/cupsenable "$queue" 2>/dev/null || true
  /usr/sbin/cupsaccept "$queue" 2>/dev/null || true
  /usr/bin/lpoptions -d "$queue" 2>/dev/null || true
  /usr/sbin/lpadmin -p "$queue" -o printer-is-shared=false 2>/dev/null || true
  echo "${c_green}Queue \"${queue}\" points at ipp://localhost:8631/ipp/print${c_reset}"
else
  echo "${c_yellow}Could not create the queue automatically.${c_reset}"
  echo "In System Settings → Printers, add an IPP printer with the address"
  echo "ipp://localhost:8631/ipp/print (AirPrint / IPP Everywhere)."
fi
remove_duplicate_sister_queues "$queue"

# CUPS only copies supply levels off the printer while a job runs through its
# backend, so Supply Levels stays empty until the first print. This job is
# named so the printer application reports status and prints nothing.
if command -v lp >/dev/null 2>&1; then
  probe="$(mktemp -t sister-status-probe)"
  printf 'status\n' > "$probe"
  lp -d "$queue" -t .sister-status "$probe" >/dev/null 2>&1 || true
  sleep 3
  rm -f "$probe"
fi

echo
echo "${c_green}${c_bold}Sister HL-2030 installed.${c_reset}"
echo
echo "This is a PAPPL printer application, not a classic CUPS PPD (that is what"
echo "macOS reports as deprecated). CUPS speaks IPP Everywhere to SisterHL2030,"
echo "and the arm64 encoder speaks to the HL-2030 over USB."
echo
echo "Printer application: $SISTER_APP"
if [[ -x "$SISTER_APP" ]]; then
  echo "        $(file -b "$SISTER_APP")"
  echo "        version $("$SISTER_APP" --version 2>/dev/null || echo unknown)"
  echo "        halftone screen: ${halftone_label}"
fi
echo "To confirm this copy is the one running:  Scripts/Check Sister HL2030.sh"
echo
echo "To test it: open a page in Preview and print to"
echo "\"Brother HL-2030 series\"."
echo
echo "On an iPhone/iPad on the same Wi‑Fi, the printer shows up as"
echo "\"Brother HL-2030\" (AirPrint). This Mac has to be switched on."
echo
echo "Print dialog modes, under ${c_bold}Quality${c_reset}:"
echo "  • Draft  — 300 dpi, toner saving"
echo "  • Normal — 600 dpi, toner saving (default)"
echo "  • Best   — HQ1200, full toner (Brother's fine mode)"
echo
echo "Supply levels: System Settings → Printers & Scanners"
echo "→ Brother HL-2030 series → Options & Supplies → Supply Levels."
echo "The HL-2030 toner only reports OK / low / empty (it is not a continuous"
echo "percentage). The drum is estimated from the page count (about 12,000)."
echo
echo "The printer application also has a web page of its own, with status,"
echo "supply levels and media settings:  http://localhost:8631/"
echo
echo "If the page comes out blank or inverted, open an issue on the"
echo "SisterHL2030 repository and describe what you saw."
pause
exit 0
