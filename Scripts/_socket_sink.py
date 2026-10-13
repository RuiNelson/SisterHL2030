#!/usr/bin/env python3
"""Capture the first raster job, accepting preceding PJL status connections.

Stands in for the printer so a PAPPL job can be captured and looked at
instead of printed. Used by "Preview PAPPL Render.sh".
"""

import socket
import sys

from _capture_listener import REPLY, RASTER_MARK


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: _socket_sink.py PORT OUTFILE")
    port, out = int(sys.argv[1]), sys.argv[2]

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(5)
    try:
        while True:
            conn, _ = srv.accept()
            conn.settimeout(10)
            data = bytearray()
            try:
                while True:
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    data.extend(chunk)
                    if b"INFO DRUMLIFE" in data and RASTER_MARK not in data:
                        conn.sendall(REPLY)
            except OSError:
                pass
            finally:
                conn.close()
            if RASTER_MARK in data:
                with open(out, "wb") as f:
                    f.write(data)
                print("captured %d bytes" % len(data))
                return
    finally:
        srv.close()


if __name__ == "__main__":
    main()
