# Shared helpers for the novice-facing SisterHL2030 scripts.
# Not meant to be run directly.

BROTHER_PKG_ID="com.brother.PrinterDriver.MonochromeLaser.pkg"
BROTHER_DIR="/Library/Printers/Brother"
BROTHER_FILTER="$BROTHER_DIR/Filter/rastertobrother2030.bundle/Contents/MacOS/rastertobrother2030"
PPD_DIR="/Library/Printers/PPDs/Contents/Resources"
SISTER_ROOT="/Library/Printers/SisterHL2030"
SISTER_APP="$SISTER_ROOT/sister-printer-app"
# Where the retired ippeveprinter façade kept its filter.
SISTER_OLD_FILTER="$SISTER_ROOT/filter/rastertosisterhl2030"
DEFAULT_QUEUE="Brother_HL_2030_series"

if [[ -z "${SCRIPT_DIR:-}" ]]; then
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

c_reset=$'\033[0m'
c_bold=$'\033[1m'
c_dim=$'\033[2m'
c_red=$'\033[31m'
c_green=$'\033[32m'
c_yellow=$'\033[33m'

if [[ ! -t 1 ]]; then
  c_reset=""; c_bold=""; c_dim=""; c_red=""; c_green=""; c_yellow=""
fi

pause() {
  echo
  read -r -p "Press Enter to close this window. " _ || true
}

die() {
  echo
  echo "${c_red}${c_bold}Error${c_reset}: $*"
  pause
  exit 1
}

banner() {
  local title="$1"
  echo
  echo "${c_bold}========================================${c_reset}"
  echo "${c_bold}  $title${c_reset}"
  echo "${c_bold}========================================${c_reset}"
  echo
}

require_macos() {
  if [[ "$(uname -s)" != "Darwin" ]]; then
    die "This program only runs on macOS."
  fi
}

ask_yes() {
  local prompt="$1"
  local answer
  echo
  read -r -p "$prompt " answer || true
  answer="$(printf '%s' "$answer" | tr '[:lower:]' '[:upper:]')"
  [[ "$answer" == "YES" ]]
}

# Prints one "  • reason" line per hit on stdout.
# Returns 0 if official Brother (Intel) software is present.
official_brother_reasons() {
  local found=1
  if pkgutil --pkg-info "$BROTHER_PKG_ID" >/dev/null 2>&1; then
    echo "  • System package: $BROTHER_PKG_ID"
    found=0
  fi
  if [[ -e "$BROTHER_DIR" ]]; then
    echo "  • Folder $BROTHER_DIR"
    found=0
  fi
  if [[ -e "$BROTHER_FILTER" ]]; then
    echo "  • Intel filter rastertobrother2030"
    found=0
  fi
  if [[ -d "$PPD_DIR" ]]; then
    local n=0
    n="$(find "$PPD_DIR" -maxdepth 1 -name 'Brother *' 2>/dev/null | wc -l | tr -d ' ')"
    if [[ "${n:-0}" -gt 0 ]]; then
      echo "  • $n Brother PPD files in $PPD_DIR"
      found=0
    fi
  fi
  return "$found"
}

has_official_brother() {
  official_brother_reasons >/dev/null
}

# Previous SisterHL2030 (classic PPD) leftovers. Install replaces these;
# they are not a reason to abort.
previous_sister_reasons() {
  local found=1
  if [[ -x "$SISTER_OLD_FILTER" ]]; then
    echo "  • ippeveprinter façade filter $SISTER_OLD_FILTER"
    found=0
  fi
  if [[ -f "$SISTER_ROOT/printer-attrs.conf" ]]; then
    echo "  • ippeveprinter attributes file (replaced by the printer application)"
    found=0
  fi
  if [[ -f "$PPD_DIR/Sister HL-2030.ppd" || -f "$PPD_DIR/Sister HL-2030.gz" ]]; then
    echo "  • Classic Sister HL-2030 PPD (this is what CUPS reports as deprecated)"
    found=0
  fi
  if LC_ALL=C lpstat -v "$DEFAULT_QUEUE" 2>/dev/null | grep -q 'usb://Brother/HL-2030'; then
    echo "  • Queue ${DEFAULT_QUEUE} still points at raw USB (PPD)"
    found=0
  fi
  return "$found"
}

detect_hl2030_uri() {
  LC_ALL=C lpinfo -v 2>/dev/null | awk '/usb:\/\/Brother\/HL-2030/{print $2; exit}'
}

# Drop extra CUPS queues that point at the Sister IPP façade, so this Mac
# keeps a single added printer. AirPrint on the LAN still uses Bonjour.
# LC_ALL=C keeps lpstat output in English, whatever the user's locale is.
#
# A queue left on a dnssd:// URI is not just untidy: if CUPS also shares that
# queue, the Mac advertises it back on Bonjour and the URI can resolve into
# that copy of itself, so jobs hang on "looking for the printer".
remove_duplicate_sister_queues() {
  local keep="${1:-$DEFAULT_QUEUE}"
  local line name uri
  while IFS= read -r line; do
    [[ "$line" == "device for "* ]] || continue
    name="${line#device for }"
    uri="${name#*: }"
    name="${name%%:*}"
    [[ "$name" == "$keep" ]] && continue
    # "HL-2030._ipp" matches both _ipp._tcp and the _ipps._tcp TLS variant,
    # which is the one macOS discovers and adds first.
    if [[ "$uri" == *":8631"* || "$uri" == *"HL-2030._ipp"* ]]; then
      echo "Removing duplicate queue: $name"
      /usr/sbin/lpadmin -x "$name" 2>/dev/null || true
    fi
  done < <(LC_ALL=C lpstat -v 2>/dev/null || true)
  local extra
  for extra in HL_2030 HL-2030 localhost Brother_HL_2030; do
    [[ "$extra" == "$keep" ]] && continue
    /usr/sbin/lpadmin -x "$extra" 2>/dev/null || true
  done
}

run_as_admin() {
  local script="$1"
  local prompt="$2"
  shift 2
  local output=""
  local status=0
  # Arguments after the prompt are passed to the privileged script.
  # Do not use a wrapper in $TMPDIR: root cannot read the user's
  # private /var/folders/.../T on recent macOS.
  output="$(/usr/bin/osascript - "$script" "$prompt" "$@" <<'APPLESCRIPT'
on run argv
  set theScript to item 1 of argv
  set thePrompt to item 2 of argv
  set cmd to "/bin/bash " & quoted form of theScript
  if (count of argv) > 2 then
    repeat with i from 3 to count of argv
      set cmd to cmd & " " & quoted form of (item i of argv)
    end repeat
  end if
  try
    return do shell script cmd with administrator privileges with prompt thePrompt
  on error errMsg number errNum
    error ("Administrator step failed (code " & errNum & "):" & return & errMsg) number errNum
  end try
end run
APPLESCRIPT
  )" || status=$?
  if [[ -n "$output" ]]; then
    echo "$output"
  fi
  if [[ "$status" -ne 0 ]]; then
    echo
    echo "macOS refused the authorization, or the privileged command exited with an error."
    return "$status"
  fi
  return 0
}
