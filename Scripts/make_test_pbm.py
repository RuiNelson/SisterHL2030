#!/usr/bin/env python3
"""Write a 600 dpi A4 (imageable area) 1-bit PBM test page to stdout."""

import sys

# PPD ImageableArea A4 at 600 dpi: (577-18) x (830-12) pt.
WIDTH = 4658
HEIGHT = 6817


def set_pixel(row: bytearray, x: int, on: bool = True) -> None:
    if 0 <= x < WIDTH:
        if on:
            row[x // 8] |= 0x80 >> (x % 8)
        else:
            row[x // 8] &= ~(0x80 >> (x % 8))


def fill_rect(rows: list, x0: int, y0: int, x1: int, y1: int) -> None:
    x0 = max(0, x0)
    x1 = min(WIDTH, x1)
    y0 = max(0, y0)
    y1 = min(HEIGHT, y1)
    for y in range(y0, y1):
        for x in range(x0, x1):
            set_pixel(rows[y], x)


# 5x7 glyphs, MSB left. Space is empty.
FONT = {
    " ": [0, 0, 0, 0, 0, 0, 0],
    "A": [0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11],
    "C": [0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E],
    "E": [0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F],
    "H": [0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11],
    "I": [0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F],
    "L": [0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F],
    "M": [0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11],
    "N": [0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11],
    "O": [0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E],
    "R": [0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11],
    "S": [0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E],
    "T": [0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04],
    "0": [0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E],
    "2": [0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F],
    "3": [0x1F, 0x01, 0x02, 0x06, 0x01, 0x11, 0x0E],
    "-": [0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00],
}


def draw_text(rows: list, text: str, x: int, y: int, scale: int) -> None:
    for ch in text:
        glyph = FONT.get(ch, FONT[" "])
        for gy, bits in enumerate(glyph):
            for gx in range(5):
                if bits & (0x10 >> gx):
                    fill_rect(
                        rows,
                        x + gx * scale,
                        y + gy * scale,
                        x + (gx + 1) * scale,
                        y + (gy + 1) * scale,
                    )
        x += 6 * scale


def main() -> None:
    bpl = (WIDTH + 7) // 8
    rows = [bytearray(bpl) for _ in range(HEIGHT)]
    t = 48
    fill_rect(rows, 0, 0, WIDTH, t)
    fill_rect(rows, 0, HEIGHT - t, WIDTH, HEIGHT)
    fill_rect(rows, 0, 0, t, HEIGHT)
    fill_rect(rows, WIDTH - t, 0, WIDTH, HEIGHT)
    draw_text(rows, "SISTER", 400, 800, 48)
    draw_text(rows, "HL-2030", 400, 1400, 36)
    draw_text(rows, "ARM64", 400, 1900, 36)
    fill_rect(rows, 400, 2500, WIDTH - 400, 2580)
    sys.stdout.buffer.write(f"P4\n{WIDTH} {HEIGHT}\n".encode("ascii"))
    for row in rows:
        sys.stdout.buffer.write(row)


if __name__ == "__main__":
    main()
