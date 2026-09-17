"""Turn Rockbox theme colour settings into themes for this player.

Reads the colour values a .cfg declares — configuration data, not artwork —
and carries each theme's author across as a credit. The authors' bitmaps,
fonts and layouts are left where they are; only the palette travels.

    python3 tools/import_rockbox_themes.py "~/Downloads/.../New Folder With Items 2"
"""
import argparse
import re
import struct
import shutil
from pathlib import Path

DIST = Path(__file__).resolve().parents[1] / "client3ds/dist/3ds/audio3ds"
OUT = DIST / "themes"
BACKDROPS = DIST / "backdrops"
# "Author: X" is explicit; a bare "by X" is a guess, so it comes second and
# gets cut at the first sign it has run into the rest of the sentence.
BACKDROP_TAG = re.compile(r"%X\(([^)]*)\)")
AUTHOR_EXPLICIT = re.compile(r"author\s*:?\s+(.+)", re.I)
AUTHOR_LOOSE = re.compile(r"(?:^|[\s_#*-])by\s+(.+)", re.I)
STOP = re.compile(r"\s*(?:[(<,.;]|\bbased\b|\bwhich\b|\band spinoffs\b|\d{4})")


def flat(path):
    """True when an image is a near-solid fill: those add nothing as a backdrop
    and would otherwise win on filename alone."""
    try:
        data = path.read_bytes()
        start = struct.unpack("<I", data[10:14])[0]
        pixels = data[start:]
    except Exception:
        return False
    step = max(3, (len(pixels) // 4000) * 3)
    return len({pixels[i:i+3] for i in range(0, len(pixels) - 3, step)}) < 8


def rgb(text, fallback):
    text = (text or "").strip().lstrip("#")
    return text.lower() if re.fullmatch(r"[0-9a-fA-F]{6}", text) else fallback


def mix(a, b, t):
    """Blend two RRGGBB strings."""
    pa, pb = [int(a[i:i+2], 16) for i in (0, 2, 4)], [int(b[i:i+2], 16) for i in (0, 2, 4)]
    return "".join(f"{int(x + (y - x) * t):02x}" for x, y in zip(pa, pb))


def luma(c):
    r, g, b = (int(c[i:i+2], 16) for i in (0, 2, 4))
    return (r * 299 + g * 587 + b * 114) / 255000


def clean_author(raw):
    """Keep the name, drop the sentence it was embedded in."""
    name = STOP.split(raw.strip(), 1)[0]
    name = name.strip(" _-:#\"'")
    if not name or len(name) < 2 or len(name) > 26:
        return ""
    if name.lower().startswith(("me", "http", "the ")) or name[0].isdigit():
        return ""
    return name


def find_author(cfg: Path):
    sources = [*cfg.parent.parent.glob(f"wps/{cfg.stem}.wps"), cfg,
               *cfg.parent.parent.glob(f"wps/{cfg.stem}.*")]
    for pattern in (AUTHOR_EXPLICIT, AUTHOR_LOOSE):
        for path in sources:
            try:
                text = path.read_text(errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                if not line.lstrip().startswith("#"):
                    continue
                m = pattern.search(line)
                if m:
                    name = clean_author(m.group(1))
                    if name:
                        return name
    return ""


def convert(cfg: Path):
    settings = {}
    for line in cfg.read_text(errors="replace").splitlines():
        if line.startswith("#") or ":" not in line:
            continue
        key, _, val = line.partition(":")
        settings[key.strip().lower()] = val.strip()

    bg = rgb(settings.get("background color"), "101010")
    fg = rgb(settings.get("foreground color"), "f0f0f0")
    accent = rgb(settings.get("line selector start color"), "")
    if not accent or accent == bg:
        accent = fg if abs(luma(fg) - luma(bg)) > 0.3 else mix(fg, bg, 0.4)
    on_accent = rgb(settings.get("line selector text color"), bg)
    dark = luma(bg) < 0.5

    # Ask the theme instead of guessing: a .wps/.sbs declares its full-screen
    # backdrop with %X(file). Picking by size or filename kept choosing USB
    # splash screens and solid fills.
    backdrop = ""
    root = cfg.parent.parent
    wanted = []
    for key in ("sbs", "wps"):
        ref = settings.get(key, "")
        if not ref:
            continue
        skin = root / ref.lstrip("/").replace(".rockbox/", "")
        if not skin.is_file():
            continue
        for m in BACKDROP_TAG.finditer(skin.read_text(errors="replace")):
            name = m.group(1).strip()
            if name and name.lower() != "d":      # %X(d) disables the backdrop
                wanted.append((skin.parent, name))
    declared = settings.get("backdrop", "")
    if declared:
        wanted.append((root, declared.lstrip("/").replace(".rockbox/", "")))

    for base, name in wanted:
        for src in (base / name, base / cfg.stem / name, root / "backdrops" / name):
            if src.is_file() and not flat(src):
                BACKDROPS.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, BACKDROPS / src.name)
                backdrop = src.name
                break
        if backdrop:
            break

    return {
        "name": cfg.stem[:22],
        "author": find_author(cfg),
        "top bg": bg,
        "bottom bg": mix(bg, "000000" if dark else "ffffff", 0.25),
        "panel": mix(bg, fg, 0.14),
        "accent": accent,
        "on accent": on_accent,
        "text": fg,
        "dim": mix(fg, bg, 0.45),
        "meter": mix(bg, fg, 0.28),
        "layout": "art" if dark else "list",
        "backdrop": backdrop,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", type=Path, help="Folder holding the .rockbox* directories")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    found = sorted(Path(args.source).expanduser().glob(".rockbox*/themes/*.cfg"))
    if not found:
        raise SystemExit(f"No themes under {args.source}")
    for cfg in found:
        t = convert(cfg)
        lines = [f"# Palette imported from the Rockbox theme \"{cfg.stem}\".",
                 "# Bitmaps, fonts and layouts stay with their author; only colours are here.",
                 ""]
        lines += [f"{k}: {v}" for k, v in t.items() if v]
        (OUT / f"{t['name']}.cfg").write_text("\n".join(lines) + "\n")
        credit = "by " + t["author"] if t["author"] else "(author not stated)"
        print(f"  {t['name']:22} {credit:32}{'  + backdrop' if t['backdrop'] else ''}")
    print(f"\n{len(found)} themes written to {OUT}")


if __name__ == "__main__":
    main()
