#!/bin/bash
# Generate an on-screen preview of the Atkinson halftone (no paper).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

require_macos
banner "Preview the halftone (on screen)"
echo "This does NOT print. It opens a black-and-white image in Preview,"
echo "screened with the same Atkinson dithering the HL-2030 uses."
echo
echo "Left = original.  Right = how it should come out on paper."
echo

cd "$ROOT"
if [[ ! -x build/sister-preview ]]; then
  echo "Building sister-preview…"
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target sister-preview -j
fi

out="$HOME/Desktop/sister-halftone-preview.bmp"
if [[ $# -ge 1 ]]; then
  echo "Processing: $1"
  ./build/sister-preview -o "$out" "$1"
else
  echo "No file given: generating a test chart (ramps, grays, colors)."
  ./build/sister-preview --chart -o "$out"
fi

echo
echo "File: $out"
open -a Preview "$out" 2>/dev/null || open "$out"
echo "If the right side is almost all black, the algorithm is inverted."
echo "If the right side shows grays as a dot pattern, it is correct and the"
echo "printing problem is in the raster macOS sends."
pause
