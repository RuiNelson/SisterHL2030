#!/bin/bash
# Build the four signed .pkg installers in distrib/:
#   InstallSisterDrivers(NewspaperStyle).pkg - native arm64 driver, AM45
#                                              (clustered-dot) halftone
#   InstallSisterDrivers(PencilStyle).pkg    - native arm64 driver, Atkinson
#                                              (error-diffusion) halftone
#   UninstallSisterDrivers.pkg  - removes either one
#   UninstallBrotherDrivers.pkg - removes the official Intel Brother package
#
# The two install packages are alternate builds of the same underlying
# package -- same pkgbuild identifier, same install location -- so running
# one after the other just switches which halftone screen this Mac's copy
# uses, and a single UninstallSisterDrivers.pkg removes either one. Each
# gets its own distribution package with a localized welcome pane
# (resources-install/*.lproj); only en.lproj gets a style-specific callout
# sentence (see the HALFTONE_NOTE substitution below) -- the other
# languages keep the existing, style-agnostic instructions rather than ship
# a machine-translated sentence in a signed installer.
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

# "Developer ID Application" (not "Installer") signs the binaries *inside*
# the payload. Notarization rejects binaries that are only ad-hoc signed
# (which is all _privileged-update-filter.sh does post-install, to survive
# Tahoe's OS_REASON_CODESIGNING kill) — it wants a Developer ID signature
# with the hardened runtime and a secure timestamp.
APP_SIGN_ID="${SISTER_APP_IDENTITY:-CD69A8CC3BDC105191BDE97A2DC2C8D8E7FDD0D8}"
if ! security find-identity -v -p codesigning 2>/dev/null | grep -q "$APP_SIGN_ID"; then
  echo "No \"$APP_SIGN_ID\" identity in the keychain. Import your Developer ID" >&2
  echo "Application certificate first." >&2
  exit 1
fi

# Notarization credential profile, created once with:
#   xcrun notarytool store-credentials sister-notary \
#       --apple-id "you@example.com" --team-id TEAMID --password app-specific-pw
NOTARY_PROFILE="${SISTER_NOTARY_PROFILE:-sister-notary}"
if ! xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1; then
  echo "No \"$NOTARY_PROFILE\" notarytool credential profile in the keychain." >&2
  echo "Run 'xcrun notarytool store-credentials $NOTARY_PROFILE' first." >&2
  exit 1
fi

notarize_and_staple() {
  local pkg="$1"
  echo "Submitting $(basename "$pkg") for notarization (this can take a few minutes)…"
  xcrun notarytool submit "$pkg" --keychain-profile "$NOTARY_PROFILE" --wait
  echo "Stapling notarization ticket to $(basename "$pkg")…"
  xcrun stapler staple "$pkg"
}

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake not found. Install Xcode Command Line Tools and cmake first." >&2
  exit 1
fi

# sister-printer-app is built twice below, once per halftone screen --
# that is the whole point of this script. sister-status never links the
# encoder library, so the screen switch cannot affect it; build it once
# here rather than twice inside the loop.
echo "Building sister-status ($VERSION, shared by both halftone variants)…"
cd "$ROOT"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSISTER_WITH_PAPPL=ON
cmake --build build --target sister-status -j

echo "Rebuilding the install payload from build/, launchd/ and docs/…"
PAYLOAD="$DISTRIB/root-install/Library/Printers/SisterHL2030"
LAUNCHD_DEST="$DISTRIB/root-install/Library/LaunchDaemons"
mkdir -p "$PAYLOAD" "$LAUNCHD_DEST"

cp "$ROOT/build/sister-status" "$PAYLOAD/sister-status"
cp "$ROOT/Scripts/_privileged-create-queue.sh" "$PAYLOAD/.create-queue.sh"
chmod 755 "$PAYLOAD/sister-status" "$PAYLOAD/.create-queue.sh"

echo "Signing sister-status for notarization…"
codesign --force --options runtime --timestamp \
  --sign "$APP_SIGN_ID" "$PAYLOAD/sister-status"

cp "$ROOT/launchd/com.ruinelson.sisterhl2030.printer.plist" \
   "$LAUNCHD_DEST/com.ruinelson.sisterhl2030.printer.plist"
chmod 644 "$LAUNCHD_DEST/com.ruinelson.sisterhl2030.printer.plist"

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

# Two builds of sister-printer-app, one per halftone screen. sister-status,
# the icons and the LaunchDaemon plist above are identical either way and
# already sit in $PAYLOAD. Same pkgbuild --identifier both times on purpose:
# these are alternate builds of the same package (same files, same install
# location), not two different products, so switching styles is an ordinary
# reinstall over the previous one rather than two receipts fighting over
# the same paths.
variant_screen=(AM45 ATKINSON)
variant_suffix=("(NewspaperStyle)" "(PencilStyle)")
variant_title=("Sister HL-2030 — Newspaper Style (AM halftone)" "Sister HL-2030 — Pencil Style (Atkinson halftone)")
variant_note=(
  "This build prints shaded areas with the <b>AM (Newspaper-style)</b> halftone screen: a crisp, evenly-spaced dot pattern, the way a newspaper photo looks up close."
  "This build prints shaded areas with the <b>Atkinson (Pencil-style)</b> halftone: a soft, scattered texture, the way pencil shading looks up close."
)
install_pkg_names=()

for i in "${!variant_screen[@]}"; do
  screen="${variant_screen[$i]}"
  suffix="${variant_suffix[$i]}"
  title="${variant_title[$i]}"
  note="${variant_note[$i]}"
  pkg_name="InstallSisterDrivers${suffix}"
  install_pkg_names+=("$pkg_name")

  echo
  echo "=== $pkg_name: compiling sister-printer-app with SISTER_HALFTONE_SCREEN=$screen ==="
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSISTER_WITH_PAPPL=ON \
    -DSISTER_HALFTONE_SCREEN="$screen"
  # --clean-first: reconfiguring the same build/ only changes a compile
  # *definition*, and a fast reconfigure+build can land in the same
  # filesystem-mtime tick, in which case make sees nothing to rebuild and
  # silently keeps the previous screen (see CLAUDE.md's "Traps that cost
  # real time here"). This is exactly that risk, twice per run, so it is
  # not optional here.
  cmake --build build --target sister-printer-app --clean-first -j

  cp "$ROOT/build/sister-printer-app" "$PAYLOAD/sister-printer-app"
  chmod 755 "$PAYLOAD/sister-printer-app"
  codesign --force --options runtime --timestamp \
    --sign "$APP_SIGN_ID" "$PAYLOAD/sister-printer-app"

  echo "Building the install component ($VERSION)…"
  pkgbuild \
    --root "$DISTRIB/root-install" \
    --identifier com.ruinelson.sisterhl2030.pkg.install \
    --version "$VERSION" \
    --install-location / \
    --scripts "$DISTRIB/scripts-install" \
    "$DISTRIB/component-install.pkg"

  # Wraps the component in a distribution package so Installer.app shows a
  # welcome pane first (connect the printer, uninstall Brother's drivers,
  # which halftone screen this build uses). The pane text is localized via
  # .lproj folders, picked automatically to match the user's language; only
  # en.lproj's copy is templated per variant (see the header comment).
  echo "Building $pkg_name.pkg ($VERSION)…"
  RES_BUILD="$DISTRIB/resources-install-build"
  rm -rf "$RES_BUILD"
  cp -R "$ROOT/distrib/resources-install" "$RES_BUILD"
  # Bash substitution, not sed: the note text has closing HTML tags like
  # </b>, and a literal / in the replacement collides with sed's own s/../..
  # delimiter -- it fails with "bad flag in substitute command", silently
  # leaving @HALFTONE_NOTE@ untouched in a signed, shipped installer.
  welcome_html="$(cat "$RES_BUILD/en.lproj/welcome.html")"
  welcome_html="${welcome_html//@HALFTONE_NOTE@/$note}"
  printf '%s\n' "$welcome_html" > "$RES_BUILD/en.lproj/welcome.html"

  dist_xml="$(cat "$ROOT/distrib/distribution-install.xml.in")"
  dist_xml="${dist_xml//@VERSION@/$VERSION}"
  dist_xml="${dist_xml//@TITLE@/$title}"
  printf '%s\n' "$dist_xml" > "$DISTRIB/distribution-install.xml"
  productbuild \
    --distribution "$DISTRIB/distribution-install.xml" \
    --package-path "$DISTRIB" \
    --resources "$RES_BUILD" \
    --sign "$SIGN_ID" \
    "$DISTRIB/$pkg_name.pkg"
  notarize_and_staple "$DISTRIB/$pkg_name.pkg"
  rm -rf "$RES_BUILD"
done

echo "Building UninstallSisterDrivers.pkg ($VERSION)…"
pkgbuild \
  --nopayload \
  --identifier com.ruinelson.sisterhl2030.pkg.uninstall-sister \
  --version "$VERSION" \
  --scripts "$DISTRIB/scripts-uninstall-sister" \
  --sign "$SIGN_ID" \
  "$DISTRIB/UninstallSisterDrivers.pkg"
notarize_and_staple "$DISTRIB/UninstallSisterDrivers.pkg"

echo "Building UninstallBrotherDrivers.pkg ($VERSION)…"
pkgbuild \
  --nopayload \
  --identifier com.ruinelson.sisterhl2030.pkg.uninstall-brother \
  --version "$VERSION" \
  --scripts "$DISTRIB/scripts-uninstall-brother" \
  --sign "$SIGN_ID" \
  "$DISTRIB/UninstallBrotherDrivers.pkg"
notarize_and_staple "$DISTRIB/UninstallBrotherDrivers.pkg"

echo
echo "Verifying signatures and notarization…"
for pkg in "${install_pkg_names[@]}" UninstallSisterDrivers UninstallBrotherDrivers; do
  echo "--- $pkg.pkg ---"
  pkgutil --check-signature "$DISTRIB/$pkg.pkg"
  spctl --assess --type install -vv "$DISTRIB/$pkg.pkg"
done
