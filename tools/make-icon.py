#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws Glimpse.ico: two overlapping windows, the front one filled.

Usage: python3 tools/make-icon.py [output.ico]
Each size is drawn separately at 8x and scaled down, so small sizes stay crisp.
"""
import sys
from PIL import Image, ImageDraw

ACCENT = (91, 79, 224, 255)       # violet used across the UI
ACCENT_SOFT = (233, 230, 255, 255)
WHITE = (255, 255, 255, 255)
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def draw(size):
    k = 8
    s = size * k
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = s / 16.0  # design grid of 16 units
    stroke = max(1.0, 1.25 * u) if size >= 24 else 1.5 * u
    r = 2.2 * u
    # back window: outlined, soft fill
    back = (0.75 * u, 0.75 * u, 11.25 * u, 10.25 * u)
    d.rounded_rectangle(back, radius=r, fill=ACCENT_SOFT, outline=ACCENT, width=int(round(stroke)))
    # back window title strip (the glimpse)
    d.rounded_rectangle((back[0], back[1], back[2], back[1] + 3.0 * u), radius=r, fill=ACCENT)
    d.rectangle((back[0], back[1] + 1.6 * u, back[2], back[1] + 3.0 * u), fill=ACCENT)
    # front window: filled, with a white rim so it reads on dark taskbars
    front = (5.0 * u, 5.0 * u, 15.25 * u, 15.25 * u)
    rim = 0.9 * u
    d.rounded_rectangle((front[0] - rim, front[1] - rim, front[2] + rim * 0.4, front[3] + rim * 0.4),
                        radius=r + rim, fill=WHITE)
    d.rounded_rectangle(front, radius=r, fill=ACCENT)
    if size >= 32:
        # title bar line on the front window
        y = front[1] + 2.6 * u
        d.rectangle((front[0] + 1.2 * u, y, front[2] - 1.2 * u, y + 0.7 * u), fill=(255, 255, 255, 170))
    return im.resize((size, size), Image.LANCZOS)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "Glimpse.ico"
    images = [draw(n) for n in SIZES]
    big = images[-1]
    big.save(out, format="ICO", sizes=[(n, n) for n in SIZES], append_images=images[:-1])
    print("wrote", out)


if __name__ == "__main__":
    main()
