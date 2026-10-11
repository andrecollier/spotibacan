"""Shared discovery of the Melee shim toolchain.

That shim is clang + lld + a pinned libctru, with host tools named .exe so the
Melee scripts find them on macOS. build.py and deps.py both need the same
compiler and the same ARM11 flags, so they live here.
"""
from dataclasses import dataclass
from pathlib import Path

DEFAULT_TOOLCHAIN = Path.home() / "melee-3ds/native-3ds/.toolchain"

# ARM11 (ARMv6K, MPCore) with the VFPv2 the 3DS actually has.
ARCH = ["--no-default-config", "--target=arm-none-eabi", "-mcpu=mpcore",
        "-mfpu=vfp", "-mfloat-abi=hard", "-mtp=soft"]


@dataclass
class Toolchain:
    cc: Path
    lld: Path
    ar: Path
    packer: Path
    sdk: Path
    arm: Path

    @property
    def includes(self):
        return ["-I" + str(self.sdk / "libctru/include"),
                "-isystem", str(self.arm / "arm-none-eabi/include")]


def find(root: Path = DEFAULT_TOOLCHAIN) -> Toolchain:
    clangs = sorted(root.glob("llvm-mingw-*/bin/clang.exe"))
    if not clangs:
        raise SystemExit(f"No toolchain at {root}; run the Melee bootstrap_macos.py first")
    cc = clangs[-1]
    tc = Toolchain(cc=cc, lld=cc.with_name("ld.lld.exe"), ar=cc.with_name("llvm-ar.exe"),
                   packer=root / "bin/3dsxtool.exe", sdk=root / "devkitpro",
                   arm=root / "devkitpro/devkitARM")
    for needed in (tc.lld, tc.ar, tc.packer, tc.sdk / "libctru/include/3ds.h"):
        if not needed.exists():
            raise SystemExit(f"Missing {needed}")
    return tc
