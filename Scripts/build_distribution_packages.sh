#!/bin/bash
# Build the three signed .pkg installers in distrib/:
#   InstallSisterDrivers.pkg    - the native arm64 driver
#   UninstallSisterDrivers.pkg  - removes it
#   UninstallBrotherDrivers.pkg - removes the official Intel Brother package
#
# Not a novice-facing script: run it by hand from a Terminal to produce
# the packages that get handed out. Requires a "Developer ID Installer"
# identity in the keychain (a different cert type than "Developer ID
# Application", which only signs binaries, not installer packages).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DISTRIB="$ROOT/distrib"
VERSION="$(sed -n 's/^project(sisterhl2030 VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"

# Pinned to the SHA-1 fingerprint, not the display name: the certificate
# ended up in both the login and System keychains (Keychain Access's default
# import target differs from where `security import` put the matching
# private key), so a name lookup matches twice and pkgbuild's --sign
# resolution becomes ambiguous. The fingerprint is unambiguous either way.
SIGN_ID="${SISTER_INSTALLER_IDENTITY:-89328DFCD49A1B576317AB81F3AF28003FB1D0A4}"
if ! security find-identity -v 2>/dev/null | grep -q "$SIGN_ID"; then
  echo "No \"$SIGN_ID\" identity in the keychain. Import your Developer ID" >&2
  echo "Installer certificate first (see distrib/DeveloperIDInstaller.csr)." >&2
  exit 1
fi

if [[ ! -x "$ROOT/build/sister-printer-app" || ! -x "$ROOT/build/sister-status" ]]; then
  echo "Build the driver first:" >&2
  echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSISTER_WITH_PAPPL=ON" >&2
  echo "  cmake --build build --target sister-printer-app sister-status -j" >&2
  exit 1
fi

echo "Rebuilding the install payload from build/, launchd/ and docs/…"
PAYLOAD="$DISTRIB/root-install/Library/Printers/SisterHL2030"
LAUNCHD_DEST="$DISTRIB/root-install/Library/LaunchDaemons"
mkdir -p "$PAYLOAD" "$LAUNCHD_DEST"

cp "$ROOT/build/sister-printer-app" "$PAYLOAD/sister-printer-app"
cp "$ROOT/build/sister-status" "$PAYLOAD/sister-status"
cp "$ROOT/Scripts/_privileged-create-queue.sh" "$PAYLOAD/.create-queue.sh"
chmod 755 "$PAYLOAD/sister-printer-app" "$PAYLOAD/sister-status" "$PAYLOAD/.create-queue.sh"

cp "$ROOT/launchd/org.sisterhl2030.printer.plist" \
   "$LAUNCHD_DEST/org.sisterhl2030.printer.plist"
chmod 644 "$LAUNCHD_DEST/org.sisterhl2030.printer.plist"

# icon.png (IPP) and .sister.icns (CUPS, linked into a PPD by postinstall)
# regenerated the same way Scripts/_privileged-icon.sh does, from the one
# tracked source image.
sips -z 512 512 -s format png "$ROOT/docs/sister.png" --out "$PAYLOAD/icon.png" >/dev/null
icon_tmp="$(mktemp -d)"
trap 'rm -rf "$icon_tmp"' EXIT
iconset="$icon_tmp/sister.iconset"
mkdir -p "$iconset"
sips -z 16 16 "$ROOT/docs/sister.png" --out "$iconset/icon_16x16.png" >/dev/null
sips -z 32 32 "$ROOT/docs/sister.png" --out "$iconset/icon_16x16@2x.png" >/dev/null
sips -z 32 32 "$ROOT/docs/sister.png" --out "$iconset/icon_32x32.png" >/dev/null
sips -z 64 64 "$ROOT/docs/sister.png" --out "$iconset/icon_32x32@2x.png" >/dev/null
sips -z 128 128 "$ROOT/docs/sister.png" --out "$iconset/icon_128x128.png" >/dev/null
sips -z 256 256 "$ROOT/docs/sister.png" --out "$iconset/icon_128x128@2x.png" >/dev/null
sips -z 256 256 "$ROOT/docs/sister.png" --out "$iconset/icon_256x256.png" >/dev/null
sips -z 512 512 "$ROOT/docs/sister.png" --out "$iconset/icon_256x256@2x.png" >/dev/null
sips -z 512 512 "$ROOT/docs/sister.png" --out "$iconset/icon_512x512.png" >/dev/null
sips -z 1024 1024 "$ROOT/docs/sister.png" --out "$iconset/icon_512x512@2x.png" >/dev/null
iconutil -c icns -o "$icon_tmp/sister.icns" "$iconset"
cp "$icon_tmp/sister.icns" "$PAYLOAD/.sister.icns"
chmod 644 "$PAYLOAD/icon.png" "$PAYLOAD/.sister.icns"
rm -rf "$icon_tmp"
trap - EXIT

echo "Building InstallSisterDrivers.pkg ($VERSION)…"
pkgbuild \
  --root "$DISTRIB/root-install" \
  --identifier org.sisterhl2030.pkg.install \
  --version "$VERSION" \
  --install-location / \
  --scripts "$DISTRIB/scripts-install" \
  --sign "$SIGN_ID" \
  "$DISTRIB/InstallSisterDrivers.pkg"

echo "Building UninstallSisterDrivers.pkg ($VERSION)…"
pkgbuild \
  --nopayload \
  --identifier org.sisterhl2030.pkg.uninstall-sister \
  --version "$VERSION" \
  --scripts "$DISTRIB/scripts-uninstall-sister" \
  --sign "$SIGN_ID" \
  "$DISTRIB/UninstallSisterDrivers.pkg"

echo "Building UninstallBrotherDrivers.pkg ($VERSION)…"
pkgbuild \
  --nopayload \
  --identifier org.sisterhl2030.pkg.uninstall-brother \
  --version "$VERSION" \
  --scripts "$DISTRIB/scripts-uninstall-brother" \
  --sign "$SIGN_ID" \
  "$DISTRIB/UninstallBrotherDrivers.pkg"

echo
echo "Verifying signatures…"
for pkg in InstallSisterDrivers UninstallSisterDrivers UninstallBrotherDrivers; do
  echo "--- $pkg.pkg ---"
  pkgutil --check-signature "$DISTRIB/$pkg.pkg"
done
