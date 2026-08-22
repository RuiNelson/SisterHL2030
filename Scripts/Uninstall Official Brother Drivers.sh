#!/bin/bash
# Remove the Intel-only official Brother CUPS package from this Mac.
# Safe to double-click from Finder if opened with Terminal.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

trap 'echo; echo "${c_red}O programa parou com um erro.${c_reset}"; pause; exit 1' ERR

require_macos

banner "Remover drivers oficiais da Brother"
echo "Este programa desinstala o software de impressão da Brother"
echo "que veio da Apple/Brother em 2014 e ${c_bold}só funciona em Intel${c_reset}"
echo "(no Apple Silicon corre, no máximo, através do Rosetta)."
echo
echo "Não apaga o SisterHL2030 (o driver novo, nativo)."
echo "A impressora pode deixar de imprimir até instalares o Sister HL-2030."
echo

echo "${c_bold}A procurar drivers oficiais…${c_reset}"
echo
reasons="$(official_brother_reasons || true)"
if [[ -z "$reasons" ]]; then
  echo "${c_green}Não encontrei drivers oficiais da Brother neste Mac.${c_reset}"
  echo "Não há nada para remover. Podes passar ao «Install Sister HL2030.sh»."
  pause
  exit 0
fi

echo "Encontrei isto:"
echo "$reasons"
echo
echo "${c_yellow}Vai ser pedida a palavra-passe de administrador do Mac.${c_reset}"
echo "É a mesma que usas para instalar programas."
echo
if ! ask_yes "Escreve SIM (em maiúsculas) e carrega Enter para desinstalar:"; then
  echo
  echo "Cancelado. Não foi alterado nada."
  pause
  exit 0
fi

echo
echo "A pedir autorização…"
run_as_admin "$SCRIPT_DIR/_privileged-uninstall.sh" \
  "O SisterHL2030 precisa de autorização para remover os drivers Intel da Brother."

echo
echo "${c_bold}A confirmar que desapareceram…${c_reset}"
echo
leftover="$(official_brother_reasons || true)"
if [[ -n "$leftover" ]]; then
  echo "${c_yellow}Ainda ficou alguma coisa:${c_reset}"
  echo "$leftover"
  echo
  echo "Podes voltar a correr este programa. Se o problema continuar,"
  echo "reinicia o Mac e tenta outra vez."
  pause
  exit 1
fi

echo "${c_green}Drivers oficiais da Brother removidos.${c_reset}"
echo
echo "Próximo passo: corre o programa"
echo "  ${c_bold}Install Sister HL2030.sh${c_reset}"
echo "que está na mesma pasta."
pause
exit 0
