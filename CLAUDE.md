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
recovered from the Linux i386 blobs and confirmed against the official macOS
`rastertobrother2030` on real hardware. The proprietary blobs in
`LinuxDrivers/` are reference material only and are never linked.

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

## Runtime architecture

Print path (there is no classic CUPS PPD queue any more — Apple deprecated it):

```
app → CUPS queue "Brother_HL_2030_series" (ipp://localhost:8631/ipp/print)
    → ippeveprinter LaunchDaemon (org.sisterhl2030.printer, -a printer-attrs.conf)
    → filter/sister-ipp-command      (src/cups/sister-ipp-command.sh)
    → filter/rastertosisterhl2030    (CUPS/PWG raster → mode-1030 job bytes)
    → /usr/libexec/cups/backend/usb  (device URI read from .../device-uri)
```

`sister-ipp-command` is the seam between IPP and the CUPS filter ABI: it
translates `IPP_MEDIA`, `IPP_MEDIA_TYPE`, `IPP_PRINT_QUALITY`,
`IPP_PRINTER_RESOLUTION` env vars into a CUPS `options` string and invokes the
filter with the classic `argv[1..6]` convention. It serializes USB access with
`flock` on `$ROOT/usb.lock`, which the status daemon also respects.

Everything installs under `/Library/Printers/SisterHL2030/`: `filter/`,
`spool/`, `device-uri`, `printer-attrs.conf`, `icon.png`, `usb.lock`. Logs from
both daemons go to `/Library/Logs/SisterHL2030.log`.

### Consumables path

`org.sisterhl2030.status` runs `sister-status --publish --loop` (every 180 s).
Because `ippeveprinter` only refreshes attributes from its command program's
stderr, the daemon can't just write them: it submits a **no-op IPP job named
`.sister-status`** via `ipptool`, which `sister-ipp-command` recognizes and
short-circuits into `sister-status --ipp`, whose `ATTR:`/`STATE:` stderr lines
`ippeveprinter` then picks up. `sister-ipp-command` also fires `--ipp` after
every real job. Keep the `.sister-status` job-name sentinel in sync across
`src/tools/sister_status.cpp` and `src/cups/sister-ipp-command.sh`.

Levels go out as **both** `printer-supply` and the classic `marker-*`
attributes. The macOS Supply Levels panel reads `marker-levels`/`marker-names`
only — publishing `printer-supply` alone makes it show "no information
available". The static half (`marker-names`, `marker-types`, `marker-colors`,
`marker-low-levels`, `marker-high-levels`) lives in `printer-attrs.conf`;
`ippeve_attr_lines()` refreshes `marker-levels` at runtime. CUPS copies
`marker-*` onto the queue only when a job passes through the backend, so the
panel updates on the next print rather than on the daemon's 3-minute poll.

The HL-2030 toner is a three-state sensor (OK/low/empty → 100/15/0 percent);
drum remaining is estimated from PJL `DRUMLIFE` against 12 000 rated pages
(`kDrumRatedPages`). USB status uses IOKit directly (`src/status/usb_printer.cc`,
VID `0x04f9` PID `0x0027`, bulk OUT `0x01` / IN `0x82`).

## Source layout

- `src/encoder/` — portable, no CUPS or macOS dependency. `job.cc` writes the
  PJL/PCL envelope and drives banding (flush every 128 lines or when a band
  would exceed 16 384 payload bytes); `line.cc` packs a scanline (white / absolute
  / delta-vs-previous with substitute and repeat edits); `block.h` is the band
  buffer; `halftone.cc` is serpentine Floyd–Steinberg plus the colorspace →
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

## PAPPL application (in progress)

`src/pappl/sister_app.cpp` is the replacement for the ippeveprinter façade:
PAPPL owns the USB device, the IPP attributes and the web UI, so it retires
`sister-ipp-command`, `printer-attrs.conf`, `device-uri`, the `usb.lock`
flock, and the no-op `.sister-status` job. It reuses the encoder unchanged —
PAPPL pushes raster a line at a time, the app buffers the page (Floyd-Steinberg
needs the whole page anyway) and calls `Job::encode_page`.

It is **opt-in** and off by default, because it fetches and builds PAPPL:

```bash
cmake -S . -B build -DSISTER_WITH_PAPPL=ON && cmake --build build -j
```

Pin notes: PAPPL **1.4.x**, not 2.x — 1.4 wants "CUPS 2.2 or later" and builds
against the 2.3 macOS ships, while 2.x needs CUPS 2.5+/libcups 3. It is built
at *configure* time, not build time, so `pkg-config` can report the link line
(it carries absolute Homebrew paths and `-framework` flags that CMake's list
handling would split).

Verified so far: `devices` enumerates the HL-2030 over USB **unprivileged**,
even with the IOKit status daemon running, and `add` produces a printer that
advertises the right quality/resolution/media set. Not yet verified: an actual
sheet of paper.

Debugging note: `papplPrinterCreate()` reports *every* `EINVAL` as "Printer
names must start with a letter or underscore", whatever the real cause. The
server log carries the true reason — read it first. Declaring a raw `format`
without a `printfile_cb` fails exactly this way.

**The laser curve is tuned for ColorSync-style greys.** The old CUPS path
never exercised colour: macOS converts to grey itself and hands the filter
`cupsColorSpace=3`, so `device_gray_to_toner()` and its curve were tuned
against that. Anything that produces greys of its own must match, or the page
comes out far too dark. `rgb_to_toner()` therefore weighs channels in **linear
light** and re-encodes; a neutral input passes through untouched, so the grey
path stays exactly as tuned. Check a change with `Scripts/decode_job.py`: for
`test_fixtures/flag.png` the proven CUPS path is ~40% black coverage.

**The PAPPL colour path is calibrated, not derived.** macOS's PDF rasteriser
lifts tone before the old CUPS path ever saw it (flag.png arrives as grey
186/144 where ColorSync's own transform gives 130/87), so a colorimetrically
correct conversion prints lighter than this driver always has. `sister_app.cpp`
applies `kToneCalibration` to match the old output by measurement. Targets, via
`Scripts/decode_job.py`: 40.2% black coverage on `flag.png`, 40.5% on the photo
fixture. Re-derive the exponent if the pipeline changes; residual per-region
error is about 4 points, light areas slightly darker and dark slightly lighter.

**Always take sRGB from PAPPL, never its 8-bit grey.** PAPPL 1.4 has no
RGB-to-grey conversion anywhere (`grep rgb_to_gray` finds nothing); for an
8-bit grey raster it copies `bpp` bytes straight out of its 3-byte RGB buffer,
so "grey" is really the **red channel**. A saturated image then arrives already
black and white and there is nothing left to halftone. The driver therefore
advertises `srgb_8` and defaults `print-color-mode` to `color` — PAPPL only
emits sRGB in that mode — and `rwriteline()` does the luma itself via
`rgb_to_toner()`. The paper is still mono.

Supplies are not wired up yet; that is the next phase, via
`papplPrinterSetSupplies()` and `papplPrinterOpenDevice()`.

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

Changes to `ipp/printer-attrs.conf` or the `launchd/*.plist` files do need a
reload:

```bash
sudo bash Scripts/_privileged-restart-ipp.sh "$PWD"
```

`Scripts/*.sh` without the `_privileged-` prefix are the novice-facing entry
points (`Install Sister HL2030.sh`, `Uninstall Official Brother Drivers.sh`,
`Preview Halftone.sh`); they source `_common.sh` and escalate through
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
- Media margins in `ipp/printer-attrs.conf` are `1` (0.01 mm), never `0`:
  Apple's `ipp2ppd` reads 0 as borderless and the print dialog then silently
  scale-to-fits at ~96%.
- `ppd/Sister-HL-2030.ppd` is historical and is deliberately **not** installed
  by `CMakeLists.txt`; the installer also deletes copies left by earlier
  versions so CUPS cannot pick the legacy PPD path. Advertised attributes live
  in `ipp/printer-attrs.conf` — change them there, not in the PPD.
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
