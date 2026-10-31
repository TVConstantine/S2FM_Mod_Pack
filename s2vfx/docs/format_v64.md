# VCS version 64 (Half-Life: Alyx)

What `python/vcs64.py` reads and writes. Field names follow ValveResourceFormat's readers (MIT), which this
was written from; every point below was checked against the game's own files (`tests/roundtrip.py`: 816
files, every byte the same).

## Where the engine looks

A shader `<name>` needs all its programs (features, vs, ps) in **both** places:

- `game\shaders\vfx\<name>_pc_50_<program>.vcs`, opened by path;
- `game\core\shaders\vfx\<name>_pc_50_<program>.vcs` (or another GAME search path), opened through the
  file system.

With a program in only one of them the material compile says "File might be corrupt or missing" or "Error
loading static combo data". Names and hashes are not checked against anything.

## File

```
u32 "vcs2", i32 version (64), u32 flags (extra programs: 1, 2 or 4)
features only:
  i32 header version, i32 description length (without the terminator), cstr description, i32 dev shader, i32 available[count]
  i32 modes, each: char name[64], char fallback[64], i32 n, n x (char combo[64], i32 value)
  16 bytes per program (count of them): program hashes
other programs: 16 bytes, this program's hash
16 bytes variable description hash, i32 variable source max
i32 n + n static combos, i32 n + n static rules, i32 n + n dynamic combos, i32 n + n dynamic rules
i32 n + n variables, i32 n + n texture channel processors, i32 n + n constant buffers
features and vs: i32 n + n vertex input signatures
i32 blocks, i64 static combo id[blocks], i32 offset[blocks], i32 end of file (only when blocks > 0)
the blocks
```

`count` is 7 programs (features, vs, ps, gs, hs, ds, cs) plus 1 to 3 by the flags.

Fixed size strings (`char x[64]`) keep whatever follows the terminator in Valve's files; a writer may pad
with zeros.

## Static combo blocks

Every block is LZMA compressed; the engine refuses uncompressed ones ("Expecting compressed combo data"):

```
i32 size of the rest, u32 "LZMA", u32 unpacked size, u32 packed size (without props), u8 props[5], data
```

A stream with an end marker is accepted.

Unpacked, a vs/ps block is: the variable index array, the attributes (`i16` count; each a cstr name, its
MurmurHash2 with seed 0x31415926 of the lower case name, a type byte, an `i16` binding and for unbound ones a
constant or an expression), for vs the input signature indices, the dynamic combos' index arrays, the constant
buffer slots, `i32` buffer size, `u8` static buffer, `i32` code count, `u8` glsl, then each piece of code as
`i32` size, the DXBC and its **MD5**, then one record per dynamic combo: `i64` dynamic id, `i32` code index,
`i32` offset and for ps three render states (rasterizer 16 bytes, depth stencil 20, blend 75), each led by a
"missing" byte. Counts written as `u16` use 0xFFFF followed by an `i32` for large values.

## Seeing a shader in S2FM

- A model takes a test material through its material override: `replacementMaterial` (a string) on the
  model's `materialOverride` element (`tests/live/d2_material.py`). Changing `modelName` does not reload the
  model, and a loose `.vmat_c` does not win over the same file in the game's archive.
- S2FM draws models in the `VrForward` mode with pc_50 shaders. A pc_40-only shader such as Valve's `black`
  is drawn with a flat grey fallback, so a template or a new shader needs pc_50 and `VrForward`.
- A replacement pixel shader must take the same inputs as the code it replaces (the vertex shader's outputs);
  `python/dxbc.py` reads the input signature and writes the matching HLSL parameters.

## Parameters and textures

- A material parameter is a variable with source 0 (set by the artist), listed in features and in ps. In each
  ps static combo it gets a field (variable index, dword offset in b0) after `firstConstant`, in the static
  combo's list and in every dynamic combo's.
- A texture is three things: an input texture variable (register type 7, what the Material Editor picks), a
  channel processor (which inputs and channels, and a command such as `Box`) and the compiled texture variable
  (register type 4, an output format and the channel processor's index). **Each program has its own channel list**,
  its inputs indexing that program's own variables; features and ps both need it. The compiled texture gets a
  field (variable, t slot) before `firstConstant`.
- A sampler is a set of state variables (`Filter` with D3D11 filter values, `AddressU`/`V`/`W` with 0 wrap and 2
  clamp), each given a field (variable, s slot).

## What a shader of our own must get right

Found writing `python/s2vfxc.py`. Each one, when wrong, gives no message: the object is drawn with a flat grey
(the depth pass draws it, the colour pass does not).

- The variable hash is not a hash of the shader: every file of the game has the same 16 bytes there (and the
  same variable source max). `python/library.py` reads it from the game.
- A program's hash must equal its entry in the features file ("Program MD5 is out of date with features").
- A state's `offset` is where its code's record starts, counted from the code count: `5 + sum(4 + size + 16)` of
  the records before it.
- The vertex input signatures are sorted by D3D semantic and index.
- Every vs and ps file has its variables sorted by name, byte by byte (features files do not: they are the vs
  list then the ps list). The engine looks its own variables up in that order.
- A constant buffer variable's `bits` is the place of its description in the file's constant buffer list.
- In an index array, everything bound goes before `firstConstant`: textures, samplers **and the engine's
  constant buffers** (`slot | control byte << 8`). After it come only material values (variable, dword offset
  in the material buffer). An engine buffer put after it is taken as a value and never bound.

Two tools found these: `tests/live/vconsole_log.py` keeps the engine's console during a live test and can send
it commands (`mat_print_shader_info <name>.vfx -show_used_combos`, `mat_print_material_info
core\materials\...\x.vmat`, `mat_print_error_materials`); `tools/probe/core_probe.cpp`, built into the package's
core for a run, logs every draw made with given bytecode and what is bound for it.

## Render states and attributes

A ps state carries three render state records, each led by a byte saying whether it is there (0) or left to the
engine (1): rasterizer (16 bytes: fill, cull, depth clip, multisample, depth bias, its clamp, slope), depth
stencil (20: depth test, write, function, two unused, stencil enable, read and write masks, front fail, depth
fail, pass, function, the same for back faces, four unused) and blend (75: alpha to coverage, independent blend,
one unused, then eight bytes each, one per target: enable, source, destination, operation, the same for alpha,
write mask, sRGB write). Valve fills every target alike. Values read off the game's shaders: cull 0 none, 1
back, 2 front; fill 0 solid, 1 wireframe; blend 0 ZERO, 1 ONE, 2 SRC_COLOR, 4 SRC_ALPHA, 5 INV_SRC_ALPHA, 8
DEST_COLOR, 12 SRC1_COLOR (the rest in D3D11's order); comparison 3 LESS_EQUAL, 7 ALWAYS. Models leave the depth
stencil record out and get depth and stencil from the engine's mode.

The engine reads what a material is from the static combo's attributes: `translucent`, `renderbackfaces`,
`NoZPrepass`, `DoNotCastShadows` and others, constants (or expressions) per static combo, hashed as above.
`python/render_state.py` builds both from `RenderState(...)` and `BoolAttribute(...)` statements;
`examples/unlit.vfx` has both sides and additive as features (`tests/d3_unlit.py` installs an additive material).

## Lighting

`include/s2vfx/lighting.hlsl` and `fog.hlsl` carry over the lighting of the game's vr_simple pixel shader, the
tools variant (`S_MODE_TOOLS_VIS` = 1): that is the variant S2FM draws models with (`mat_print_shader_info
-show_used_combos` lists it loaded), and the only one with S2FM's lights. The game variant has a fast path
instead (one runtime light, effect lights, no light list). What it reads:

| What | Where |
|---|---|
| point, spot and orthographic lights (cookies, shadows) | `PerViewLightingConstantBufferVr_t`, `g_tVrShadowDepthBufferDepth`, `g_tVrLightCookieTexture` |
| sun, cascades | `SunLightShaderParamsCB`, `g_tShadowDepthMapAtlas` |
| projected lights (diffuse only), gobos | `SFMLightsConstantBuffer_t`, `g_tSFMShadowDepthTexture`, `g_tSFMCookieTextureArray`, gated by `g_bIsSFM` |
| ambient cube and the map's baked lights | `D_BAKED_LIGHTING_FROM_PROBE`, `PerBatchLightProbeVolumeConstantBuffer_t`, `g_tLightProbeVolumeTexture*`, `PerViewRuntimeFastPathLightingConstantBuffer_t` |
| environment maps | `g_tEnvironmentMap` (cube array), box data in the fast path buffer, index from `TEXCOORD12` |
| dynamic AO, fog | `g_tDynamicAmbientOcclusion*` (gated by `g_bOtherFxEnabled.x`); gradient, cube, volumetric fog and vignette in `PerViewConstantBufferVR_t` |

`include/s2vfx/engine_lighting.hlsl` is generated from the buffer descriptions in vr_simple's ps file (offsets
in dwords, matrices as rows of float4). Four things the file must say, each found by a part going missing:

- The ps static combo's attributes `NeedsLightProbe` and `environmentmapped` (and `environmentmapped` in the vs):
  without them the engine never sets `D_BAKED_LIGHTING_FROM_PROBE` and gives environment map 0.
- Values the engine sets (`g_bIsSFM`, `g_bCubemapNormalization`: variables with source 1) sit in the material's
  buffer like parameters, and a block holding one has `staticCB` 0. Marked static, the engine never writes them
  and they read 0 (all 627 such blocks in the game have 0).
- An engine buffer's control byte is per stage: the lighting buffer has 1 in vertex shaders, 13 in pixel shaders.
- Comparison samplers take filter `0x95` (`COMPARISON_MIN_MAG_MIP_LINEAR`); `BorderColor` and `MaxAniso` are
  states of their own.

Checked in S2FM on Alyx with the same checker material on Valve's vr_simple and on `examples/lit.vfx`, RTAO off:
mean colour over her jacket 120.7, 35.1, 207.7 against 124.5, 41.0, 207.6 (the poses differ a little).
Valve's buffer variables do not always keep `bits` as the place in the file's list (vr_simple's lighting buffer
has five copies with different bits); ours do, and the buffers bind.

## Our own vertex shader

`include/s2vfx/vertex.hlsl` (the compiler's) also applies the flexes in its `D_MORPH` variants, which the engine
picks for meshes with flexes: the vertex's number is `TEXCOORD14` (a float), the object's texel 4 holds the
atlas row width (w) and x offset (z), texel 5 the y offset (w); `g_tCompositeMorphTextureAtlas` has the position
delta in its left half and the normal delta in its right half, both added to the bind pose before skinning
(`tests/d3_morph.py`, `tests/live/d3_morph.py`: with the face controls keyed, Alyx's mouth opens).


`include/s2vfx_vertex.hlsl` replaces every vertex shader of a model shader (`tests/d3b_vs.py`: 120 variants of
vr_black_unlit, each with the same input and output signatures as the code it replaces). Skinned characters keep
their pose. It uses the registers the engine binds for the template (PerView at b0, PerViewVR at b1, the transform
texture at t11 with s0), so the template's index arrays stay valid. A depth mode of our own (`Depth(S_MODE_DEPTH)`, the combo in ps
only) keeps the depth pass on our vertex shader: a pixel shader with no output and no discard is stored as no
code (size 0, the MD5 of nothing), as Valve's own depth modes are, and the engine draws with no pixel shader.
`include/engine_cbuffers.hlsl` is generated from the reflection data of the game's vertex shaders
(`python/genheaders.py`, every offset checked).
