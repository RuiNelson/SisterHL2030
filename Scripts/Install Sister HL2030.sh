#!/bin/bash
# Build and install the native Apple Silicon SisterHL2030 CUPS filter.
# Refuses to run while official Intel Brother drivers are still present.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

trap 'echo; echo "${c_red}O programa parou com um erro.${c_reset}"; echo "Lê a mensagem acima — na maior parte dos casos basta instalar o que falta e voltar a tentar."; pause; exit 1' ERR

require_macos

banner "Instalar Sister HL-2030"
echo "Este programa instala um driver ${c_bold}nativo para Apple Silicon${c_reset}"
echo "da impressora Brother HL-2030 (series)."
echo
echo "Não usa o software Intel da Brother nem o Rosetta."
echo

echo "${c_bold}1/4  A verificar se ainda há drivers oficiais Intel…${c_reset}"
echo
reasons="$(official_brother_reasons || true)"
if [[ -n "$reasons" ]]; then
  echo "${c_red}${c_bold}Ainda estão instalados os drivers oficiais da Brother.${c_reset}"
  echo
  echo "Encontrei:"
  echo "$reasons"
  echo
  echo "O SisterHL2030 ${c_bold}não se instala por cima${c_reset} do pacote Intel."
  echo "Assim evita-se uma fila de impressão a meio caminho, a chamar"
  echo "o filtro antigo."
  echo
  echo "O que fazer:"
  echo "  1. Corre ${c_bold}Uninstall Official Brother Drivers.sh${c_reset}"
  echo "     (está na mesma pasta que este programa)."
  echo "  2. Volta a correr ${c_bold}Install Sister HL2030.sh${c_reset}."
  echo
  pause
  exit 1
fi
echo "${c_green}Não há drivers oficiais. Podemos continuar.${c_reset}"
echo

echo "${c_bold}    A verificar uma instalação anterior do SisterHL2030…${c_reset}"
echo
sister_old="$(previous_sister_reasons || true)"
if [[ -n "$sister_old" ]]; then
  echo "Encontrei a versão antiga (PPD CUPS), que o macOS marca como obsoleta:"
  echo "$sister_old"
  echo
  echo "Não precisas de a desinstalar à mão. Este programa ${c_bold}substitui-a${c_reset}:"
  echo "remove o PPD, instala o serviço IPP Everywhere e aponta a mesma fila."
else
  echo "Não há instalação anterior do SisterHL2030."
fi
echo

arch="$(uname -m)"
if [[ "$arch" != "arm64" ]]; then
  echo "${c_yellow}Atenção:${c_reset} este Mac reporta arquitectura \"${arch}\", não Apple Silicon."
  echo "O SisterHL2030 foi feito para Macs M1/M2/M3/M4."
  if ! ask_yes "Mesmo assim queres continuar? Escreve SIM:"; then
    echo "Cancelado."
    pause
    exit 0
  fi
  echo
fi

echo "${c_bold}2/4  A preparar as ferramentas de compilação…${c_reset}"
if ! xcode-select -p >/dev/null 2>&1; then
  die "Não encontrei as Command Line Tools da Apple.
No Terminal, corre:

    xcode-select --install

Quando a instalação acabar, volta a este programa."
fi
if ! command -v cmake >/dev/null 2>&1; then
  die "Não encontrei o CMake (é o programa que constrói o driver).

Se tens o Homebrew, no Terminal corre:

    brew install cmake

Não tens o Homebrew? Abre https://brew.sh e segue as instruções,
depois instala o cmake e volta aqui."
fi
echo "CMake e compilador encontrados."
echo

echo "${c_bold}3/4  A compilar o encoder para este Mac…${c_reset}"
echo "${c_dim}(demora menos de um minuto na primeira vez)${c_reset}"
echo
cd "$ROOT"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target rastertosisterhl2030 -j
echo
if ! file "$ROOT/build/rastertosisterhl2030" | grep -q 'arm64\|x86_64\|executable'; then
  die "A compilação não produziu o filtro. Vê as mensagens acima."
fi
echo "${c_green}Encoder compilado.${c_reset}  $(file -b "$ROOT/build/rastertosisterhl2030")"
echo

uri="$(detect_hl2030_uri || true)"
queue="$DEFAULT_QUEUE"
if [[ -n "$uri" ]]; then
  echo "Encontrei a HL-2030 ligada por USB:"
  echo "  $uri"
else
  echo "${c_yellow}Não vi uma HL-2030 no USB neste momento.${c_reset}"
  echo "O serviço IPP mesmo assim vai ser instalado. Liga a impressora,"
  echo "grava o URI USB em /Library/Printers/SisterHL2030/device-uri"
  echo "e adiciona a impressora em Definições do Sistema."
fi
echo
echo "${c_yellow}Vai ser pedida a palavra-passe de administrador do Mac.${c_reset}"
if ! ask_yes "Escreve SIM e carrega Enter para instalar:"; then
  echo
  echo "Cancelado. O driver foi compilado mas não foi instalado."
  pause
  exit 0
fi

echo
echo "${c_bold}4/4  A instalar…${c_reset}"
run_as_admin "$SCRIPT_DIR/_privileged-install.sh" \
  "O SisterHL2030 precisa de autorização para instalar o driver nativo da HL-2030." \
  "$ROOT" "$queue" "${uri:--}"

echo
if [[ ! -x "$SISTER_FILTER" ]]; then
  die "A instalação não deixou o filtro em $SISTER_FILTER."
fi

echo "A esperar pelo serviço IPP Everywhere em localhost:8631…"
ipp_ready=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do
  if /usr/bin/nc -z localhost 8631 2>/dev/null; then
    ipp_ready=1
    break
  fi
  sleep 0.5
done
if [[ "$ipp_ready" -ne 1 ]]; then
  echo "${c_yellow}O serviço IPP ainda não está à escuta. Vou configurar a fila na mesma.${c_reset}"
  echo "Se a impressora não aparecer, corre outra vez este instalador."
fi

echo "A criar a fila CUPS como impressora IPP Everywhere (sem PPD)…"
if /usr/sbin/lpadmin -p "$queue" \
  -v "ipp://localhost:8631/ipp/print" \
  -E \
  -D "Brother HL-2030 series" \
  -L "SisterHL2030" \
  -o printer-is-shared=false; then
  /usr/sbin/cupsenable "$queue" 2>/dev/null || true
  /usr/sbin/cupsaccept "$queue" 2>/dev/null || true
  echo "${c_green}Fila \"${queue}\" aponta para ipp://localhost:8631/ipp/print${c_reset}"
else
  echo "${c_yellow}Não consegui criar a fila automaticamente.${c_reset}"
  echo "Em Definições do Sistema → Impressoras, adiciona a impressora"
  echo "\"HL-2030\" (IPP Everywhere / AirPrint) que o SisterHL2030 anuncia."
fi

echo
echo "${c_green}${c_bold}Sister HL-2030 instalado.${c_reset}"
echo
echo "Isto já não é um PPD CUPS clássico (é isso que o macOS marca como obsoleto)."
echo "O CUPS fala IPP Everywhere com o SisterHL2030; o encoder arm64 fala"
echo "com a HL-2030 no USB."
echo
echo "Filtro: $SISTER_FILTER"
if [[ -x "$SISTER_FILTER" ]]; then
  echo "        $(file -b "$SISTER_FILTER")"
fi
echo
echo "Para testar: abre uma página no Preview e imprime na"
echo "\"Brother HL-2030 series\"."
echo
echo "Modos no diálogo de impressão, em ${c_bold}Qualidade${c_reset}:"
echo "  • Rascunho — 300 dpi, com poupança de toner"
echo "  • Normal   — 600 dpi, com poupança de toner (predefinição)"
echo "  • Alta     — HQ1200, toner cheio (o modo fino da Brother)"
echo
echo "Se a página sair em branco ou invertida, abre um problema no"
echo "repositório SisterHL2030 e descreve o que viste."
pause
exit 0
