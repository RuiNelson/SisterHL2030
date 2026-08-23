#!/usr/bin/env python3
"""Decode a Brother HL-2030 mode-1030 job back into a bitmap.

Test infrastructure: lets us check what the printer would actually put on
paper -- in particular whether an image was halftoned -- without printing.
See docs/protocol.md for the format.
"""

import struct
import sys
import zlib


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


def decode(data):
    """Return (rows, bpl) for the first page in the job."""
    start = data.find(b"\x1b*b1030m")
    if start < 0:
        raise SystemExit("no mode-1030 data found (not a HL-2030 job?)")
    i = start + len(b"\x1b*b1030m")

    rows = []
    ref = None
    bpl = None
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
        while i < end:
            line, i, bpl = decode_line(data, i, ref, bpl)
            if bpl is None:
                bpl = len(line)
            if len(line) < bpl:
                line.extend(bytearray(bpl - len(line)))
            rows.append(bytes(line))
            ref = line
        if len(rows) and nlines == 0:
            break
    return rows, (bpl or 0)


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
    print("black coverage: %.1f%%" % (100.0 * black / total))
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
