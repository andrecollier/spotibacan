#!/usr/bin/env python3
"""Serve SpotiBacan.cia over the LAN and print the QR FBI should scan.

    python3 client3ds/serve_cia.py

Both devices must share the same WiFi. FBI -> Remote Install -> Scan QR
Code points at the printed URL; Ctrl-C here once it installs.
"""
import socket
import subprocess
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DIST = ROOT / "dist"
PORT = 8077


def lan_ip() -> str:
    # A UDP "connection" costs nothing and reveals which interface faces the
    # LAN; fall back to parsing ipconfig if the probe has no route.
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        out = subprocess.run(["ipconfig", "getifaddr", "en0"],
                             capture_output=True, text=True).stdout.strip()
        return out or "127.0.0.1"


def main():
    cia = DIST / "SpotiBacan.cia"
    if not cia.exists():
        sys.exit("No SpotiBacan.cia yet; run: python3 client3ds/build.py")

    url = f"http://{lan_ip()}:{PORT}/{cia.name}"
    qr_png = DIST / "SpotiBacan-qr.png"
    import segno
    segno.make(url, error="m").save(qr_png, scale=8, border=2)

    print(f"\n  Serving {cia.name} ({cia.stat().st_size:,} bytes)")
    print(f"  URL: {url}")
    print(f"  QR:  {qr_png}")
    print("\n  FBI -> Remote Install -> Scan QR Code. Ctrl-C to stop.\n")

    import functools
    handler = functools.partial(SimpleHTTPRequestHandler, directory=str(DIST))
    ThreadingHTTPServer(("0.0.0.0", PORT), handler).serve_forever()


if __name__ == "__main__":
    main()
