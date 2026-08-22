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
ESC & l 1 h 1001 H                       # tray 1 (macOS rastertobrother2030)
ESC & l <copies> X
ESC * b 1 0 3 0 m                        # enter compression mode 1030
  …bands…
1 0 3 0 M                                # leave mode 1030
FF                                       # form feed (0x0c)
```

HQ1200 in the blob is `RAS1200MODE = TRUE` with `RESOLUTION = 600` (and
a variant that also sets `PAPERFEEDSPEED`). The PPD advertises it as
`1200x600dpi`.

Sister maps the IPP print dialog **Qualidade** as:

| Qualidade | PJL |
| --- | --- |
| Rascunho (`print-quality=draft`) | `RESOLUTION = 300`, `ECONOMODE = ON` |
| Normal | `RESOLUTION = 600`, `ECONOMODE = ON` |
| Alta (`print-quality=high`) | `RAS1200MODE = TRUE`, `ECONOMODE = OFF` |

### Media type (PJL)

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

At 300 dpi, dimensions are halved; at 1200 they are doubled. The Linux PPD
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
| Delta vs previous line | `edit_count` (1..254) + that many **edits** |

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

## What we do not emit (v1)

- HBP language (`@PJL ENTER LANGUAGE = HBP` and `@F`/`@X`/`@L`/`@S`/`@J`
  commands). Present in the library for other models.
- Duplex (`ESC & l 2 S`). The HL-2030 PPD has no duplex UI.
- PCL unit-of-measure / extra margin commands. The blob can emit them;
  the 1030 path used by `rawtobr2` does not require them for a basic page.
