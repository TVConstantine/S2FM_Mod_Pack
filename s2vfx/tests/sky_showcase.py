"""Test materials for s2vfx_sky, on the weather shader's noises (run tests/weather_noise.py first):

    s2vfx_sky_day        a clear morning with fair weather cumulus
    s2vfx_sky_sunset     the sun at the horizon, more haze, the clouds lit from below
    s2vfx_sky_night      the sun well below, stars, the Milky Way and a half moon
    s2vfx_sky_overcast   heavy cover, a low thick layer
    s2vfx_sky_seed7      the day's clouds with another seed
    s2vfx_sky_s1 .. s6   the day's settings, six seeds (Seed Variety 1): six kinds of sky
    s2vfx_sky_aurora     a dark night with the aurora over the north, a thin crescent low down
    s2vfx_sky_moonlit    a full moon high up lighting the night air and the clouds
    s2vfx_sky_galaxy     a moonless night, the Milky Way's core overhead with its dust and nebulae
    s2vfx_sky_morning    Time of Day at 7:30, the sun just up in the east
    s2vfx_sky_afternoon  an afternoon, half cloudy, some cirrus, seed 3

    python tests/sky_showcase.py [content folder]

Compile the .vmat files with resourcecompiler.
"""
import os
import sys

FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\content\core\materials\s2vfx_test")
REL = "materials/s2vfx_test"
NOISE = ['\tTextureShapeNoise "%s/s2vfx_o_volume_shape.tga"' % REL,
         '\tTextureDetailNoise "%s/s2vfx_o_volume_detail.tga"' % REL]


def vmat(name, values):
    lines = ['\tshader "s2vfx_sky.vfx"'] + NOISE + ['\t%s "%s"' % kv for kv in values.items()]
    with open(os.path.join(FOLDER, name + ".vmat"), "w", newline="\n") as f:
        f.write("Layer0\n{\n" + "\n".join(lines) + "\n}\n")


vmat("s2vfx_sky_day", {"g_flSunElevation": "35.000", "g_flCloudCoverage": "0.450"})
vmat("s2vfx_sky_sunset", {"g_flSunElevation": "2.000", "g_flSunAzimuth": "200.000", "g_flHaze": "2.500",
                          "g_flCloudCoverage": "0.400", "g_flCirrus": "1.000"})
vmat("s2vfx_sky_night", {"g_flSunElevation": "-25.000", "g_flStars": "1.000", "g_flMilkyWay": "1.500",
                         "g_flMoonElevation": "35.000", "g_flMoonAzimuth": "120.000", "g_flCloudCoverage": "0.250",
                         "g_flExposure": "4.000"})
vmat("s2vfx_sky_overcast", {"g_flSunElevation": "40.000", "g_flCloudCoverage": "0.900", "g_flCloudDensity": "3.000",
                            "g_flCloudBottom": "0.800", "g_flCloudThickness": "3.000"})
vmat("s2vfx_sky_seed7", {"g_flSunElevation": "35.000", "g_flCloudCoverage": "0.450", "g_flSeed": "7.000"})
# The same settings, six seeds: each its own kind of sky.
for seed in range(1, 7):
    vmat("s2vfx_sky_s%d" % seed, {"g_flSunElevation": "35.000", "g_flCloudCoverage": "0.450", "g_flSeed": "%d.000" % (seed * 37)})
night = {"g_flSunElevation": "-25.000", "g_flExposure": "4.000", "g_flCloudCoverage": "0.200"}
vmat("s2vfx_sky_aurora", dict(night, g_flAurora="2.000", g_flAuroraAzimuth="90.000", g_flMoonElevation="8.000",
                              g_flMoonAzimuth="200.000", g_flMilkyWay="1.000"))
vmat("s2vfx_sky_moonlit", {"g_flSunElevation": "-30.000", "g_flSunAzimuth": "45.000", "g_flExposure": "4.000",
                           "g_flMoonElevation": "50.000", "g_flMoonAzimuth": "225.000", "g_flMoonGlow": "1.000",
                           "g_flCloudCoverage": "0.400"})
vmat("s2vfx_sky_galaxy", dict(night, g_flMoonElevation="-20.000", g_flMilkyWay="3.000", g_flMilkyWayTilt="90.000",
                              g_flNebulae="1.500", g_flStars="1.500", g_flCloudCoverage="0.000"))
vmat("s2vfx_sky_morning", {"g_bTimeOfDay": "1", "g_flTimeOfDay": "7.500", "g_flNoonElevation": "55.000",
                           "g_flCloudCoverage": "0.400", "g_flCirrus": "0.600"})
vmat("s2vfx_sky_afternoon", {"g_flSunAzimuth": "220.000", "g_flSunElevation": "28.000", "g_flCloudCoverage": "0.500",
                             "g_flCirrus": "0.400", "g_flSeed": "3.000"})
print("written into", FOLDER)
