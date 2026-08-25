#!/usr/bin/env python3
"""Generate test_fixtures/calibration.pdf, the darkness calibration target.

Two A4 pages of step wedges, continuous gradients with a printed scale,
colour swatches and fine detail, plus a reporting form. Print it, read the
numbers off the paper, fill in the form, and the answers give the constants
the darkness model in src/encoder/halftone.h rests on. Each screen carries
its own independent set, so a sheet is only ever evidence about the style it
was printed with -- tick the Style box.

Everything is vector: no bitmap, so nothing here is resampled on its way to
the printer and the only halftone in the output is the driver's own.

    python3 Scripts/make_calibration_pdf.py [-o test_fixtures/calibration.pdf]

No third-party modules -- it writes the PDF itself.
"""

import argparse
import os
import re
import subprocess
import sys
import zlib

A4_W, A4_H = 595.276, 841.890
MARGIN = 40.0

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# --------------------------------------------------------------------------
# Minimal PDF writer
# --------------------------------------------------------------------------


class Pdf:
    def __init__(self):
        self.objects = [None]  # 1-based; index 0 unused

    def add(self, body):
        """Append an object, returning its "N 0 R" reference number."""
        self.objects.append(body)
        return len(self.objects) - 1

    def reserve(self):
        self.objects.append(None)
        return len(self.objects) - 1

    def set(self, num, body):
        self.objects[num] = body

    def stream(self, dictionary, data, compress=True):
        if compress:
            data = zlib.compress(data, 9)
            dictionary = dictionary + " /Filter /FlateDecode"
        head = "<< %s /Length %d >>\nstream\n" % (dictionary, len(data))
        return head.encode("ascii") + data + b"\nendstream"

    def write(self, path):
        out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
        offsets = [0] * (len(self.objects))
        for num in range(1, len(self.objects)):
            body = self.objects[num]
            if body is None:
                raise RuntimeError("object %d was reserved but never set" % num)
            if isinstance(body, str):
                body = body.encode("ascii")
            offsets[num] = len(out)
            out += b"%d 0 obj\n" % num
            out += body
            out += b"\nendobj\n"
        xref = len(out)
        count = len(self.objects)
        out += b"xref\n0 %d\n" % count
        out += b"0000000000 65535 f \n"
        for num in range(1, count):
            out += b"%010d 00000 n \n" % offsets[num]
        out += b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
            count,
            self.catalog,
            xref,
        )
        with open(path, "wb") as f:
            f.write(bytes(out))


# --------------------------------------------------------------------------
# Content stream helpers
# --------------------------------------------------------------------------


# The base-14 fonts are declared /WinAnsiEncoding, which is Latin-1 plus a
# handful of typographic characters in the 0x80..0x9f range Latin-1 leaves
# undefined. Map those by hand and let Latin-1 do the rest.
WINANSI = {
    "\u2018": "\x91", "\u2019": "\x92", "\u201c": "\x93", "\u201d": "\x94",
    "\u2022": "\x95", "\u2013": "\x96", "\u2014": "\x97", "\u2026": "\x85",
    "\u2020": "\x86", "\u2122": "\x99",
}


def winansi(text):
    for src, dst in WINANSI.items():
        text = text.replace(src, dst)
    return text.encode("latin-1", "replace")


def esc(text):
    return text.replace("\\", r"\\").replace("(", r"\(").replace(")", r"\)")


class Canvas:
    """Accumulates one page's content stream, plus the shadings it needs."""

    def __init__(self, pdf):
        self.pdf = pdf
        self.ops = []
        self.shadings = {}  # name -> object number

    def op(self, line):
        self.ops.append(line)

    def text(self, x, y, s, size=9.0, font="F1", gray=0.0):
        self.op("BT /%s %.2f Tf %.3f g %.2f %.2f Td (%s) Tj ET"
                % (font, size, gray, x, y, esc(s)))

    def text_centre(self, cx, y, s, size=9.0, font="F1", gray=0.0):
        # Helvetica/Courier widths are close enough to 0.5 em for labels this
        # small; the target does not need typesetting, it needs legends.
        self.text(cx - 0.26 * size * len(s), y, s, size, font, gray)

    def rect_fill(self, x, y, w, h, colour):
        self.op("%s %.4f %.4f %.4f %.4f re f" % (colour_op(colour, fill=True),
                                                 x, y, w, h))

    def rect_stroke(self, x, y, w, h, width=0.4, gray=0.0):
        self.op("%.3f G %.3f w %.4f %.4f %.4f %.4f re S" % (gray, width, x, y, w, h))

    def line(self, x0, y0, x1, y1, width=0.4, gray=0.0):
        self.op("%.3f G %.3f w %.3f %.3f m %.3f %.3f l S"
                % (gray, width, x0, y0, x1, y1))

    def gradient(self, x, y, w, h, c0, c1, space):
        """Axial shading from c0 to c1, clipped to the given box."""
        fn = self.pdf.add(
            "<< /FunctionType 2 /Domain [0 1] /C0 [%s] /C1 [%s] /N 1 >>"
            % (" ".join("%.4f" % v for v in c0), " ".join("%.4f" % v for v in c1))
        )
        sh = self.pdf.add(
            "<< /ShadingType 2 /ColorSpace /%s /Coords [%.3f %.3f %.3f %.3f] "
            "/Function %d 0 R /Extend [true true] >>"
            % (space, x, y, x + w, y, fn)
        )
        name = "Sh%d" % sh
        self.shadings[name] = sh
        self.op("q %.3f %.3f %.3f %.3f re W n /%s sh Q" % (x, y, w, h, name))

    def data(self):
        return winansi("\n".join(self.ops))


def colour_op(colour, fill=True):
    """`colour` is ("g", v) for DeviceGray or ("rgb", r, g, b)."""
    if colour[0] == "g":
        return "%.4f %s" % (colour[1], "g" if fill else "G")
    return "%.4f %.4f %.4f %s" % (colour[1], colour[2], colour[3],
                                  "rg" if fill else "RG")


# --------------------------------------------------------------------------
# Page furniture
# --------------------------------------------------------------------------


def driver_version():
    try:
        text = open(os.path.join(ROOT, "CMakeLists.txt")).read()
        m = re.search(r"project\s*\(\s*sisterhl2030\s+VERSION\s+([0-9.]+)", text)
        version = m.group(1) if m else "?"
    except OSError:
        version = "?"
    try:
        sha = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=5)
        if sha.returncode == 0:
            version += "+" + sha.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return version


def model_constants():
    """The constants this sheet is meant to measure, read from the source."""
    out = {}
    try:
        text = open(os.path.join(ROOT, "src/encoder/halftone.h")).read()
        for key in ("kAtkinsonSuppressionUm", "kAtkinsonDensity",
                    "kAm45SuppressionUm", "kAm45Density"):
            m = re.search(r"constexpr float %s = ([0-9.]+)f" % key, text)
            if m:
                out[key] = m.group(1)
    except OSError:
        pass
    return out


def heading(c, x, y, number, title):
    c.text(x, y, "%s  %s" % (number, title), 10.5, "F2")
    c.line(x, y - 3.5, A4_W - MARGIN, y - 3.5, 0.6, 0.35)


def caption(c, x, y, s):
    c.text(x, y, s, 7.5, "F1", 0.25)


def checkbox_row(c, x, y, label, options):
    c.text(x, y, label, 8.5, "F2")
    cx = x + 52
    for opt in options:
        c.rect_stroke(cx, y - 1.2, 8, 8, 0.5, 0.0)
        c.text(cx + 11, y, opt, 8.5)
        cx += 11 + 5.2 * len(opt) + 14
    return cx


def rule_line(c, x, y, w, label=None, size=8.5):
    """A labelled blank for writing an answer on."""
    if label:
        c.text(x, y, label, size)
        x += 4.9 * size / 8.5 * len(label) + 6
    c.line(x, y - 1.5, x + w, y - 1.5, 0.5, 0.45)
    return x + w


# --------------------------------------------------------------------------
# The measurement blocks
# --------------------------------------------------------------------------


def step_wedge(c, x, y, w, h, values, space, label_every=1, label_size=5.0,
               fmt="%d"):
    """A row of patches. `values` are percentage coverages, 0 = paper."""
    n = len(values)
    pw = w / n
    for i, pct in enumerate(values):
        v = 1.0 - pct / 100.0
        colour = ("g", v) if space == "gray" else ("rgb", v, v, v)
        c.rect_fill(x + i * pw, y, pw, h, colour)
    c.rect_stroke(x, y, w, h, 0.4, 0.0)
    for i, pct in enumerate(values):
        if i % label_every:
            continue
        c.text_centre(x + (i + 0.5) * pw, y - 8, fmt % pct, label_size)
    return y - 8


def scaled_gradient(c, x, y, w, h, c0, c1, space, name):
    """A continuous gradient with an index line every 10% and a tick scale."""
    c.gradient(x, y, w, h, c0, c1, space)
    c.rect_stroke(x, y, w, h, 0.4, 0.0)
    for pct in range(0, 101, 5):
        px = x + w * pct / 100.0
        major = pct % 10 == 0
        # Index line drawn INTO the ramp, so a spot on the print can be named
        # by number and not just pointed at.
        if 0 < pct < 100 and major:
            c.line(px, y, px, y + h, 0.5, 1.0)
        c.line(px, y - 1, px, y - (5.0 if major else 2.5), 0.5, 0.0)
        if major:
            c.text_centre(px, y - 12, "%d" % pct, 5.5)
    c.text(x - 12, y + h / 2 - 2.5, name, 8.0, "F2")
    return y - 12


RAMPS = [
    ("R", [1, 0, 0]), ("G", [0, 1, 0]), ("B", [0, 0, 1]),
    ("C", [0, 1, 1]), ("M", [1, 0, 1]), ("Y", [1, 1, 0]),
]

SWATCHES = [
    ("Red", (255, 0, 0)), ("Green", (0, 255, 0)), ("Blue", (0, 0, 255)),
    ("Cyan", (0, 255, 255)), ("Magenta", (255, 0, 255)), ("Yellow", (255, 255, 0)),
    ("Orange", (255, 128, 0)), ("Violet", (128, 0, 255)), ("Sky", (128, 192, 255)),
    ("Skin", (240, 200, 170)), ("Olive", (128, 128, 0)), ("Navy", (0, 0, 128)),
    ("Maroon", (128, 0, 0)), ("Forest", (0, 100, 0)), ("Slate", (112, 128, 144)),
    ("Grey 25", (191, 191, 191)), ("Grey 50", (128, 128, 128)),
    ("Grey 75", (64, 64, 64)), ("Grey 90", (26, 26, 26)), ("Black", (0, 0, 0)),
]


def swatch_grid(c, x, y, w, cols=5, cell_h=44.0, gap=6.0):
    cw = (w - gap * (cols - 1)) / cols
    row_y = y
    for i, (name, rgb) in enumerate(SWATCHES):
        col = i % cols
        if col == 0 and i:
            row_y -= cell_h + 20
        cx = x + col * (cw + gap)
        c.rect_fill(cx, row_y - cell_h, cw, cell_h,
                    ("rgb", rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0))
        c.rect_stroke(cx, row_y - cell_h, cw, cell_h, 0.4, 0.0)
        c.text(cx, row_y - cell_h - 8, name, 7.0, "F2")
        c.text(cx, row_y - cell_h - 15, "%d/%d/%d" % rgb, 6.0, "F3", 0.3)
    return row_y - cell_h - 15


def detail_block(c, x, y, w):
    """Line widths, small type and a converging wedge: what the screen keeps."""
    # 1 device pixel at 600 dpi is 72/600 = 0.12 pt; at 1200 it is 0.06.
    widths = [0.06, 0.12, 0.24, 0.36, 0.50, 0.75, 1.00]
    caption(c, x, y, "Rules, in points. 0.12 pt is one 600 dpi pixel, "
                     "0.06 pt one 1200 dpi pixel: at Normal quality the "
                     "first column cannot resolve and should thin or break.")
    ly = y - 14
    col = w / len(widths)
    for i, lw in enumerate(widths):
        lx = x + i * col
        c.line(lx, ly, lx + col - 22, ly, lw, 0.0)        # horizontal
        c.line(lx, ly - 6, lx, ly - 44, lw, 0.0)          # vertical
        c.line(lx + 10, ly - 6, lx + 34, ly - 44, lw, 0.0)  # diagonal
        c.text(lx, ly - 53, "%.2f" % lw, 6.5, "F3", 0.2)
    # Second row: the type ladder on the left, converging rules on the right.
    row = ly - 68
    caption(c, x, row, "Type ladder")
    ty = row - 12
    for size in (4, 5, 6, 7, 8, 10):
        c.text(x, ty, "%dpt  Handgloves 0123456789 mnop" % size, float(size))
        ty -= size + 4.0

    wedge_w = 190.0
    wx = A4_W - MARGIN - wedge_w
    caption(c, wx, row, "Converging rules \u2014 mark where they merge")
    wy = row - 14
    for i in range(18):
        c.line(wx, wy - i * 3.2, wx + wedge_w, wy - i * 0.5, 0.12, 0.0)
    c.line(wx, wy + 4, wx, wy - 58, 0.4, 0.5)
    for pct in range(0, 5):
        px = wx + wedge_w * pct / 4.0
        c.line(px, wy - 62, px, wy - 66, 0.5, 0.0)
        c.text_centre(px, wy - 74, "%d" % (25 * pct), 6.0)
    return min(ty, wy - 78)


def report_form(c, x, y, w):
    prompts = [
        "First wedge patch clearly darker than bare paper (block 1)",
        "First wedge patch that matches the 100 % patch (block 1)",
        "Patch that reads as a true mid-grey, halfway paper to solid",
        "First visible patch on the highlight wedge (block 3)",
        "First patch matching 100 % on the shadow wedge (block 4)",
        "Percentage where each block 5 ramp stops changing (K/R/G/B/C/M/Y)",
    ]
    for prompt in prompts:
        c.text(x, y, "• " + prompt, 8.5)
        c.line(x + w - 92, y - 1.5, x + w, y - 1.5, 0.5, 0.45)
        y -= 17
    y -= 2
    checkbox_row(c, x, y, "Overall:", ["too light", "about right", "too dark"])
    y -= 17
    checkbox_row(c, x, y, "Solids:", ["grey or patchy", "even and solid"])
    y -= 17
    checkbox_row(c, x, y, "Texture:", ["too coarse", "about right", "too fine"])
    y -= 22
    c.text(x, y, "Notes", 8.5, "F2")
    for i in range(4):
        c.line(x + 34, y - 1.5 - i * 14, x + w, y - 1.5 - i * 14, 0.5, 0.45)
    return y - 60


# --------------------------------------------------------------------------


def build(path):
    pdf = Pdf()
    version = driver_version()
    consts = model_constants()

    fonts = {
        "F1": pdf.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                      "/Encoding /WinAnsiEncoding >>"),
        "F2": pdf.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold "
                      "/Encoding /WinAnsiEncoding >>"),
        "F3": pdf.add("<< /Type /Font /Subtype /Type1 /BaseFont /Courier "
                      "/Encoding /WinAnsiEncoding >>"),
    }

    pages_obj = pdf.reserve()
    page_objs = []

    # ---- page 1 ----------------------------------------------------------
    c = Canvas(pdf)
    x = MARGIN
    w = A4_W - 2 * MARGIN
    y = A4_H - MARGIN - 8

    c.text(x, y, "Sister HL-2030 — halftone calibration", 15, "F2")
    y -= 13
    stamp = "driver %s" % version
    if consts:
        # Both screens' constants, since the sheet does not know which build
        # printed it -- that is what the Style box below is for.
        stamp += "   pencil %s um / density %s   newspaper %s um / density %s" % (
            consts.get("kAtkinsonSuppressionUm", "?"),
            consts.get("kAtkinsonDensity", "?"),
            consts.get("kAm45SuppressionUm", "?"),
            consts.get("kAm45Density", "?"))
    c.text(x, y, stamp, 7.0, "F3", 0.35)
    y -= 10
    c.text(x, y, "Print at 100 % scale with scale-to-fit OFF. Page 1 of 2.",
           8.0, "F1", 0.25)
    y -= 18

    nx = checkbox_row(c, x, y, "Quality:", ["Draft", "Normal", "High"])
    checkbox_row(c, nx + 10, y, "Style:", ["Pencil", "Newspaper"])
    y -= 17
    rx = rule_line(c, x, y, 110, "Date")
    rule_line(c, rx + 20, y, 150, "Paper / toner")
    y -= 24

    coarse = list(range(0, 101, 5))
    heading(c, x, y, "1", "Grey step wedge, DeviceGray  —  0 to 100 % in 5 % steps")
    y -= 13
    caption(c, x, y, "The reference wedge. Numbers below each patch are the "
                     "requested coverage.")
    y -= 8
    y = step_wedge(c, x, y - 36, w, 36, coarse, "gray")
    y -= 16

    heading(c, x, y, "2", "The same wedge as neutral DeviceRGB")
    y -= 13
    caption(c, x, y, "Should match block 1 exactly. A difference means the "
                     "colour path, not the halftone.")
    y -= 8
    y = step_wedge(c, x, y - 26, w, 26, coarse, "rgb")
    y -= 16

    heading(c, x, y, "3", "Highlight wedge  —  0 to 20 % in 1 % steps")
    y -= 13
    caption(c, x, y, "Where the first dots survive. This is the most "
                     "sensitive test of the erosion constant.")
    y -= 8
    y = step_wedge(c, x, y - 28, w, 28, list(range(0, 21)), "gray")
    y -= 16

    heading(c, x, y, "4", "Shadow wedge  —  80 to 100 % in 1 % steps")
    y -= 13
    caption(c, x, y, "Where the last white holes close up.")
    y -= 8
    y = step_wedge(c, x, y - 28, w, 28, list(range(80, 101)), "gray")
    y -= 20

    heading(c, x, y, "5", "Continuous ramps with a printed scale")
    y -= 13
    caption(c, x, y, "White index lines mark every 10 %; ticks below mark "
                     "every 5 %. A mono printer renders the colour ramps by "
                     "luma, so each reaches a different final grey.")
    y -= 12
    y = scaled_gradient(c, x + 14, y - 30, w - 14, 30,
                        [1.0], [0.0], "DeviceGray", "K")
    y -= 11
    for name, c1 in RAMPS:
        y = scaled_gradient(c, x + 14, y - 22, w - 14, 22,
                            [1, 1, 1], c1, "DeviceRGB", name)
        y -= 11
    page_objs.append(finish_page(pdf, pages_obj, c, fonts))

    # ---- page 2 ----------------------------------------------------------
    c = Canvas(pdf)
    y = A4_H - MARGIN - 8
    c.text(x, y, "Sister HL-2030 — halftone calibration", 15, "F2")
    y -= 13
    c.text(x, y, "Page 2 of 2. Same print run as page 1.", 8.0, "F1", 0.25)
    y -= 20

    heading(c, x, y, "6", "Colour swatches")
    y -= 13
    caption(c, x, y, "macOS reduces colour to grey by luminance before the "
                     "driver sees it, so these are a record of what the "
                     "printer does, not something the driver can adjust.")
    y -= 9
    caption(c, x, y, "Report any pair that comes out as the same grey.")
    y -= 8
    y = swatch_grid(c, x, y, w)
    y -= 22

    heading(c, x, y, "7", "Fine detail")
    y -= 13
    y = detail_block(c, x, y, w)
    y -= 20

    heading(c, x, y, "8", "What did you see?")
    y -= 16
    report_form(c, x, y, w)
    page_objs.append(finish_page(pdf, pages_obj, c, fonts))

    pdf.set(pages_obj,
            "<< /Type /Pages /Count %d /Kids [%s] >>"
            % (len(page_objs), " ".join("%d 0 R" % n for n in page_objs)))
    pdf.catalog = pdf.add("<< /Type /Catalog /Pages %d 0 R >>" % pages_obj)
    pdf.write(path)


def finish_page(pdf, parent, canvas, fonts):
    content = pdf.add(pdf.stream("", canvas.data()))
    font_res = " ".join("/%s %d 0 R" % (k, v) for k, v in sorted(fonts.items()))
    res = "<< /Font << %s >>" % font_res
    if canvas.shadings:
        res += " /Shading << %s >>" % " ".join(
            "/%s %d 0 R" % (k, v) for k, v in sorted(canvas.shadings.items()))
    res += " >>"
    return pdf.add(
        "<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.3f %.3f] "
        "/Resources %s /Contents %d 0 R >>" % (parent, A4_W, A4_H, res, content))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--output",
                    default=os.path.join(ROOT, "test_fixtures", "calibration.pdf"))
    args = ap.parse_args()
    build(args.output)
    print("wrote %s (%d bytes)" % (args.output, os.path.getsize(args.output)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
