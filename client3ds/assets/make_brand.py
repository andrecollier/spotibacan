#!/usr/bin/env python3
"""SpotiBacan brand assets: 3DS icon (48x48) + banner (256x128), drawn big
and downscaled so the edges stay clean. The symbol is the in-app
visualizer's four bars, in Spotify green on the app's dark background."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).parent
GREEN = (0x1D, 0xB9, 0x54, 0xFF)
DARK = (0x12, 0x12, 0x12, 0xFF)
WHITE = (0xF4, 0xF4, 0xF4, 0xFF)


def draw_bars(draw, cx, cy, max_h, bar_w, gap, color):
    """Four visualizer bars centred on (cx, cy); heights like the app's
    now-playing badge mid-animation."""
    heights = [0.38, 0.72, 0.52, 0.92]
    total_w = 4 * bar_w + 3 * gap
    x = cx - total_w / 2
    for h in heights:
        bh = max_h * h
        draw.rounded_rectangle(
            [x, cy - bh / 2, x + bar_w, cy + bh / 2],
            radius=bar_w / 2, fill=color)
        x += bar_w + gap


def make_icon():
    S = 512
    img = Image.new("RGBA", (S, S), DARK)
    d = ImageDraw.Draw(img)
    draw_bars(d, S / 2, S / 2, max_h=S * 0.52, bar_w=S * 0.115,
              gap=S * 0.075, color=GREEN)
    img.resize((48, 48), Image.LANCZOS).save(OUT / "icon.png")
    img.save(OUT / "icon-512.png")  # large copy for the channel / previews


def find_font(size):
    candidates = [
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial Bold.ttf",
    ]
    for c in candidates:
        if Path(c).exists():
            return ImageFont.truetype(c, size)
    return ImageFont.load_default()


def make_banner():
    W, H = 1024, 512  # 4x the 256x128 the HOME menu banner wants
    img = Image.new("RGBA", (W, H), DARK)
    d = ImageDraw.Draw(img)

    # Symbol tile on the left, same proportions as the icon.
    tile = 340
    tx, ty = 64, (H - tile) // 2
    d.rounded_rectangle([tx, ty, tx + tile, ty + tile], radius=48,
                        fill=(0x1B, 0x1B, 0x1B, 0xFF))
    draw_bars(d, tx + tile / 2, ty + tile / 2, max_h=tile * 0.52,
              bar_w=tile * 0.115, gap=tile * 0.075, color=GREEN)

    text_x = tx + tile + 60
    max_w = W - text_x - 64

    # Shrink until the name fits the space left of the tile.
    size = 150
    font = find_font(size)
    while d.textlength("SpotiBacan", font=font) > max_w and size > 40:
        size -= 6
        font = find_font(size)
    sub = find_font(int(size * 0.38))
    d.text((text_x, H / 2 - int(size * 0.85)), "SpotiBacan", font=font,
           fill=WHITE)
    d.text((text_x + 6, H / 2 + int(size * 0.45)),
           "Spotify para Nintendo 3DS", font=sub, fill=GREEN)

    img.resize((256, 128), Image.LANCZOS).save(OUT / "banner.png")
    img.save(OUT / "banner-1024.png")


make_icon()
make_banner()
print("icon.png (48x48), banner.png (256x128), + large copies in", OUT)
