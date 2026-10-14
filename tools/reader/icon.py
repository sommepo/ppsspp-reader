#!/usr/bin/env python3
"""Makes the app's icon files from one square picture on a plain ground.

    tools/reader/icon.py MASTER.png OUT_DIR

MASTER is the picture as it is to look, 1024 pixels or more, its ground one colour to the
edges. OUT_DIR gets what the builds take: app.ico and app.png (the Windows program and the
desktop window), and android/res with the launcher icon in every density. The builds take
the set in private/icon when there is one and the committed one otherwise
(docs/reader/icon.md). Needs Pillow.
"""
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
# Android cuts the launcher icon's 108 units to a shape of its own choosing; only a circle
# of 66 units in the middle is always seen.
SAFE = 66 / 108

ADAPTIVE = """<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/ic_launcher_background"/>
    <foreground android:drawable="@mipmap/ic_launcher_foreground"/>
</adaptive-icon>
"""
BACKGROUND = """<?xml version="1.0" encoding="utf-8"?>
<resources>
    <color name="ic_launcher_background">#{:02X}{:02X}{:02X}</color>
</resources>
"""


def shaped(picture, size, circle=False):
    """The picture at this size with rounded corners, or cut to a circle."""
    out = picture.resize((size, size), Image.LANCZOS).convert("RGBA")
    mask = Image.new("L", (size * 4, size * 4), 0)
    draw = ImageDraw.Draw(mask)
    edge = size * 4 - 1
    if circle:
        draw.ellipse([0, 0, edge, edge], fill=255)
    else:
        draw.rounded_rectangle([0, 0, edge, edge], radius=size * 4 * 0.18, fill=255)
    out.putalpha(mask.resize((size, size), Image.LANCZOS))
    return out


def foreground(master, ground):
    """The picture made small enough on its ground that no launcher shape cuts into it."""
    n = master.width
    box = ImageChops.difference(master, Image.new("RGB", master.size, ground)).getbbox()
    reach = 0.0
    if box:
        for x in (box[0], box[2]):
            for y in (box[1], box[3]):
                reach = max(reach, ((x - n / 2) ** 2 + (y - n / 2) ** 2) ** 0.5 / (n / 2))
    scale = min(1.0, SAFE * 0.97 / reach) if reach else 1.0
    inner = round(n * scale)
    out = Image.new("RGB", (n, n), ground)
    out.paste(master.resize((inner, inner), Image.LANCZOS), ((n - inner) // 2,) * 2)
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    master = Image.open(sys.argv[1]).convert("RGB")
    if master.width != master.height or master.width < 1024:
        sys.exit("the picture has to be square, 1024 pixels or more")
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    ground = master.getpixel((0, 0))

    shaped(master, 256).save(out / "app.png")
    shaped(master, 256).save(out / "app.ico", sizes=[(s, s) for s in ICO_SIZES])

    res = out / "android" / "res"
    front = foreground(master, ground)
    n = front.width
    seen = front.crop((n // 6, n // 6, n - n // 6, n - n // 6))  # the 72 units a launcher shows
    for name, factor in DENSITIES.items():
        folder = res / f"mipmap-{name}"
        folder.mkdir(parents=True, exist_ok=True)
        size = round(108 * factor)
        front.resize((size, size), Image.LANCZOS).save(folder / "ic_launcher_foreground.png")
        shaped(master, round(48 * factor)).save(folder / "ic_launcher.png")
        shaped(seen, round(48 * factor), circle=True).save(folder / "ic_launcher_round.png")
    (res / "mipmap-anydpi-v26").mkdir(parents=True, exist_ok=True)
    for name in ("ic_launcher.xml", "ic_launcher_round.xml"):
        (res / "mipmap-anydpi-v26" / name).write_text(ADAPTIVE)
    (res / "values").mkdir(parents=True, exist_ok=True)
    (res / "values" / "ic_launcher_background.xml").write_text(BACKGROUND.format(*ground))


if __name__ == "__main__":
    main()
