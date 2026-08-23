#!/usr/bin/env python3
"""Accept one raw print job on a TCP port and write it to a file.

Stands in for the printer so a PAPPL job can be captured and looked at
instead of printed. Used by "Preview PAPPL Render.sh".
"""

import socket
import sys


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: _socket_sink.py PORT OUTFILE")
    port, out = int(sys.argv[1]), sys.argv[2]

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)

    conn, _ = srv.accept()
    total = 0
    with open(out, "wb") as f:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            f.write(data)
            f.flush()
            total += len(data)
    conn.close()
    srv.close()
    print("captured %d bytes" % total)


if __name__ == "__main__":
    main()
