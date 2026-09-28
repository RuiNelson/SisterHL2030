#!/bin/bash
# Build the four .pkg installers in distrib/:
#   InstallSisterDrivers.NewspaperStyle.pkg - native universal (arm64 +
#                                             x86_64) driver, AM45
#                                             (clustered-dot) halftone
#   InstallSisterDrivers.PencilStyle.pkg    - native universal driver,
#                                             Atkinson (error-diffusion)
#                                             halftone
#   UninstallSisterDrivers.pkg  - removes either one
#   UninstallBrotherDrivers.pkg - removes the official Intel Brother package
#
# Every binary is built once per architecture in its own tree under
# distrib/build-<arch> and the thin results are joined with lipo, so one .pkg
# installs natively on Apple Silicon and on Intel Macs. The static OpenSSL,
# libpng and libusb archives PAPPL links are single-architecture, which is
# why the two builds cannot be one fat compile. SISTER_ARCHS (space-separated,
# arm64 and/or x86_64) picks what to build; the default is both on an Apple
# Silicon Mac and x86_64 alone on an Intel one. The x86_64 half on Apple
# Silicon needs Rosetta and an Intel Homebrew under /usr/local (with cmake,
# pkg-config, openssl@3, libpng and libusb) -- the whole configure and build
# then runs as `arch -x86_64`.
#
# Signed with a Developer ID when signing identities are configured (see
# docs/signing.md); unsigned with a warning otherwise. The unsigned packages
# install and run on the build Mac via a Gatekeeper bypass, but only the
# signed + notarized ones can be handed out to other people.
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
# the packages that get handed out. Signing identities and the notarization
# profile come from Scripts/signing.local.sh (gitignored; copy it from
# signing.local.sh.example) or from the SISTER_* environment variables,
# which take precedence over that file. Without them the script still builds
# everything, but unsigned (see docs/signing.md). Requires a
# "Developer ID Installer" identity in the keychain for a signed build (a
# different cert type than "Developer ID Application", which only signs
# binaries, not installer packages).
set -euo pipefail

c_red=$'\033[31m'
c_bold=$'\033[1m'
c_reset=$'\033[0m'
if [[ ! -t 1 ]]; then
  c_red=""; c_bold=""; c_reset=""
fi

warn_unsigned() {
  echo "${c_red}${c_bold}WARNING: building UNSIGNED packages.${c_reset}" >&2
  echo "${c_red}$*${c_reset}" >&2
  echo "${c_red}Gatekeeper will refuse a double-click install. Right-click the${c_reset}" >&2
  echo "${c_red}.pkg in Finder, choose Open, then Open again; if that is refused,${c_reset}" >&2
  echo "${c_red}open it once more via System Settings > Privacy & Security > Open Anyway.${c_reset}" >&2
  echo "${c_red}To build signed packages instead, see docs/signing.md.${c_reset}" >&2
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DISTRIB="$ROOT/distrib"
VERSION="$(sed -n 's/^project(sisterhl2030 VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"

SIGNING_LOCAL="$SCRIPT_DIR/signing.local.sh"
if [ -f "$SIGNING_LOCAL" ]; then
  # shellcheck source=signing.local.sh
  . "$SIGNING_LOCAL"
fi

# Pinned to the SHA-1 fingerprint, not the display name: the certificate
# ended up in both the login and System keychains (Keychain Access's default
# import target differs from where `security import` put the matching
# private key), so a name lookup matches twice and pkgbuild's --sign
# resolution becomes ambiguous. The fingerprint is unambiguous either way.
#
# Any identity left unset means an unsigned build, not an error: the script
# warns (in red) and builds everything unsigned instead. An identity that IS
# set but is not found in the keychain is still a hard error, since that
# means a misconfigured signing.local.sh rather than a deliberate choice.
SIGN_ID="${SISTER_INSTALLER_IDENTITY:-}"
if [ -n "$SIGN_ID" ] && ! security find-identity -v 2>/dev/null | grep -q "$SIGN_ID"; then
  echo "No \"$SIGN_ID\" identity in the keychain. Import your Developer ID" >&2
  echo "Installer certificate first (see distrib/DeveloperIDInstaller.csr)." >&2
  exit 1
fi

# "Developer ID Application" (not "Installer") signs the binaries *inside*
# the payload. Notarization rejects binaries that are only ad-hoc signed
# (which is all _privileged-update-filter.sh does post-install, to survive
# Tahoe's OS_REASON_CODESIGNING kill) — it wants a Developer ID signature
# with the hardened runtime and a secure timestamp.
APP_SIGN_ID="${SISTER_APP_IDENTITY:-}"
if [ -n "$APP_SIGN_ID" ] && ! security find-identity -v -p codesigning 2>/dev/null | grep -q "$APP_SIGN_ID"; then
  echo "No \"$APP_SIGN_ID\" identity in the keychain. Import your Developer ID" >&2
  echo "Application certificate first." >&2
  exit 1
fi

# Notarization credential profile, created once with:
#   xcrun notarytool store-credentials <profile> \
#       --apple-id "you@example.com" --team-id TEAMID --password app-specific-pw
NOTARY_PROFILE="${SISTER_NOTARY_PROFILE:-}"

SIGNED=0
if [ -n "$SIGN_ID" ] && [ -n "$APP_SIGN_ID" ]; then
  SIGNED=1
fi

NOTARIZE=0
if [ "$SIGNED" = 1 ]; then
  if [ -z "$NOTARY_PROFILE" ]; then
    echo "SISTER_NOTARY_PROFILE is not set: packages will be signed but NOT notarized." >&2
    echo "Gatekeeper on other Macs will still ask for a Right-click > Open on first install." >&2
    echo "Run 'xcrun notarytool store-credentials <profile>' and set it in" >&2
    echo "Scripts/signing.local.sh to notarize (see docs/signing.md)." >&2
  elif ! xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1; then
    echo "No \"$NOTARY_PROFILE\" notarytool credential profile in the keychain." >&2
    echo "Run 'xcrun notarytool store-credentials $NOTARY_PROFILE' first." >&2
    exit 1
  else
    NOTARIZE=1
  fi
fi

if [ "$SIGNED" = 0 ]; then
  missing=""
  [ -z "$SIGN_ID" ] && missing="${missing} SISTER_INSTALLER_IDENTITY"
  [ -z "$APP_SIGN_ID" ] && missing="${missing} SISTER_APP_IDENTITY"
  warn_unsigned "Missing:${missing}."
  if [ -n "$NOTARY_PROFILE" ]; then
    echo "Skipping notarization: only signed packages can be notarized." >&2
  fi
fi

# Wrapper commands that add --sign only in a signed build. (Plain functions
# rather than an arg array: macOS ships bash 3.2, where expanding an empty
# array under `set -u` aborts the script.)
pkgbuild_signed() {
  if [ "$SIGNED" = 1 ]; then
    pkgbuild --sign "$SIGN_ID" "$@"
  else
    pkgbuild "$@"
  fi
}

productbuild_signed() {
  if [ "$SIGNED" = 1 ]; then
    productbuild --sign "$SIGN_ID" "$@"
  else
    productbuild "$@"
  fi
}

sign_binary() {
  local bin="$1"
  if [ "$SIGNED" = 1 ]; then
    codesign --force --options runtime --timestamp \
      --sign "$APP_SIGN_ID" "$bin"
  else
    # Same ad-hoc signature the privileged install scripts apply, so the
    # unsigned payload at least runs on the build Mac (Tahoe kills a wholly
    # unsigned binary under /Library/Printers). It carries no identity and
    # Gatekeeper still treats the package as unsigned.
    codesign --force --sign - "$bin"
  fi
}

notarize_and_staple() {
  local pkg="$1"
  if [ "$NOTARIZE" = 0 ]; then
    return 0
  fi
  echo "Submitting $(basename "$pkg") for notarization (this can take a few minutes)…"
  xcrun notarytool submit "$pkg" --keychain-profile "$NOTARY_PROFILE" --wait
  echo "Stapling notarization ticket to $(basename "$pkg")…"
  xcrun stapler staple "$pkg"
}

# Nothing in the payload may load a library from outside the OS. PAPPL needs
# OpenSSL, libpng and libusb, and its pkg-config line points at whatever
# package manager built them: linked as dylibs they become absolute
# LC_LOAD_DYLIB paths into a Homebrew tree the user does not have, with no
# rpath and no fallback, and the daemon then cannot start at all. CMakeLists.txt
# links the static archives instead -- this is the check that it really did.
assert_system_only() {
  local bin="$1" strays
  strays="$(otool -L "$bin" | tail -n +2 |
            grep -vE '^[[:space:]]+(/usr/lib/|/System/Library/)' || true)"
  if [ -n "$strays" ]; then
    echo "$(basename "$bin") links libraries the user's Mac will not have:" >&2
    echo "$strays" >&2
    echo "The package would install a daemon that cannot launch. Aborting." >&2
    exit 1
  fi
}

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake not found. Install Xcode Command Line Tools and cmake first." >&2
  exit 1
fi

HOST_ARCH="$(uname -m)"
if [ -n "${SISTER_ARCHS:-}" ]; then
  ARCHS="$SISTER_ARCHS"
elif [ "$HOST_ARCH" = arm64 ]; then
  ARCHS="arm64 x86_64"
else
  ARCHS="x86_64"
fi

# Runs a command as the given architecture. The host's own architecture runs
# it directly; x86_64 on Apple Silicon goes through Rosetta with the Intel
# Homebrew first in PATH so cmake, pkg-config and the static archives are all
# x86_64. arm64 cannot be built on an Intel Mac.
arch_run() {
  local a="$1"; shift
  if [ "$a" = "$HOST_ARCH" ]; then
    "$@"
  elif [ "$a" = x86_64 ] && [ "$HOST_ARCH" = arm64 ]; then
    PATH="/usr/local/bin:/usr/local/sbin:$PATH" arch -x86_64 "$@"
  else
    echo "Cannot build $a on a $HOST_ARCH Mac." >&2
    exit 1
  fi
}

for a in $ARCHS; do
  case "$a" in
    arm64|x86_64) ;;
    *) echo "Unsupported architecture \"$a\" in SISTER_ARCHS (arm64 or x86_64)." >&2; exit 1 ;;
  esac
  if ! arch_run "$a" cmake --version >/dev/null 2>&1; then
    echo "No runnable $a cmake. For x86_64 on Apple Silicon, install Rosetta" >&2
    echo "(softwareupdate --install-rosetta) and Homebrew under /usr/local:" >&2
    echo "  arch -x86_64 /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\"" >&2
    echo "  arch -x86_64 /usr/local/bin/brew install cmake pkg-config openssl@3 libpng libusb" >&2
    exit 1
  fi
done

# Configure + build one target for one architecture in its own tree.
# $1 arch, $2 target, then any extra -D flags. --clean-first on every build:
# see the halftone note in the loop below, and it is also the safe choice
# when a tree is reused across runs.
build_arch() {
  local a="$1" target="$2"; shift 2
  arch_run "$a" cmake -S "$ROOT" -B "$DISTRIB/build-$a" \
    -DCMAKE_BUILD_TYPE=Release -DSISTER_WITH_PAPPL=ON \
    -DCMAKE_OSX_ARCHITECTURES="$a" "$@"
  arch_run "$a" cmake --build "$DISTRIB/build-$a" --target "$target" \
    --clean-first -j
}

# Joins the per-architecture builds of one binary into a single universal
# file at $2. $1 is the binary's name in each tree. Each thin binary is
# checked first: otool -L on a fat file prints a header line per slice that
# assert_system_only would mistake for a stray library.
lipo_arches() {
  local name="$1" out="$2" a inputs=()
  for a in $ARCHS; do
    assert_system_only "$DISTRIB/build-$a/$name"
    inputs+=("$DISTRIB/build-$a/$name")
  done
  lipo -create "${inputs[@]}" -output "$out"
  echo "$name: $(lipo -archs "$out")"
}

# sister-printer-app is built once per halftone screen below -- that is the
# whole point of this script. sister-status never links the encoder library,
# so the screen switch cannot affect it; build it once here rather than
# twice inside the loop.
echo "Building sister-status ($VERSION, shared by both halftone variants; $ARCHS)…"
cd "$ROOT"
for a in $ARCHS; do
  build_arch "$a" sister-status
done

echo "Rebuilding the install payload from distrib/build-<arch>/, launchd/ and docs/…"
PAYLOAD="$DISTRIB/root-install/Library/Printers/SisterHL2030"
LAUNCHD_DEST="$DISTRIB/root-install/Library/LaunchDaemons"
mkdir -p "$PAYLOAD" "$LAUNCHD_DEST"

lipo_arches sister-status "$PAYLOAD/sister-status"
cp "$ROOT/Scripts/_privileged-create-queue.sh" "$PAYLOAD/.create-queue.sh"
chmod 755 "$PAYLOAD/sister-status" "$PAYLOAD/.create-queue.sh"

if [ "$SIGNED" = 1 ]; then
  echo "Signing sister-status for notarization…"
else
  echo "Ad-hoc signing sister-status (unsigned build)…"
fi
sign_binary "$PAYLOAD/sister-status"

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
# Dots, not parentheses: GitHub release assets rewrite "(Foo)" to ".Foo",
# so these names have to match what the Releases page actually shows.
variant_suffix=(".NewspaperStyle" ".PencilStyle")
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
  # --clean-first (in build_arch): reconfiguring the same tree only changes
  # a compile *definition*, and a fast reconfigure+build can land in the same
  # filesystem-mtime tick, in which case make sees nothing to rebuild and
  # silently keeps the previous screen (see CLAUDE.md's "Traps that cost
  # real time here"). This is exactly that risk, twice per run, so it is
  # not optional here.
  for a in $ARCHS; do
    build_arch "$a" sister-printer-app -DSISTER_HALFTONE_SCREEN="$screen"
  done

  lipo_arches sister-printer-app "$PAYLOAD/sister-printer-app"
  chmod 755 "$PAYLOAD/sister-printer-app"
  if [ "$SIGNED" = 1 ]; then
    echo "Signing sister-printer-app for notarization…"
  else
    echo "Ad-hoc signing sister-printer-app (unsigned build)…"
  fi
  sign_binary "$PAYLOAD/sister-printer-app"

  echo "Building the install component ($VERSION)…"
  pkgbuild_signed \
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
  productbuild_signed \
    --distribution "$DISTRIB/distribution-install.xml" \
    --package-path "$DISTRIB" \
    --resources "$RES_BUILD" \
    "$DISTRIB/$pkg_name.pkg"
  notarize_and_staple "$DISTRIB/$pkg_name.pkg"
  rm -rf "$RES_BUILD"
done

echo "Building UninstallSisterDrivers.pkg ($VERSION)…"
pkgbuild_signed \
  --nopayload \
  --identifier com.ruinelson.sisterhl2030.pkg.uninstall-sister \
  --version "$VERSION" \
  --scripts "$DISTRIB/scripts-uninstall-sister" \
  "$DISTRIB/UninstallSisterDrivers.pkg"
notarize_and_staple "$DISTRIB/UninstallSisterDrivers.pkg"

echo "Building UninstallBrotherDrivers.pkg ($VERSION)…"
pkgbuild_signed \
  --nopayload \
  --identifier com.ruinelson.sisterhl2030.pkg.uninstall-brother \
  --version "$VERSION" \
  --scripts "$DISTRIB/scripts-uninstall-brother" \
  "$DISTRIB/UninstallBrotherDrivers.pkg"
notarize_and_staple "$DISTRIB/UninstallBrotherDrivers.pkg"

echo
if [ "$SIGNED" = 1 ] && [ "$NOTARIZE" = 1 ]; then
  echo "Verifying signatures and notarization…"
  for pkg in "${install_pkg_names[@]}" UninstallSisterDrivers UninstallBrotherDrivers; do
    echo "--- $pkg.pkg ---"
    pkgutil --check-signature "$DISTRIB/$pkg.pkg"
    spctl --assess --type install -vv "$DISTRIB/$pkg.pkg"
  done
elif [ "$SIGNED" = 1 ]; then
  echo "Verifying signatures (signed but NOT notarized: spctl assessment is skipped,"
  echo "it only passes with a stapled notarization ticket)…"
  for pkg in "${install_pkg_names[@]}" UninstallSisterDrivers UninstallBrotherDrivers; do
    echo "--- $pkg.pkg ---"
    pkgutil --check-signature "$DISTRIB/$pkg.pkg"
  done
else
  echo "Verifying package structure (unsigned build: no signature to assess)…"
  for pkg in "${install_pkg_names[@]}" UninstallSisterDrivers UninstallBrotherDrivers; do
    echo "--- $pkg.pkg ---"
    pkgutil --check-signature "$DISTRIB/$pkg.pkg" || true
  done
  echo
  warn_unsigned "The packages above install, but only via the Gatekeeper bypass."
fi
