#!/usr/bin/env python3
"""SpotiBacan brand assets: adapts the source artwork to the console sizes.

brand-icon-src.png   -> icon.png (48x48, HOME menu tile via icon.smdh)
brand-banner-src.png -> banner.png (256x128, keeps its transparency: the
                        banner has no background, so it must not be
                        flattened onto one)"""
from pathlib import Path

from PIL import Image

OUT = Path(__file__).parent

icon = Image.open(OUT / "brand-icon-src.png").convert("RGBA")
icon.resize((48, 48), Image.LANCZOS).save(OUT / "icon.png")
icon.save(OUT / "icon-512.png")

banner = Image.open(OUT / "brand-banner-src.png").convert("RGBA")
banner.resize((256, 128), Image.LANCZOS).save(OUT / "banner.png")
banner.save(OUT / "banner-1024.png")

print("icon.png (48x48) + banner.png (256x128, transparent) regenerated")
