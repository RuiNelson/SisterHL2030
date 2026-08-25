#!/usr/bin/env python3
"""Exercise the PAPPL path against a socket sink: quality modes and Atkinson.

No hardware. Starts a throwaway sister-printer-app, submits test_fixtures/flag.png
at draft/normal/high, and checks the PJL envelope plus that the page was
dithered (mixed bytes). Exit non-zero on failure.
"""

import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "build" / "sister-printer-app"
DECODE = ROOT / "Scripts" / "decode_job.py"
FIXTURE = ROOT / "test_fixtures" / "flag.png"

# A small PNG with print-scaling=none only blackens the image box, and
# mode 1030 omits trailing white, so decoded width is the content, not A4.
# Draft must still be half of Normal (300 vs 600 dpi).


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def sink_server(port, outfile, stop):
    """Accept connections until `stop` is set; keep the last mode-1030 job."""
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(5)
    srv.settimeout(0.3)
    while not stop.is_set():
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        conn.settimeout(2.0)
        buf = bytearray()
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                buf.extend(chunk)
                if b"@PJL" in chunk:
                    conn.sendall(
                        b"\x1b%-12345X@PJL INFO STATUS\r\nCODE=10001\r\n"
                        b'DISPLAY="READY           "\r\nONLINE=TRUE\r\n\x0c'
                        b"@PJL INFO PAGECOUNT\r\nPAGECOUNT=100\r\n\x0c"
                        b"@PJL INFO DRUMLIFE\r\nDRUMLIFE=100\r\n\x0c"
                        b"@PJL ECHO SisterHL2030\r\n\x0c"
                    )
        except OSError:
            pass
        finally:
            conn.close()
        if b"\x1b*b1030m" in buf:
            Path(outfile).write_bytes(bytes(buf))
    srv.close()


def wait_port(port, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), 0.3):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def decode_stats(prn):
    out = subprocess.check_output(
        [sys.executable, str(DECODE), str(prn)], text=True
    )
    stats = {}
    for line in out.splitlines():
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        stats[k.strip()] = v.strip()
    mixed = float(stats.get("mixed bytes", "0").split("%")[0])
    width = int(stats.get("bytes/line", "0  (width 0").split("width")[-1].split()[0])
    rows = int(stats.get("rows", "0"))
    return out, mixed, width, rows


def decode_bitmap(prn, pbm):
    """Decode a job back to its rows of bits, as lists of 0/1 per pixel."""
    subprocess.check_output(
        [sys.executable, str(DECODE), str(prn), str(pbm)], text=True
    )
    data = pbm.read_bytes()
    # P4 header: magic, width, height, then packed rows.
    fields = []
    pos = 2
    while len(fields) < 2:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while data[pos : pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    width, height = fields
    stride = (width + 7) // 8
    return width, height, data[pos:], stride


def check_hq1200_grid(job, work):
    """HQ1200 must be dithered at 1200x600, not at 600 and pixel-doubled.

    Two things follow from that and from nothing else: every row appears
    exactly twice (the vertical half is replication the bitmap format
    demands), and adjacent column pairs are NOT all identical (the
    horizontal half is real 1200 dpi halftone detail, which is precisely
    what pixel-doubling a 600 dpi dither could never produce).
    """
    failures = 0
    width, height, bits, stride = decode_bitmap(job, work / "high.pbm")
    rows = [bits[i * stride : (i + 1) * stride] for i in range(height)]
    band = range(height // 3, height // 3 + 200, 2)
    if any(rows[y] != rows[y + 1] for y in band):
        print("FAIL: high: rows are not emitted in identical pairs; the "
              "dither did not run on the 1200x600 grid", file=sys.stderr)
        failures += 1
    if all(rows[y] == rows[y + 2] for y in band):
        print("FAIL: high: every other row is identical too, so the page "
              "carries no vertical detail at all", file=sys.stderr)
        failures += 1
    # Column pairs: pixel-doubling a 600 dpi dither makes bit 2k == bit 2k+1
    # for every k. Real 1200x600 halftoning breaks that on most rows.
    split_pairs = 0
    for y in band:
        row = rows[y]
        for byte in row[stride // 4 : stride // 2]:
            for shift in (6, 4, 2, 0):
                pair = (byte >> shift) & 0b11
                if pair in (0b01, 0b10):
                    split_pairs += 1
    if split_pairs == 0:
        print("FAIL: high: no split column pair anywhere; the 1-bit page was "
              "pixel-doubled from 600 dpi rather than halftoned at 1200",
              file=sys.stderr)
        failures += 1
    else:
        print(f"ok high: {split_pairs} split column pairs (real 1200 dpi "
              f"halftone detail), rows in exact pairs")
    return failures


def main():
    failures = 0
    if not APP.is_file():
        print("FAIL: missing", APP, file=sys.stderr)
        return 1
    if not FIXTURE.is_file():
        print("FAIL: missing", FIXTURE, file=sys.stderr)
        return 1

    work = Path(tempfile.mkdtemp(prefix="sister-pappl-test-"))
    sink_port = free_port()
    ipp_port = free_port()
    stop_sink = threading.Event()
    procs = []

    def stop():
        stop_sink.set()
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()

    try:
        threading.Thread(
            target=sink_server,
            args=(sink_port, str(work / "current.prn"), stop_sink),
            daemon=True,
        ).start()
        if not wait_port(sink_port):
            print("FAIL: sink did not listen", file=sys.stderr)
            return 1

        env = os.environ.copy()
        env["HOME"] = str(work)
        env["XDG_CONFIG_HOME"] = str(work)
        server = subprocess.Popen(
            [
                str(APP),
                "server",
                "-o",
                f"server-port={ipp_port}",
                "-o",
                f"spool-directory={work / 'spool'}",
                "-o",
                f"log-file={work / 'server.log'}",
                "-o",
                "log-level=info",
            ],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        procs.append(server)
        if not wait_port(ipp_port):
            print("FAIL: PAPPL server did not listen", file=sys.stderr)
            print((work / "server.log").read_text()[-2000:]
                  if (work / "server.log").is_file() else "")
            return 1

        subprocess.check_call(
            [
                str(APP),
                "add",
                "-u",
                f"ipp://127.0.0.1:{ipp_port}/",
                "-d",
                "preview",
                "-m",
                "sister-hl2030",
                "-v",
                f"socket://127.0.0.1:{sink_port}",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        cases = (
            ("draft", "3", "RESOLUTION = 300", "ECONOMODE = ON", False),
            ("normal", "4", "RESOLUTION = 600", "ECONOMODE = ON", False),
            ("high", "5", "RAS1200MODE = TRUE", "ECONOMODE = OFF", True),
        )
        current = work / "current.prn"
        widths = {}
        for name, pq, pjl_a, pjl_b, want_ras in cases:
            if current.exists():
                current.unlink()
            case_fail = 0

            subprocess.check_call(
                [
                    str(APP),
                    "submit",
                    "-u",
                    f"ipp://127.0.0.1:{ipp_port}/",
                    "-d",
                    "preview",
                    "-o",
                    f"print-quality={pq}",
                    str(FIXTURE),
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            deadline = time.time() + 40
            while time.time() < deadline:
                if current.is_file() and current.stat().st_size > 200:
                    time.sleep(1.5)
                    break
                time.sleep(0.4)
            data = current.read_bytes() if current.is_file() else b""
            job = work / f"{name}.prn"
            if data:
                job.write_bytes(data)
            if not data:
                print(f"FAIL: {name}: no job captured", file=sys.stderr)
                failures += 1
                continue
            text = data.decode("latin-1", "replace")
            if pjl_a not in text:
                print(f"FAIL: {name}: missing {pjl_a!r}", file=sys.stderr)
                case_fail += 1
            if pjl_b not in text:
                print(f"FAIL: {name}: missing {pjl_b!r}", file=sys.stderr)
                case_fail += 1
            if want_ras and "RESOLUTION = 600" not in text:
                print(f"FAIL: {name}: HQ1200 still needs RESOLUTION 600",
                      file=sys.stderr)
                case_fail += 1
            if name != "high" and "RAS1200MODE = TRUE" in text:
                print(f"FAIL: {name}: RAS1200MODE should be off", file=sys.stderr)
                case_fail += 1
            _, mixed, width, rows = decode_stats(job)
            widths[name] = width
            if mixed < 20.0:
                print(
                    f"FAIL: {name}: mixed bytes {mixed:.1f}% (Atkinson missing?)",
                    file=sys.stderr,
                )
                case_fail += 1
            failures += case_fail
            status = "ok" if case_fail == 0 else "see FAIL"
            print(
                f"{status} {name}: {width}x{rows} mixed={mixed:.1f}% "
                f"{pjl_a}; {pjl_b}"
            )
        if "draft" in widths and "normal" in widths:
            # 300 dpi is half of 600. Allow one pixel of rounding.
            if widths["draft"] * 2 < widths["normal"] - 8:
                print(
                    f"FAIL: draft width {widths['draft']} is not half of "
                    f"normal {widths['normal']}",
                    file=sys.stderr,
                )
                failures += 1
        if "high" in widths and "normal" in widths:
            # HQ1200 pixel grid is twice 600 dpi. A 600 dpi Fine page
            # prints at half size.
            if widths["high"] + 8 < 2 * widths["normal"]:
                print(
                    f"FAIL: high width {widths['high']} is not double "
                    f"normal {widths['normal']} (HQ1200 half-size bug)",
                    file=sys.stderr,
                )
                failures += 1
        high_job = work / "high.prn"
        if high_job.is_file():
            failures += check_hq1200_grid(high_job, work)
    finally:
        stop()

    if failures:
        print(f"{failures} failure(s)", file=sys.stderr)
        log = work / "server.log"
        if log.is_file():
            print("--- server.log ---", file=sys.stderr)
            print(log.read_text()[-3000:], file=sys.stderr)
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
