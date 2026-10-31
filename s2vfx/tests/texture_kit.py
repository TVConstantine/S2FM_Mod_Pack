"""Example texture sets, 1024 x 1024 (a ramp and a mask smaller), each with a material that uses it: what a set of
textures for each shader looks like, to start your own from.

    s2vfx_kit_painted_metal   lit: painted steel, chipped down to the bare metal, with rust in the chips
                              (color, normal, roughness, metalness, ambient occlusion)
    s2vfx_kit_tiles           lit: glazed ceramic tiles in grout, 4 x 4 a texture (color, normal, roughness, AO)
    s2vfx_kit_velvet          lit: red velvet, sheen on (color, normal)
    s2vfx_kit_brushed_disc    lit: a disc of metal brushed in circles, anisotropy on (rotation, mask)
    s2vfx_kit_glow_panel      lit: a dark panel with glowing lines, self illumination on (color, normal, mask)
    s2vfx_kit_sign            unlit: a sign cut out of its square, see-through round it (color, translucency)
    s2vfx_kit_stained_glass   glass: coloured panes in lead, the glass bumpy, one pane frosted (color, normal,
                              roughness)
    s2vfx_kit_pond            water: a pond's ripples from the detail normal map (detail normal)
    s2vfx_kit_shield          energy: a hex shield, its flow swirling round (flow, normal, rim ramp)

    python tests/texture_kit.py [content folder]

Textures are .tga. Compile the materials with resourcecompiler. The normal maps are OpenGL style (green up).
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\content\core\materials\s2vfx_test")
REL = "materials/s2vfx_test"
N = 1024
rng = np.random.default_rng(1234)


# ------------------------------------------------------------------------------------------ building blocks
def noise(cells, size=N, seed=None):
    """Smooth noise that tiles: a random grid of cells x cells, bicubic, wrapped round."""
    g = (np.random.default_rng(seed) if seed is not None else rng).random((cells, cells))
    big = Image.fromarray(np.tile(g, (3, 3)).astype(np.float32), "F").resize((size * 3, size * 3), Image.BICUBIC)
    return np.asarray(big)[size:2 * size, size:2 * size].astype(np.float64)


def fbm(base, octaves, size=N, seed=0):
    out, amp, total = np.zeros((size, size)), 1.0, 0.0
    for k in range(octaves):
        out += noise(base * 2 ** k, size, seed + k) * amp
        total += amp
        amp *= 0.5
    return out / total


def blur(a, passes):
    for _ in range(passes):
        a = (a + np.roll(a, 1, 0) + np.roll(a, -1, 0) + np.roll(a, 1, 1) + np.roll(a, -1, 1)) / 5.0
    return a


def normals(height, strength):
    """A tangent space normal map from a height (0..1, wrapped round): OpenGL style, green up."""
    gx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 0.5 * strength
    gy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * 0.5 * strength
    n = np.dstack([-gx, gy, np.ones_like(height)])
    return n / np.linalg.norm(n, axis=2, keepdims=True) * 0.5 + 0.5


def save(name, a):
    a = np.clip(a, 0.0, 1.0)
    mode = "L" if a.ndim == 2 else "RGB"
    Image.fromarray((a * 255 + 0.5).astype(np.uint8), mode).save(os.path.join(FOLDER, name))
    return "%s/%s" % (REL, name)


def vmat(name, shader, lines):
    body = ['\tshader "%s"' % shader] + ['\t' + l for l in lines]
    with open(os.path.join(FOLDER, name + ".vmat"), "w", newline="\n") as f:
        f.write("Layer0\n{\n" + "\n".join(body) + "\n}\n")


def rgb(c):
    return np.array(c, dtype=np.float64)


os.makedirs(FOLDER, exist_ok=True)

# ------------------------------------------------------------------------------------------ painted metal
# The paint chips where a noise is low; rust sits in and round the chips; the paint stands a little above the metal.
chips = fbm(6, 5, seed=10)
paint = np.clip((chips - 0.38) / 0.04, 0, 1)                         # 1 paint, 0 bare
rustNear = np.clip(1.0 - blur(paint, 12) * 1.6, 0, 1) * fbm(16, 3, seed=20)
rust = np.clip(rustNear * 2.2 - 0.25, 0, 1) * (1 - paint)
grain = fbm(64, 3, seed=30)
paintColor = rgb([0.12, 0.33, 0.35]) * (0.85 + 0.3 * fbm(8, 3, seed=40))[..., None]
metalColor = rgb([0.62, 0.62, 0.64]) * (0.8 + 0.4 * grain)[..., None]
rustColor = rgb([0.42, 0.18, 0.07]) * (0.7 + 0.6 * fbm(32, 3, seed=50))[..., None]
color = paintColor * paint[..., None] + metalColor * (1 - paint)[..., None]
color = color * (1 - rust[..., None]) + rustColor * rust[..., None]
height = blur(paint, 2) * 0.6 + grain * 0.15 - rust * 0.2
metal = (1 - paint) * (1 - rust)
rough = 0.42 * paint + (0.28 + 0.2 * grain) * (1 - paint) + 0.4 * rust
ao = 1.0 - np.clip(blur(1 - paint, 6) - (1 - paint), 0, 1) * 1.5
T = {k: save("s2vfx_k_metal_%s.tga" % k, v) for k, v in
     (("color", color), ("normal", normals(height, 40.0)), ("rough", rough), ("metal", metal), ("ao", ao))}
vmat("s2vfx_kit_painted_metal", "s2vfx_lit.vfx", [
    'F_SPECULAR 1', 'F_METALNESS_TEXTURE 1', 'TextureColor "%s"' % T["color"], 'TextureNormal "%s"' % T["normal"],
    'TextureRoughness "%s"' % T["rough"], 'TextureMetalness "%s"' % T["metal"],
    'TextureAmbientOcclusion "%s"' % T["ao"]])

# ------------------------------------------------------------------------------------------ tiles
# 4 x 4 tiles with bevelled edges in rough grout, each tile its own shade.
y, x = np.mgrid[0:N, 0:N] / N * 4.0
fx, fy = np.abs(x % 1.0 - 0.5), np.abs(y % 1.0 - 0.5)
edge = np.maximum(fx, fy)                                           # 0.5 at the grout
tile = np.clip((0.47 - edge) / 0.02, 0, 1)
shade = np.random.default_rng(7).random((4, 4))[np.minimum(y.astype(int), 3), np.minimum(x.astype(int), 3)]
glaze = fbm(24, 3, seed=60)
color = (rgb([0.85, 0.82, 0.74]) * (0.85 + 0.2 * shade + 0.05 * glaze)[..., None]) * tile[..., None] + \
        (rgb([0.32, 0.31, 0.29]) * (0.8 + 0.4 * fbm(96, 2, seed=70))[..., None]) * (1 - tile)[..., None]
height = np.clip((0.49 - edge) / 0.05, 0, 1) ** 0.5 + glaze * 0.03
rough = 0.08 + 0.1 * glaze * tile + (1 - tile) * 0.82
ao = 0.55 + 0.45 * np.clip((0.49 - edge) / 0.06, 0, 1)
T = {k: save("s2vfx_k_tiles_%s.tga" % k, v) for k, v in
     (("color", color), ("normal", normals(height, 25.0)), ("rough", rough), ("ao", ao))}
vmat("s2vfx_kit_tiles", "s2vfx_lit.vfx", [
    'F_SPECULAR 1', 'TextureColor "%s"' % T["color"], 'TextureNormal "%s"' % T["normal"],
    'TextureRoughness "%s"' % T["rough"], 'TextureAmbientOcclusion "%s"' % T["ao"]])

# ------------------------------------------------------------------------------------------ velvet
# A fine weave with a crushed pile: the sheen does the velvet, the textures only its grain.
y, x = np.mgrid[0:N, 0:N]
weave = (np.sin(x / N * np.pi * 2 * 256) * np.sin(y / N * np.pi * 2 * 256)) * 0.5 + 0.5
crush = fbm(8, 4, seed=80)
color = rgb([0.36, 0.02, 0.05]) * (0.7 + 0.5 * crush + 0.1 * weave)[..., None]
height = weave * 0.3 + crush * 0.7
T = {k: save("s2vfx_k_velvet_%s.tga" % k, v) for k, v in (("color", color), ("normal", normals(height, 6.0)))}
vmat("s2vfx_kit_velvet", "s2vfx_lit.vfx", [
    'F_SPECULAR 1', 'F_SHEEN 1', 'TextureColor "%s"' % T["color"], 'TextureNormal "%s"' % T["normal"],
    'g_flRoughnessRemapMin "0.800"', 'g_flRoughnessRemapMax "0.800"', 'g_flSheen "1.000"',
    'g_vSheenTint "[1.000000 0.450000 0.500000 0.000000]"', 'g_flSheenRoughness "0.400"'])

# ------------------------------------------------------------------------------------------ brushed disc
# The brushing runs round the centre: the rotation map is the angle round it (0..1 a full turn), the mask the disc.
cy, cx = (np.mgrid[0:N, 0:N] + 0.5) / N - 0.5
angle = (np.arctan2(cy, cx) / (2 * np.pi) + 0.25) % 1.0
disc = np.clip((0.48 - np.hypot(cx, cy)) / 0.005, 0, 1)
rings = fbm(4, 2, seed=90) * 0.2 + 0.8
T = {"rotation": save("s2vfx_k_brushed_rotation.tga", angle), "mask": save("s2vfx_k_brushed_mask.tga", disc),
     "color": save("s2vfx_k_brushed_color.tga", (rgb([0.8, 0.8, 0.82]) * rings[..., None]))}
vmat("s2vfx_kit_brushed_disc", "s2vfx_lit.vfx", [
    'F_SPECULAR 1', 'F_ANISOTROPY 1', 'TextureColor "%s"' % T["color"], 'g_flMetalness "1.000"',
    'g_flRoughnessRemapMin "0.300"', 'g_flRoughnessRemapMax "0.300"', 'g_flAnisotropic "0.900"',
    'TextureAnisotropicRotation "%s"' % T["rotation"], 'TextureAnisotropicMask "%s"' % T["mask"]])

# ------------------------------------------------------------------------------------------ glow panel
# A dark plate in panels; thin lines and a few dots glow (the mask, in colour: it tints the glow).
img = Image.new("L", (N, N), 0)
draw = ImageDraw.Draw(img)
for k in range(1, 4):
    draw.line([(0, k * N // 4), (N, k * N // 4)], fill=255, width=6)
    draw.line([(k * N // 4, 0), (k * N // 4, N)], fill=255, width=6)
for r in range(12):
    px, py = np.random.default_rng(r + 100).integers(40, N - 40, 2)
    draw.ellipse([px - 10, py - 10, px + 10, py + 10], fill=255)
lines = np.asarray(img, dtype=np.float64) / 255.0
seams = np.clip(blur(lines, 3) * 3.0, 0, 1)
color = rgb([0.09, 0.1, 0.11]) * (0.8 + 0.4 * fbm(32, 3, seed=110))[..., None]
glow = lines[..., None] * rgb([0.2, 0.85, 1.0])
T = {"color": save("s2vfx_k_panel_color.tga", color), "normal": save("s2vfx_k_panel_normal.tga", normals(-seams, 8.0)),
     "glow": save("s2vfx_k_panel_glow.tga", glow)}
vmat("s2vfx_kit_glow_panel", "s2vfx_lit.vfx", [
    'F_SPECULAR 1', 'F_SELF_ILLUM 1', 'TextureColor "%s"' % T["color"], 'TextureNormal "%s"' % T["normal"],
    'g_flMetalness "0.800"', 'g_flRoughnessRemapMin "0.350"', 'g_flRoughnessRemapMax "0.350"',
    'TextureSelfIllumMask "%s"' % T["glow"], 'g_flSelfIllumBrightness "8.000"', 'g_flSelfIllumScale "1.000"',
    'g_flSelfIllumAlbedoFactor "0.000"'])

# ------------------------------------------------------------------------------------------ sign
# A round sign with an arrow, see-through round it: the colour, and the translucency a separate grey texture
# (white solid, black clear).
img = Image.new("RGB", (N, N), (0, 0, 0))
mask = Image.new("L", (N, N), 0)
d, m = ImageDraw.Draw(img), ImageDraw.Draw(mask)
for draw_, fill in ((d, (230, 170, 20)), (m, 255)):
    draw_.ellipse([60, 60, N - 60, N - 60], fill=fill)
d.ellipse([110, 110, N - 110, N - 110], fill=(30, 30, 30))
d.polygon([(260, 470), (620, 470), (620, 340), (800, 512), (620, 684), (620, 554), (260, 554)], fill=(240, 240, 240))
T = {"color": "%s/%s" % (REL, "s2vfx_k_sign_color.tga"), "mask": "%s/%s" % (REL, "s2vfx_k_sign_mask.tga")}
img.save(os.path.join(FOLDER, "s2vfx_k_sign_color.tga"))
mask.save(os.path.join(FOLDER, "s2vfx_k_sign_mask.tga"))
vmat("s2vfx_kit_sign", "s2vfx_unlit.vfx", [
    'F_TRANSLUCENT 1', 'TextureColor "%s"' % T["color"], 'TextureTranslucency "%s"' % T["mask"]])

# ------------------------------------------------------------------------------------------ stained glass
# Cells of coloured glass in lead lines: each cell a colour, the glass a little bumpy, one cell in four frosted.
S = 256
pts = np.random.default_rng(200).random((40, 2))
yy, xx = (np.mgrid[0:S, 0:S] + 0.5) / S
best, second, owner = np.full((S, S), 9.0), np.full((S, S), 9.0), np.zeros((S, S), int)
for k, (px, py) in enumerate(pts):
    for ox in (-1, 0, 1):
        for oy in (-1, 0, 1):
            dd = np.hypot(xx - px - ox, yy - py - oy)
            closer = dd < best
            second = np.where(closer, best, np.minimum(second, dd))
            owner = np.where(closer, k, owner)
            best = np.minimum(best, dd)
up = lambda a, mode=Image.BICUBIC: np.asarray(Image.fromarray(a.astype(np.float32), "F").resize((N, N), mode))
lead = np.clip(1.0 - (up(second - best) - 0.008) / 0.01, 0, 1)
owner = np.asarray(Image.fromarray(owner.astype(np.uint8), "L").resize((N, N), Image.NEAREST)).astype(int)
palette = np.array([[0.75, 0.1, 0.1], [0.1, 0.25, 0.75], [0.9, 0.7, 0.1], [0.1, 0.6, 0.25], [0.55, 0.15, 0.65],
                    [0.9, 0.9, 0.8]])
pick = np.random.default_rng(201).integers(0, len(palette), len(pts))
color = palette[pick[owner]] * (0.9 + 0.2 * fbm(16, 3, seed=210))[..., None]
color = color * (1 - lead[..., None]) + rgb([0.05, 0.05, 0.05]) * lead[..., None]
frosted = (np.random.default_rng(202).random(len(pts)) < 0.25)[owner]
height = fbm(12, 4, seed=220) * 0.5 - lead * 0.6
rough = np.where(frosted, 0.55, 0.03) * (1 - lead) + lead * 0.9
T = {k: save("s2vfx_k_glass_%s.tga" % k, v) for k, v in
     (("color", color), ("normal", normals(height, 18.0)), ("rough", rough))}
vmat("s2vfx_kit_stained_glass", "s2vfx_glass.vfx", [
    'TextureColor "%s"' % T["color"], 'TextureNormal "%s"' % T["normal"], 'TextureRoughness "%s"' % T["rough"],
    'g_flIor "1.520"', 'g_flThickness "6.000"'])

# ------------------------------------------------------------------------------------------ pond
# Small ripples from the wind, in a detail normal map that tiles: the shader's own waves do the big swell.
height = fbm(16, 5, seed=300) * 0.7 + fbm(48, 3, seed=310) * 0.3
T = {"normal": save("s2vfx_k_pond_normal.tga", normals(height, 18.0))}
vmat("s2vfx_kit_pond", "s2vfx_water.vfx", [
    'TextureDetailNormal "%s"' % T["normal"], 'g_flDetailNormalStrength "1.500"', 'g_flDetailNormalScale "96.000"'])

# ------------------------------------------------------------------------------------------ shield
# Flow: a slow swirl round the centre (red and green, 0.5 still); Normal: hexagonal cells; Rim Ramp: the ring's
# colours from its inside (left) to its outside (right).
cy, cx = (np.mgrid[0:N, 0:N] + 0.5) / N - 0.5
r = np.hypot(cx, cy) + 1e-6
flow = np.dstack([-cy / r * 0.5 + 0.5, cx / r * 0.5 + 0.5, np.zeros_like(r)])
qx, qy = (np.mgrid[0:N, 0:N][1] + 0.5) / N * 12.0, (np.mgrid[0:N, 0:N][0] + 0.5) / N * 12.0 / 0.866
row = np.floor(qy)
hx = qx + 0.5 * (row % 2)
hexd = np.maximum(np.abs(hx % 1.0 - 0.5), np.abs(qy % 1.0 - 0.5) * 0.9)
cells = np.clip((0.46 - hexd) / 0.08, 0, 1)
ramp = np.zeros((16, 256, 3))
for i in range(256):
    t = i / 255.0
    ramp[:, i] = np.interp(t, [0.0, 0.4, 0.75, 1.0], [0.0, 0.1, 0.6, 1.0]) * np.array(
        [np.interp(t, [0, 0.5, 1], [0.1, 0.3, 0.9]), np.interp(t, [0, 0.5, 1], [0.4, 0.8, 1.0]), 1.0])
T = {"flow": save("s2vfx_k_shield_flow.tga", flow), "normal": save("s2vfx_k_shield_normal.tga", normals(cells, 6.0)),
     "ramp": save("s2vfx_k_shield_ramp.tga", ramp)}
# Energy draws only the parts its features turn on: refraction, the rim, the shell's glow and the reflections here.
vmat("s2vfx_kit_shield", "s2vfx_energy.vfx", [
    'F_REFRACTION 1', 'F_RIM 1', 'F_SHELL 1', 'F_REFLECTION 1',
    'TextureFlow "%s"' % T["flow"], 'TextureNormal "%s"' % T["normal"], 'TextureRimRamp "%s"' % T["ramp"],
    'g_flNormalStrength "3.000"', 'g_flRefractStrength "2.000"', 'g_vRimColor "[1.000000 1.000000 1.000000 0.000000]"',
    'g_flRimBrightness "3.000"', 'g_flRimPosition "0.500"', 'g_flRimWidth "1.000"', 'g_flRimOpacity "0.800"',
    'g_flShellOpacity "0.000"', 'g_vShellGlowColor "[0.200000 0.700000 1.000000 0.000000]"', 'g_flShellGlow "2.000"',
    'g_flReflectionStrength "1.500"'])

print("written into", FOLDER)
