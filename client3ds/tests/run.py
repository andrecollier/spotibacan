"""Build and run the host-side tests.

    python3 client3ds/tests/run.py

These use the Mac's own compiler, not the 3DS toolchain: everything tested here
is portable C, and catching a cipher bug on the host is far cheaper than
catching it as "the access point hung up".
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

TESTS = [("shannon", ["test_shannon.c", ROOT / "platform3ds/shannon.c"])]


def main():
    bad = 0
    for name, sources in TESTS:
        print(f"== {name} ==")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / name
            cmd = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                   f"-I{HERE}", f"-I{ROOT / 'platform3ds'}",
                   *[str(s if isinstance(s, Path) else HERE / s) for s in sources],
                   "-o", str(binary)]
            subprocess.run(cmd, check=True)
            bad |= subprocess.run([str(binary)]).returncode
    sys.exit(bad)


if __name__ == "__main__":
    main()
