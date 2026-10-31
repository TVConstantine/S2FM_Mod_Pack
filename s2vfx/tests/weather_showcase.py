"""Materials for s2vfx_weather (examples/weather.vfx) and their textures, into core's materials/s2vfx_test:

    python tests/weather_showcase.py [content folder]

then compile the vmats with resourcecompiler. The noise atlases come from tests/weather_noise.py."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

import weather_noise  # noqa: E402

FOLDER = sys.argv[1] if len(sys.argv) > 1 else weather_noise.FOLDER
T = "materials/s2vfx_test/"
NOISE = ['TextureShapeNoise "%ss2vfx_o_volume_shape.tga"' % T, 'TextureDetailNoise "%ss2vfx_o_volume_detail.tga"' % T]


def ramp(name, keys):
    """A colour ramp, left to right, the same in every row."""
    xs = np.linspace(0, 1, 256)
    rgb = np.stack([np.interp(xs, [k for k, _ in keys], [c[i] for _, c in keys]) for i in range(3)], 1)
    img = (np.repeat((np.clip(rgb, 0, 1) ** (1 / 2.2))[None], 8, axis=0) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(img, "RGB").save(os.path.join(FOLDER, name))


def vmat(name, body):
    with open(os.path.join(FOLDER, name + ".vmat"), "w") as f:
        f.write('Layer0\n{\n\tshader "s2vfx_weather.vfx"\n' + "".join("\t%s\n" % l for l in body) + "}\n")


V = lambda x, y, z: '"[%f %f %f 0.000000]"' % (x, y, z)
PRESETS = {
    "s2vfx_weather_fair": ['g_flCoverage "0.55"', 'g_flWispiness "0.35"', 'g_flCloudWindSpeed "0.03"'],
    "s2vfx_weather_cloudy": ["F_SHAPE 1", "g_vShapeSize " + V(1, 1, 0.35), 'g_flCoverage "0.65"', 'g_flCloudSize "0.6"',
                            'g_flCloudBase "0.1"', 'g_flCloudTop "0.9"'],
    "s2vfx_weather_overcast": ["F_SHAPE 1", "F_CLOUD_TYPE 1", "g_vShapeSize " + V(1, 1, 0.25), 'g_flCoverage "0.9"',
                              'g_flDensity "0.8"', 'g_flAbsorption "0.15"', 'g_flAmbientStrength "1.2"'],
    "s2vfx_weather_storm": ["F_SHAPE 1", "F_CLOUD_TYPE 2", "F_RAIN 1", "F_LIGHTNING 1", "g_vShapeSize " + V(1, 1, 0.6),
                           'g_flCoverage "0.7"', 'g_flDensity "1.5"', 'g_flAbsorption "0.3"', 'g_flCloudBase "0.35"',
                           'g_flCloudSize "0.5"', 'g_flLightningRate "0.5"'],
    "s2vfx_weather_cirrus": ["F_SHAPE 1", "F_CLOUD_TYPE 3", "g_vShapeSize " + V(1, 1, 0.2), 'g_flCoverage "0.5"',
                            'g_flDensity "0.3"', 'g_flWispiness "0.8"', 'g_flDetailScale "6.0"'],
    "s2vfx_weather_sunset": ['g_flCoverage "0.55"', 'g_flLightStrength "1.5"', 'g_flSilverLining "0.85"',
                            'g_vAmbientColor "[0.250000 0.120000 0.080000 0.000000]"'],
    "s2vfx_weather_smoke": ['g_flCoverage "0.75"', 'g_flDensity "2.5"', 'g_flAbsorption "0.6"', 'g_flCloudSize "0.5"',
                           'g_flEvolution "0.15"', 'g_flCloudWindSpeed "0.0"'],
    "s2vfx_weather_explosion": ['g_flCoverage "0.8"', 'g_flDensity "2.5"', 'g_flAbsorption "0.6"', 'g_flCloudSize "0.5"',
                               'g_flEvolution "0.3"', "F_EMISSION 1", 'g_flEmissionStrength "6.0"',
                               'g_flEmissionCore "0.8"', 'TextureEmissionRamp "%ss2vfx_o_volume_fire_ramp.tga"' % T],
    "s2vfx_weather_groundfog": ["F_SHAPE 1", "F_CLOUD_TYPE 1", "g_vShapeSize " + V(1, 1, 0.15), 'g_flCoverage "0.8"',
                               'g_flDensity "0.4"', 'g_flCloudTop "0.6"', 'g_flWispiness "0.6"'],
    "s2vfx_weather_nebula": ["F_EMISSION 1", 'g_bUseSun "0"', 'g_bUseSfmLights "0"', 'g_flAmbientStrength "0.0"',
                            'g_flCoverage "0.6"', 'g_flDensity "0.6"', 'g_flEmissionStrength "0.6"', 'g_flEmissionCore "1.0"',
                            'g_vEmissionColor "[1.000000 1.000000 1.000000 0.000000]"',
                            'TextureEmissionRamp "%ss2vfx_o_volume_nebula_ramp.tga"' % T, 'g_bSeedByPosition "1"'],
}

# Precipitation: a layer (box) of clouds with its band high in the box, the kind and its settings below it.
LAYER = ["F_SHAPE 1", "g_vShapeSize " + V(1, 1, 0.5), 'g_flCoverage "0.7"', 'g_flCloudSize "0.5"', 'g_flCloudBase "0.45"',
         'g_flCloudTop "0.95"']
PRECIPITATION = {
    "s2vfx_weather_drizzle": ["F_RAIN 1", 'g_flRainAmount "0.3"', 'g_flDropSize "0.6"', 'g_flStreakLength "0.4"',
                              'g_flRainSpeed "0.7"', 'g_flVariation "0.2"', 'g_flRainDarkness "0.3"',
                              'g_flHazeDensity "0.6"'],
    "s2vfx_weather_rain": ["F_RAIN 1", 'g_flRainAmount "0.8"'],
    "s2vfx_weather_downpour": ["F_RAIN 1", 'g_flRainAmount "1.6"', 'g_flDropSize "1.3"', 'g_flWindLean "0.25"',
                               'g_flGustiness "0.6"', 'g_flVariation "0.6"', 'g_flDensity "1.4"',
                               'g_flHazeDensity "1.3"'],
    "s2vfx_weather_thunderstorm": ["F_RAIN 1", "F_LIGHTNING 1", "F_CLOUD_TYPE 2", 'g_flRainAmount "1.4"',
                                   'g_flGustiness "0.7"', 'g_flWindLean "0.3"', 'g_flDensity "1.5"',
                                   'g_flAbsorption "0.3"', 'g_flLightningRate "0.5"'],
    "s2vfx_weather_snow": ["F_RAIN 2", 'g_flRainAmount "0.8"', 'g_flWindLean "0.05"', 'g_flTurbulence "0.6"',
                           'g_flCoverage "0.85"', 'g_flAmbientStrength "1.3"'],
    "s2vfx_weather_blizzard": ["F_RAIN 2", 'g_flRainAmount "1.8"', 'g_flWindLean "0.6"', 'g_flGustiness "0.8"',
                               'g_flTurbulence "1.0"', 'g_flCoverage "0.95"', 'g_flAmbientStrength "1.3"',
                               'g_flHazeDensity "1.5"', 'g_flHazeNear "128.0"'],
    "s2vfx_weather_sleet": ["F_RAIN 3", 'g_flRainAmount "0.9"', 'g_flSleetMix "0.5"', 'g_flWindLean "0.15"'],
    "s2vfx_weather_hail": ["F_RAIN 4", "F_LIGHTNING 1", "F_CLOUD_TYPE 2", 'g_flRainAmount "0.9"', 'g_flDensity "1.5"'],
    "s2vfx_weather_virga": ["F_RAIN 1", 'g_flRainAmount "0.8"', 'g_flEvaporation "0.7"', 'g_flCoverage "0.55"'],
    "s2vfx_weather_rainbow_shower": ["F_RAIN 1", 'g_flRainAmount "0.7"', 'g_flRainbow "1.0"', 'g_flCoverage "0.5"',
                                     'g_flVariation "0.7"'],
}
# What each kind leaves on the ground (F_SURFACES: splashes, wetness, puddles, snow); virga never reaches it.
SURFACES = {
    "s2vfx_weather_drizzle": ['g_flWetness "0.6"', 'g_flPuddles "0.15"', 'g_flSplashAmount "0.5"'],
    "s2vfx_weather_rain": ['g_flPuddles "0.4"'],
    "s2vfx_weather_downpour": ['g_flPuddles "0.7"', 'g_flSplashAmount "1.5"'],
    "s2vfx_weather_thunderstorm": ['g_flPuddles "0.6"', 'g_flSplashAmount "1.3"'],
    "s2vfx_weather_snow": ['g_flSnowCover "1.0"'],
    "s2vfx_weather_blizzard": ['g_flSnowCover "1.0"'],
    "s2vfx_weather_sleet": ['g_flWetness "0.8"', 'g_flSnowCover "0.6"'],
    "s2vfx_weather_hail": ['g_flWetness "0.7"'],
    "s2vfx_weather_rainbow_shower": ['g_flWetness "0.7"', 'g_flPuddles "0.2"'],
}
for _name, _body in PRECIPITATION.items():
    # The layer's lines with the kind's on top: a kind may change a layer setting (one line a key).
    _lines = {line.split()[0]: line for line in LAYER}
    _lines.update({line.split()[0]: line for line in _body})
    if _name in SURFACES:
        _lines.update({line.split()[0]: line for line in ["F_SURFACES 1"] + SURFACES[_name]})
    PRESETS[_name] = list(_lines.values())

if __name__ == "__main__":
    if not os.path.exists(os.path.join(FOLDER, "s2vfx_o_volume_shape.tga")):
        weather_noise.write(FOLDER)
    ramp("s2vfx_o_volume_fire_ramp.tga", [(0.0, (0.0, 0.0, 0.0)), (0.35, (0.5, 0.05, 0.0)), (0.65, (1.0, 0.4, 0.05)),
                                           (1.0, (1.0, 0.9, 0.5))])
    ramp("s2vfx_o_volume_nebula_ramp.tga", [(0.0, (0.05, 0.0, 0.15)), (0.4, (0.5, 0.1, 0.6)), (0.7, (0.1, 0.5, 0.8)),
                                             (1.0, (0.6, 0.85, 1.0))])
    for name, body in PRESETS.items():
        vmat(name, body + NOISE)
        # Its shadow on its own (Shadow Only), for a copy of the cloud's model: the engine leaves see-through
        # materials out of its shadow maps. It leaves the surfaces to the cloud (F_SURFACES out).
        vmat(name + "_shadow", [l for l in body if not l.startswith("F_SURFACES")] + NOISE + ["F_SHADOW_ONLY 1"])
    # Draws nothing and casts nothing: the live test (volume_clouds.py, S2VFX_VOLUME_HIDE) puts it on a big volume of
    # the session's own, to keep it out of the pictures.
    vmat("s2vfx_weather_hidden", NOISE + ["F_SHADOW_ONLY 1", 'g_flCastShadow "0.0"'])
    print("written into", FOLDER)
