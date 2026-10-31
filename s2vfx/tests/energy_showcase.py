"""Four materials for s2vfx_energy (examples/energy.vfx), with their textures:

    s2vfx_energy_core      Episode One's Citadel core as first matched: a ragged orange ring, a dark deep interior,
                           heaving in lumps inside a heat haze, its ring turning to turquoise and back every 12 s
    s2vfx_energy_hex       a blue shell of sharp hexagonal cells, a dark navy middle, a gold fringe
    s2vfx_energy_swirl     a black middle, gold and teal bands combed by marble streaks
    s2vfx_energy_citadel   the Citadel core as filmed: a thin ring turning orange to cyan and back every 8 s (by
                           yellow and green, never dark), heaving in lumps inside a heat haze

    python tests/energy_showcase.py [content folder]

Writes s2vfx_o_energy_*.tga and the four vmats into the folder (default: core's materials/s2vfx_test), then
compile the vmats with resourcecompiler.
"""
import os
import sys

import numpy as np
from PIL import Image

FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\content\core\materials\s2vfx_test")


def save_rgb(name, a):
    Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB").save(os.path.join(FOLDER, name))


def ramp(name, keys, rows=8):
    """A ramp across (left: the ring's inner edge, right: its outer), the same in every row."""
    xs = np.linspace(0, 1, 256)
    rgb = np.stack([np.interp(xs, [k for k, _ in keys], [v[c] for _, v in keys]) for c in range(3)], 1)
    save_rgb(name, np.repeat((np.clip(rgb, 0, 1) ** (1 / 2.2))[None], rows, axis=0))


def hex_normal(name, n=1024, cols=16, rows=18):
    """Pointy-top hexagons that tile (rows 1.5 r apart, the square very slightly stretched): flat tops, a narrow
    steep bevel, a groove between."""
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    px, py = x / n * cols, y / n * rows
    best = np.full((n, n), 1e9)
    hexd = np.zeros((n, n))
    for j in range(-1, rows + 1):
        for i in range(-1, cols + 1):
            dx = px - (i + 0.5 + (0.5 if j % 2 else 0.0))
            dy = (py - (j + 0.5)) * (1.5 / np.sqrt(3))
            d2 = dx * dx + dy * dy
            m = d2 < best
            best[m] = d2[m]
            ax, ay = np.abs(dx), np.abs(dy)
            hexd[m] = np.maximum(ax, ax * 0.5 + ay * (np.sqrt(3) / 2))[m]
    r = np.clip(hexd / 0.5, 0, 1)
    bevel = np.clip((r - 0.78) / 0.14, 0, 1)
    height = np.where(r < 0.78, 1.0 - 0.15 * r * r, (1.0 - 0.15 * 0.78 ** 2) * (1 - bevel))
    gx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 0.5 * n / cols
    gy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * 0.5 * n / rows
    nrm = np.stack([-gx * 0.12, -gy * 0.12, np.ones_like(gx)], -1)
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
    save_rgb(name, nrm * 0.5 + 0.5)


def citadel_ramp(name, rows=8):
    """Across (u) a thin ring: a white-hot inner edge, its orange glowing outward; the same in every row (the shader
    turns it to the cycle colour and back)."""
    u = np.linspace(0, 1, 256)[None, :]
    edge = np.exp(-((u - 0.15) / 0.05) ** 2)
    glow = np.where(u > 0.15, np.exp(-(u - 0.15) / 0.3), np.exp(-((u - 0.15) / 0.04) ** 2))
    img = glow[..., None] * np.array((1.0, 0.55, 0.1)) + edge[..., None] * np.array([1.0, 0.95, 0.8]) * 0.8
    save_rgb(name, np.repeat(np.clip(img, 0, 1) ** (1 / 2.2), rows, axis=0))


def vmat(name, body):
    with open(os.path.join(FOLDER, name + ".vmat"), "w") as f:
        f.write('Layer0\n{\n\tshader "s2vfx_energy.vfx"\n' + "".join("\t%s\n" % l for l in body) + "}\n")


T = "materials/s2vfx_test/"
ramp("s2vfx_o_energy_rim_ramp.tga", [(0.0, (0.25, 0.85, 1.0)), (0.1, (0.6, 0.95, 1.0)), (0.2, (1.0, 0.95, 0.7)),
                                     (0.45, (1.0, 0.75, 0.25)), (0.75, (1.0, 0.4, 0.05)), (1.0, (0.6, 0.08, 0.02))])
ramp("s2vfx_o_energy_hex_ramp.tga", [(0.0, (0.0, 0.01, 0.04)), (0.38, (0.01, 0.04, 0.12)), (0.46, (0.9, 0.7, 0.25)),
                                     (0.52, (0.1, 0.5, 1.0)), (0.8, (0.1, 0.6, 1.0)), (1.0, (0.3, 0.8, 1.0))])
ramp("s2vfx_o_energy_swirl_ramp.tga", [(0.0, (0.0, 0.0, 0.0)), (0.36, (0.0, 0.0, 0.0)), (0.42, (1.0, 0.85, 0.3)),
                                       (0.58, (0.9, 0.6, 0.12)), (0.72, (0.3, 0.75, 0.8)), (0.86, (0.03, 0.2, 0.25)),
                                       (1.0, (0.0, 0.03, 0.05))])
hex_normal("s2vfx_o_energy_hex_normal.tga")
citadel_ramp("s2vfx_o_energy_citadel_ramp.tga")

vmat("s2vfx_energy_core", ["F_REFRACTION 1", "F_RIM 1", "F_INTERIOR 1", "F_SHELL 1", "F_REFLECTION 1", "F_BLOB 1",
                           "F_HEAT_HAZE 1", "F_CHARGE 1", 'TextureRimRamp "%ss2vfx_o_energy_rim_ramp.tga"' % T, 'g_flRimPulse "0.400"',
                           'g_flRimPulseRate "50.000"', 'g_flRimRadialFlow "0.500"', 'g_flRimBreathe "0.700"',
                           'g_flRimColorCycle "12.000"'])
vmat("s2vfx_energy_hex", ["F_REFRACTION 1", "F_RIM 1", "F_SHELL 1", "F_REFLECTION 1",
                          'TextureNormal "%ss2vfx_o_energy_hex_normal.tga"' % T, 'g_flNormalStrength "4.000"',
                          'g_flNormalTiling "1.000"', 'g_flTurbulenceDetail "0.300"', 'g_flRefractStrength "2.000"',
                          'TextureRimRamp "%ss2vfx_o_energy_hex_ramp.tga"' % T,
                          'g_vRimColor "[1.000000 1.000000 1.000000 0.000000]"', 'g_flRimBrightness "2.000"',
                          'g_flRimPosition "0.500"', 'g_flRimWidth "1.000"', 'g_flRimSoftness "0.100"',
                          'g_flRimNoise "0.300"', 'g_flRimOpacity "0.800"', 'g_flShellOpacity "0.000"',
                          'g_vShellGlowColor "[0.200000 0.600000 1.000000 0.000000]"', 'g_flShellGlow "3.000"',
                          'g_flShellStreaks "0.000"', 'g_flReflectionStrength "1.500"', 'g_flRimPulse "0.500"',
                          'g_flRimPulseRate "40.000"', 'g_flRimRadialFlow "0.500"'])
vmat("s2vfx_energy_swirl", ["F_REFRACTION 1", "F_RIM 1", "F_REFLECTION 1", 'g_flTurbulenceScale "4.000"',
                            'g_flRefractStrength "0.500"', 'TextureRimRamp "%ss2vfx_o_energy_swirl_ramp.tga"' % T,
                            'g_vRimColor "[1.000000 1.000000 1.000000 0.000000]"', 'g_flRimBrightness "1.500"',
                            'g_flRimPosition "0.500"', 'g_flRimWidth "1.000"', 'g_flRimSoftness "0.100"',
                            'g_flRimNoise "0.250"', 'g_flRimStreaks "0.800"', 'g_flRimStreakLines "10.000"',
                            'g_flRimOpacity "1.000"', 'g_flReflectionStrength "0.500"', 'g_flRimPulse "0.600"',
                            'g_flRimPulseRate "35.000"', 'g_flRimRadialFlow "0.700"'])
vmat("s2vfx_energy_citadel", ["F_REFRACTION 1", "F_RIM 1", "F_INTERIOR 1", "F_SHELL 1", "F_REFLECTION 1", "F_BLOB 1",
                              "F_HEAT_HAZE 1", "F_CHARGE 1", 'g_bChargeInward "1"',
                              'g_vRimCycleColor "[0.200000 0.850000 1.000000 0.000000]"',
                              'g_flRefractStrength "1.000"', 'TextureRimRamp "%ss2vfx_o_energy_citadel_ramp.tga"' % T,
                              'g_vRimColor "[1.000000 1.000000 1.000000 0.000000]"', 'g_flRimBrightness "6.000"',
                              'g_flRimPosition "0.330"', 'g_flRimWidth "0.300"', 'g_flRimSoftness "0.300"',
                              'g_flRimNoise "0.200"', 'g_flRimColorCycle "8.000"', 'g_flRimRadialFlow "0.300"',
                              'g_vInteriorColor "[0.030000 0.050000 0.070000 0.000000]"',
                              'g_flInteriorReflectivity "3.000"', 'g_flInteriorGlow "0.150"',
                              'g_flInteriorMarble "0.200"', 'g_flInteriorRipples "0.500"',
                              'g_vShellColor "[0.800000 0.900000 0.950000 0.000000]"', 'g_flShellOpacity "0.450"',
                              'g_flShellBrightness "0.500"', 'g_flShellGlow "0.800"', 'g_flShellStreaks "0.400"',
                              'g_flReflectionStrength "1.000"'])
print("written into", FOLDER)
