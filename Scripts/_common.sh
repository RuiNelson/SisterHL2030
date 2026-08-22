# Shared helpers for the novice-facing SisterHL2030 scripts.
# Not meant to be run directly.

BROTHER_PKG_ID="com.brother.PrinterDriver.MonochromeLaser.pkg"
BROTHER_DIR="/Library/Printers/Brother"
BROTHER_FILTER="$BROTHER_DIR/Filter/rastertobrother2030.bundle/Contents/MacOS/rastertobrother2030"
PPD_DIR="/Library/Printers/PPDs/Contents/Resources"
SISTER_ROOT="/Library/Printers/SisterHL2030"
SISTER_FILTERDIR="$SISTER_ROOT/filter"
SISTER_FILTER="$SISTER_FILTERDIR/rastertosisterhl2030"
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
  read -r -p "Carrega Enter para fechar esta janela. " _ || true
}

die() {
  echo
  echo "${c_red}${c_bold}Erro${c_reset}: $*"
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
    die "Este programa só funciona no macOS."
  fi
}

ask_yes() {
  local prompt="$1"
  local answer
  echo
  read -r -p "$prompt " answer || true
  answer="$(printf '%s' "$answer" | tr '[:lower:]' '[:upper:]')"
  [[ "$answer" == "SIM" ]]
}

# Prints one "  • reason" line per hit on stdout.
# Returns 0 if official Brother (Intel) software is present.
official_brother_reasons() {
  local found=1
  if pkgutil --pkg-info "$BROTHER_PKG_ID" >/dev/null 2>&1; then
    echo "  • Pacote do sistema: $BROTHER_PKG_ID"
    found=0
  fi
  if [[ -e "$BROTHER_DIR" ]]; then
    echo "  • Pasta $BROTHER_DIR"
    found=0
  fi
  if [[ -e "$BROTHER_FILTER" ]]; then
    echo "  • Filtro Intel rastertobrother2030"
    found=0
  fi
  if [[ -d "$PPD_DIR" ]]; then
    local n=0
    n="$(find "$PPD_DIR" -maxdepth 1 -name 'Brother *' 2>/dev/null | wc -l | tr -d ' ')"
    if [[ "${n:-0}" -gt 0 ]]; then
      echo "  • $n ficheiros PPD Brother em $PPD_DIR"
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
  if [[ -x "$SISTER_FILTER" ]]; then
    echo "  • Filtro $SISTER_FILTER"
    found=0
  fi
  if [[ -f "$PPD_DIR/Sister HL-2030.ppd" || -f "$PPD_DIR/Sister HL-2030.gz" ]]; then
    echo "  • PPD clássico Sister HL-2030 (é isto que o CUPS marca como obsoleto)"
    found=0
  fi
  if lpstat -v "$DEFAULT_QUEUE" 2>/dev/null | grep -q 'usb://Brother/HL-2030'; then
    echo "  • Fila ${DEFAULT_QUEUE} ainda aponta para o USB em bruto (PPD)"
    found=0
  fi
  return "$found"
}

detect_hl2030_uri() {
  lpinfo -v 2>/dev/null | awk '/usb:\/\/Brother\/HL-2030/{print $2; exit}'
}

# The IPP façade used to advertise via Bonjour as "HL-2030", so macOS
# showed a second printer next to the CUPS queue. Keep only `keep`.
remove_duplicate_sister_queues() {
  local keep="${1:-$DEFAULT_QUEUE}"
  local line name uri
  while IFS= read -r line; do
    name=""
    uri=""
    if [[ "$line" == "dispositivo para "* ]]; then
      name="${line#dispositivo para }"
      uri="${name#*: }"
      name="${name%%:*}"
    elif [[ "$line" == "device for "* ]]; then
      name="${line#device for }"
      uri="${name#*: }"
      name="${name%%:*}"
    else
      continue
    fi
    [[ "$name" == "$keep" ]] && continue
    if [[ "$uri" == *":8631"* || "$uri" == *"HL-2030._ipp._tcp"* ]]; then
      echo "A remover fila duplicada: $name"
      /usr/sbin/lpadmin -x "$name" 2>/dev/null || true
    fi
  done < <(lpstat -v 2>/dev/null || true)
  local extra
  for extra in HL_2030 HL-2030 localhost; do
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
    error ("Passo de administrador falhou (codigo " & errNum & "):" & return & errMsg) number errNum
  end try
end run
APPLESCRIPT
  )" || status=$?
  if [[ -n "$output" ]]; then
    echo "$output"
  fi
  if [[ "$status" -ne 0 ]]; then
    echo
    echo "O macOS recusou a autorização, ou o comando privilegiado saiu com erro."
    return "$status"
  fi
  return 0
}
