"""Draws the ReShade panel's icon into reshade/tool/images/appicon.ico and appicon.png.

In the colours of ReShade's own icon: a tile with a cyan to navy gradient, a colour wheel ring, and three stacked
layers in the middle for the panel's three stages. Drawn at 1024 px and scaled down per size.

    python tools/reshade_icon.py [preview.png]
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "..", "reshade", "tool", "images")
SIZES = [16, 24, 32, 48, 64, 128, 256]


def draw(n=1024):
    s = n / 256.0
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32) / (n - 1)

    # The tile: a diagonal gradient from cyan (top left) to navy (bottom right).
    t = np.clip((xx * 0.55 + yy * 0.75) / 1.3, 0.0, 1.0)[..., None]
    top, bottom = np.array([0, 214, 255], np.float32), np.array([0, 32, 113], np.float32)
    tile = (top * (1 - t) + bottom * t).astype(np.uint8)
    tile_rgba = np.dstack([tile, np.full((n, n), 255, np.uint8)])
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle([10 * s, 10 * s, 246 * s, 246 * s], radius=46 * s, fill=255)
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    im.paste(Image.fromarray(tile_rgba, "RGBA"), (0, 0), mask)
    ImageDraw.Draw(im).rounded_rectangle([10 * s, 10 * s, 246 * s, 246 * s], radius=46 * s,
                                         outline=(8, 20, 48, 255), width=max(1, int(5 * s)))

    # The colour wheel: a ring whose hue goes round, red at the top.
    cx, cy = 0.5, 0.5
    dx, dy = xx - cx, yy - cy
    r = np.sqrt(dx * dx + dy * dy)
    hue = (np.arctan2(dx, -dy) / (2 * math.pi)) % 1.0
    outer, inner = 92 / 256.0, 58 / 256.0
    ring_alpha = np.clip((outer - r) * n, 0, 1) * np.clip((r - inner) * n, 0, 1)
    rgb = np.zeros((n, n, 3), np.float32)
    h6 = hue * 6.0
    i = np.floor(h6).astype(int) % 6
    f = h6 - np.floor(h6)
    q, p_ = 1 - f, f
    table = [(1, p_, 0), (q, 1, 0), (0, 1, p_), (0, q, 1), (p_, 0, 1), (1, 0, q)]
    for k, (a, b, c) in enumerate(table):
        sel = i == k
        for ch, v in enumerate((a, b, c)):
            rgb[..., ch] = np.where(sel, v if np.isscalar(v) else v, rgb[..., ch])
    ring = np.dstack([(rgb * 255).astype(np.uint8), (ring_alpha * 255).astype(np.uint8)])
    shadow = Image.fromarray((np.dstack([np.zeros((n, n, 3)), ring_alpha * 120])).astype(np.uint8), "RGBA")
    shadow = shadow.filter(ImageFilter.GaussianBlur(5 * s))
    im = Image.alpha_composite(im, shadow)
    im = Image.alpha_composite(im, Image.fromarray(ring, "RGBA"))

    # Three layers in the middle, back to front.
    d = ImageDraw.Draw(im)
    for k, (shade, oy) in enumerate([(170, 16), (215, 0), (255, -16)]):
        y = 128 + oy
        pts = [(128 * s, (y - 18) * s), (164 * s, y * s), (128 * s, (y + 18) * s), (92 * s, y * s)]
        d.polygon(pts, fill=(shade, shade, shade, 255), outline=(0, 30, 80, 255))
    return im


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    big = draw(1024)
    images = [big.resize((k, k), Image.LANCZOS) for k in SIZES]
    images[-1].save(os.path.join(OUT_DIR, "appicon.png"))
    images[-1].save(os.path.join(OUT_DIR, "appicon.ico"), sizes=[(k, k) for k in SIZES])
    print("appicon.ico:", sorted(Image.open(os.path.join(OUT_DIR, "appicon.ico")).info["sizes"]))
    if len(sys.argv) > 1:
        sheet = Image.new("RGBA", (sum(SIZES) + 10 * len(SIZES), 256), (48, 48, 48, 255))
        x = 0
        for im in images:
            sheet.paste(im, (x, 0), im)
            x += im.size[0] + 10
        sheet.save(sys.argv[1])


if __name__ == "__main__":
    main()
