# S2FM Mod Pack

Mods for Source 2 Filmmaker (Half-Life: Alyx Workshop Tools, the 6 May 2022 build), in one package:

- **ReShade** inside S2FM's own frame, at three points (after the opaque surfaces, every render sample, the
  finished picture), with the scene's depth and a panel in the Tools menu.
- **Particle refraction** in S2FM's viewports (the frame copy the game leaves out).
- **RTAO**, ray traced ambient occlusion in S2FM's viewports and renders, driven by the camera's SSAO settings.
  Needs DirectX Raytracing 1.1; off on other cards.

All three run in one `dxgi.dll` placed beside the game (see [docs/why_dxgi.md](docs/why_dxgi.md)).

User instructions are in [installer/README.txt](installer/README.txt), which ships in the package.

## Layout

| Folder | What |
|---|---|
| `core/` | the `dxgi.dll` core: D3D11 hooks, constant buffer copies, views, crash reports |
| `common/` | the shader table (built from the game's `.vcs` files), the S2FM document reader |
| `reshade/` | ReShade's stages (`stages.cpp`), the ReShade core (`core/`), the panel (`tool/`), tests (`harness/`) |
| `refraction/` | particle refraction and its blur |
| `rtao/` | RTAO: capture, D3D12 rays, the pass over the picture ([docs/rtao.md](docs/rtao.md)) |
| `package/` | which features the core registers, the version |
| `installer/` | `Install.bat`, `Uninstall.bat`, `s2fm_mods.ps1`, the user readme |
| `tools/` | SDK setup, the package builder, the icon |
| `tests/` | `installer_test.py`: install and uninstall on a test drive |

## Building

With Visual Studio 2026 (x64 tools), the Windows SDK, Git, Python 3 with `pefile`, and the game installed:

1. Once:
   - `python tools/reshade_sdk.py`: ReShade 6.8.0's source with our patch, into `third_party/reshade`.
   - Unpack Qt 4.8.3's source (`qt-everywhere-opensource-src-4.8.3` from
     https://download.qt.io/archive/qt/4.8/4.8.3/) into `third_party/`, then `python tools/qt4_sdk.py`: the
     headers, import libraries for the game's Qt DLLs, and moc. The game is looked for in Steam's default folder;
     set `S2FM_GAME_BIN` to its `game\bin\win64` otherwise.
2. `build_core.cmd`: `bin/core/dxgi.dll`.
3. `build_reshade.cmd`: `reshade/bin/s2fm_reshade_core.dll`, the panel `reshade/bin/s2fm_reshade.dll` and
   `reshade/bin/rs_harness.exe <test>` (`preset` and `assign` need `S2FM_TEST_PRESET` set to a preset file).
4. `python tools/make_package.py`: `dist/S2FM_Mod_Pack_<version>.zip`.

The version is in `package/version.h`.

## Installing for development

With the game closed:

    powershell -ExecutionPolicy Bypass -File installer\s2fm_mods.ps1 -Action install
    powershell -ExecutionPolicy Bypass -File installer\s2fm_mods.ps1 -Action uninstall [-Purge]

Run from the repository, the script takes the built files from the build folders. `-Game "X:\...\Half-Life Alyx"`
gives the game folder; without it the game is found through Steam's library folders.

## Credits

- [ReShade](https://reshade.me) by Patrick Mours (crosire) and contributors, BSD 3-Clause
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu, with the Hacker Disassembler Engine by
  Vyacheslav Patkov, BSD 2-Clause
- Used by ReShade: [stb](https://github.com/nothings/stb) by Sean Barrett,
  [fpng](https://github.com/richgel999/fpng) by Richard Geldreich, Jr.,
  the [JPEG XL](https://github.com/libjxl/libjxl) simple lossless encoder by the JPEG XL Project Authors,
  [UTF8-CPP](https://github.com/nemtrif/utfcpp) by Nemanja Trifunovic
- The panel uses the game's own Qt 4 libraries (not included)
- The shader file format follows [ValveResourceFormat](https://github.com/ValveResourceFormat/ValveResourceFormat)

Source 2 Filmmaker and Half-Life: Alyx are made by Valve. This project contains no Valve files and is not made or
endorsed by Valve.

## License

BSD 3-Clause, see [LICENSE.txt](LICENSE.txt). Third party licenses are collected into `THIRD_PARTY_NOTICES.txt` in
the package by `tools/make_package.py`.
