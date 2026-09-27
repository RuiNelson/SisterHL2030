# Signing the SisterHL2030 packages

`Scripts/build_distribution_packages.sh` produces four `.pkg` files in
`distrib/`. With signing identities configured they come out signed and
notarized, ready to hand to other people. Without them the script still
builds all four, but unsigned: they install on the build Mac through a
Gatekeeper bypass, and nothing else.

## What "signed" covers

Three layers, inside out:

1. **Binaries** — `sister-printer-app` and `sister-status` are signed with
   a **Developer ID Application** certificate, hardened runtime
   (`--options runtime`) and a secure timestamp. Notarization requires
   this; an ad-hoc signature is not enough.
2. **Packages** — all four `.pkg` files are signed with a **Developer ID
   Installer** certificate (`pkgbuild --sign` / `productbuild --sign`).
   That is a different certificate type from the Application one: an
   Application identity cannot sign a package.
3. **Notarization** — each `.pkg` is submitted to Apple (`notarytool
   submit --wait`) and the ticket is stapled to it (`stapler staple`).
   This is what makes a double-click install work on someone else's Mac.

The installer-identity lookup is pinned to the certificate's **SHA-1
fingerprint**, not its display name: the certificate ended up in both the
login and System keychains, so a name lookup matches twice and
`pkgbuild --sign` cannot resolve it. The fingerprint is unambiguous
either way. List yours with `security find-identity -v`.

## One-time setup

1. Join the Apple Developer Program and create **Developer ID
   Application** and **Developer ID Installer** certificates. Install both
   in your keychain (see the local, gitignored
   `distrib/DeveloperIDInstaller.csr` if you need to reissue the
   Installer one).
2. Copy the template and fill in your fingerprints:
   ```bash
   cp Scripts/signing.local.sh.example Scripts/signing.local.sh
   ```
   `signing.local.sh` is gitignored and never committed. The same three
   values can come from the environment instead —
   `SISTER_INSTALLER_IDENTITY`, `SISTER_APP_IDENTITY`,
   `SISTER_NOTARY_PROFILE` — which take precedence over the file.
3. Create the notarization credential profile once:
   ```bash
   xcrun notarytool store-credentials sister-notary \
       --apple-id "you@example.com" --team-id TEAMID --password app-specific-pw
   ```
   The `--password` is an app-specific password, not your Apple ID
   password.

## Build modes

| Identities + profile | Result |
| --- | --- |
| All three set and valid | Signed binaries, signed packages, notarized + stapled. `spctl --assess` passes. Hand-outable. |
| Identities set, profile missing | Signed but **not** notarized. The script says so and skips `spctl`. Installs elsewhere only via Right-click > Open on first launch. |
| Anything unset | **Unsigned** build (see below). |

An identity that *is* set but is not found in the keychain is still a
hard error — that means a misconfigured `signing.local.sh`, not a
deliberate unsigned build.

## Unsigned builds

The script prints a red `WARNING: building UNSIGNED packages` naming the
missing variables, then:

- signs both binaries **ad-hoc** (`codesign --force --sign -`, the same
  signature the privileged install scripts apply). This carries no
  identity — it only stops Tahoe from killing the binaries outright
  (`OS_REASON_CODESIGNING`) so they run on the build Mac;
- builds the `.pkg` files with no `--sign`;
- skips notarization entirely (only signed packages can be notarized);
- verifies with `pkgutil --check-signature` only. `spctl --assess` is
  skipped because it can only fail without a signature.

To install an unsigned package on the build Mac, Gatekeeper must be
bypassed per package: **Right-click the `.pkg` in Finder, choose Open,
then Open again** in the dialog. If that is refused, open it once more
via **System Settings > Privacy & Security > Open Anyway** (the button
appears after a blocked attempt). Do not disable Gatekeeper system-wide
(`spctl --master-disable`) for this — the per-package bypass is enough
and leaves the rest of the system's protection intact.

Unsigned packages are for local testing only. They cannot be handed out:
every other Mac will block them the same way, with no way for you to
pre-approve them.

## Verifying a build

```bash
pkgutil --check-signature distrib/InstallSisterDrivers.NewspaperStyle.pkg
spctl --assess --type install -vv distrib/InstallSisterDrivers.NewspaperStyle.pkg
codesign -dv --verbose=4 build/sister-printer-app
```

A fully green build reports `accepted` for the `spctl` assessment and a
`Developer ID Installer` authority in the `pkgutil` output. If `spctl`
rejects a package whose `pkgutil` output shows a valid Installer
signature, the package was signed but not notarized: it still needs the
Right-click > Open bypass on first install.
