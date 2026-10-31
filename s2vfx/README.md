# s2vfxc

A shader compiler for the Half-Life: Alyx Workshop Tools (the 6 May 2022 build). It turns `.vfx` shader source into
the `.vcs` files the engine loads. Valve did not ship their shader compiler; this one is written from scratch.

Released with the S2FM Mod Pack 1.1.0. The shaders here are made for Source 2 Filmmaker (S2FM).


## What you need

- Windows 10 or 11, 64-bit.
- Half-Life: Alyx with the Workshop Tools (the 6 May 2022 build).
- The S2FM Mod Pack for four of the example shaders (see "The S2FM Mod Pack" below).
- For the example material scripts: Python 3 with numpy and Pillow (`pip install numpy pillow`).
- Only to build the compiler yourself: Visual Studio 2022 or later (C++) and CMake.


## Building (optional)

A built `s2vfxc.exe` comes in `compilerin\` of the release package (`S2FM_Mod_Pack_1.1.0.zip`). To build it yourself:

    cmake -S . -B build
    cmake --build build --config Release

The program is `build\Release\s2vfxc.exe`. It finds the `include\` folder by looking upward from where it is.


## Compiling a shader

Close Half-Life: Alyx and its tools first. Then, from this folder:

    bin\s2vfxc.exe examples\lit.vfx --name s2vfx_lit --install --s2fm

- `--install` writes the shader into the game (`game\shaders\vfx` and `game\core\shaders\vfx`).
- `--name` is the shader's name. A material uses it as `shader "s2vfx_lit.vfx"`.
- `--s2fm` builds only what S2FM draws with: faster, and half the size. A shader built this way does not draw in the
  game or in VR. Leave it out for a full build.

Other options:

- `--out <folder>` writes the files there instead of into the game.
- `--game <folder>` is the Half-Life: Alyx `game` folder, if it is not in Steam's default place.
- `-D NAME=VALUE` defines a value for the whole shader.
- `--only NAME=VALUE` builds one value of a combo and copies it to the others (quick test builds).
- `--quiet` prints less.

Compiling takes from a few seconds (unlit) to a few minutes (weather).


## The example shaders

| File | Install as | What it is | Needs the Mod Pack | Material script |
|---|---|---|---|---|
| `unlit.vfx` | `s2vfx_unlit` | a colour texture times a tint | no | - |
| `lit.vfx` | `s2vfx_lit` | lit surfaces: metal, roughness, clear coat, sheen, anisotropy, skin, glow, glitter, holographic | no | `tests\showcase.py` |
| `glass.vfx` | `s2vfx_glass` | clear, frosted and ice glass that bends what is behind it | yes | `tests\glass_showcase.py` |
| `water.vfx` | `s2vfx_water` | water with waves, refraction, depth colour, caustics, foam and floating debris | yes | `tests\water_debris.py` |
| `energy.vfx` | `s2vfx_energy` | a glowing, refracting energy core | yes | `tests\energy_showcase.py` |
| `weather.vfx` | `s2vfx_weather` | clouds, rain, snow, sleet, hail, smoke, fog and nebulae | yes | `tests\weather_showcase.py` |
| `sky.vfx` | `s2vfx_sky` | a procedural sky: atmosphere, clouds, sun, moon, stars, Milky Way, aurora; its HDRI and sky light | for the HDRI and sky light | `tests\sky_showcase.py` |

The release package's Manual.html describes every shader and its settings, how to make textures for them, and
this compiler in full.


## Example materials

The scripts write textures and `.vmat` files into the game's `content\core\materials\s2vfx_test` folder (give a
folder to write somewhere else, for example if the game is not in Steam's default place):

    python tests\weather_showcase.py
    python tests\water_foam_tex.py      (the water's foam, and s2vfx_water_default)

The other scripts: `showcase.py` and `lit_board.py` (lit), `unlit_showcase.py`, `glass_showcase.py`,
`water_debris.py`, `energy_showcase.py`, `sky_showcase.py`, `sky_light_variants.py` (materials lit by the sky) and
`texture_kit.py` (example texture sets, 1024 x 1024, for lit, unlit, glass, water and energy). `lit_board.py` uses
`unlit_showcase.py`'s textures and `sky_light_variants.py` the glass, water and energy materials: run them after.

Compile the materials with the game's resource compiler. From the Half-Life Alyx folder, for each material:

    game\bin\win64\resourcecompiler.exe -i content\core\materials\s2vfx_test\s2vfx_weather_rain.vmat

(Materials in `core` are not compiled by themselves when S2FM loads them; materials in an addon are.)

Then pick the material on a model in S2FM (the model's material override), or open it in the Material Editor and
change its settings there.

Your own materials are best kept in your addon (`content\hlvr_addons\<your addon>\materials\`): the tools compile
those by themselves. In the Material Editor a shader shows by its name, `s2vfx_weather` as "S2vfx Weather". A new
weather material needs the two noise textures (`TextureShapeNoise` `materials/s2vfx_test/s2vfx_o_volume_shape.tga`,
`TextureDetailNoise` `materials/s2vfx_test/s2vfx_o_volume_detail.tga`), or there are no clouds.


## Using the weather shader

- Put the material on `models/dev/sphere`. The shader builds its volume from the sphere. Scale the sphere for a
  bigger cloud.
- Shape: Ellipsoid for single clouds, Box for cloud layers. Rain, snow, sleet and hail fall only from Box layers.
- Precipitation falls from under the clouds down to whatever it meets ("Rain Length" is the longest fall). It only
  reaches the ground on the sphere, not on flat models.
- The engine does not put see-through materials in its shadow maps. For a cloud's shadow, place a second sphere at the
  same place with the `_shadow` version of the material (Shadow Only).
- The camera may be inside the volume (under a cloud layer, in the rain).


## The S2FM Mod Pack

`glass`, `water`, `energy` and `weather` read the scene's depth and a copy of the frame. The engine does not give
these to custom shaders; the S2FM Mod Pack (a `dxgi.dll` beside the game) does. Use the Mod Pack version that comes
with the release package (1.1.0 or newer). Without it these four draw wrong: no refraction, and weather draws over everything.

The Mod Pack also fixes a loop where materials of custom shaders in an addon compile again and again (with a large
addon).


## Known issues

- Tested in S2FM only, not in the game or in VR.
- The Mod Pack's RTAO darkens s2vfx_lit as it does the game's own surfaces. Glass, water, energy and
  weather (see-through), unlit and the sky are left out of it, as such surfaces are in the game.
- The weather shader takes a few minutes to compile.


## Reporting a problem

Send what you did, a screenshot or a short video, the compiler's output, and the console text from the tools
(vconsole) if anything shows there.


## Folders

| Folder | What |
|---|---|
| `src/` | the compiler in C++ |
| `include/s2vfx/` | include files for shaders: the engine's buffers, the vertex stage, lighting, fog, precipitation |
| `examples/` | the example shaders |
| `tests/` | the scripts that make the example materials; `cpp/roundtrip.cpp` reads every game shader file and writes it back byte for byte (a check of the file format) |
| `third_party/lzma/` | the LZMA SDK's C sources (public domain) |
| `docs/` | `writing_shaders.md` (how to write a shader) and `format_v64.md` (what is known about the `.vcs` files) |
