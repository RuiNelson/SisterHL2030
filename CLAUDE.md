# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A clean-room, native Apple Silicon driver for the Brother HL-2030, a host-based
laser with no PostScript/PCL-PDL/AirPrint of its own. The host rasterizes and
sends Brother's "mode 1030" compressed stream over USB. GPL-2.0-or-later; every
source file carries the SPDX header and `namespace sisterhl2030`.

`docs/protocol.md` is the normative spec for the wire format (job envelope, PJL
page header, band/line packing). Treat it as the source of truth: change the
encoder and the doc together, and never guess at byte layouts — the format was
recovered from Brother's own Linux i386 driver blobs and confirmed against the
official macOS `rastertobrother2030` on real hardware. Those blobs are
Brother's proprietary material: they are kept on the maintainer's machine
only, gitignored, and never linked into this driver or committed to this
repository.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure
```

Two test binaries, no framework — they count failures and return non-zero:

```bash
./build/test_encoder   # ctest name: encoder
./build/test_status    # ctest name: status
```

Run one by name with `ctest --test-dir build -R status --output-on-failure`.

`sister-status` and its `-framework IOKit` link are guarded by `if(APPLE)`; the
encoder library, `sister-rawtobr`, and both tests build anywhere.

The driver version is `project(sisterhl2030 VERSION …)` in `CMakeLists.txt`
plus the git sha (`0.9.1+abc1234def56`). Bump the CMake version when shipping
a user-visible driver change. `sister-printer-app --version` prints the full
string; PAPPL advertises the semver half as `printer-firmware-string-version`.
`Scripts/Check Sister HL2030.sh` compares the build, the install, and the
running daemon.

## Releases

Versioning is semver (`MAJOR.MINOR.PATCH`), pre-1.0 while the driver is in
beta. To cut a release: bump `project(sisterhl2030 VERSION …)` in
`CMakeLists.txt`, update this file if the example version string above is
now stale, rebuild the three packages with
`Scripts/build_distribution_packages.sh`, commit, then tag the commit
`vMAJOR.MINOR.PATCH` (e.g. `v0.9.1`) with `git tag`. The tag is what
`gh release create` attaches the three `.pkg` files from `distrib/` to.
Release notes are user-facing only — what changed for someone printing to
the HL-2030, in short bullet-point prose — never internal detail like
tests, CI, or documentation changes.

## Runtime architecture

The driver is a PAPPL printer application. It owns the USB device end to end:

```
app / CUPS queue "Brother_HL_2030_series" (ipp://localhost:8631/ipp/print)
    → sister-printer-app  (LaunchDaemon com.ruinelson.sisterhl2030.printer, port 8631)
        rasterize → sRGB → luma → Atkinson → mode 1030 → USB
    → PJL status on the same device for toner and drum
```

Everything lives in `/Library/Printers/SisterHL2030/`: `sister-printer-app`,
`sister-status`, `icon.png` and `spool/`. PAPPL keeps its printer list in
`/private/var/lib/sister-printer-app.state` when running as root. Logs go to
`/Library/Logs/SisterHL2030.log`. The app serves its own web UI on port 8631
and advertises `_ipp._tcp` with the `_print,_universal` subtypes, which is what
makes AirPrint work; the Bonjour name is set to "Brother HL-2030" from
`status_cb`, since an IPP printer name cannot contain spaces.

The **ippeveprinter façade is retired**. `sister-ipp-command`,
`printer-attrs.conf`, the `device-uri` file, the `usb.lock` flock and the
`org.sisterhl2030.status` daemon are all gone — PAPPL provides natively what
each of them worked around. `rastertosisterhl2030` still builds as a classic
CUPS filter but is no longer installed; it is the reference path for comparing
encoder output.

All identifiers — the LaunchDaemon label, the pkg identifiers in
`Scripts/build_distribution_packages.sh` — use `com.ruinelson.sisterhl2030`
as their reverse-DNS base. The two exceptions are historical, not current:
`org.sisterhl2030.status` above is the label the retired façade daemon used,
and `org.sisterhl2030.printer` is the label every 1.1.x release used for the
printer daemon before this rename. Both stay hard-coded as cleanup targets
(in `_privileged-install.sh`, `_privileged-sister-uninstall.sh` and their
pkg equivalents) so installing over, or uninstalling, an older copy doesn't
leave one of them running under the old name — do not "fix" them to the new
base.

### Consumables

`status_cb()` opens the device, writes `pjl_supply_query()`, reads until
`pjl_response_complete()`, and hands the result to `papplPrinterSetSupplies()`.
PAPPL derives `marker-levels`/`marker-names` from that itself, so the macOS
Supply Levels panel works with no extra plumbing, and it advertises
`printer-supply-info-uri` over plain http rather than the self-signed https
that made the façade's supplies page unreachable.

CUPS is the awkward half: it copies `marker-*` off the printer only while a
job runs through its backend, so the macOS Supply Levels panel stays empty
until something prints. A job named `.sister-status` (`kStatusJobName`) makes
the app report status and print nothing, which populates the queue without
paper. The installer submits one; to refresh by hand:

```bash
lp -d Brother_HL_2030_series -t .sister-status /etc/hosts
```

Toner is a three-state sensor (OK/low/empty → 100/15/0). Drum remaining is
`DRUMLIFE` against 12 000 rated pages. `Scripts/_fake_printer.py` answers PJL
the way the real device does, so this path can be exercised with no hardware:

```bash
python3 Scripts/_fake_printer.py 9199 --toner=low &
./build/sister-printer-app add -u ipp://localhost:8632/ -d p \
    -m sister-hl2030 -v socket://127.0.0.1:9199
```

### Traps that cost real time here

- `papplMainloop`'s `footer_html` argument is **not** optional. Pass null and
  every web page segfaults the daemon inside `papplClientHTMLFooter`.
- `papplLogJob` implements its own printf subset; `%zu` crashes it.
- `papplPrinterCreate` reports every `EINVAL` as "Printer names must start with
  a letter or underscore". Read the server log for the real reason.
- Declaring a raw `format` obliges you to set `printfile_cb`.
- Never advertise `black_1`. PAPPL (and some clients) then threshold with a
  clustered-dot screen, and Atkinson has nothing to spread. 8-bit sGray/sRGB
  only; we dither.
- PJL `RESOLUTION` must match the bitmap dpi. A 300 dpi raster with
  `RESOLUTION = 600` prints at half size — that is the 100%-scale bug.

## Source layout

- `src/encoder/` — portable, no CUPS or macOS dependency. `job.cc` writes the
  PJL/PCL envelope and drives banding (flush every 128 lines or when a band
  would exceed 16 384 payload bytes); `line.cc` packs a scanline (white / absolute
  / delta-vs-previous with substitute and repeat edits); `block.h` is the band
  buffer; `halftone.cc` is serpentine Atkinson dithering plus the colorspace →
  toner curves and the 600→300 box downsample.
- `src/cups/rastertosisterhl2030.cpp` — the only CUPS-linked code. Reads
  `cups_page_header2_t`, maps colorspaces, resolves quality → `PageParams`.
- `src/status/` — PJL parsing (`pjl.cc`, pure, unit-tested) split from IOKit USB
  transport (`usb_printer.cc`, macOS-only).
- `src/tools/` — `sister-rawtobr` (PBM `P4` on stdin → job stream on stdout,
  the hardware smoke test), `sister-preview` (halftone preview to BMP, no
  printing), `sister-status`.

Quality mapping lives in `requested_resolution()` in the filter and
`write_page_header()` in `job.cc`: draft = 300 dpi + ECONOMODE ON, normal =
600 dpi + ECONOMODE ON, high = `RAS1200MODE = TRUE` with `RESOLUTION = 600`
and ECONOMODE OFF.

## Checking output without printing

`Scripts/decode_job.py` decodes a mode-1030 job back into a bitmap, so you can
see what would land on paper without spending any. It round-trips
`sister-rawtobr` output exactly, so its results are trustworthy.

```bash
./build/sister-rawtobr < page.pbm > job.prn
python3 Scripts/decode_job.py job.prn out.pbm
```

`mixed bytes` is the halftone signal: packed bytes that are neither `0x00` nor
`0xFF`. A dithered photo runs ~90%; a page that lost its tone is near 0%.

To capture a whole PAPPL job without a printer, point a printer at a socket
(`-v socket://127.0.0.1:9100`) and listen on that port — the `file://` scheme
is rejected by the server's device-type check, so socket is the way in.

## Iterating on an installed system

After changing the encoder, filter, or IPP command script, rebuild then copy
into the live install — no daemon restart needed, both are exec'd per job:

```bash
cmake --build build -j && sudo bash Scripts/_privileged-update-filter.sh "$PWD"
```

macOS Tahoe kills an unsigned binary under `/Library/Printers` (`OS_REASON_CODESIGNING`). The privileged scripts ad-hoc sign after copy (`codesign --force --sign -`). Confirm with `Scripts/Check Sister HL2030.sh`.

Changes to the `launchd/*.plist` files, or anything that must restart the
daemon, need a reload:

```bash
sudo bash Scripts/_privileged-restart-ipp.sh "$PWD"
```

`Scripts/*.sh` without the `_privileged-` prefix are the novice-facing entry
points (`Install Sister HL2030.sh`, `Uninstall Sister HL2030.sh`,
`Uninstall Official Brother Drivers.sh`, `Preview Halftone.sh`,
`Preview PAPPL Render.sh`); they source `_common.sh` and escalate through
`osascript … with administrator privileges`. Install refuses to proceed while
the official Intel Brother package is present, so the two filters can never
stack.

Everything in this repository — code, comments, scripts, user-facing prompts,
docs, commit messages — is written in **English**, and that includes strings
compared against device output. PJL `DISPLAY` text follows the printer's own
front-panel language setting, so it is never a reliable discriminator: map
supply state from `CODE`, and treat the English `DISPLAY` matching in
`toner_from_code_and_display()` as a last-resort fallback for unmapped codes.
Shell helpers that parse `lpstat`/`lpinfo` run them under `LC_ALL=C` so CUPS
output is English whatever the user's locale is.

## Gotchas

- Bit polarity is 1 = black, MSB-first, rows byte-padded. A blank or inverted
  page almost always means polarity or the halftone direction, not banding.
- `driver_data->left_right` / `bottom_top` are `1` (0.01 mm), never `0`: a zero
  margin reads as borderless and the print dialog then silently scale-to-fits
  at ~96%. `scaling_default` is `PAPPL_SCALING_NONE` for the same reason —
  together they are the README's "defaults to 100% scale".
- No classic Sister PPD is shipped. The installer deletes copies left by
  earlier versions so CUPS cannot pick the legacy PPD path. Advertised
  attributes come from `driver_cb()` in `sister_app.cpp` — change them there.
- The CUPS queue must be an **AirPrint PPD** (`lpadmin -P` of Apple's
  ipp2ppd output), not `lpadmin -m everywhere`. Everywhere makes System
  Settings list the printer, but on macOS Tahoe its print dialog has no
  Quality control, and its PPD maps Normal to 300 dpi (half-size pages).
  ipp2ppd maps Draft=300 + `print-quality=3`, Normal=600 + 4, High=600 + 5
  (HQ1200). It only emits the Fine `APPrinterPreset` when High uses a
  different resolution than Normal; ours does not, so
  `_privileged-create-queue.sh` adds `Black and White - Fine` itself.
  Tahoe's Quality popup is those three presets
  (`com.apple.print.preset.quality` draft/mid/high), not the OpenUI
  `cupsPrintQuality` keyword. CUPS may warn that PPDs are deprecated;
  every AirPrint printer on macOS is this path. Check MakeModel is
  `…-AirPrint`, not `…- IPP Everywhere`.
- Only one CUPS queue should exist, and it must stay on `ipp://localhost:8631`
  with `printer-is-shared=false`. `remove_duplicate_sister_queues` in
  `_common.sh` prunes the extras Bonjour/AirPrint discovery creates — its glob
  says `HL-2030._ipp` on purpose, so it also catches the `_ipps._tcp` variant
  macOS discovers first. A queue left on a `dnssd://` URI *and* shared is the
  bad case: CUPS advertises it back on Bonjour, the URI resolves into that copy
  of itself, and jobs hang on "looking for the printer" — which also starves
  the queue of `marker-*`, so Supply Levels goes blank.
- Commit messages in this repo are a single imperative sentence ending in a
  period ("Map print quality to resolution and toner save.").
