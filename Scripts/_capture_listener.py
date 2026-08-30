#!/usr/bin/env python3
"""Stand in for the HL-2030 and keep every connection as its own file.

Unlike _socket_sink.py (one job, one file) this is meant to sit in front of a
*live* queue for a while, where the driver's ~1 s PJL status polls open and
close their own connections between jobs. Writing them all to one path would
let a 179-byte status poll overwrite the print job, so each connection gets a
numbered file tagged by what it turned out to hold.

It answers PJL status too, so status_cb stays happy and the queue does not
go into an error state while the capture is running.

usage: _capture_listener.py PORT OUTDIR
"""

import os
import socket
import sys
import time

# Matches _fake_printer.py's healthy-printer answer; see docs/protocol.md.
REPLY = (
    b"\x1b%-12345X@PJL INFO STATUS\r\nCODE=10001\r\n"
    b'DISPLAY="READY           "\r\nONLINE=TRUE\r\n\x0c'
    b"@PJL INFO PAGECOUNT\r\nPAGECOUNT=3616\r\n\x0c"
    b"@PJL INFO DRUMLIFE\r\nDRUMLIFE=3616\r\n\x0c"
    b"@PJL ECHO SisterHL2030\r\n\x0c"
)

# What tells a print job apart from a status poll: mode 1030 is only entered
# by a page (encoder/job.cc, write_page_header).
RASTER_MARK = b"\x1b*b1030m"


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: _capture_listener.py PORT OUTDIR")
    port, outdir = int(sys.argv[1]), sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(5)
    print("listening on %d, writing to %s" % (port, outdir), flush=True)

    seq = 0
    while True:
        conn, _ = srv.accept()
        buf = bytearray()
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                buf.extend(chunk)
                # Answer status queries, but never mid-page: once the raster
                # starts, the driver is writing and is not going to read.
                if b"@PJL" in chunk and RASTER_MARK not in buf:
                    conn.sendall(REPLY)
        except OSError:
            pass
        finally:
            conn.close()

        if not buf:
            continue
        seq += 1
        kind = "job" if RASTER_MARK in buf else "status"
        path = os.path.join(outdir, "%03d-%s.prn" % (seq, kind))
        with open(path, "wb") as f:
            f.write(bytes(buf))
        print("%s  %-6s  %9d bytes  %s"
              % (time.strftime("%H:%M:%S"), kind, len(buf),
                 os.path.basename(path)), flush=True)


if __name__ == "__main__":
    main()
