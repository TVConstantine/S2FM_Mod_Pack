"""Three glass materials for s2vfx_glass, with their 1024 x 1024 textures:

    s2vfx_glass_clear     solid, faintly green bottle glass, IOR 1.5: bends what is behind it
    s2vfx_glass_frosted   a thin pane, frosted with a clear border and a clear diamond pattern
    s2vfx_glass_ice       ice, IOR 1.31, blue, a lumpy surface with cracks, patchy roughness, a little cloudy

    python tests/glass_showcase.py [content folder]

Writes s2vfx_o_glass_*.tga and the three vmats into the folder (default: core's materials/s2vfx_test), then
compile the vmats with resourcecompiler.
"""
import os
import sys

import numpy as np
from PIL import Image

SIZE = 1024
FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\content\core\materials\s2vfx_test")
rng = np.random.default_rng(7)
y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64) / SIZE


def save_gray(name, a):
    Image.fromarray(np.uint8(np.clip(a, 0, 1) * 255 + 0.5), "L").save(os.path.join(FOLDER, name))


def smooth_noise(cells):
    """Value noise that tiles: a random grid, upsampled with a smooth step."""
    g = rng.random((cells, cells))
    fx, fy = x * cells, y * cells
    ix, iy = np.floor(fx).astype(int), np.floor(fy).astype(int)
    tx, ty = fx - ix, fy - iy
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    a, b = g[iy % cells, ix % cells], g[iy % cells, (ix + 1) % cells]
    c, d = g[(iy + 1) % cells, ix % cells], g[(iy + 1) % cells, (ix + 1) % cells]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def cracks(points):
    """Thin lines where two cells of a tiling Voronoi pattern meet."""
    p = rng.random((points, 2))
    best = np.full((SIZE, SIZE), 9.0)
    second = np.full((SIZE, SIZE), 9.0)
    for px, py in p:
        for ox in (-1, 0, 1):
            for oy in (-1, 0, 1):
                d = np.hypot(x - px - ox, y - py - oy)
                second = np.where(d < best, best, np.minimum(second, d))
                best = np.minimum(best, d)
    return np.clip(1 - (second - best) / 0.006, 0, 1)


def normal_map(name, height, strength):
    gy, gx = np.gradient(height * strength)
    n = np.dstack([-gx * SIZE, gy * SIZE, np.ones_like(height)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    Image.fromarray(np.uint8(np.clip(n * 0.5 + 0.5, 0, 1) * 255 + 0.5), "RGB").save(os.path.join(FOLDER, name))


os.makedirs(FOLDER, exist_ok=True)

# Frosted pane: frosted inside a clear border, with a clear diamond lattice.
border = (np.minimum(np.minimum(x, 1 - x), np.minimum(y, 1 - y)) < 0.08)
u, v = (x + y) * 6 % 1, (x - y) * 6 % 1
lattice = (np.abs(u - 0.5) < 0.04) | (np.abs(v - 0.5) < 0.04)
save_gray("s2vfx_o_glass_frost_rough.tga", np.where(border | lattice, 0.0, 1.0))

# Ice: lumps and cracks in the height, patchy roughness, cracks a little rougher.
crack = cracks(24)
lumps = smooth_noise(6) * 0.7 + smooth_noise(16) * 0.3
normal_map("s2vfx_o_glass_ice_normal.tga", lumps * 0.004 - crack * 0.0015, 1.0)
save_gray("s2vfx_o_glass_ice_rough.tga", 0.15 + smooth_noise(10) * 0.35 + crack * 0.4)

T = "materials/s2vfx_test/s2vfx_o_glass_%s.tga"
materials = {
    "s2vfx_glass_clear": [
        'g_vColorTint "[0.850000 0.950000 0.880000 0.000000]"',
        'g_flIor "1.500"',
        'g_flThickness "8.000"',
    ],
    "s2vfx_glass_frosted": [
        'g_vColorTint "[0.950000 0.970000 1.000000 0.000000]"',
        'g_bThinWalled "1"',
        'g_flThickness "2.000"',
        'TextureRoughness "%s"' % (T % "frost_rough"),
        'g_flRoughnessRemapMin "0.000"',
        'g_flRoughnessRemapMax "0.600"',
    ],
    "s2vfx_glass_ice": [
        'g_vColorTint "[0.780000 0.900000 1.000000 0.000000]"',
        'g_flIor "1.310"',
        'g_flThickness "12.000"',
        'g_flCloudiness "0.150"',
        'TextureNormal "%s"' % (T % "ice_normal"),
        'TextureRoughness "%s"' % (T % "ice_rough"),
    ],
}
for name, lines in materials.items():
    with open(os.path.join(FOLDER, name + ".vmat"), "w", newline="\n") as f:
        f.write("Layer0\n{\n\tshader \"s2vfx_glass.vfx\"\n" + "".join("\t%s\n" % l for l in lines) + "}\n")
print("written to", FOLDER)
