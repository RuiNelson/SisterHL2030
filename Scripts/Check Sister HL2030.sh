#!/bin/bash
# Show whether the SisterHL2030 driver that is installed and running matches
# the one just built. No administrator password.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

require_macos

banner "Check Sister HL-2030 driver"

INSTALLED_APP="$SISTER_ROOT/sister-printer-app"
BUILT_APP="$ROOT/build/sister-printer-app"
DEST_VERSION="$SISTER_ROOT/VERSION"

fail=0
say() { printf '%-12s %s\n' "$1" "$2"; }

built=""
if [[ -x "$BUILT_APP" ]]; then
  built="$("$BUILT_APP" --version 2>/dev/null || true)"
fi
installed=""
if [[ -x "$INSTALLED_APP" ]]; then
  installed="$("$INSTALLED_APP" --version 2>/dev/null || true)"
fi
stamped=""
if [[ -f "$DEST_VERSION" ]]; then
  stamped="$(tr -d '[:space:]' < "$DEST_VERSION")"
fi

running=""
if command -v ipptool >/dev/null 2>&1 && nc -z localhost 8631 2>/dev/null; then
  running="$(ipptool -tv ipp://localhost:8631/ipp/print \
    /usr/share/cups/ipptool/get-printer-attributes.test 2>/dev/null |
    awk -F'= ' '/printer-firmware-string-version/{print $2; exit}')"
  running="${running%%,*}"
  running="$(printf '%s' "$running" | tr -d '[:space:]')"
fi

pid=""
if launchctl print system/org.sisterhl2030.printer >/dev/null 2>&1; then
  pid="$(launchctl print system/org.sisterhl2030.printer 2>/dev/null |
    awk '/pid =/{print $3; exit}')"
fi

echo "Driver version (semver+git). A mismatch means the running copy is stale."
echo
if [[ -n "$built" ]]; then
  say "built" "$built"
else
  say "built" "(no build/sister-printer-app — compile first)"
fi
if [[ -n "$installed" ]]; then
  say "installed" "$installed"
elif [[ -x "$INSTALLED_APP" ]]; then
  say "installed" "(binary present but --version failed; not signed?)"
  fail=1
else
  say "installed" "(missing $INSTALLED_APP)"
  fail=1
fi
if [[ -n "$stamped" ]]; then
  say "stamp file" "$stamped"
fi
if [[ -n "$running" ]]; then
  say "running IPP" "$running"
else
  say "running IPP" "(nothing on localhost:8631)"
  fail=1
fi
if [[ -n "$pid" ]]; then
  say "launchd" "org.sisterhl2030.printer pid $pid"
else
  say "launchd" "not running"
  fail=1
fi

echo
if [[ -n "$built" && -n "$installed" && "$built" != "$installed" ]]; then
  echo "${c_red}Installed binary is not the one in build/.${c_reset}"
  echo "Run ${c_bold}Scripts/Install Sister HL2030.sh${c_reset} or:"
  echo "  sudo bash Scripts/_privileged-update-filter.sh \"$ROOT\""
  fail=1
fi
if [[ -n "$installed" && -n "$stamped" && "$installed" != "$stamped" ]]; then
  echo "${c_yellow}VERSION stamp does not match the installed binary.${c_reset}"
fi
if [[ -n "$installed" && -n "$running" ]]; then
  inst_semver="${installed%%+*}"
  if [[ "$running" != "$inst_semver" ]]; then
    echo "${c_red}Running firmware $running is not installed $inst_semver.${c_reset}"
    echo "The LaunchDaemon is still the previous binary. Restart it:"
    echo "  sudo bash Scripts/_privileged-update-filter.sh \"$ROOT\""
    fail=1
  fi
fi

if [[ -x "$BUILT_APP" && -x "$INSTALLED_APP" ]]; then
  bsum="$(shasum -a 256 "$BUILT_APP" | awk '{print $1}')"
  isum="$(shasum -a 256 "$INSTALLED_APP" | awk '{print $1}')"
  if [[ "$bsum" == "$isum" ]]; then
    say "checksum" "build and install are identical"
  elif [[ -n "$built" && "$built" == "$installed" ]]; then
    say "checksum" "differ (ad-hoc signature on the installed copy)"
  else
    say "checksum" "build and install differ"
    fail=1
  fi
fi

PPD="/etc/cups/ppd/${DEFAULT_QUEUE}.ppd"
if [[ -f "$PPD" ]] && grep -q 'cupsInteger1 4 /HWResolution\[600 600\]' "$PPD"; then
  say "queue PPD" "Quality Draft/Normal/Fine at 300/600/600 dpi"
else
  say "queue PPD" "missing or stale (Normal is not 600 dpi)"
  fail=1
fi

echo
if [[ "$fail" -eq 0 ]]; then
  echo "${c_green}${c_bold}The installed driver is the latest build and it is running.${c_reset}"
  [[ -t 0 ]] && pause
  exit 0
fi
echo "${c_red}${c_bold}The driver is not the latest, or it is not running.${c_reset}"
[[ -t 0 ]] && pause
exit 1
