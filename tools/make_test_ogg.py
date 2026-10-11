"""Build Ogg Vorbis test files for the decode probe.

    python3 tools/make_test_ogg.py "/ruta/a/un.mp3"

The decode benchmark needs a real Vorbis stream on the card. Spotify's own
files cannot be used for this -- they are encrypted, and the key is exactly
what is blocked -- so one is encoded from a local track instead.

Note on bitrate: this Mac's ffmpeg has only the native `vorbis` encoder, which
ignores -b:a and encodes by quality. The default lands near 230 kbps, which is
*harder* than the 160 kbps Spotify serves, so a realtime factor measured on it
is a conservative number. -q 2 gives roughly 96 kbps for the easy case.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "testdata"


def encode(src: Path, dst: Path, quality=None, seconds=35):
    cmd = ["ffmpeg", "-v", "error", "-y", "-i", str(src),
           # Cover art is a video stream, and without -vn ffmpeg tries to
           # encode it as theora and fails with a confusing message.
           "-vn", "-t", str(seconds),
           "-c:a", "vorbis", "-strict", "-2",
           "-ar", "44100", "-ac", "2"]
    if quality is not None:
        cmd += ["-q:a", str(quality)]
    cmd.append(str(dst))
    subprocess.run(cmd, check=True)
    print(f"  {dst.name:16} {dst.stat().st_size:9,} bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", type=Path, help="any audio file ffmpeg can read")
    ap.add_argument("--seconds", type=int, default=35)
    args = ap.parse_args()

    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg is not on PATH")
    if not args.source.exists():
        raise SystemExit(f"no such file: {args.source}")

    OUT.mkdir(exist_ok=True)
    print(f"From {args.source.name}:")
    encode(args.source, OUT / "test.ogg", None, args.seconds)
    encode(args.source, OUT / "test-low.ogg", 2, args.seconds)
    print(f"\nCopy {OUT / 'test.ogg'} to SD:/3ds/probe_audio/test.ogg")


if __name__ == "__main__":
    main()
