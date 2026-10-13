#!/bin/bash
# Build pinned static dependencies for one architecture and macOS minimum.
# Usage: build_static_deps.sh arm64|x86_64 [PREFIX]
# Prints only the completed prefix to stdout, suitable for command substitution.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ARCH="${1:?Specify arm64 or x86_64}"
case "$ARCH" in
  arm64) OPENSSL_TARGET=darwin64-arm64-cc ;;
  x86_64) OPENSSL_TARGET=darwin64-x86_64-cc ;;
  *) echo "Unsupported architecture: $ARCH" >&2; exit 1 ;;
esac
PREFIX="${2:-$ROOT/distrib/deps-$ARCH}"
MIN="${SISTER_MACOS_MIN:-11.0}"
STAMP_VALUE="deps-3-$ARCH-macos$MIN-openssl3.6.4-libpng1.6.58-libusb1.0.30"
WORK="$ROOT/distrib/deps-$ARCH-work"
if [ "$(cat "$PREFIX/.sister-deps-stamp" 2>/dev/null || true)" = "$STAMP_VALUE" ]; then
  echo "$PREFIX"
  exit 0
fi
mkdir -p "$WORK" "$PREFIX"
# Resolve before entering the source directory, including a relative prefix.
PREFIX="$(cd "$PREFIX" && pwd)"
cd "$WORK"
fetch() {
  [ -f "$3" ] || curl -fsSL -o "$3" "$1"
  echo "$2  $3" | shasum -a 256 -c - >&2
}
fetch https://github.com/openssl/openssl/releases/download/openssl-3.6.4/openssl-3.6.4.tar.gz \
  9bffaa1ad1e07b354c21bd3324ec02fa15579f45a7d0494b3e74bc449b7333ef openssl-3.6.4.tar.gz
fetch https://download.sourceforge.net/libpng/libpng-1.6.58.tar.xz \
  28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775 libpng-1.6.58.tar.xz
fetch https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2 \
  fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf libusb-1.0.30.tar.bz2
rm -rf openssl-3.6.4 libpng-1.6.58 libusb-1.0.30
export MACOSX_DEPLOYMENT_TARGET="$MIN"
export CC="clang -arch $ARCH -mmacosx-version-min=$MIN"
jobs="${SISTER_BUILD_JOBS:-$(sysctl -n hw.ncpu)}"
echo "Building $ARCH OpenSSL for macOS ${MIN}…" >&2
tar xzf openssl-3.6.4.tar.gz
(cd openssl-3.6.4 &&
  ./Configure "$OPENSSL_TARGET" no-shared no-tests no-apps no-docs \
    "-mmacosx-version-min=$MIN" --prefix="$PREFIX" --libdir=lib >/dev/null &&
  make -j"$jobs" >/dev/null && make install_sw >/dev/null)
echo "Building $ARCH libpng for macOS ${MIN}…" >&2
tar xJf libpng-1.6.58.tar.xz
(cd libpng-1.6.58 &&
  ./configure --host="$ARCH-apple-darwin" --prefix="$PREFIX" \
    --disable-shared --enable-static >/dev/null &&
  make -j"$jobs" >/dev/null && make install >/dev/null)
echo "Building $ARCH libusb for macOS ${MIN}…" >&2
tar xjf libusb-1.0.30.tar.bz2
(cd libusb-1.0.30 &&
  # SDK 27 declares pipe2, but macOS 11 cannot supply it at runtime.
  ac_cv_func_pipe2=no ./configure --host="$ARCH-apple-darwin" --prefix="$PREFIX" \
    --disable-shared --enable-static >/dev/null &&
  make -j"$jobs" >/dev/null && make install >/dev/null)
for archive in libssl.a libcrypto.a libpng16.a libusb-1.0.a; do
  archs="$(lipo -archs "$PREFIX/lib/$archive")"
  [ "$archs" = "$ARCH" ] || { echo "$archive is '$archs', expected $ARCH" >&2; exit 1; }
done
rm -rf "$WORK"/openssl-3.6.4 "$WORK"/libpng-1.6.58 "$WORK"/libusb-1.0.30
echo "$STAMP_VALUE" > "$PREFIX/.sister-deps-stamp"
echo "$PREFIX"
