"""A copy of test materials lit by the mod pack's baked sky (Sky Light 1), each beside its original as <name>_sky.vmat:

    python tests/sky_light_variants.py [content folder]

Takes s2vfx_glass_clear, s2vfx_glass_frosted, s2vfx_water_default and s2vfx_energy_core (written by the showcase
scripts). Compile the .vmat files with resourcecompiler.
"""
import os
import sys

FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\content\core\materials\s2vfx_test")
NAMES = ["s2vfx_glass_clear", "s2vfx_glass_frosted", "s2vfx_water_default", "s2vfx_energy_core"]

for name in NAMES:
    with open(os.path.join(FOLDER, name + ".vmat"), encoding="utf-8") as f:
        lines = f.read().splitlines()
    lines = [l for l in lines if "g_flSkyLight" not in l]
    shader = next(i for i, l in enumerate(lines) if l.strip().startswith("shader "))
    lines.insert(shader + 1, '\tg_flSkyLight "1.000"')
    with open(os.path.join(FOLDER, name + "_sky.vmat"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("written", name + "_sky.vmat")
