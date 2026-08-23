#!/bin/bash
# Remove SisterHL2030 from this Mac: the printer application, its
# LaunchDaemon, its printer state and the print queues pointing at it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

trap 'echo; echo "${c_red}The program stopped with an error.${c_reset}"; pause; exit 1' ERR

require_macos

banner "Remove Sister HL-2030"
echo "This removes the SisterHL2030 driver from this Mac:"
echo "  • the printer application in $SISTER_ROOT"
echo "  • its background service"
echo "  • the print queues that point at it"
echo
echo "Your printer itself is not touched, and nothing of Brother's is"
echo "reinstalled. After this the HL-2030 will not print until you install"
echo "a driver again."
echo

found=0
if [[ -e "$SISTER_ROOT" ]]; then
  echo "  • Found $SISTER_ROOT"
  found=1
fi
if [[ -f /Library/LaunchDaemons/org.sisterhl2030.printer.plist ]]; then
  echo "  • Found the background service"
  found=1
fi
if LC_ALL=C lpstat -v 2>/dev/null | grep -q ':8631\|HL-2030._ipp'; then
  echo "  • Found a print queue pointing at SisterHL2030"
  found=1
fi
if [[ "$found" -eq 0 ]]; then
  echo "${c_green}SisterHL2030 does not seem to be installed.${c_reset}"
  pause
  exit 0
fi

echo
echo "${c_yellow}You will be asked for this Mac's administrator password.${c_reset}"
if ! ask_yes "Type YES and press Enter to remove it:"; then
  echo
  echo "Cancelled. Nothing was changed."
  pause
  exit 0
fi

echo
run_as_admin "$SCRIPT_DIR/_privileged-sister-uninstall.sh" \
  "SisterHL2030 needs permission to remove itself." "$DEFAULT_QUEUE"

echo
if [[ -e "$SISTER_ROOT" ]]; then
  echo "${c_yellow}Something is still left in $SISTER_ROOT.${c_reset}"
  echo "You can run this program again."
else
  echo "${c_green}SisterHL2030 removed.${c_reset}"
fi
pause
exit 0
