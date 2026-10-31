"""A tiling foam texture for s2vfx_water: marbled, veined foam (thick veins, thin threads between them, small bubbles),
1024 x 1024, as a height (how thick the foam is: the thin parts go first as it thins out) and the normal made from it.

    python tests/water_foam_tex.py [content folder]

Writes s2vfx_o_foam_height.tga, s2vfx_o_foam_normal.tga and s2vfx_water_default.vmat (plain water with this foam)
(default: core's materials/s2vfx_test).
"""
import os
import sys

import numpy as np
from PIL import Image

SIZE = 1024
FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\content\core\materials\s2vfx_test")
rng = np.random.default_rng(3)
y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64) / SIZE


def noise(cells, u=None, v=None):
    """Value noise that tiles over the texture, at (u, v) in texture units (default the texel grid)."""
    u = x if u is None else u
    v = y if v is None else v
    g = rng.random((cells, cells))
    fx, fy = (u % 1.0) * cells, (v % 1.0) * cells
    ix, iy = np.floor(fx).astype(int), np.floor(fy).astype(int)
    tx, ty = fx - ix, fy - iy
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    a, b = g[iy % cells, ix % cells], g[iy % cells, (ix + 1) % cells]
    c, d = g[(iy + 1) % cells, ix % cells], g[(iy + 1) % cells, (ix + 1) % cells]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def fbm(base, octaves, u=None, v=None):
    total, weight, s = 0.0, 1.0, 0.0
    for o in range(octaves):
        total = total + noise(base * 2 ** o, u, v) * weight
        s += weight
        weight *= 0.5
    return total / s


def ridges(base, octaves, u, v):
    """1 along thin ridges: where the noise crosses its middle."""
    return 1.0 - np.abs(fbm(base, octaves, u, v) * 2.0 - 1.0)


# Domain warp: the veins wander like marble.
wu = x + (fbm(4, 4) - 0.5) * 0.25
wv = y + (fbm(4, 4) - 0.5) * 0.25
veins = ridges(6, 5, wu, wv) ** 6                      # the thick veins
threads = ridges(18, 4, wu * 1.0 + 0.37, wv) ** 10     # thin threads between them
# Small bubbles: round dots, one at a random place in some of the cells of a fine grid.
cells = 96
cx, cy = x * cells, y * cells
ix, iy = np.floor(cx).astype(int), np.floor(cy).astype(int)
jitter = rng.random((cells, cells, 3))
jx, jy, keep = jitter[iy % cells, ix % cells, 0], jitter[iy % cells, ix % cells, 1], jitter[iy % cells, ix % cells, 2]
dist = np.hypot(cx - ix - (0.25 + 0.5 * jx), cy - iy - (0.25 + 0.5 * jy))
bubbles = np.clip(1.0 - dist / (0.12 + 0.12 * keep), 0, 1) ** 0.5 * (keep > 0.55) * 0.6
body = fbm(8, 4)                                       # a soft body under the veins, thicker in patches
height = np.clip(veins * 0.9 + threads * 0.45 + bubbles * 0.35 + (body - 0.45) * 0.6, 0, 1)
height = (height - height.min()) / (height.max() - height.min())

os.makedirs(FOLDER, exist_ok=True)
Image.fromarray(np.uint8(height * 255 + 0.5), "L").save(os.path.join(FOLDER, "s2vfx_o_foam_height.tga"))
gy, gx = np.gradient(height * 3.0)
n = np.dstack([-gx * SIZE / 64, gy * SIZE / 64, np.ones_like(height)])
n /= np.linalg.norm(n, axis=2, keepdims=True)
Image.fromarray(np.uint8(np.clip(n * 0.5 + 0.5, 0, 1) * 255 + 0.5), "RGB").save(os.path.join(FOLDER, "s2vfx_o_foam_normal.tga"))
# Plain water with this foam: the shader's own settings otherwise.
with open(os.path.join(FOLDER, "s2vfx_water_default.vmat"), "w") as f:
    f.write('Layer0\n{\n\tshader "s2vfx_water.vfx"\n'
            '\tTextureFoam "materials/s2vfx_test/s2vfx_o_foam_height.tga"\n'
            '\tTextureFoamNormal "materials/s2vfx_test/s2vfx_o_foam_normal.tga"\n}\n')
print("written to", FOLDER)
