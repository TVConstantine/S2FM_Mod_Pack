# Why a dxgi.dll

The mods need to see the engine's D3D11 work from the start. They come in as `dxgi.dll` beside the game, which the
game loads by itself. An engine tool (a DLL listed in `game/bin/sdkenginetools.txt`) would be simpler to install,
but it loads too late.

## Measured

The core logs the start order ("start order" lines): when the game's modules and each tool load, against the device
and the shaders made so far.

| Seconds after the core | What | Device | Shaders made |
|---|---|---|---|
| 0.0 | the core; engine2, rendersystemdx11 and d3d11 already loaded | not yet | 0 |
| 1.0 | the device is made | made | 0 |
| 1.8 | the swapchain, the first present | made | 13 |
| 2.2 | client.dll, toolframework2.dll | made | 537 |
| 29.0 | sfm.dll (S2FM itself is a tool) | made | 7,656 |
| 48 | the session's first frames | made | 9,863 |

Tools listed in `sdkenginetools.txt` load only when opened from the Tools menu, or when S2FM reopens one left open,
after sfm.dll.

## What a tool would miss

- **Shader bytecode.** D3D11 cannot give it back after a shader is made. The shader table names every shader by it,
  which is how all three parts find their draws, and RTAO builds its capture shaders from it.
- **Constant buffer copies.** Made when each buffer is made (camera, fog, alpha reference).

So both parts live in one `dxgi.dll` and ship as one package. RTAO is applied before ReShade's stage A by the order
the core registers them in.
