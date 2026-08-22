#!/bin/bash
# Remove the Intel-only official Brother CUPS package from this Mac.
# Safe to double-click from Finder if opened with Terminal.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

trap 'echo; echo "${c_red}The program stopped with an error.${c_reset}"; pause; exit 1' ERR

require_macos

banner "Remove the official Brother drivers"
echo "This program uninstalls the Brother printing software that shipped"
echo "from Apple/Brother in 2014 and ${c_bold}only runs on Intel${c_reset}"
echo "(on Apple Silicon it runs through Rosetta at best)."
echo
echo "It does not delete SisterHL2030 (the new, native driver)."
echo "The printer may stop printing until you install Sister HL-2030."
echo

echo "${c_bold}Looking for the official drivers…${c_reset}"
echo
reasons="$(official_brother_reasons || true)"
if [[ -z "$reasons" ]]; then
  echo "${c_green}No official Brother drivers found on this Mac.${c_reset}"
  echo "There is nothing to remove. You can go straight to \"Install Sister HL2030.sh\"."
  pause
  exit 0
fi

echo "Found this:"
echo "$reasons"
echo
echo "${c_yellow}You will be asked for this Mac's administrator password.${c_reset}"
echo "It is the same one you use to install programs."
echo
if ! ask_yes "Type YES (in capitals) and press Enter to uninstall:"; then
  echo
  echo "Cancelled. Nothing was changed."
  pause
  exit 0
fi

echo
echo "Requesting authorization…"
run_as_admin "$SCRIPT_DIR/_privileged-uninstall.sh" \
  "SisterHL2030 needs permission to remove Brother's Intel drivers."

echo
echo "${c_bold}Confirming they are gone…${c_reset}"
echo
leftover="$(official_brother_reasons || true)"
if [[ -n "$leftover" ]]; then
  echo "${c_yellow}Something is still left:${c_reset}"
  echo "$leftover"
  echo
  echo "You can run this program again. If the problem persists,"
  echo "restart the Mac and try once more."
  pause
  exit 1
fi

echo "${c_green}Official Brother drivers removed.${c_reset}"
echo
echo "Next step: run"
echo "  ${c_bold}Install Sister HL2030.sh${c_reset}"
echo "from the same folder."
pause
exit 0
