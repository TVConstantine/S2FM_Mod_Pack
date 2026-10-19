# RTAO

Ray traced ambient occlusion in S2FM's own viewports and renders.

## How it works

1. **Capture.** Every opaque draw that reads the engine's dynamic AO texture is drawn once more, right after the
   engine draws it, with the same vertex shader and a geometry shader made from that shader's bytecode. The geometry
   shader streams the positions out (no pixels are drawn). Skinning, morphs and object transforms are already
   applied, by the engine's own shader.
2. **Cut outs.** For a draw whose pixel shader discards (leaves, fences), the uv and vertex alpha are streamed out
   too, and the alpha of its colour texture is copied into a 512x512 mask. A ray that hits it passes where the alpha
   is under the material's alpha test reference.
3. **Structure.** On a D3D12 device on the same graphics card (DXR 1.1 needed), the positions become one bottom
   level structure and one top level structure. When the draws are the same as the frame before, the structure is
   refitted instead of rebuilt, and rebuilt every 8 frames.
4. **AO.** A compute shader with inline ray queries traces the AO for each pixel from the scene's depth. It follows
   the meaning of the camera's SSAO Radius, Bias and Strength sliders in S2FM. The rays go round the model's smooth
   normal (the vertex shader's own, `TEXCOORD1`, streamed out with the positions), so low polygon models do not
   show their faces in the AO; rays that would go under the face itself are turned back up.
5. **Blended draws** (glass, additive glows) are not occluders: they are left out of the rays.
   A map's 3D sky is left out too: the engine draws it with the camera's matrix but in the last part of the depth
   range (0.95 to 1), scaled up round the map, and its huge terrain would cross the real ground.
6. **Applied.** After the opaque surfaces, one full screen pass multiplies the picture by the AO. What fog, glowing
   surfaces and see-through draws add is kept out of the AO. A glowing surface's glow is kept out only where it is
   still in front at the end: a character drawn after it covers it.

The two devices meet only through shared fences, so the CPU never waits for the card.

## Settings

The ReShade panel has an RTAO switch and the rays per pixel. They are saved in `s2fm_rtao.ini` beside `dxgi.dll`.

## Test words

The first line of `s2fm_rtao_debug.txt` beside `dxgi.dll` (read four times a second):

| Word | Effect |
|---|---|
| `off` | no AO |
| `raysN` | rays per pixel, 1 to 16 |
| `radiusN`, `strengthN`, `biasN` | override the camera's sliders |
| `t9depth` | rays from the engine's own AO depth instead of the scene depth |
| `nofog` | AO over the fog too; `fogonly` shows only the fog |
| `debug` | normals in the dump |
| `measure` | picture means and timings into `s2fm_rtao_out.txt` |
| `dump` | with `measure`: the AO, depth and picture as image files |
| `norefit` | rebuild the structure every frame |
| `nocutouts`, `solidcutouts` | leave cut outs out, or trace them as solid |
| `flatnormals` | turn the AO by the faces' normals instead of the smooth ones |
| `solidblended` | trace blended draws as solid |
| `withsky` | trace the map's 3D sky too |
| `glowall` | add the glow of glowing surfaces even where something drawn later covers them |
| `not9` | ignore the engine's AO depth, as in a scene of models alone |
| `views` | log every view the engine draws for a few frames |
| `offscreen` | also take the shadow view's geometry (see below) |

A line that starts with `off` turns RTAO off, so put other words first.

## Off screen geometry

The engine draws only what the camera sees, so an object completely outside the view casts no AO. The rays already
see every object the camera draws, whole, so this matters only right at the edges of the frame.

`offscreen` also takes the geometry of the shadow view the engine draws before the camera. A triangle is left out
when any corner of it is inside the camera's view (the camera already has that object) or when it is beyond the
AO's reach. Measured in a test scene at 16 rays: one edge 0.007 darker, AO time 8 ms to 31 ms, 146 MB to 265 MB.
It is off by default.

Keeping triangles that only cross the edge of the view made a dark band at the base of a wall (0.18 darker), because
the shadow view's copies of those surfaces do not lie exactly where the camera's do. The cause was not found.

## Measured

| Scene | Draws that read the AO | Triangles | Skinned |
|---|---|---|---|
| small test scene | 114 | 387k | 91% |
| large scene | ~460 | ~1.5M | 85% |

Refit in the large scene: bottom structure 6.0 ms to 1.5 ms per frame, frame time 22.9 ms to 18.2 ms.
