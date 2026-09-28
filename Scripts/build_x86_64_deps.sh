#!/bin/bash
# Cross-build the static x86_64 OpenSSL, libpng and libusb that the x86_64
# half of sister-printer-app links (see "Architectures" in CLAUDE.md).
#
# Why this exists: Homebrew is arm64-only on current macOS (its installer
# refuses Intel), so there is no /usr/local tree to take x86_64 archives
# from, and the /opt/homebrew ones are arm64. Apple clang builds x86_64
# happily on an Apple Silicon Mac, so build the three libraries ourselves,
# static, into a private prefix and point pkg-config at it.
#
# Versions and SHA-256 sums match Homebrew's formulae at the time of writing,
# and OpenSSL's own published .sha256.
#
#   Scripts/build_x86_64_deps.sh [PREFIX]     (default distrib/deps-x86_64)
#
# Prints the prefix on success. Idempotent: a finished prefix is reused.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PREFIX="${1:-$ROOT/distrib/deps-x86_64}"
STAMP_VALUE="deps-2-openssl3.6.4-libpng1.6.58-libusb1.0.30"
WORK="$ROOT/distrib/deps-x86_64-work"
MIN=11.0

if [ "$(cat "$PREFIX/.sister-deps-stamp" 2>/dev/null || true)" = "$STAMP_VALUE" ]; then
  echo "$PREFIX"
  exit 0
fi

mkdir -p "$WORK"
cd "$WORK"

fetch() {  # url sha256 file
  [ -f "$3" ] || curl -fsSL -o "$3" "$1"
  echo "$2  $3" | shasum -a 256 -c - >&2
}

fetch https://github.com/openssl/openssl/releases/download/openssl-3.6.4/openssl-3.6.4.tar.gz \
  9bffaa1ad1e07b354c21bd3324ec02fa15579f45a7d0494b3e74bc449b7333ef openssl-3.6.4.tar.gz
fetch https://download.sourceforge.net/libpng/libpng-1.6.58.tar.xz \
  28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775 libpng-1.6.58.tar.xz
fetch https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2 \
  fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf libusb-1.0.30.tar.bz2

rm -rf "$PREFIX" openssl-3.6.4 libpng-1.6.58 libusb-1.0.30
mkdir -p "$PREFIX"
export CC="clang -arch x86_64 -mmacosx-version-min=$MIN"
jobs="$(sysctl -n hw.ncpu)"

echo "Building x86_64 OpenSSL…" >&2
tar xzf openssl-3.6.4.tar.gz
(cd openssl-3.6.4 &&
  ./Configure darwin64-x86_64-cc no-shared no-tests no-apps no-docs \
    "-mmacosx-version-min=$MIN" --prefix="$PREFIX" --libdir=lib >/dev/null &&
  make -j"$jobs" >/dev/null && make install_sw >/dev/null)

echo "Building x86_64 libpng…" >&2
tar xJf libpng-1.6.58.tar.xz
(cd libpng-1.6.58 &&
  ./configure --host=x86_64-apple-darwin --prefix="$PREFIX" \
    --disable-shared --enable-static >/dev/null &&
  make -j"$jobs" >/dev/null && make install >/dev/null)

echo "Building x86_64 libusb…" >&2
tar xjf libusb-1.0.30.tar.bz2
(cd libusb-1.0.30 &&
  # The SDK declares pipe2 (macOS 27+), so configure finds it and libusb
  # calls it unguarded; on any older macOS it is a null symbol at runtime.
  # Deny it, and libusb falls back to pipe() + fcntl().
  ac_cv_func_pipe2=no ./configure --host=x86_64-apple-darwin --prefix="$PREFIX" \
    --disable-shared --enable-static >/dev/null &&
  make -j"$jobs" >/dev/null && make install >/dev/null)

for a in libssl.a libcrypto.a libpng16.a libusb-1.0.a; do
  archs="$(lipo -archs "$PREFIX/lib/$a")"
  [ "$archs" = x86_64 ] || { echo "$a is '$archs', expected x86_64" >&2; exit 1; }
done
rm -rf "$WORK"/openssl-3.6.4 "$WORK"/libpng-1.6.58 "$WORK"/libusb-1.0.30
echo "$STAMP_VALUE" > "$PREFIX/.sister-deps-stamp"
echo "$PREFIX"
