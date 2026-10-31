"""Test materials for s2vfx_lit, each choice on its own (for a row of spheres in S2FM), and our surface beside Valve's
(Valve Shading):

    s2vfx_lit_b_plastic / _valve     white plastic, roughness 0.5
    s2vfx_lit_b_rough / _valve       white, roughness 1 (the diffuse's shape: EON against Valve's)
    s2vfx_lit_b_gold / _valve        gold, roughness 0.3, its edges tinted (F82)
    s2vfx_lit_b_translucent          blended, opacity from a grey ramp
    s2vfx_lit_b_alphatest            cut out: rings
    s2vfx_lit_b_additive             added
    s2vfx_lit_b_tiled                a checker 4 x 4 times, its upper half tinted red (tint mask)
    s2vfx_lit_b_glow                 self illumination past the old limits (brightness 12, scale 3)
    s2vfx_lit_b_coat                 red paint under an amber coat with an orange peel normal map of its own
    s2vfx_lit_b_mirror               a near mirror metal (roughness 0.05): where a light's size shows most
    s2vfx_lit_b_sky_rough / _mirror / _gold / _metal   lit by the mod pack's baked sky (Sky Light 1): white
                                     roughness 1, the mirror, the gold, a silver metal of roughness 0.5

    python tests/lit_board.py [content folder]

Uses the textures tests/unlit_showcase.py writes (run it first). Compile the .vmat files with resourcecompiler.
"""
import os
import sys

FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\content\core\materials\s2vfx_test")
REL = "materials/s2vfx_test"


def vmat(name, lines):
    body = ['\tshader "s2vfx_lit.vfx"', '\tF_SPECULAR 1'] + lines
    with open(os.path.join(FOLDER, name + ".vmat"), "w", newline="\n") as f:
        f.write("Layer0\n{\n" + "\n".join(body) + "\n}\n")


def rough(r):
    return ['\tg_flRoughnessRemapMin "%.3f"' % r, '\tg_flRoughnessRemapMax "%.3f"' % r]


for valve in (False, True):
    tail = "_valve" if valve else ""
    extra = ['\tg_bValveShading "1"'] if valve else []
    vmat("s2vfx_lit_b_plastic" + tail, rough(0.5) + extra)
    vmat("s2vfx_lit_b_rough" + tail, rough(1.0) + extra)
    vmat("s2vfx_lit_b_gold" + tail, rough(0.3) + extra + [
        '\tg_vColorTint "[1.000000 0.820000 0.450000 0.000000]"', '\tg_flMetalness "1.000"',
        '\tg_vSpecularTint "[1.000000 0.700000 0.450000 0.000000]"'])
vmat("s2vfx_lit_b_translucent", rough(0.4) + ['\tF_TRANSLUCENT 1', '\tTextureTranslucency "%s/s2vfx_u_ramp.tga"' % REL,
                                             '\tg_vColorTint "[0.300000 0.600000 1.000000 0.000000]"'])
vmat("s2vfx_lit_b_alphatest", rough(0.6) + ['\tF_ALPHA_TEST 1', '\tTextureTranslucency "%s/s2vfx_u_rings.tga"' % REL])
vmat("s2vfx_lit_b_additive", rough(0.3) + ['\tF_ADDITIVE_BLEND 1', '\tTextureTranslucency "%s/s2vfx_u_ramp.tga"' % REL,
                                          '\tg_vColorTint "[1.000000 0.500000 0.100000 0.000000]"'])
vmat("s2vfx_lit_b_tiled", rough(0.5) + ['\tTextureColor "%s/s2vfx_u_checker.tga"' % REL,
                                       '\tg_vTexCoordScale "[4.000 4.000]"', '\tF_TINT_MASK 1',
                                       '\tTextureTintMask "%s/s2vfx_u_upper.tga"' % REL,
                                       '\tg_vColorTint "[1.000000 0.150000 0.150000 0.000000]"'])
vmat("s2vfx_lit_b_glow", rough(0.5) + ['\tF_SELF_ILLUM 1', '\tTextureSelfIllumMask "%s/s2vfx_u_rings.tga"' % REL,
                                      '\tg_flSelfIllumBrightness "12.000"', '\tg_flSelfIllumScale "3.000"',
                                      '\tg_vSelfIllumTint "[0.200000 1.000000 0.400000 0.000000]"',
                                      '\tg_flSelfIllumAlbedoFactor "0.000"'])
# A red car paint under a coat with an orange peel of its own (its normal map), the coat tinted amber: its tint deepens
# toward the outline.
import numpy as np
from PIL import Image
n = 256
rng = np.random.default_rng(7)
h = rng.standard_normal((n, n))
for _ in range(14):
    h = (h + np.roll(h, 1, 0) + np.roll(h, -1, 0) + np.roll(h, 1, 1) + np.roll(h, -1, 1)) / 5.0
gx, gy = np.gradient(h)
nx, ny = -gx * 25.0, -gy * 25.0
nz = np.ones_like(nx)
ln = np.sqrt(nx * nx + ny * ny + nz * nz)
img = np.dstack([nx / ln, ny / ln, nz / ln]) * 0.5 + 0.5
Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB").save(os.path.join(FOLDER, "s2vfx_l_peel_normal.tga"))
vmat("s2vfx_lit_b_coat", rough(0.45) + ['\tF_CLEARCOAT 1', '\tg_flClearcoatRoughness "0.040"',
                                       '\tg_vColorTint "[0.700000 0.050000 0.050000 0.000000]"',
                                       '\tg_vClearcoatTint "[1.000000 0.750000 0.350000 0.000000]"',
                                       '\tg_bClearcoatOwnNormalMap "1"', '\tg_flClearcoatNormalStrength "0.150"',
                                       '\tTextureClearcoatNormal "%s/s2vfx_l_peel_normal.tga"' % REL])
vmat("s2vfx_lit_b_mirror", rough(0.05) + ['\tg_flMetalness "1.000"', '\tg_vColorTint "[0.950000 0.950000 0.950000 0.000000]"'])
sky = ['\tg_flSkyLight "1.000"']
vmat("s2vfx_lit_b_sky_rough", rough(1.0) + sky)
vmat("s2vfx_lit_b_sky_mirror", rough(0.05) + sky + ['\tg_flMetalness "1.000"',
                                                   '\tg_vColorTint "[0.950000 0.950000 0.950000 0.000000]"'])
vmat("s2vfx_lit_b_sky_gold", rough(0.3) + sky + ['\tg_vColorTint "[1.000000 0.820000 0.450000 0.000000]"',
                                                '\tg_flMetalness "1.000"',
                                                '\tg_vSpecularTint "[1.000000 0.700000 0.450000 0.000000]"'])
vmat("s2vfx_lit_b_sky_metal", rough(0.5) + sky + ['\tg_flMetalness "1.000"',
                                                 '\tg_vColorTint "[0.900000 0.900000 0.900000 0.000000]"'])
print("written into", FOLDER)
