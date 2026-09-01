# Brother HL-2030 job format

Recovered from `libbrcomplpr2.so` / `rawtobr2` (Linux LPR 2.0.1),
cross-checked against brlaser, then confirmed against the official
macOS `rastertobrother2030` (x86_64, 2014) running under Rosetta on
the connected HL-2030 (`usb://Brother/HL-2030%20series?serial=B9J561723`,
VID `0x04f9` PID `0x0027`). SisterHL2030 follows the **macOS** job
shape.

Bit polarity: **1 = black (toner), 0 = white**. Packed MSB-first,
row-major, each row padded to a whole number of bytes.

## Job envelope

Official macOS filter (what this printer actually accepts on USB):

```
ESC % - 1 2 3 4 5 X @ P J L LF
  …page PJL + PCL…
ESC % - 1 2 3 4 5 X
```

No 128-byte NUL prefix, no `@PJL JOB NAME` / `EOJ`. Those appear only
in the Linux `rawtobr2` path. `ESC %-12345X` is the UEL.

## Page header (PCL path)

`rawtobr2` calls `brcomp1030`, which enters **PCL**, not the HBP language
(the library also contains an HBP command table; the HL-2030 encoder does
not use it).

```
ESC %-12345X@PJL LF
@PJL SET RAS1200MODE = OFF LF            # TRUE for HQ1200 (macOS uses OFF, not FALSE)
@PJL SET RESOLUTION = 600 LF             # 300 or 600; HQ1200 still sets 600
@PJL SET ECONOMODE = OFF LF              # ON = toner save (draft + normal)
@PJL SET MEDIATYPE = REGULAR LF          # see table
@PJL SET ORIENTATION = PORTRAIT LF
@PJL SET PAPER = A4 LF                   # LETTER, A4, …
@PJL SET PAGEPROTECT = AUTO LF
@PJL ENTER LANGUAGE = PCL LF
ESC E                                    # PCL reset
ESC & l 1 h 1001 H                       # tray 1 / fixed (see paper source)
ESC & l <copies> X
ESC * b 1 0 3 0 m                        # enter compression mode 1030
  …bands…
1 0 3 0 M                                # leave mode 1030
FF                                       # form feed (0x0c)
```

HQ1200 in the blob is `RAS1200MODE = TRUE` with `RESOLUTION = 600` (and
a variant that also sets `PAPERFEEDSPEED`). The PPD advertises it as
`1200x600dpi`.

Sister maps the print dialog **Quality** as:

| Quality | PJL |
| --- | --- |
| Draft (`print-quality=3`) | `RESOLUTION = 300`, `ECONOMODE = ON` |
| Normal (`print-quality=4`) | `RESOLUTION = 600`, `ECONOMODE = ON` |
| Fine (`print-quality=5`, IPP high) | `RAS1200MODE = TRUE`, `ECONOMODE = OFF` |

### Media type (PJL)

The printer accepts the official driver's vocabulary:

| Driver option | `@PJL SET MEDIATYPE` |
| --- | --- |
| Plain | `REGULAR` |
| Thin | `THIN` |
| Thick | `THICK` |
| Thicker | `THICK2` |
| Transparency | `TRANSPARENCY` |
| Envelope | `ENVELOPES` |
| Env. thick | `ENVTHICK` |
| Env. thin | `ENVTHIN` |

The shipping PAPPL path only emits a subset, from the IPP `media-type`:

| IPP `media-type` | `@PJL SET MEDIATYPE` |
| --- | --- |
| stationery (default), stationery-letterhead, auto, other | `REGULAR` |
| cardstock, labels | `THICK` |
| transparency | `TRANSPARENCY` |
| envelope | `ENVELOPES` |

`THICK` forces the manual slot (see paper source below), so only media
that genuinely cannot take the cassette path is mapped to it.
Letterhead is ordinary-weight paper and stays `REGULAR`.

### Paper size at 600 dpi

From `paperinf` (width × height in pixels at 600 dpi):

| Name | Width | Height |
| --- | ---: | ---: |
| A4 | 4969 | 7015 |
| Letter | 5100 | 6600 |
| Legal | 5100 | 8400 |
| Executive | 4350 | 6300 |
| A5 | 3505 | 4960 |
| A6 | 2479 | 3505 |
| B5 | 4159 | 5899 |
| B6 | 2950 | 4160 |
| C5 | 3835 | 5410 |
| DL | 2599 | 5194 |
| Com-10 | 2475 | 5700 |
| Monarch | 2325 | 4500 |

### Imageable area

`paperinf` is the **sheet**, not what the engine paints. Brother's PPD
declares the same unprintable box for every cassette size — 18 pt left and
right, 12 pt top and bottom, symmetric on both axes
(`LinuxDrivers/cupswrapperHL2030-2.0.1`, `*ImageableArea`) — and its driver
never puts more than that on the wire:

| Name | Sheet (pt) | ImageableArea (pt) | Raster at 600 dpi | Bytes/line |
| --- | --- | --- | ---: | ---: |
| Letter | 612 × 792 | `18 12 594 780` | 4800 × 6400 | 600 |
| A4 | 595 × 842 | `18 12 577 830` | 4658 × 6817 | 583 |

`captures/official-hello-letter-600.bin`, from the official macOS
`rastertobrother2030`, decodes to exactly 6400 rows — 768 pt to the line.

This matters because Sister advertises **0.01 mm** margins (see below), so
CUPS rasterises the whole sheet: 5100 × 6600 for Letter, 4960 × 7015 for A4.
That is 38 bytes too many on every line and 200 lines too many on every page.
The band decoder has a line buffer sized from the box above, so the surplus
overruns it and scanlines come back blank — the fault looks like a halftone
or a lost-bytes bug and is neither. `crop_to_imageable`
(`src/encoder/halftone.h`) cuts the page back to the box, centred, after the
resample and before the halftone, so the wire only ever carries what the
engine can paint.

Sister advertises the cassette sizes plus the two envelopes that have
names in `paperinf`:

A4, Letter, Legal, Executive, Folio, A5, A6, B5 (ISO and JIS), B6,
Com-10, DL. Anything else is sent as `PAPER = A4`. JIS B5 has no
separate `paperinf` row, so it is sent as `B5`.

### Paper source (PCL)

The macOS `rastertobrother2030` tray-1 job emits `ESC & l 1 h 1001 H`
(paper source 1, then Brother "fixed tray" 1001). Sister keeps that for
the cassette. Manual feed and envelopes follow the HL-2070N column of
Brother's PCL technical reference (same family as the HL-2030):

| Source | Command |
| --- | --- |
| Tray 1 (default) | `ESC & l 1 h 1001 H` |
| Manual feed slot | `ESC & l 2 H` |
| Envelope from the manual slot | `ESC & l 3 H` |

`sourcetray = MANUAL` (IPP `manual` / `by-pass-tray`) selects the slot.
Thick stock (`THICK` / `THICK2`) and envelopes also select it even if
the job asked for tray 1 — those media cannot feed from the cassette.
That is why the media-type table above maps only cardstock and labels to
`THICK`: anything sent as thick stock loses the cassette.

At 300 dpi, dimensions are halved; at 1200 they are doubled — and
`RAS1200MODE` with a 600 dpi bitmap prints at half size, so the doubled
bitmap is not optional.

The bitmap doubling both ways does not mean the engine resolves 1200 both
ways: Brother's own PPD calls the mode `1200x600dpi`, the laser being
modulated twice per 600 dpi pixel along the scan while the drum still
advances at 600. Sister therefore halftones Fine quality on a **1200×600**
grid and emits each dithered row twice. The horizontal half of the mode is
real halftone detail; the vertical half is replication the format demands.

(Earlier releases dithered at 600×600 and pixel-doubled the 1-bit result,
on the belief that Atkinson could not cover a 1200 dpi page in time. That
was never measured and is not true: serpentine Atkinson over a 1200×600 A4
page — 70 million samples — takes about 155 ms on Apple Silicon, and 291 ms
over a full 1200×1200 one.)

The Linux PPD
declares 18×12 pt unprintable margins. Sister advertises **0.01 mm**
hardware margins. Zero made Apple's AirPrint path label sizes as
`A4.Borderless` and the print dialog then Scale-to-Fits at ~96%. The laser
still clips a few millimetres at the physical edge.

## Compression mode 1030

Graphics data is sent in **bands**. `saveDumb_1030` flushes a band when:

- adding the next line would make the payload **greater than 16384**
  (`0x4000`) bytes, or
- the line counter is a multiple of **128** (`count & 0x7f == 0` before
  increment, i.e. every 128 lines).

The first line of every band is encoded as absolute (no delta against
the previous line), including after a size flush mid-page.

### Band header

ASCII decimal length, then `w`, then two bytes:

```
"<nbytes>w"  0x00  <nlines>
<nlines packed lines>
```

`nbytes` is the size of (`0x00` + `nlines` + packed lines), i.e.
`payload + 2`. This is PCL raster transfer (`ESC * b <count> W`) with the
escape already implied by mode 1030; the blob emits the `%dw` form
directly.

### Packed line

| Encoding | Bytes |
| --- | --- |
| White (all bits 0) | `0xFF` |
| Absolute (no reference) | `0x01` + one **substitute** of the whole line |
| Delta vs previous line | `edit_count` (0..254) + that many **edits** (`0` = copy previous) |

Edits, in stream order:

**Substitute** (copy literal bytes at an offset from the last edit):

```
b0 = (min(offset, 15) << 3) | min(count - 1, 7)
[overflow of offset - 15]
[overflow of (count - 1) - 7]
count literal bytes
```

**Repeat** (run of identical bytes, minimum length 2); distinguished by
the high bit of `b0`:

```
b0 = 0x80 | (min(offset, 3) << 5) | min(count - 2, 31)
[overflow of offset - 3]
[overflow of (count - 2) - 31]
1 byte value
```

**Overflow**: if `extra < 0`, emit nothing. If `extra < 255`, emit one
byte `extra`. Otherwise emit `extra / 255` bytes of `0xFF` followed by
`extra % 255`.

`SendData_1030` emits a single `0xFF` line when the compressor reports
the line is empty.

## Checking a job without printing

`Scripts/decode_job.py` reverses everything above — bands, delta lines,
substitute and repeat edits — back into a PBM, and round-trips
`sister-rawtobr` output byte-for-byte. Use it to confirm halftoning survived
the encoder instead of spending paper on it.

## What we do not emit (v1)

- HBP language (`@PJL ENTER LANGUAGE = HBP` and `@F`/`@X`/`@L`/`@S`/`@J`
  commands). Present in the library for other models.
- Duplex (`ESC & l 2 S`). The HL-2030 PPD has no duplex UI.
- PCL unit-of-measure / extra margin commands. The blob can emit them;
  the 1030 path used by `rawtobr2` does not require them for a basic page.

## Status readback (consumables)

The HL-2030 USB interface is printer-class **bidirectional** (class 7,
subclass 1, protocol 2): bulk OUT `0x01`, bulk IN `0x82`. IEEE 1284
device ID is `MFG:Brother;CMD:PJL,HBP;MDL:HL-2030 series;CLS:PRINTER;`.

The shipping driver reads supplies in `status_cb` in
`sister-printer-app`, on the same device the job is written to. The
`sister-status` CLI talks USB directly for debugging; it is not how
System Settings gets its numbers.

Each command is its own UEL-wrapped transaction (CRLF, unlike the job
stream which uses LF). `pjl_supply_query()` concatenates four of them:

```
ESC %-12345X@PJL CR LF
@PJL INFO STATUS CR LF
ESC %-12345X
ESC %-12345X@PJL CR LF
@PJL INFO PAGECOUNT CR LF
ESC %-12345X
ESC %-12345X@PJL CR LF
@PJL INFO DRUMLIFE CR LF
ESC %-12345X
ESC %-12345X@PJL CR LF
@PJL ECHO SisterHL2030 CR LF
ESC %-12345X
```

The reply to command N often arrives after command N+1 has already been
sent, so `DRUMLIFE` is still in flight when `ECHO` goes out; the echo
exists to shake that last reply loose. Its own reply is surplus.
Completion is `CODE=` *and* `DRUMLIFE=` in the buffer
(`pjl_response_complete()`), not a form-feed and not the `ECHO` text —
the hardware path never confirmed the device answers `ECHO`.

The IN pipe is drained on the way *in*. Nothing else on the print path
ever reads, and PAPPL clears no pipe on open, so an unread block survives
across close/open and the next poll would treat it as a fresh answer.
A trailing drain would cost another empty read to do less. The wait is
bound on the wall clock: `papplDeviceRead` is a blocking bulk transfer,
so counting reads is really counting timeouts, and the device is
unavailable for exactly as long as this callback runs.

Confirmed on `04f9:0027` / serial `B9J561723` / firmware `Ver1.29`:

| Command | Response |
| --- | --- |
| `INFO STATUS` | `CODE=…` `DISPLAY="…"` `ONLINE=TRUE` |
| `INFO PAGECOUNT` | `PAGECOUNT=` total pages |
| `INFO DRUMLIFE` | `DRUMLIFE=` pages on the current drum |
| `INFO ID` | `"Brother HL-2030 series:84UZ81:Ver1.29"` |
| `INFO CONSUMABLE` / `INFO TONER` / `DINQUIRE TONERLOW` | `"?"` (unsupported) |

`CODE=10001` is ready, `CODE=40000` is sleep. Toner low is `CODE=10006`
(and `40038` when the panel wants Go); empty is `CODE=40010`. Engine
intervention, from the same `INFO STATUS` block:

| `CODE` | Sister sets |
| --- | --- |
| `40021` | cover open (Brother TRG example: `DISPLAY="12 COVER OPEN"`) |
| `40022` | media jam |
| `11000`–`11999` | media empty (paper-source status) |
| `41000`–`41999` | media empty (no other tray to pull from) |

`DISPLAY` is the front-panel string and follows the printer's own language
setting, so Sister keys off `CODE` and only reads `DISPLAY` as a fallback
for codes it does not map (English `COVER` / `JAM` / `NO PAPER`; a
manual-slot wait becomes `media-needed` rather than empty). These flags
go to `papplPrinterSetReasons()` so the print dialog can show a jam or
an open cover instead of a job that sits there.

The cartridge sensor is **not** a continuous percentage: Sister maps OK → 100,
low → 15, empty → 0. Drum remaining is `round(100 × (12000 − DRUMLIFE) /
12000)` (rated life from the service/user manuals); `CODE=40129` forces
empty, `CODE=40130` clamps to low.

`status_cb` hands the two levels to `papplPrinterSetSupplies()`. PAPPL
derives both `printer-supply` and the classic `marker-levels` /
`marker-names` set from that, which is what the macOS Supply Levels
panel reads. CUPS only copies `marker-*` onto the queue while a job
runs through its backend, so a job named `.sister-status` reports
status and prints nothing, to populate the panel without paper.
