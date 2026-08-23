#!/usr/bin/env python3
"""A stand-in HL-2030 that answers PJL status queries.

Lets the consumables path be exercised with no printer attached: point a
printer at socket://127.0.0.1:PORT and this replies the way the real one
does. Print data is swallowed and optionally saved.

usage: _fake_printer.py PORT [OUTFILE] [--toner=ok|low|empty] [--drumlife=N]
"""

import socket
import sys

CODES = {"ok": 10001, "low": 10006, "empty": 40010}
DISPLAY = {"ok": "READY", "low": "TONER LOW", "empty": "                "}


def reply(toner, drumlife, pagecount):
    code = CODES[toner]
    return (
        "\x1b%%-12345X@PJL INFO STATUS\r\nCODE=%d\r\nDISPLAY=\"%-16s\"\r\n"
        "ONLINE=TRUE\r\n\x0c"
        "@PJL INFO PAGECOUNT\r\nPAGECOUNT=%d\r\n\x0c"
        "@PJL INFO DRUMLIFE\r\nDRUMLIFE=%d\r\n\x0c"
        "@PJL ECHO SisterHL2030\r\n\x0c" % (code, DISPLAY[toner], pagecount, drumlife)
    ).encode()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if not args:
        raise SystemExit(__doc__)

    port = int(args[0])
    out = args[1] if len(args) > 1 else None
    toner = "ok"
    drumlife = 3616
    for f in flags:
        if f.startswith("--toner="):
            toner = f.split("=", 1)[1]
        elif f.startswith("--drumlife="):
            drumlife = int(f.split("=", 1)[1])

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(5)
    print("fake HL-2030 on %d (toner=%s drumlife=%d)" % (port, toner, drumlife),
          flush=True)

    while True:
        conn, _ = srv.accept()
        job = bytearray()
        try:
            while True:
                data = conn.recv(65536)
                if not data:
                    break
                job.extend(data)
                if b"@PJL" in data:
                    conn.sendall(reply(toner, drumlife, 3616))
        except OSError:
            pass
        finally:
            conn.close()
        if out and job:
            with open(out, "wb") as f:
                f.write(bytes(job))
            print("saved %d bytes" % len(job), flush=True)


if __name__ == "__main__":
    main()
