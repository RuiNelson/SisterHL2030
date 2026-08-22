#!/bin/bash
# Generate an on-screen preview of the Floyd–Steinberg halftone (no paper).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

require_macos
banner "Pré-visualizar halftone (ecrã)"
echo "Isto NÃO imprime. Abre no Preview uma imagem a preto e branco"
echo "com a mesma trama Floyd–Steinberg que a HL-2030 usa."
echo
echo "Esquerda = original.  Direita = como deve sair no papel."
echo

cd "$ROOT"
if [[ ! -x build/sister-preview ]]; then
  echo "A compilar sister-preview…"
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target sister-preview -j
fi

out="$HOME/Desktop/sister-halftone-preview.bmp"
if [[ $# -ge 1 ]]; then
  echo "A processar: $1"
  ./build/sister-preview -o "$out" "$1"
else
  echo "Sem ficheiro: a gerar um gráfico de teste (rampas, cinzentos, cores)."
  ./build/sister-preview --chart -o "$out"
fi

echo
echo "Ficheiro: $out"
open -a Preview "$out" 2>/dev/null || open "$out"
echo "Se o lado direito estiver quase todo preto, o algoritmo está invertido."
echo "Se o lado direito tiver cinzentos em trama de pontos, está correcto"
echo "e o problema da impressão está no raster que o macOS envia."
pause
