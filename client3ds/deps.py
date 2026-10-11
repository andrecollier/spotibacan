"""Fetch and build mbedTLS 3.6 LTS for the 3DS.

    python3 client3ds/deps.py

The console's own sslc is not used: we need control over the cipher list and
the trust anchors, and the same library then covers the AES-CTR and bignum work
the Spotify protocol needs later. 3.6 is the LTS branch; 4.x drops the legacy
API and makes PSA mandatory, which TLS 1.2 does not need.

The tarball is a pinned release verified by SHA-256, and it is only ever read:
nothing inside deps/ is executed, and only .c paths are handed to clang.
"""
import argparse
import concurrent.futures
import hashlib
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain

ROOT = Path(__file__).resolve().parent
NAME = "mbedtls-3.6.7"
SHA256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"
URL = f"https://github.com/Mbed-TLS/mbedtls/releases/download/{NAME}/{NAME}.tar.bz2"

# The Vorbis decoder: one public-domain file, pinned the same way. Spotify
# streams Ogg Vorbis, and the ARM11 has to decode it in software -- there is no
# second processor to hand it to, which is the whole reason this port is easier
# than the PSP one.
STB_VORBIS_URL = "https://raw.githubusercontent.com/nothings/stb/master/stb_vorbis.c"
STB_VORBIS_SHA256 = "4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636"


def fetch(deps: Path) -> Path:
    deps.mkdir(parents=True, exist_ok=True)
    tar = deps / f"{NAME}.tar.bz2"
    if not tar.exists():
        print(f"Downloading {NAME}...")
        with urllib.request.urlopen(URL, timeout=120) as r:
            tar.write_bytes(r.read())
    got = hashlib.sha256(tar.read_bytes()).hexdigest()
    if got != SHA256:
        raise SystemExit(f"{tar} hashes {got}, expected {SHA256}")

    tree = deps / NAME
    if not (tree / "library/ssl_tls.c").exists():
        print(f"Extracting {NAME}...")
        with tarfile.open(tar) as t:
            for m in t.getmembers():
                # A release tarball should hold nothing but its own subtree.
                if m.name.startswith(("/", "..")) or ".." in Path(m.name).parts:
                    raise SystemExit(f"Refusing path {m.name!r} in {tar}")
            t.extractall(deps, filter="data")
    return tree


def fetch_stb_vorbis(deps: Path) -> Path:
    deps.mkdir(parents=True, exist_ok=True)
    dst = deps / "stb_vorbis.c"
    if not dst.exists():
        print("Downloading stb_vorbis...")
        with urllib.request.urlopen(STB_VORBIS_URL, timeout=60) as r:
            dst.write_bytes(r.read())
    got = hashlib.sha256(dst.read_bytes()).hexdigest()
    if got != STB_VORBIS_SHA256:
        raise SystemExit(f"{dst} hashes {got}, expected {STB_VORBIS_SHA256}\n"
                         "stb has no releases, so this pins a commit of master; "
                         "if upstream moved, review the diff before updating.")
    return dst


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--toolchain", type=Path, default=toolchain.DEFAULT_TOOLCHAIN)
    ap.add_argument("--probe", action="store_true",
                    help="also compile mbedtls_strerror and the handshake debug hooks")
    ap.add_argument("-j", type=int, default=8)
    args = ap.parse_args()

    tc = toolchain.find(args.toolchain)
    fetch_stb_vorbis(ROOT / "deps")
    tree = fetch(ROOT / "deps")
    out = ROOT / "build" / ("mbedtls-probe" if args.probe else "mbedtls")
    out.mkdir(parents=True, exist_ok=True)

    flags = ["-std=gnu11", "-O2", "-g", "-ffunction-sections", "-fdata-sections",
             "-fshort-enums", "-D__3DS__",
             '-DMBEDTLS_CONFIG_FILE="mbedtls_config_3ds.h"',
             "-I" + str(ROOT / "platform3ds"),
             "-I" + str(tree / "include"),
             "-I" + str(tree / "library"),
             # Warnings in a pinned upstream tree are not ours to fix.
             "-w"]
    if args.probe:
        flags.append("-DPROBE_BUILD")

    sources = sorted((tree / "library").glob("*.c"))
    objects = [out / (s.stem + ".o") for s in sources]

    # Rebuild on a config change too, not just a source change -- the config
    # decides which half of mbedTLS even compiles.
    newest_input = max((ROOT / "platform3ds/mbedtls_config_3ds.h").stat().st_mtime,
                       Path(__file__).stat().st_mtime)

    def compile_one(pair):
        src, obj = pair
        if obj.exists() and obj.stat().st_mtime > max(src.stat().st_mtime,
                                                      newest_input):
            return
        subprocess.run([str(tc.cc), *toolchain.ARCH, *flags, *tc.includes,
                        "-c", str(src), "-o", str(obj)], check=True)

    print(f"Compiling {len(sources)} mbedTLS sources...")
    with concurrent.futures.ThreadPoolExecutor(args.j) as pool:
        for _ in pool.map(compile_one, zip(sources, objects)):
            pass

    lib = ROOT / "build" / ("libmbedtls-probe.a" if args.probe else "libmbedtls.a")
    lib.unlink(missing_ok=True)
    subprocess.run([str(tc.ar), "rcs", str(lib), *[str(o) for o in objects]], check=True)
    print(f"Built {lib}  ({lib.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
