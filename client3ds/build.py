"""Build the 3DS audio client with the toolchain set up for the Melee port.

That shim is clang + lld + the pinned libctru, with host tools named .exe so
the Melee scripts find them on macOS. Nothing here needs devkitARM's gcc.

    python3 client3ds/build.py
"""
import argparse
import subprocess
import time
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEFAULT_TOOLCHAIN = Path.home() / "melee-3ds/native-3ds/.toolchain"
APP = "audio3ds"


def run(args):
    subprocess.run([str(a) for a in args], check=True)


def layout(sdk: Path) -> str:
    """libctru's 3dsx linker script, adjusted for lld.

    GNU ld infers the TLS layout; lld needs an explicit PT_TLS segment.
    3dsxtool only consumes the three PT_LOAD segments either way.
    """
    text = (sdk / "devkitARM/arm-none-eabi/lib/3dsx.ld").read_text()
    text = text.replace("data   PT_LOAD FLAGS(6)",
                        "tls    PT_TLS FLAGS(4);\n\tdata   PT_LOAD FLAGS(6)")
    for start, end in (("\t.tdata :", "\t.tbss :"), ("\t.tbss :", "\t/*")):
        a = text.index(start)
        b = text.index(end, a + len(start))
        text = text[:a] + text[a:b].replace(": data", ": data : tls") + text[b:]
    text = text.replace(".bss ALIGN(4)", ".bss ALIGN(8)")
    # lld otherwise orphans .got past __bss_start__, where the CRT clears it.
    return text.replace("*(.data.*)", "*(.data.*)\n\t\t*(.got .got.*)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--toolchain", type=Path, default=DEFAULT_TOOLCHAIN)
    ap.add_argument("--debug", action="store_true", help="-O0 with assertions")
    args = ap.parse_args()

    tc = args.toolchain
    clangs = sorted(tc.glob("llvm-mingw-*/bin/clang.exe"))
    if not clangs:
        raise SystemExit(f"No toolchain at {tc}; run the Melee bootstrap_macos.py first")
    cc = clangs[-1]
    lld = cc.with_name("ld.lld.exe")
    packer = tc / "bin/3dsxtool.exe"
    sdk = tc / "devkitpro"
    arm = sdk / "devkitARM"
    for needed in (lld, packer, sdk / "libctru/include/3ds.h"):
        if not needed.exists():
            raise SystemExit(f"Missing {needed}")

    out = ROOT / "build"
    out.mkdir(exist_ok=True)
    arch = ["--no-default-config", "--target=arm-none-eabi", "-mcpu=mpcore",
            "-mfpu=vfp", "-mfloat-abi=hard", "-mtp=soft"]
    stamp = time.strftime("%H:%M:%S")
    flags = ["-std=gnu11", "-Wall", "-Wextra", "-D__3DS__",
             f'-DBUILD_STAMP="{stamp}"',
             "-O0" if args.debug else "-O2", "-g",
             "-ffunction-sections", "-fdata-sections", "-fshort-enums"]
    includes = ["-I" + str(sdk / "libctru/include"),
                "-isystem", str(arm / "arm-none-eabi/include")]

    objects = []
    for src in sorted((ROOT / "source").glob("*.c")):
        obj = out / (src.stem + ".o")
        run([cc, *arch, *flags, *includes, "-c", src, "-o", obj])
        objects.append(obj)
    if not objects:
        raise SystemExit("No sources under client3ds/source")

    libs = arm / "arm-none-eabi/lib/armv6k/fpu"
    gcc = sorted((arm / "lib/gcc/arm-none-eabi").iterdir())[-1] / "armv6k/fpu"
    script = out / "3dsx.ld"
    script.write_text(layout(sdk))

    elf = out / f"{APP}.elf"
    run([lld, "-T", script, "--gc-sections", "-Map=" + str(out / f"{APP}.map"),
         libs / "3dsx_crt0.o", gcc / "crti.o", gcc / "crtbegin.o", *objects,
         "-L" + str(sdk / "libctru/lib"), "-L" + str(libs), "-L" + str(gcc),
         "--start-group", "-lcitro2d", "-lcitro3d", "-lctru", "-lm", "-lc",
         "-lsysbase", "-lgcc", "--end-group",
         gcc / "crtend.o", gcc / "crtn.o", "-o", elf])

    # Homebrew Launcher lists SD:/3ds/<name>/<name>.3dsx, so the folder and the
    # executable must share a name.
    dest = ROOT / f"dist/3ds/{APP}/{APP}.3dsx"
    dest.parent.mkdir(parents=True, exist_ok=True)
    run([packer, elf, dest])
    print(f"\nBuilt {dest}  ({dest.stat().st_size:,} bytes)")
    print(f"Copy dist/3ds/ to the SD root, then edit "
          f"SD:/3ds/{APP}/server.txt with your bridge's host:port")


if __name__ == "__main__":
    main()
