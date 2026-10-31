# Writing a shader for Half-Life: Alyx

This is how to write a `.vfx` shader that s2vfxc compiles for the Half-Life: Alyx Workshop Tools (S2FM, the
Material Editor). The examples in `examples\` follow everything below; start by reading `unlit.vfx`, then `lit.vfx`.


## The file

A `.vfx` file is made of blocks:

| Block | What goes in it |
|---|---|
| `HEADER` | `Description = "...";` shown in the Material Editor's shader list |
| `MODES` | which passes the shader draws in |
| `FEATURES` | the switches a material can set (`F_...`) |
| `COMMON` | code shared by both stages (usually just `#include "s2vfx/common.hlsl"`) |
| `VS` | the vertex stage: `MainVs` |
| `PS` | the pixel stage: combos, render states, material parameters and `MainPs` |

Inside the blocks it is normal HLSL (Shader Model 5).


## The smallest shader

A colour texture times a tint:

```hlsl
HEADER
{
    Description = "My Unlit: a colour texture times a tint";
}

MODES
{
    VrForward();
    Depth(S_MODE_DEPTH);
    ToolsVis(S_MODE_TOOLS_VIS);
    ToolsWireframe("vr_tools_wireframe.vfx");
}

FEATURES
{
}

COMMON
{
    #include "s2vfx/common.hlsl"
}

VS
{
    #include "s2vfx/vertex.hlsl"

    PixelInput MainVs(VertexInput i)
    {
        return VertexMain(i);
    }
}

PS
{
    StaticCombo(S_MODE_DEPTH, 0..1, Sys(ALL));

    float3 g_vColorTint < UiType(Color); Default3(1.0, 1.0, 1.0); UiGroup("Color,10/20"); >;

    CreateInputTexture2D(TextureColor, Srgb, 8, "", "_color", "Color,10/10", Default3(1.0, 1.0, 1.0));
    Texture2D g_tColor < Channel(RGB, Box(TextureColor), Srgb); OutputFormat(DXT1); SrgbRead(true); >;
    SamplerState g_sColor < Filter(MIN_MAG_MIP_LINEAR); AddressU(WRAP); AddressV(WRAP); >;

#if S_MODE_DEPTH
    void MainPs(PixelInput i)
    {
    }
#else
    float4 MainPs(PixelInput i) : SV_Target0
    {
        return float4(g_tColor.Sample(g_sColor, i.vTexCoord).rgb * g_vColorTint, 1.0);
    }
#endif
}
```

Compile and install it (close the game first):

    bin\s2vfxc.exe my_unlit.vfx --name my_unlit --install --s2fm

and make a material that uses it, in your addon's content folder (the tools compile materials in an addon by
themselves), for example `content\hlvr_addons\<your addon>\materials\my_test.vmat`:

```
Layer0
{
	shader "my_unlit.vfx"
	TextureColor "materials/default/default.tga"
	g_vColorTint "[1.000000 0.500000 0.200000 0.000000]"
}
```

Or make it in the Material Editor: a new material, then pick the shader from the list (`my_unlit` shows as "My
Unlit").


## Modes

- `VrForward()` the normal colour pass.
- `Depth(S_MODE_DEPTH)` the depth passes (depth prepass, shadow maps). The pixel stage is compiled again with
  `S_MODE_DEPTH` = 1: a `MainPs` with no output (as above) draws depth only. A `MainPs` that returns `SV_Depth` writes
  its own depth.
- `ToolsVis(S_MODE_TOOLS_VIS)` the tools' view. S2FM draws with this one (`S_MODE_TOOLS_VIS` = 1).
- `ToolsWireframe("vr_tools_wireframe.vfx")` the wireframe view, borrowed from the game.


## What the pixel stage gets

`PixelInput` (from `s2vfx/common.hlsl`):

| Field | What |
|---|---|
| `vPositionWs` | the position in the world, less the lighting offset. `WorldPosition(i.vPositionWs)` (lighting.hlsl) gives the real one |
| `vNormalWs.xyz` | the normal in the world (`w`: the face flexes' wrinkle amount) |
| `vTangentWs`, `vBitangentWs` | the tangent frame, for normal maps |
| `vTexCoord` | the texture coordinates |
| `vTint.rgb` | the model's tint (its colour in S2FM) |
| `vPositionPs` | the pixel (`SV_Position`) |

The engine's per-view values come from `s2vfx/engine_cbuffers.hlsl`, for example `g_flTime` (S2FM's timeline time),
`g_vCameraPositionWs`, `g_vViewportSize`.


## Material parameters

A parameter is a global with annotations; the Material Editor shows it and the material stores it.

```hlsl
float g_flAmount < UiType(Slider); Default(0.5); Range(0.0, 1.0); UiGroup("Look,20/10"); >;
float3 g_vColor < UiType(Color); Default3(1.0, 1.0, 1.0); UiGroup("Look,20/20"); >;
float2 g_vScale < UiType(VectorText); Default2(1.0, 1.0); UiGroup("Look,20/30"); >;
bool g_bEnabled < UiType(CheckBox); Default(1); UiGroup("Look,20/40"); >;
```

- `UiGroup("Group,10/20")`: the group's name, the group's place among groups (10), the parameter's place in it (20).
- Colours are stored as the Material Editor shows them (sRGB). Use `SrgbGammaToLinear` (material.hlsl) before
  lighting with them.
- `UiFeature(F_X)` shows the parameter only when feature `F_X` is on.

Textures are two lines: the input the material picks, and the compiled texture the shader reads.

```hlsl
CreateInputTexture2D(TextureMask, Linear, 8, "", "_mask", "Look,20/50", Default3(1.0, 1.0, 1.0));
Texture2D g_tMask < Channel(R, Box(TextureMask), Linear); OutputFormat(ATI1N); SrgbRead(false); >;
```

- Colour textures: `Srgb`, `SrgbRead(true)`, `OutputFormat(DXT1)` or `BC7`.
- Data (masks, noise, normals): `Linear`, `SrgbRead(false)`. One channel `ATI1N`, two `ATI2N`, full quality `BC7` or
  `RGBA8888`.
- Normal maps: see `lit.vfx` (`HemiOctIsoRoughness_RG_B`, read with `DecodeNormal` and `NormalToWorld`).


## Features and combos

A feature is a switch on the material. A static combo turns it into code:

```hlsl
FEATURES
{
    Feature(F_GLOW, 0..1, "Look");
    Feature(F_MODE, 0..2 (0="Soft", 1="Hard", 2="Off"), "Look");
}

PS
{
    StaticCombo(S_GLOW, F_GLOW, Sys(ALL));
    // ...
#if S_GLOW
    // code only in the glow variant
#endif
}
```

Every combo doubles (or more) the variants that are compiled. For a switch that only changes what the code reads,
use `UiOnly`: one variant, and the feature's value lands in a float you test at run time:

```hlsl
StaticCombo(S_MODE, F_MODE, Sys(ALL), UiOnly(g_flMode));
float g_flMode < Default(0.0); >;
// ... if (g_flMode > 0.5) { ... }
```


## Render states

In the `PS` block, per static combo:

```hlsl
// See-through, added onto what is behind (glow):
RenderState(BlendEnable, true);
RenderState(SrcBlend, ONE);
RenderState(DstBlend, ONE);
RenderState(DepthWriteEnable, false);
BoolAttribute(translucent, true);

// Both sides of the faces:
RenderState(CullMode, NONE);
BoolAttribute(renderbackfaces, true);
```

States can depend on a combo: `RenderState(CullMode, S_BACKFACES ? NONE : DEFAULT);`. Other useful ones:
`DepthEnable`, `DepthFunc`, `ColorWriteEnable0`, `BoolAttribute(NoZPrepass, true)`.


## Lighting

`s2vfx/lighting.hlsl` lights a surface the way the game does: the sun with its shadows, S2FM's lights (point, spot,
orthographic, projected) with their shadows, the light probe or lightmap ambient and the environment maps.
`s2vfx/fog.hlsl` adds the map's fog. A small lit shader's pixel stage:

```hlsl
PS
{
    StaticCombo(S_MODE_DEPTH, 0..1, Sys(ALL));

    #include "s2vfx/lighting.hlsl"
    #include "s2vfx/fog.hlsl"
    #include "s2vfx/material.hlsl"

    float3 g_vColor < UiType(Color); Default3(0.8, 0.8, 0.8); UiGroup("Color,10/10"); >;
    float g_flRoughness < UiType(Slider); Default(0.5); Range(0.0, 1.0); UiGroup("Color,10/20"); >;
    float g_flMetalness < UiType(Slider); Default(0.0); Range(0.0, 1.0); UiGroup("Color,10/30"); >;

#if S_MODE_DEPTH
    void MainPs(PixelInput i)
    {
    }
#else
    float4 MainPs(PixelInput i) : SV_Target0
    {
        const float3 albedo = SrgbGammaToLinear(g_vColor) * i.vTint.rgb;
        const float3 normalWs = normalize(i.vNormalWs.xyz);
        LightingSurface s = LitSurface(i, albedo, normalWs, g_flRoughness, g_flMetalness, 1.0);
        float3 color = ShadeSurface(s);
        color = ApplyFog(color, i.vPositionWs, i.vPositionPs.xy);
        return float4(color, 1.0);
    }
#endif
}
```

The `VS` block of a lit shader also needs `BoolAttribute(environmentmapped, true);` (lit.vfx has it), or the
environment maps stay black.


## Reading the scene behind (refraction, soft edges)

`s2vfx/screen.hlsl` gives `SceneBehind(pixel, blur)` (the frame copy) and `SceneDistance(...)` (the scene's depth).
The engine does not give these to custom shaders; the S2FM Mod Pack does. Without the Mod Pack they read nothing.


## Mistakes that cost time

- **HLSL keywords as names.** `texture`, `pass`, `line`, `half`, `sample`, `point` and similar give a syntax error
  in the generated file. Pick other names.
- **Names already taken.** A global with the name of one of the engine's (`g_vWindDirection`) or of the includes'
  (`g_sNoise` in fog.hlsl) clashes. Give your own globals names of your own.
- **Texture reads inside loops.** `Sample` needs screen gradients and fails (error X3014) in loops and branches that
  depend on data. Use `SampleLevel(sampler, uv, 0)` there.
- **Compile time.** The HLSL compiler inlines everything; a big function (lighting) called in several places, or a
  loop inside a loop, makes one variant take minutes. Call heavy functions in one place. Build with `--s2fm` while
  working, and with `--only` to try one variant.
- **See-through materials cast no shadow.** The engine leaves translucent materials out of its shadow maps. The
  weather shader's answer: a second, opaque material on a copy of the model that draws nothing on screen and only
  writes depth in the shadow pass (`F_SHADOW_ONLY` in weather.vfx).
- **Installing while the game runs.** Close the game and its tools before `--install`.
- **Materials in core.** A `.vmat` in `content\core` is not compiled when S2FM loads it; compile it with
  `game\bin\win64\resourcecompiler.exe -i <file>`. Materials in an addon are compiled when loaded.
- **Seeing what a value is.** Return it as a colour from `MainPs`, behind a `#if` define, and build with
  `-D MYDEBUG=1`. The examples keep a few such views (`S2VFX_DEBUG_VOLUME` in weather.vfx).


## Where to look

- `examples\unlit.vfx`: the smallest shader with two features and blend states.
- `examples\lit.vfx`: textures, normal maps, detail textures, UiOnly features, the full lighting.
- `examples\glass.vfx`, `water.vfx`: reading the scene behind (refraction, depth).
- `examples\energy.vfx`: moving the surface in the vertex stage (`S2VFX_DISPLACE`), effects over time.
- `examples\weather.vfx`: a volume ray marched inside a model, its own depth pass, Shadow Only, a big include.
- `docs\format_v64.md`: what is known about the `.vcs` files.
