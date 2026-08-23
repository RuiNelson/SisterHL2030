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

The driver is a PAPPL printer application. It owns the USB device end to end:

```
app / CUPS queue "Brother_HL_2030_series" (ipp://localhost:8631/ipp/print)
    → sister-printer-app  (LaunchDaemon org.sisterhl2030.printer, port 8631)
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

### Consumables

`status_cb()` opens the device, writes `pjl_supply_query()`, reads until
`pjl_response_complete()`, and hands the result to `papplPrinterSetSupplies()`.
PAPPL derives `marker-levels`/`marker-names` from that itself, so the macOS
Supply Levels panel works with no extra plumbing, and it advertises
`printer-supply-info-uri` over plain http rather than the self-signed https
that made the façade's supplies page unreachable.

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
- `ppd/Sister-HL-2030.ppd` is historical and is deliberately **not** installed
  by `CMakeLists.txt`; the installer also deletes copies left by earlier
  versions so CUPS cannot pick the legacy PPD path. Advertised attributes come
  from `driver_cb()` in `sister_app.cpp` — change them there, not in the PPD.
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
