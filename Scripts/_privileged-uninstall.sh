#!/bin/bash
# Root helper. Invoked by "Uninstall Official Brother Drivers.sh".
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"

PKG_ID="com.brother.PrinterDriver.MonochromeLaser.pkg"
BROTHER_DIR="/Library/Printers/Brother"
PPD_DIR="/Library/Printers/PPDs/Contents/Resources"
LOG="/tmp/sisterhl2030-uninstall.log"

set +e
(
  set -euo pipefail
  echo "SisterHL2030 privileged uninstall"
  echo "date: $(date)"

  if pkgutil --pkg-info "$PKG_ID" >/dev/null 2>&1; then
    while IFS= read -r f; do
      [[ -n "$f" ]] || continue
      rm -f "/$f"
    done < <(pkgutil --only-files --files "$PKG_ID")

    while IFS= read -r d; do
      [[ -n "$d" ]] || continue
      rmdir "/$d" 2>/dev/null || true
    done < <(pkgutil --only-dirs --files "$PKG_ID" | /usr/bin/tail -r)

    pkgutil --forget "$PKG_ID" >/dev/null || true
    echo "Pacote $PKG_ID removido."
  else
    echo "O pacote $PKG_ID ja nao estava registado."
  fi

  rm -rf "$BROTHER_DIR"
  if [[ -d "$PPD_DIR" ]]; then
    find "$PPD_DIR" -maxdepth 1 \( -name 'Brother *' -o -name 'Brother*' \) -exec rm -f {} + 2>/dev/null || true
  fi

  echo "Concluido. SisterHL2030 nao foi tocado."
) >"$LOG" 2>&1
status=$?
chmod 644 "$LOG" 2>/dev/null || true
cat "$LOG"
exit "$status"
