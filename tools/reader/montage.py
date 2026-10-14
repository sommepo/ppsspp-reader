#!/usr/bin/env python3
# SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tile harness screenshots into one labelled contact sheet.

    montage.py OUT.png IN1.png IN2.png ... [--cols N] [--scale F]
"""
import os
import sys

from PIL import Image, ImageDraw


def main() -> int:
    args = sys.argv[1:]
    cols, scale = 4, 1.0
    files = []
    i = 0
    while i < len(args):
        if args[i] == "--cols":
            cols = int(args[i + 1]); i += 2
        elif args[i] == "--scale":
            scale = float(args[i + 1]); i += 2
        else:
            files.append(args[i]); i += 1
    out, inputs = files[0], files[1:]
    images = [Image.open(p).convert("RGB") for p in inputs]
    if scale != 1.0:
        images = [im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS) for im in images]
    w = max(im.width for im in images)
    h = max(im.height for im in images) + 12
    rows = (len(images) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * w, rows * h), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    for n, (im, path) in enumerate(zip(images, inputs)):
        x, y = (n % cols) * w, (n // cols) * h
        sheet.paste(im, (x, y + 12))
        draw.text((x + 2, y), os.path.basename(path), fill=(255, 255, 0))
    sheet.save(out)
    print(out, sheet.size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
