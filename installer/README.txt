S2FM Mod Pack
=============

Mods for Source 2 Filmmaker (Half-Life: Alyx Workshop Tools, the 6 May 2022 build):

- ReShade inside S2FM's own frame, with a panel in the Tools menu. Effects can run at three points:
  A after the opaque surfaces, B on every render sample, C on the finished picture.
- Particle refraction (shock waves, heat haze) in S2FM's viewports.
- RTAO: ray traced ambient occlusion in S2FM's viewports and renders.


Requirements
------------

- Half-Life: Alyx with the Workshop Tools, the 6 May 2022 build. On newer builds the effects still run, but the
  camera's SSAO settings, ReShade's timer and the refraction blur settings are not read.
- RTAO needs a graphics card with DirectX Raytracing 1.1 (NVIDIA RTX 20 series or newer, AMD RX 6000 or newer,
  Intel Arc). On other cards RTAO stays off; ReShade and refraction still work.


Install
-------

1. Close Half-Life: Alyx.
2. Unzip this folder anywhere.
3. Double-click Install.bat.

The game is found through Steam, on any drive. If it is not found, you are asked for the Half-Life Alyx folder.
If Windows says access is denied, right-click Install.bat and choose "Run as administrator".

If a dxgi.dll is already in game\bin\win64 (for example another ReShade), it is moved to
game\bin\win64\s2fm_mods_backup\ and put back on uninstall. Only one dxgi.dll can be loaded, so that one does not
run while S2FM Mod Pack is installed.


First start
-----------

The first start after installing takes about 40 seconds longer: a table of the game's shaders is built once and
kept in game\bin\win64\s2fm_shader_table.bin.


ReShade effects
---------------

Put ReShade effects (.fx files and their folders) here:

    Half-Life Alyx\game\bin\win64\reshade-shaders\Shaders\
    Half-Life Alyx\game\bin\win64\reshade-shaders\Textures\

This is the same folder ReShade itself uses. If you have a ReShade.ini in game\bin\win64, its EffectSearchPaths and
TextureSearchPaths are used instead. Paths may be on any drive.

Open the panel from S2FM's Tools menu: ReShade. Choose a preset, or make one with New and add effects from the list.
Presets are .ini files in game\bin\win64.


Using the ReShade panel
-----------------------

Top bar:
- Preset: choose a preset. "(none)" turns the effects off.
- New: makes an empty preset. Add effects to it, then save it with Preset > Save (Ctrl+S).
- Reload: compiles the effects again (F5), for example after editing an .fx file.
- Effects: turns all effects off and on, to compare.
- RTAO and rays: see RTAO below.

Left, "In this preset": the preset's effects, grouped by where they run.
- A, after the opaque surfaces: before sky, fog, glass and particles. For ambient occlusion and global illumination.
- B, every sample, after post: on every render sample. For depth of field, anti-aliasing and bloom.
- C, the finished picture: once, at the end. For colour grading, LUTs, sharpening and grain.
- Each effect is placed automatically. To move it, drag it to another group, or right-click it.
- Tick or untick an effect to turn it on or off. Drag it up or down to change the order.

Left, "Add an effect": every effect in the effects folder, grouped by collection.
- Type in the search box to find an effect by name or collection.
- Double-click an effect, or select it and press "Add to the preset". Effects already in the preset are marked.

Right: the settings of the selected effect. Changes show in the viewport at once. The arrow button next to a
setting puts it back to its default. Changes are written to the preset file only when you save.

Bottom bar: how many effects compiled, the picture size, and what each group costs per frame. Click the first
part to see compiler messages.


Particle refraction
-------------------

Refraction bends what is behind a particle, for shock waves, heat haze and ripples. Half-Life: Alyx turned it off;
S2FM Mod Pack brings it back in S2FM's viewports and renders. It is set up in the Particle Editor, on the particle's
Render Sprites renderer:

1. Refraction group: tick "refract background".
2. "texture": a refraction texture (a normal map style texture: red and green give the direction of the bend,
   alpha gives its strength). Use the main "texture" slot; the separate normal map slot is not used.
3. "refract amount": how strong the bend is (-2 to 2).
4. "refract blur radius": blurs what is seen through the particle, in pixels of a 1080 line picture.
   "refract blur type": Gaussian (soft) or Box.

For a clean bend with no colour of its own:
- untick "use additive blending";
- set "self illum amount" to 1 and "diffuse lighting amount" to 0;
- keep the particle's colour white.
With additive blending on, the bent picture is added on top and the particle looks bright.


RTAO
----

RTAO is switched on and off in the ReShade panel (top right), where the rays per pixel are set too. How far and how
dark it reaches is set by the camera's SSAO Radius, Bias and Strength in S2FM.


Crash reports
-------------

If the game crashes, a report is written to:

    Half-Life Alyx\game\bin\win64\s2fm_mods_crash\

Each crash gives a .txt and a .dmp file. Please send both when reporting a problem. The .txt file says whether an
S2FM Mod Pack file was involved.


Uninstall
---------

Close the game and double-click Uninstall.bat. Everything is put back as it was. Your presets and settings in
game\bin\win64\s2fm_reshade\ are kept. To remove those too, run from a command prompt in this folder:

    Uninstall.bat -Purge

If Steam's "Verify integrity of game files" is run, the ReShade entry in the Tools menu is removed. Run Install.bat
again to add it back.


Credits
-------

- ReShade by Patrick Mours (crosire) and contributors - https://reshade.me
- MinHook by Tsuda Kageyu - https://github.com/TsudaKageyu/minhook
- Hacker Disassembler Engine by Vyacheslav Patkov (part of MinHook)
- stb by Sean Barrett (used by ReShade)
- fpng by Richard Geldreich, Jr. (used by ReShade)
- JPEG XL simple lossless encoder by the JPEG XL Project Authors (used by ReShade)
- UTF8-CPP by Nemanja Trifunovic (used by ReShade)
- The panel uses the Qt 4 libraries that come with the game.

Source 2 Filmmaker and Half-Life: Alyx are made by Valve. This package contains no Valve files and is not made or
endorsed by Valve.


License
-------

S2FM Mod Pack is under the BSD 3-Clause License (LICENSE.txt). The licenses of the parts above are in
THIRD_PARTY_NOTICES.txt.
