#!/usr/bin/env python3
"""Decode a Brother HL-2030 mode-1030 job back into a bitmap.

Test infrastructure: lets us check what the printer would actually put on
paper -- in particular whether an image was halftoned -- without printing.
See docs/protocol.md for the format.
"""

import re
import struct
import sys
import zlib


# Width x height in pixels at 600 dpi, from docs/protocol.md's "Paper size
# at 600 dpi" table (recovered from the blob's `paperinf`). Used only as a
# fallback bpl estimate -- see pjl_bpl_hint() below for why it is not exact.
PAPER_SIZES_600DPI = {
    "A4": (4969, 7015),
    "LETTER": (5100, 6600),
    "LEGAL": (5100, 8400),
    "EXECUTIVE": (4350, 6300),
    "A5": (3505, 4960),
    "A6": (2479, 3505),
    "B5": (4159, 5899),
    "B6": (2950, 4160),
    "C5": (3835, 5410),
    "DL": (2599, 5194),
    "COM10": (2475, 5700),
    "MONARCH": (2325, 4500),
    "FOLIO": (5100, 7800),
}


def read_overflow(data, i):
    """Overflow bytes: 0xFF means 'add 255 and keep reading'."""
    extra = 0
    while i < len(data) and data[i] == 0xFF:
        extra += 255
        i += 1
    if i < len(data):
        extra += data[i]
        i += 1
    return extra, i


def decode_line(data, i, ref, bpl):
    """Decode one packed line. Returns (line, next_index, bpl)."""
    count = data[i]
    i += 1
    if count == 0xFF:                       # all-white line
        return bytearray(bpl or 0), i, bpl

    line = bytearray(ref) if ref is not None else bytearray(bpl or 0)
    pos = 0
    for _ in range(count):
        b0 = data[i]
        i += 1
        if b0 & 0x80:                       # repeat run
            offset = (b0 >> 5) & 0x03
            n = (b0 & 0x1F) + 2
            if offset == 3:
                extra, i = read_overflow(data, i)
                offset += extra
            if n - 2 == 31:
                extra, i = read_overflow(data, i)
                n += extra
            value = data[i]
            i += 1
            pos += offset
            if bpl is None and pos + n > len(line):
                line.extend(bytearray(pos + n - len(line)))
            line[pos:pos + n] = bytes([value]) * n
            pos += n
        else:                               # substitute literal bytes
            offset = (b0 >> 3) & 0x0F
            n = (b0 & 0x07) + 1
            if offset == 15:
                extra, i = read_overflow(data, i)
                offset += extra
            if n - 1 == 7:
                extra, i = read_overflow(data, i)
                n += extra
            pos += offset
            if bpl is None and pos + n > len(line):
                line.extend(bytearray(pos + n - len(line)))
            line[pos:pos + n] = data[i:i + n]
            i += n
            pos += n
    return line, i, bpl


def pjl_bpl_hint(data, header_end):
    """Best-effort bpl guess from the PJL page header (PAPER / RESOLUTION /
    RAS1200MODE), for the rare page where no band-start line ever reveals
    the true width on its own (see decode()).

    This is only an estimate: sister-rawtobr and similar raw-testing tools
    write whatever PAPER name the caller asked for without checking it
    against the actual pixel data, and even for real jobs the printer's
    internal `paperinf` table (this function's source, see
    docs/protocol.md) does not always agree byte-for-byte with the
    CUPS/PAPPL raster width for the same nominal page size. Prefer the
    band-start signal whenever it's available.
    """
    header = data[:header_end]
    m_paper = re.search(rb"@PJL SET PAPER\s*=\s*(\S+)", header)
    if not m_paper:
        return None
    paper = m_paper.group(1).decode("ascii", "replace").upper()
    size = PAPER_SIZES_600DPI.get(paper)
    if size is None:
        print("decode_job: unrecognized PJL PAPER=%s, cannot estimate "
              "width from it" % paper, file=sys.stderr)
        return None
    width_600 = size[0]

    m_ras = re.search(rb"@PJL SET RAS1200MODE\s*=\s*(TRUE|OFF|FALSE)", header)
    if m_ras and m_ras.group(1) == b"TRUE":
        # Fine/HQ1200: Atkinson runs at 600 dpi, then the 1-bit page is
        # nearest-neighbour upsampled to the doubled grid before it hits
        # the wire (docs/protocol.md, "Paper size at 600 dpi").
        width_px = width_600 * 2
    else:
        m_res = re.search(rb"@PJL SET RESOLUTION\s*=\s*(\d+)", header)
        resolution = int(m_res.group(1)) if m_res else 600
        if resolution >= 600:
            width_px = width_600
        else:
            width_px = width_600 // 2  # matches box_downsample_2x's floor

    return (width_px + 7) // 8


def decode(data):
    """Return (rows, bpl) for the first page in the job.

    bpl (bytes per line) is never carried explicitly in the band data: a
    delta-encoded line omits any trailing bytes that match the reference
    line, so a decoded line can come out shorter than the true page width
    whenever the inked content doesn't reach the right edge. Latching bpl
    from whichever row happens to be the first non-blank one (the old
    approach) is wrong for exactly that reason -- that row may be a delta
    line that stops short.

    The one place width IS unambiguous is the first line of every band:
    job.cc always writes it via the no-reference/"Absolute" path (never a
    delta), and line.cc's no-reference encoder always emits the whole
    line, trailing zeros included, unless the line is genuinely blank (see
    docs/protocol.md). So: scan band-start lines for the first non-blank
    one and trust its length -- it is guaranteed complete by construction.
    Only if no band-start line ever reveals it (a page whose content
    never touches a band boundary) do we fall back to a PJL-header
    estimate, and finally to the old first-non-blank-row heuristic.
    """
    start = data.find(b"\x1b*b1030m")
    if start < 0:
        raise SystemExit("no mode-1030 data found (not a HL-2030 job?)")
    i = start + len(b"\x1b*b1030m")

    rows = []
    ref = None
    bpl = None
    first_nonblank_len = None  # old heuristic, kept as last-resort fallback
    while i < len(data):
        if data[i:i + 5] == b"1030M":
            break
        j = i
        while j < len(data) and data[j:j + 1].isdigit():
            j += 1
        if j == i or data[j:j + 1] != b"w":
            break
        nbytes = int(data[i:j])
        i = j + 1
        if data[i] != 0x00:
            break
        nlines = data[i + 1]
        i += 2
        end = i + nbytes - 2
        band_start = True
        while i < end:
            line, i, bpl = decode_line(data, i, ref, bpl)
            if len(line) > 0:
                if first_nonblank_len is None:
                    first_nonblank_len = len(line)
                # Only a band-start line is guaranteed complete; a mid-band
                # delta line can be legitimately shorter than the page.
                if band_start and bpl is None:
                    bpl = len(line)
            if bpl is not None and len(line) < bpl:
                line.extend(bytearray(bpl - len(line)))
            rows.append(bytes(line))
            ref = line
            band_start = False
        if len(rows) and nlines == 0:
            break

    if bpl is None:
        hint = pjl_bpl_hint(data, start)
        if hint is not None:
            print("decode_job: no band-start line revealed the page width; "
                  "estimating bpl=%d from the PJL header (PAPER/RESOLUTION/"
                  "RAS1200MODE) -- treat this as a best-effort guess" % hint,
                  file=sys.stderr)
            bpl = hint
        elif first_nonblank_len:
            print("decode_job: no band-start line and no usable PJL header; "
                  "falling back to the first non-blank row's length "
                  "(bpl=%d) -- this can be wrong if that row doesn't reach "
                  "the true right edge" % first_nonblank_len, file=sys.stderr)
            bpl = first_nonblank_len

    bpl = bpl or 0
    # Pad any leading blank lines / short delta lines now that the width
    # is known.
    rows = [r + bytes(bpl - len(r)) if len(r) < bpl else r for r in rows]
    return rows, bpl


def write_png(path, rows, bpl):
    """1-bit greyscale PNG. Packed rows use 1 = black; PNG uses 0 = black."""
    width, height = bpl * 8, len(rows)

    raw = bytearray()
    for row in rows:
        raw.append(0)                       # filter type: none
        raw.extend(bytes(b ^ 0xFF for b in row))

    def chunk(tag, payload):
        out = struct.pack(">I", len(payload)) + tag + payload
        return out + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def crop_rows(rows, bpl, cx, cy, cw_bytes, ch):
    """A 1:1 slice of the page, so the dots are visible as dots."""
    x0 = max(0, min(bpl - cw_bytes, cx - cw_bytes // 2))
    y0 = max(0, min(len(rows) - ch, cy - ch // 2))
    return [r[x0:x0 + cw_bytes] for r in rows[y0:y0 + ch]], cw_bytes


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    detail = "--detail" in sys.argv
    if not args:
        raise SystemExit(
            "usage: decode_job.py JOB.prn [OUT.png|OUT.pbm] [--detail]\n"
            "  --detail  also write OUT-detail.png, a 1:1 crop showing the dots")
    data = open(args[0], "rb").read()
    rows, bpl = decode(data)
    if not rows:
        raise SystemExit("no raster rows decoded")

    width = bpl * 8
    black = sum(bin(b).count("1") for row in rows for b in row)
    total = len(rows) * width
    # Bytes that are neither empty nor solid indicate a dot pattern; a hard
    # black/white page is almost entirely 0x00 and 0xFF.
    mixed = sum(1 for row in rows for b in row if b not in (0x00, 0xFF))
    nbytes = len(rows) * bpl

    print("rows          : %d" % len(rows))
    print("bytes/line    : %d  (width %d px)" % (bpl, width))
    if total == 0:
        print("black coverage: n/a (no rows decoded)")
    else:
        print("black coverage: %.1f%%" % (100.0 * black / total))
    if nbytes == 0:
        print("mixed bytes   : n/a (no rows decoded)")
    else:
        print("mixed bytes   : %.1f%%  <- dither patterns" % (100.0 * mixed / nbytes))

    if len(args) > 1:
        out = args[1]
        if out.endswith(".png"):
            write_png(out, rows, bpl)
        else:
            with open(out, "wb") as f:
                f.write(b"P4\n%d %d\n" % (width, len(rows)))
                for row in rows:
                    f.write(row)
        print("wrote         : %s" % out)

        if detail:
            cw, ch = 100, 700          # 800 px wide, 700 rows, at 1:1
            crop, cbpl = crop_rows(rows, bpl, bpl // 2, len(rows) // 2, cw, ch)
            dpath = out.rsplit(".", 1)[0] + "-detail.png"
            write_png(dpath, crop, cbpl)
            print("wrote         : %s  (1:1, dots visible)" % dpath)


if __name__ == "__main__":
    main()
