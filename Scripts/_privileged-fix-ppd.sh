#!/bin/bash
# Patch an already-installed CUPS PPD so Supply Levels and Quality work.
# Usage: _privileged-fix-ppd.sh [QUEUE]
#
# Prefer Scripts/_privileged-create-queue.sh, which rebuilds the PPD from
# Apple's ipp2ppd. This script is the smaller hammer: drop *APSupplies
# (which makes macOS open the admin page instead of drawing level bars),
# turn on cupsIPPSupplies, and rewrite the Quality lines if they still
# map Normal to 300 dpi.
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"
set -euo pipefail

QUEUE="${1:-Brother_HL_2030_series}"
if [[ -f "$(cd "$(dirname "$0")" && pwd)/_privileged-create-queue.sh" ]]; then
  exec /bin/bash "$(cd "$(dirname "$0")" && pwd)/_privileged-create-queue.sh" "$QUEUE"
fi
echo "Missing _privileged-create-queue.sh" >&2
exit 1
