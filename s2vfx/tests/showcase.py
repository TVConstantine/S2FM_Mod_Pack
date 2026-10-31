"""A showcase material for s2vfx_lit: one 1024 x 1024 set of textures, a 3 x 3 grid with one of the shader's choices
in each cell, so they can all be seen at a glance on one model.

    row 1: painted copper (metalness texture: metal squares and paint)  | gold, roughness 0 to 1 left to right
           (multiscatter)                                               | rough silver (multiscatter)
    row 2: red car paint under a clear coat | brushed aluminium, brushed in circles (anisotropy and its rotation
           map) | dark blue velvet (sheen)
    row 3: skin with pores (subsurface) | glowing orange and cyan rings (self illumination) | white plastic,
           roughness 0 to 1 top to bottom

    python tests/showcase.py [content folder]

Writes s2vfx_o_showcase_*.tga and s2vfx_lit_showcase.vmat into the folder (default: core's materials/s2vfx_test),
then compile the vmat with resourcecompiler.
"""
import os
import sys

import numpy as np
from PIL import Image

SIZE = 1024
GROUT = 6
FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\content\core\materials\s2vfx_test")

y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)
cell = SIZE / 3.0
col, row = np.minimum((x // cell).astype(int), 2), np.minimum((y // cell).astype(int), 2)
u, v = (x - col * cell) / cell, (y - row * cell) / cell          # 0..1 inside a cell
grout = (np.minimum(u, 1 - u) * cell < GROUT / 2) | (np.minimum(v, 1 - v) * cell < GROUT / 2)


def at(r, c):
    return (row == r) & (col == c) & ~grout


color = np.zeros((SIZE, SIZE, 3)) + 0.08                       # grout: dark grey
rough = np.full((SIZE, SIZE), 0.9)
metal = np.zeros((SIZE, SIZE))
masks = {k: np.zeros((SIZE, SIZE)) for k in ("coat", "sheen", "aniso", "sss")}
aniso_rot = np.zeros((SIZE, SIZE))
glow = np.zeros((SIZE, SIZE, 3))
height = np.zeros((SIZE, SIZE))

# Painted copper: an 8 x 8 checker of bare metal and teal paint.
c = at(0, 0)
bare = ((np.floor(u * 8) + np.floor(v * 8)) % 2 == 0) & c
color[c] = (0.10, 0.42, 0.48)
color[bare] = (0.95, 0.64, 0.54)
metal[bare] = 1.0
rough[c] = 0.55
rough[bare] = 0.3

# Gold, roughness 0 to 1 left to right.
c = at(0, 1)
color[c] = (1.0, 0.78, 0.34)
metal[c] = 1.0
rough[c] = u[c]

# Rough silver.
c = at(0, 2)
color[c] = (0.95, 0.93, 0.88)
metal[c] = 1.0
rough[c] = 0.85

# Red car paint under a clear coat.
c = at(1, 0)
color[c] = (0.55, 0.02, 0.03)
rough[c] = 0.5
masks["coat"][c] = 1.0

# Brushed aluminium in circles: the tangent turned to run round the cell's middle.
c = at(1, 1)
color[c] = (0.91, 0.92, 0.93)
metal[c] = 1.0
rough[c] = 0.35
masks["aniso"][c] = 1.0
aniso_rot[c] = (np.arctan2(v[c] - 0.5, u[c] - 0.5) / (2 * np.pi) + 0.25) % 1.0

# Dark blue velvet.
c = at(1, 2)
color[c] = (0.04, 0.07, 0.30)
rough[c] = 0.9
masks["sheen"][c] = 1.0

# Skin with pores: small domes in the height.
c = at(2, 0)
color[c] = (0.80, 0.50, 0.40)
rough[c] = 0.5
masks["sss"][c] = 1.0
pu, pv = (u * 14) % 1 - 0.5, (v * 14) % 1 - 0.5
pore = np.clip(1 - np.hypot(pu, pv) / 0.35, 0, 1)
height[c] = -pore[c] ** 2 * 2.0

# Glowing rings: orange and cyan, on dark glossy plastic.
c = at(2, 1)
color[c] = (0.03, 0.03, 0.03)
rough[c] = 0.3
radius = np.hypot(u - 0.5, v - 0.5)
ring = (np.abs((radius * 10) % 1 - 0.5) < 0.12) & (radius < 0.48) & c
orange = ring & (np.floor(radius * 10) % 2 == 0)
glow[ring] = (0.0, 0.85, 1.0)
glow[orange] = (1.0, 0.45, 0.05)

# White plastic, roughness 0 to 1 top to bottom.
c = at(2, 2)
color[c] = (0.80, 0.80, 0.80)
rough[c] = v[c]

# Grout: a small bevel in the height so the cells read as tiles.
height[grout] = -1.5


def save_gray(name, a):
    Image.fromarray(np.uint8(np.clip(a, 0, 1) * 255 + 0.5), "L").save(os.path.join(FOLDER, name))


def save_rgb(name, a):
    Image.fromarray(np.uint8(np.clip(a, 0, 1) * 255 + 0.5), "RGB").save(os.path.join(FOLDER, name))


def srgb(a):
    return np.where(a <= 0.0031308, a * 12.92, 1.055 * np.power(a, 1 / 2.4) - 0.055)


# The normal from the height: x right, y up, z out (as tests/d4_normals.py writes them).
gy, gx = np.gradient(height)
n = np.dstack([-gx, gy, np.ones_like(height)])
n /= np.linalg.norm(n, axis=2, keepdims=True)

os.makedirs(FOLDER, exist_ok=True)
save_rgb("s2vfx_o_showcase_color.tga", srgb(color))
save_rgb("s2vfx_o_showcase_normal.tga", n * 0.5 + 0.5)
save_gray("s2vfx_o_showcase_rough.tga", rough)
save_gray("s2vfx_o_showcase_metal.tga", metal)
save_gray("s2vfx_o_showcase_coat.tga", masks["coat"])
save_gray("s2vfx_o_showcase_sheen.tga", masks["sheen"])
save_gray("s2vfx_o_showcase_aniso.tga", masks["aniso"])
save_gray("s2vfx_o_showcase_anisorot.tga", aniso_rot)
save_gray("s2vfx_o_showcase_sss.tga", masks["sss"])
save_rgb("s2vfx_o_showcase_glow.tga", srgb(glow))

T = "materials/s2vfx_test/s2vfx_o_showcase_%s.tga"
lines = [
    'shader "s2vfx_lit.vfx"',
    "F_SPECULAR 1",
    'TextureColor "%s"' % (T % "color"),
    'TextureNormal "%s"' % (T % "normal"),
    'TextureRoughness "%s"' % (T % "rough"),
    'F_METALNESS_TEXTURE 1',
    'TextureMetalness "%s"' % (T % "metal"),
    'g_bMultiscatter "1"',
    'F_CLEARCOAT 1',
    'TextureClearcoatMask "%s"' % (T % "coat"),
    'g_flClearcoatRoughness "0.030"',
    'F_SHEEN 1',
    'TextureSheenMask "%s"' % (T % "sheen"),
    'g_vSheenTint "[0.700000 0.750000 1.000000 0.000000]"',
    'F_ANISOTROPY 1',
    'g_flAnisotropic "0.800"',
    'TextureAnisotropicMask "%s"' % (T % "aniso"),
    'TextureAnisotropicRotation "%s"' % (T % "anisorot"),
    'F_SUBSURFACE 1',
    'TextureSubsurfaceMask "%s"' % (T % "sss"),
    'g_flSubsurfaceScale "15.000"',
    'F_SELF_ILLUM 1',
    'TextureSelfIllumMask "%s"' % (T % "glow"),
    'g_flSelfIllumBrightness "2.000"',
    'g_flSelfIllumAlbedoFactor "0.000"',
]
with open(os.path.join(FOLDER, "s2vfx_lit_showcase.vmat"), "w", newline="\n") as f:
    f.write("Layer0\n{\n" + "".join("\t%s\n" % l for l in lines) + "}\n")
print("written to", FOLDER)
