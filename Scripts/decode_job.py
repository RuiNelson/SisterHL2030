#!/usr/bin/env python3
"""Decode a Brother HL-2030 mode-1030 job back into a bitmap.

Test infrastructure: lets us check what the printer would actually put on
paper -- in particular whether an image was halftoned -- without printing.
See docs/protocol.md for the format.
"""

import sys


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


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: decode_job.py JOB.prn [OUT.pbm]")
    data = open(sys.argv[1], "rb").read()
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

    if len(sys.argv) > 2:
        with open(sys.argv[2], "wb") as f:
            f.write(b"P4\n%d %d\n" % (width, len(rows)))
            for row in rows:
                f.write(row)
        print("wrote         : %s" % sys.argv[2])


if __name__ == "__main__":
    main()
