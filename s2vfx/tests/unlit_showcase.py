"""Test materials for s2vfx_unlit's choices, each on its own (for a row of spheres in S2FM):

    s2vfx_unlit_opaque        a colour checker, an orange tint
    s2vfx_unlit_translucent   blended, opacity from a grey ramp (an RGB image: its grey is the translucency)
    s2vfx_unlit_trans_rgba    blended, opacity from an RGBA image whose colour is red and whose alpha is the ramp
                              (which of its channels the translucency takes)
    s2vfx_unlit_additive      added, 4 times as bright
    s2vfx_unlit_alphatest     cut out: rings
    s2vfx_unlit_tiled         the checker 4 x 4 times, scrolling
    s2vfx_unlit_tintmask      the tint only on the upper half
    s2vfx_unlit_backdrop      coloured stripes, both sides, no shadow: a backdrop for the others

    python tests/unlit_showcase.py [content folder]

Writes the textures (s2vfx_u_*.tga) and the .vmat files into the folder (default: core's materials/s2vfx_test);
compile the .vmat files with resourcecompiler.
"""
import os
import sys

import numpy as np
from PIL import Image

SIZE = 256
FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\content\core\materials\s2vfx_test")
REL = "materials/s2vfx_test"
os.makedirs(FOLDER, exist_ok=True)

y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64) / SIZE


def save(name, a):
    a = np.clip(a, 0.0, 1.0)
    img = Image.fromarray((a * 255 + 0.5).astype(np.uint8), "RGBA" if a.ndim == 3 and a.shape[2] == 4 else
                          "RGB" if a.ndim == 3 else "L")
    if img.mode == "L":
        img = img.convert("RGB")
    img.save(os.path.join(FOLDER, name + ".tga"))


checker = ((np.floor(x * 8) + np.floor(y * 8)) % 2)[..., None] * np.array([0.9, 0.9, 0.9]) + 0.1
save("s2vfx_u_checker", checker)
ramp = x
save("s2vfx_u_ramp", ramp)
rgba = np.dstack([np.ones_like(x), np.zeros_like(x) + 0.2, np.zeros_like(x) + 0.2, ramp])
save("s2vfx_u_ramp_rgba", rgba)
r = np.hypot(x - 0.5, y - 0.5)
save("s2vfx_u_rings", (np.sin(r * 60.0) > 0).astype(np.float64))
save("s2vfx_u_upper", (y < 0.5).astype(np.float64))
# A backdrop for the board: bright stripes of colour, so what is see-through shows them.
stripes = np.floor(x * 12) % 3
save("s2vfx_u_stripes", np.stack([stripes == 0, stripes == 1, stripes == 2], axis=-1).astype(np.float64) * 0.8 + 0.2)


def vmat(name, lines):
    body = ['\tshader "s2vfx_unlit.vfx"', '\tTextureColor "%s/s2vfx_u_checker.tga"' % REL] + lines
    with open(os.path.join(FOLDER, name + ".vmat"), "w", newline="\n") as f:
        f.write("Layer0\n{\n" + "\n".join(body) + "\n}\n")


with open(os.path.join(FOLDER, "s2vfx_unlit_backdrop.vmat"), "w", newline="\n") as f:
    f.write('Layer0\n{\n\tshader "s2vfx_unlit.vfx"\n\tTextureColor "%s/s2vfx_u_stripes.tga"\n'
            '\tF_RENDER_BACKFACES 1\n\tF_DO_NOT_CAST_SHADOWS 1\n}\n' % REL)
vmat("s2vfx_unlit_opaque", ['\tg_vColorTint "[1.000000 0.600000 0.200000 0.000000]"'])
vmat("s2vfx_unlit_translucent", ['\tF_TRANSLUCENT 1', '\tTextureTranslucency "%s/s2vfx_u_ramp.tga"' % REL])
vmat("s2vfx_unlit_trans_rgba", ['\tF_TRANSLUCENT 1', '\tTextureTranslucency "%s/s2vfx_u_ramp_rgba.tga"' % REL])
vmat("s2vfx_unlit_additive", ['\tF_ADDITIVE_BLEND 1', '\tg_flBrightness "2.000"',
                              '\tg_vColorTint "[0.200000 0.700000 1.000000 0.000000]"'])
vmat("s2vfx_unlit_alphatest", ['\tF_ALPHA_TEST 1', '\tTextureTranslucency "%s/s2vfx_u_rings.tga"' % REL])
vmat("s2vfx_unlit_tiled", ['\tg_vTexCoordScale "[4.000 4.000]"', '\tg_vTexCoordScrollSpeed "[0.200 0.000]"'])
vmat("s2vfx_unlit_tintmask", ['\tF_TINT_MASK 1', '\tTextureTintMask "%s/s2vfx_u_upper.tga"' % REL,
                              '\tg_vColorTint "[1.000000 0.100000 0.100000 0.000000]"'])
print("written into", FOLDER)
