#!/usr/bin/env python3
# SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draw the layouts a profile reported over a harness screenshot.

    layout_check.py SHOT.png OUT.png [--crop] < layouts

Reads layout JSON lines (the harness's `layouts` command) from standard input and
draws, for the latest layout of every window still on screen, the text box in blue
and each character cell in red. The picture shows at a glance whether the cells sit
on the characters. A layout's rectangles are in the units of its "space"; they are
scaled to the screenshot. --crop keeps only the area around the boxes, enlarged.
Needs Pillow (the analysis Python, $READER_RE_PYTHON).
"""
import argparse
import json
import sys

from PIL import Image, ImageDraw


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("shot")
    ap.add_argument("out")
    ap.add_argument("--crop", action="store_true")
    args = ap.parse_args()

    current = {}
    for line in sys.stdin:
        line = line.strip()
        if not line.startswith("{"):
            continue
        layout = json.loads(line)
        if "glyphs" not in layout and not layout.get("closed"):
            continue
        if layout.get("closed"):
            current.pop(layout["window"], None)
        else:
            current[layout["window"]] = layout

    image = Image.open(args.shot).convert("RGB")
    draw = ImageDraw.Draw(image)
    area = None

    def rect(layout, r, colour):
        nonlocal area
        fx = image.width / layout["space"][0]
        fy = image.height / layout["space"][1]
        x, y, w, h = r
        box = [x * fx, y * fy, (x + w) * fx - 1, (y + h) * fy - 1]
        draw.rectangle(box, outline=colour)
        if area is None:
            area = list(box)
        else:
            area = [min(area[0], box[0]), min(area[1], box[1]), max(area[2], box[2]), max(area[3], box[3])]

    for layout in current.values():
        if layout.get("box"):
            rect(layout, layout["box"], (40, 120, 255))
        cells = [g for g in layout["glyphs"] if g]
        for g in cells:
            rect(layout, g, (255, 40, 40))
        print(f"window {layout['window']} seq {layout['seq']}: "
              f"{len(cells)} of {len(layout['glyphs'])} characters placed")
    if args.crop and area:
        pad = 24
        image = image.crop((max(0, int(area[0]) - pad), max(0, int(area[1]) - pad),
                            min(image.width, int(area[2]) + pad), min(image.height, int(area[3]) + pad)))
        image = image.resize((image.width * 2, image.height * 2), Image.LANCZOS)
    image.save(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
