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

echo "Refreshing the install payload from build/…"
PAYLOAD="$DISTRIB/root-install/Library/Printers/SisterHL2030"
cp "$ROOT/build/sister-printer-app" "$PAYLOAD/sister-printer-app"
cp "$ROOT/build/sister-status" "$PAYLOAD/sister-status"
chmod 755 "$PAYLOAD/sister-printer-app" "$PAYLOAD/sister-status" "$PAYLOAD/.create-queue.sh"
cp "$ROOT/Scripts/_privileged-create-queue.sh" "$PAYLOAD/.create-queue.sh"
chmod 755 "$PAYLOAD/.create-queue.sh"
cp "$ROOT/launchd/org.sisterhl2030.printer.plist" \
   "$DISTRIB/root-install/Library/LaunchDaemons/org.sisterhl2030.printer.plist"
chmod 644 "$DISTRIB/root-install/Library/LaunchDaemons/org.sisterhl2030.printer.plist"

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
