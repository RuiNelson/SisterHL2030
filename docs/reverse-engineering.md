# Reverse engineering notes

Scope: Linux LPR 2.0.1 i386 binaries shipped in `LinuxDrivers/`, used as
a protocol reference. SisterHL2030 does not link them.

The reimplementation lives under GPL-2+ (see `LICENSE` / `NOTICE`).

## Artifacts

| File | Type | Role |
| --- | --- | --- |
| `rawtobr2` | ELF 32-bit LSB exe, stripped | Raster → Brother stream. CLI: `-pi`, `-rc`, `-func`. |
| `libbrcomplpr2.so` | ELF 32-bit LSB .so, stripped | Encoder. Only undefined symbol consumed by `rawtobr2`: `brcomp1030`. |
| `brprintconflsr2` | ELF 32-bit LSB exe, stripped | Writes `brHL2030rc` from CLI flags (`-res`, `-pt`, `-md`, …). |
| `braddprinter` | ELF 32-bit LSB exe, stripped | Install-time printcap helper. Not needed at print time. |
| `filterHL2030` | POSIX sh, GPL-2 | `stdin` → Ghostscript `bit` → `rawtobr2`. |
| `psconvert2` | POSIX sh, GPL-2 | Ghostscript `-sDEVICE=bit -r$RES -g$W x $H`. |
| `cupswrapperHL2030` | POSIX sh, GPL-2 | Installs PPD + `brlpdwrapperHL2030`. |
| `brcupsconfig.c` | C, GPL-2 | Maps PPD / lp options onto `brprintconflsr2`. |

Pipeline recovered from the scripts:

```
CUPS PS → brlpdwrapperHL2030 → filterHL2030
       → gs -sDEVICE=bit → rawtobr2 -pi paperinf -rc brHL2030rc
       → brcomp1030() → USB
```

## `libbrcomplpr2.so` exports used for HL-2030

```
brcomp1030        0x3488  size 0x5f5   entry: width/height from arg2
SendData_1030     0x411c  size 0x45d   per-line send, 0xFF if empty
serialSend_1030   0x4c13  size 0xc4    band flush, format "%dw"
saveDumb_1030     0x4b9d  size 0x76    band accumulator, cap 0x4000,
                                       flush when (count & 0x7f) == 0
invertData        0x5f42               bitwise NOT of a buffer
```

`.rodata` is a table of 0x41-byte records: byte 0 = length, bytes 1… =
payload. That is where the PJL/PCL strings in `docs/protocol.md` come
from, including:

```
ESC %-12345X@PJL
@PJL ENTER LANGUAGE = PCL
ESC * b 1030 m
1030 M
```

The same table contains an unused (for this model) HBP dialect
(`@PJL ENTER LANGUAGE = HBP`, `@F`, `@X`, `@L`, `@S`, `@J`).

Dither matrices `matrix600`, `matrix1200`, `matrix1200_ZL2` sit in
`.data`. v1 does not use them: input is already 1-bit.

## `rawtobr2` behaviour

- Opens paper info (`-pi`) and rc file (`-rc`).
- Scales page pixels: 300 dpi halves `paperinf` 600-dpi sizes; 1200/HQ
  variants double them.
- Calls `brcomp1030(in, out, geometry, settings)`.
- One code path XOR-inverts each raster row (`xorb $0xff`), matching
  Ghostscript `bit` polarity vs toner polarity.

## Method

macOS `/usr/bin/objdump` accepts `elf32-i386`. Work so far is
`objdump -T/-s/-d` plus `strings`. Full decompilation of the line codec
is not required: empty-line (`0xFF`), band cap, and `%dw` header fell
out of `SendData_1030` / `saveDumb_1030`, and the edit grammar matches
the independently published brlaser description.

Further decompilation is reserved for mismatches against a USB capture
from a real HL-2030.

## USB identity (confirmed on hardware)

| | |
| --- | --- |
| Vendor | `0x04f9` (1273) Brother Industries |
| Product | `0x0027` (39) HL-2030 series |
| Serial | `B9J561723` |
| Speed | USB full-speed (12 Mbit/s) |
| CUPS URI | `usb://Brother/HL-2030%20series?serial=B9J561723` |
| 1284 DeviceID | `MFG:Brother;MDL:HL-2030 series` (from the installed PPD) |

PJL status readback (`INFO STATUS` / `PAGECOUNT` / `DRUMLIFE`) was
confirmed on the connected HL-2030; see `docs/protocol.md`. The Linux
encoder blobs do not implement it — that lives in the Windows/macOS
Status Monitor, not in `rawtobr2`.

The Mac already has Brother's 2014 CUPS package:
`/Library/Printers/Brother/Filter/rastertobrother2030.bundle` — universal
**x86_64 + i386**, no arm64. It runs today only via Rosetta (`oahd`).
Capturing its stdout on a CUPS raster of a text page produced a 12 290-byte
mode-1030 job used as the envelope reference (see `docs/protocol.md`).
