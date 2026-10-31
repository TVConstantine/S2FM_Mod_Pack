"""A murky water with algae floating on it, for s2vfx_water's debris layer: 1024 x 1024 tiling textures (the algae's
colour, height and normal) and s2vfx_water_algae.vmat.

    python tests/water_debris.py [content folder]

Writes s2vfx_o_algae_*.tga and the vmat into the folder (default: core's materials/s2vfx_test), then compile the vmat
with resourcecompiler.
"""
import os
import sys

import numpy as np
from PIL import Image

SIZE = 1024
FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\content\core\materials\s2vfx_test")
rng = np.random.default_rng(11)
y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64) / SIZE


def noise(cells):
    """Value noise that tiles."""
    g = rng.random((cells, cells))
    fx, fy = x * cells, y * cells
    ix, iy = np.floor(fx).astype(int), np.floor(fy).astype(int)
    tx, ty = fx - ix, fy - iy
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    a, b = g[iy % cells, ix % cells], g[iy % cells, (ix + 1) % cells]
    c, d = g[(iy + 1) % cells, ix % cells], g[(iy + 1) % cells, (ix + 1) % cells]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def fbm(base, octaves):
    total, weight, s = 0.0, 1.0, 0.0
    for o in range(octaves):
        total = total + noise(base * 2 ** o) * weight
        s += weight
        weight *= 0.5
    return total / s


def srgb(a):
    return np.where(a <= 0.0031308, a * 12.92, 1.055 * np.power(a, 1 / 2.4) - 0.055)


def save(name, a, mode):
    Image.fromarray(np.uint8(np.clip(a, 0, 1) * 255 + 0.5), mode).save(os.path.join(FOLDER, name))


os.makedirs(FOLDER, exist_ok=True)
# Height: big patches with ragged edges and small clumps.
height = fbm(4, 6) * 0.75 + fbm(32, 3) * 0.25
height = (height - height.min()) / (height.max() - height.min())
# Colour: dark green to yellow green, brown where thin.
tone = fbm(16, 4)
green = np.dstack([0.10 + 0.12 * tone, 0.18 + 0.16 * tone, 0.04 + 0.03 * tone])
brown = np.array([0.16, 0.12, 0.05])
thin = np.clip((0.6 - height) * 3, 0, 1)[..., None]
color = green * (1 - thin) + brown * thin
save("s2vfx_o_algae_color.tga", srgb(color), "RGB")
save("s2vfx_o_algae_height.tga", height, "L")
gy, gx = np.gradient(height * 6.0)
n = np.dstack([-gx * SIZE / 64, gy * SIZE / 64, np.ones_like(height)])
n /= np.linalg.norm(n, axis=2, keepdims=True)
save("s2vfx_o_algae_normal.tga", n * 0.5 + 0.5, "RGB")

T = "materials/s2vfx_test/s2vfx_o_algae_%s.tga"
lines = [
    'shader "s2vfx_water.vfx"',
    'g_vWaterDecayColor "[0.450000 0.600000 0.350000 0.000000]"',
    'g_flWaterDecayDistance "40.000"',
    'g_vWaterFogColor "[0.140000 0.200000 0.090000 0.000000]"',
    'g_flWaterFogDistance "120.000"',
    'g_flWaveScale "120.000"',
    'g_flCausticsStrength "0.500"',
    'g_bDebris "1"',
    'g_flDebrisAmount "0.600"',
    'g_flDebrisScale "160.000"',
    'TextureDebris "%s"' % (T % "color"),
    'TextureDebrisHeight "%s"' % (T % "height"),
    'TextureDebrisNormal "%s"' % (T % "normal"),
    # The foam texture from tests/water_foam_tex.py.
    'TextureFoam "materials/s2vfx_test/s2vfx_o_foam_height.tga"',
    'TextureFoamNormal "materials/s2vfx_test/s2vfx_o_foam_normal.tga"',
]
with open(os.path.join(FOLDER, "s2vfx_water_algae.vmat"), "w", newline="\n") as f:
    f.write("Layer0\n{\n" + "".join("\t%s\n" % l for l in lines) + "}\n")
print("written to", FOLDER)
