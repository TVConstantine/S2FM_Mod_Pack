// Source 2's lighting for a model's pixel shader, as S2FM draws it. Carried over from the game's own vr_simple pixel
// shader, the tools variant (S_MODE_TOOLS_VIS), which is the one S2FM's viewports and renders use: the light list
// (point, spot and orthographic lights, with cookies and shadows), the sun with its cascades, S2FM's projected
// lights, the tools ambient, the light probe volume (ambient and the map's baked lights with their shadows), the
// environment maps and the engine's dynamic ambient occlusion. The terms are written without a 1/pi, as Valve's are:
// the light colours are scaled for them.
//
// The surface (ours, not Valve's): GGX with the height-correlated Smith term (Heitz 2014) and its light lost between
// the microfacets put back (Turquin 2019), Schlick's Fresnel from an IOR with the F82 tint for metals (OpenPBR),
// geometric specular anti-aliasing (Tokuyoshi and Kaplanyan 2021), and a diffuse of EON (OpenPBR's energy preserving
// Oren-Nayar, Portsmouth, Kutz and Hill 2025) lying under the specular as a layer, which takes the light the specular
// reflects on the way in and on the way out. The environment map's reflection uses the same lobe's albedo
// (brdf_table.hlsl, python/brdf_table.py) and is occluded by the ambient occlusion (Lagarde 2014) and by the surface's
// horizon. "Valve Shading" (g_bValveShading) gives back the game's own terms, to compare.
//
// A PS block includes this after common.hlsl and calls:
//     LightingSurface s = LitSurface(i, albedo, normalWs, roughness, metalness, occlusion);
//     float3 color = ShadeSurface(s);
// or takes the parts (DirectLighting, AmbientDiffuse, AmbientSpecular) for a shading model of its own.
//
// Positions: the vertex stage hands vPositionWs less g_vHighPrecisionLightingOffsetWs. The light list, its shadows
// and the baked shadows work in that space; the sun, the projected lights, the probes, the environment maps and the
// camera in true world space (WorldPosition).

#include "engine_lighting.hlsl"
#include "sheen_table.hlsl"
#include "subsurface_table.hlsl"
#include "spectrum.hlsl"
#include "brdf_table.hlsl"

// The engine picks it when the object takes its ambient from a light probe volume (characters and props).
DynamicCombo(D_BAKED_LIGHTING_FROM_PROBE, 0..1, Sys(ALL));

// What the engine reads to give the object a light probe and an environment map; without them it sets neither
// (no probe combo, environment map index 0). The VS block wants BoolAttribute(environmentmapped, true) too.
BoolAttribute(NeedsLightProbe, true);
BoolAttribute(environmentmapped, true);

Texture2D g_tVrShadowDepthBufferDepth;          // the light list's shadow maps, one atlas; also the baked lights'
Texture3D g_tVrLightCookieTexture;              // light cookies
Texture2D g_tShadowDepthMapAtlas;               // the sun's cascades
Texture2DArray g_tSFMShadowDepthTexture;        // projected lights: shadows
Texture2DArray g_tSFMCookieTextureArray;        // projected lights: gobos
TextureCubeArray g_tEnvironmentMap;
Texture2D g_tBRDFLookup;
Texture2D g_tDynamicAmbientOcclusion;           // the engine's dynamic AO (four proxies per texel)
Texture2D g_tDynamicAmbientOcclusionDepth;      // and its depth
#if D_BAKED_LIGHTING_FROM_PROBE
Texture3D g_tLightProbeVolumeTexture;                   // six faces of an ambient cube, stacked along z
Texture3D g_tLightProbeVolumeTextureDirectLightIndices; // up to four baked lights per probe
Texture3D g_tLightProbeVolumeTextureDirectLightScalars; // and their strengths
#endif

SamplerComparisonState g_sShadowCompare < Filter(COMPARISON_MIN_MAG_MIP_LINEAR); AddressU(CLAMP); AddressV(CLAMP); >;
SamplerState g_sCookie < Filter(MIN_MAG_LINEAR_MIP_POINT); AddressU(BORDER); AddressV(BORDER); AddressW(BORDER); BorderColor(0, 0, 0, 0); >;
SamplerState g_sLinearClamp < Filter(MIN_MAG_MIP_LINEAR); AddressU(CLAMP); AddressV(CLAMP); AddressW(CLAMP); >;
SamplerState g_sLinearWrap < Filter(MIN_MAG_MIP_LINEAR); AddressU(WRAP); AddressV(WRAP); >;
SamplerState g_sBilinearClamp < Filter(MIN_MAG_LINEAR_MIP_POINT); AddressU(CLAMP); AddressV(CLAMP); AddressW(CLAMP); >;
SamplerState g_sPointClamp < Filter(MIN_MAG_MIP_POINT); AddressU(CLAMP); AddressV(CLAMP); AddressW(CLAMP); >;

// Set by the engine: S2FM is running (its projected lights are in use), and environment maps are normalised by the
// probe's ambient.
bool g_bIsSFM;
bool g_bCubemapNormalization;

// S2FM's projected lights with the same diffuse and specular as the other lights (on), or diffuse only as Valve's
// shaders have them (off): Valve's flat Lambert takes no roughness and no metalness, so under a projected light a
// rough metal looks like painted plastic. On is a deliberate departure from Valve.
bool g_bProjectedLightSpecular < UiType(CheckBox); Default(1); UiGroup("Lighting"); >;

// The game's own diffuse, specular, Fresnel and environment terms (vr_complex's) instead of ours, to set beside them.
bool g_bValveShading < UiType(CheckBox); Default(0); UiGroup("Lighting"); >;

// The S2FM Mod Pack's light table (its refract.cpp, LightFogTable), 128 x 3 texels: row 0 texel 0 the count of
// S2FM's projected lights, then each one's world position (xyz) and fog weight (w); row 1 each light of the engine's
// light list this draw has, by its index (x its fog weight, z its size, S2FM's radius), with 1 in texel 0's y when the
// row is there; row 2 each projected light's size, in the texel row 0 has its position. The engine knows nothing of
// a light's size: without the mod pack every light is a point. Declared through an input texture (the material
// compiler crashes on a bare one).
CreateInputTexture2D(TextureLightFog, Linear, 8, "", "_black_mask", "Mod Pack,99/10", Default3(0.0, 0.0, 0.0));
Texture2D g_tLightFog < Channel(RGBA, Box(TextureLightFog), Linear); OutputFormat(BC7); SrgbRead(false); >;

// Light list light n's size (0: a point, or no table).
float ListLightSize(int n)
{
    return g_tLightFog.Load(int3(0, 1, 0)).y > 0.5 ? max(g_tLightFog.Load(int3(n, 1, 0)).z, 0.0) : 0.0;
}

// The size of the projected light at world position p: the table's nearest within 64 units, else 0.
float ProjectedLightSize(float3 p)
{
    const int count = (int)clamp(g_tLightFog.Load(int3(0, 0, 0)).x, 0.0, 63.0);
    float nearest = 64.0 * 64.0, size = 0.0;
    [loop] for (int k = 1; k <= count; k++)
    {
        const float3 d = g_tLightFog.Load(int3(k, 0, 0)).xyz - p;
        if (dot(d, d) < nearest)
        {
            nearest = dot(d, d);
            size = g_tLightFog.Load(int3(k, 2, 0)).x;
        }
    }
    return max(size, 0.0);
}

// The sky as light: the S2FM Mod Pack's bake of a procedural sky (s2vfx_sky; its sky/sky_bake.cpp), an
// equirectangular picture of the world's directions (Z up, as Blender lays one out) with its sun's disk left out (the
// sun is a light of its own). Mips 0 to 5 the sky's reflection, prefiltered with GGX for perceptual roughness 0, 0.2,
// ... 1; mips 6 on its diffuse light (cosine weighted, over pi). Without the mod pack, or before a sky is baked, the
// texture is the material's default (a few texels, one mip), and the map's ambient and environment map are kept.
// Sky Light mixes from the map's light (0) to the sky's (1).
float g_flSkyLight < UiType(Slider); Default(0.0); Range(0.0, 1.0); UiGroup("Lighting"); >;
float g_flSkyLightStrength < UiType(Slider); Default(1.0); Range(0.0, 10.0); UiGroup("Lighting"); >;
// Read texel by texel (filtered here, wrapping round and clamped at the poles): a material's sampler is the engine's
// to make, and a sampled reflection came out as blurred as the last mips.
CreateInputTexture2D(TextureSkyLight, Linear, 8, "", "_black_mask", "Mod Pack,99/20", Default3(0.0, 0.0, 0.0));
Texture2D g_tSkyLight < Channel(RGBA, Box(TextureSkyLight), Linear); OutputFormat(BC7); SrgbRead(false); >;

static const float kSkyLightMips = 5.0;      // the last prefiltered mip (roughness 1)
static const float kSkyLightDiffuse = 6.0;   // the diffuse light's mip

// How much of the sky's light this surface takes: none without a bake.
float SkyLightShare()
{
    if (g_flSkyLight <= 0.0)
        return 0.0;
    uint w, h, mips;
    g_tSkyLight.GetDimensions(0, w, h, mips);
    return w >= 256 && mips > (uint)kSkyLightDiffuse ? saturate(g_flSkyLight) : 0.0;
}

// One mip, bilinear.
float3 SkyLightMip(float2 uv, uint mip)
{
    uint w, h, mips;
    g_tSkyLight.GetDimensions(mip, w, h, mips);
    const float2 p = uv * float2(w, h) - 0.5;
    const int2 i = (int2)floor(p);
    const float2 f = p - i;
    const int x0 = (i.x + (int)w) % (int)w;
    const int x1 = (i.x + 1 + (int)w) % (int)w;
    const int y0 = clamp(i.y, 0, (int)h - 1);
    const int y1 = clamp(i.y + 1, 0, (int)h - 1);
    const float3 a = lerp(g_tSkyLight.Load(int3(x0, y0, mip)).rgb, g_tSkyLight.Load(int3(x1, y0, mip)).rgb, f.x);
    const float3 b = lerp(g_tSkyLight.Load(int3(x0, y1, mip)).rgb, g_tSkyLight.Load(int3(x1, y1, mip)).rgb, f.x);
    return lerp(a, b, f.y);
}

// The sky's light toward d at mip lod, between its two nearest mips.
float3 SkyLightAt(float3 d, float lod)
{
    const float2 uv = float2(atan2(d.y, -d.x) * (0.5 / 3.14159265) + 0.5, 0.5 - asin(clamp(d.z, -1.0, 1.0)) / 3.14159265);
    lod = clamp(lod, 0.0, kSkyLightDiffuse);
    const uint m = (uint)floor(lod);
    const float t = lod - m;
    float3 c = SkyLightMip(uv, m);
    if (t > 0.0)
        c = lerp(c, SkyLightMip(uv, min(m + 1, (uint)kSkyLightDiffuse)), t);
    return c * g_flSkyLightStrength;
}

struct LightingSurface
{
    float3 positionWs;          // as PixelInput carries it: less the lighting offset
    float3 normalWs;            // the shading normal, normalised
    float3 diffuseColor;        // albedo times (1 - metalness)
    float3 specularColor;       // F0: 0.04 for dielectrics, the albedo for metals
    float3 specularEnergy;      // what the specular is scaled by for the light lost to one bounce (1: none)
    float3 f82;                 // the metal's F82 tint, as the b of F0 + (1 - F0)(1 - c)^5 - b c (1 - c)^6 (0: none)
    float layerF0;              // the dielectric's F0, the specular layer the diffuse lies under
    float diffuseRoughness;     // EON's roughness (0: Lambert)
    float3 geometricNormalWs;   // the mesh's normal (the horizon of the reflections)
    float roughness;
    float occlusion;            // ambient occlusion, 1 for none: the ambient light and the environment map
    float directDiffuseOcclusion;   // how much of it the direct light takes (DirectOcclusion)
    float directSpecularOcclusion;
    uint envMap;                // PixelInput.nObjectId: environment map index << 8 | cube array slice
    // A clear coat over the surface (0 for none): its own normal, roughness and F0, and a tint on what is under it.
    float coat;
    float3 coatNormalWs;
    float coatRoughness;
    float coatF0;
    float3 coatTint;
    // Sheen, a fabric's soft rim (0 for none): its colour (times its amount) and roughness.
    float3 sheen;
    float sheenRoughness;
    // Anisotropy (0 for none): how much the highlights stretch along anisoTangentWs (on the shading normal).
    float anisotropic;
    float3 anisoTangentWs;
    // Subsurface scattering (0 for none): how much, the pre-integrated term's A, C and E per channel (from Blender's
    // d and the curve's curvature, subsurface_table.hlsl) and the normal each channel sees (the normal map blurred
    // over that channel's d).
    float subsurface;
    float3 subsurfaceA;
    float3 subsurfaceC;
    float3 subsurfaceE;
    float3x3 subsurfaceNormals;
    // Glitter (0 for none): how much of the surface here is a mirror flake, and that flake's normal, roughness and
    // F0 (the flakes are metal).
    float glitter;
    float3 glitterNormalWs;
    float glitterRoughness;
    float3 glitterF0;
    // Holographic foil (0 for none): a diffraction grating whose grooves run across holoDirWs, holoSpacing nm
    // apart; holoRoughness spreads its rainbows sideways.
    float holographic;
    float3 holoDirWs;
    float holoSpacing;
    float holoRoughness;
};

struct DirectLight
{
    float3 diffuse;             // light reaching the surface, before the diffuse colour
    float3 specular;
    float3 coat;                // the clear coat's highlights, before its amount
    float3 sheen;               // the sheen's light, before its colour
    float3 glitter;             // the flakes' highlights, before their amount
    float3 holo;                // the grating's rainbows, before its amount
};

float3 WorldPosition(float3 positionWs)
{
    return positionWs + g_vHighPrecisionLightingOffsetWs.xyz;
}

// The engine's dynamic ambient occlusion (soft contact shadows from AO proxies, at a lower resolution): the texel
// whose depth is nearest the pixel's, its four proxies weighted by how much the normal faces each. 1 when off.
float DynamicAmbientOcclusion(float3 geometricNormal, float4 svPosition)
{
    if (!g_bOtherFxEnabled.x)
        return 1.0;
    float4 w = max(float4(dot(g_vAmbientOcclusionProxyLightPositions[0].xyz, geometricNormal),
                          dot(g_vAmbientOcclusionProxyLightPositions[1].xyz, geometricNormal),
                          dot(g_vAmbientOcclusionProxyLightPositions[2].xyz, geometricNormal),
                          dot(g_vAmbientOcclusionProxyLightPositions[3].xyz, geometricNormal)), 0.0);
    float width, height;
    g_tDynamicAmbientOcclusionDepth.GetDimensions(width, height);
    const float2 texel = 1.0 / float2(width, height);
    const float2 uv = floor(svPosition.xy * g_vAoProxyDownres.x) * texel + 0.5 * texel;
    // Of the four texels around, the one whose depth is nearest the pixel's (Valve's order and comparisons).
    const float4 dz = g_tDynamicAmbientOcclusionDepth.Gather(g_sPointClamp, uv) - svPosition.z;
    float best = dz.w;
    float2 offset = 0.0;
    if (abs(dz.z) < best) { best = dz.z; offset = float2(texel.x, 0.0); }
    if (abs(dz.x) < best) { best = dz.x; offset = float2(0.0, texel.y); }
    if (abs(dz.y) < best) offset = texel;
    const float4 ao = g_tDynamicAmbientOcclusion.Sample(g_sPointClamp, uv + offset);
    w *= g_vAmbientOcclusionProxyLightStrengths;
    const float ambient = g_vAmbientOcclusionProxyAmbientStrength.x;
    return (dot(w, ao) + ambient) / (dot(w, 1.0) + ambient);
}

// Valve's surface: the albedo split by metalness, the roughness raised where the normal changes fast across a
// pixel (less specular aliasing), and the given occlusion times the engine's dynamic one. As vr_simple has it, the
// occlusion darkens the ambient light, the environment map and the direct specular; DirectOcclusion changes that.
LightingSurface LitSurface(PixelInput i, float3 albedo, float3 normalWs, float roughness, float metalness, float occlusion)
{
    LightingSurface s;
    s.positionWs = i.vPositionWs;
    s.normalWs = normalize(normalWs);
    s.diffuseColor = albedo * (1.0 - metalness);
    s.specularColor = lerp(0.04, albedo, metalness);
    s.f82 = 0.0;
    s.layerF0 = 0.04;
    s.diffuseRoughness = saturate(roughness);
    const float3 n = normalize(i.vNormalWs.xyz);
    s.geometricNormalWs = n;
    if (g_bValveShading)
    {
        const float3 dx = ddx_coarse(n);
        const float3 dy = ddy_coarse(n);
        s.roughness = max(roughness, pow(min(max(dot(dx, dx), dot(dy, dy)), 1.0), 0.333));
    }
    else
    {
        // How fast the normal turns across the pixel widens the lobe as far as that spread (Tokuyoshi and Kaplanyan
        // 2021): a curved surface far away stays smooth instead of sparkling. The mesh's normal, which the vertex
        // stage hands over, so that a shader can light a surface inside a branch too.
        const float3 dx = ddx_coarse(n), dy = ddy_coarse(n);
        const float variance = 0.25 * (dot(dx, dx) + dot(dy, dy));
        const float a = roughness * roughness;
        s.roughness = sqrt(sqrt(saturate(a * a + min(2.0 * variance, 0.18))));
    }
#if D_MULTIVIEW_INSTANCING
    s.envMap = i.nObjectId.x;
#else
    s.envMap = i.nObjectId;
#endif
#if S2VFX_NO_DYNAMIC_AO
    // A see-through surface (glass.vfx) leaves the engine's dynamic AO unread: a shader that reads it is taken by the
    // mod pack's RTAO for an opaque surface, one that hides what is behind it and starts the AO's frame.
    s.occlusion = occlusion;
#else
    s.occlusion = occlusion * DynamicAmbientOcclusion(n, i.vPositionPs);
#endif
    s.directDiffuseOcclusion = 1.0;
    s.directSpecularOcclusion = s.occlusion;
    s.specularEnergy = 1.0;
    s.coat = 0.0;
    s.coatNormalWs = s.normalWs;
    s.coatRoughness = 0.0;
    s.coatF0 = 0.04;
    s.coatTint = 1.0;
    s.sheen = 0.0;
    s.sheenRoughness = 0.5;
    s.anisotropic = 0.0;
    s.anisoTangentWs = 0.0;
    s.subsurface = 0.0;
    s.subsurfaceA = 1.0;
    s.subsurfaceC = 0.0;
    s.subsurfaceE = 0.0;
    s.subsurfaceNormals = float3x3(s.normalWs, s.normalWs, s.normalWs);
    s.glitter = 0.0;
    s.glitterNormalWs = s.normalWs;
    s.glitterRoughness = 0.1;
    s.glitterF0 = 1.0;
    s.holographic = 0.0;
    s.holoDirWs = 0.0;
    s.holoSpacing = 1600.0;
    s.holoRoughness = 0.1;
    return s;
}

// Subsurface scattering with Blender's Christensen-Burley profile (Subsurface Weight, Radius and Scale; bssrdf.h:
// d = Radius * Scale * 0.25 / pi). A forward pixel shader cannot gather its neighbours' light as Cycles and EEVEE
// do, so the profile comes in two parts that need no neighbours: pre-integrated over the surface's curvature for
// every light (Penner 2011, fitted in subsurface_table.hlsl), and the normal map blurred over each channel's d (the
// caller reads the normal map's mip whose texel is d wide) for the lights and the ambient alike.
void Subsurface(inout LightingSurface s, float weight, float3 d, float curvature, float3 normalR, float3 normalG,
                float3 normalB)
{
    s.subsurface = saturate(weight);
    // u = d / Rs per channel; the pre-integrated term's A, C, E at that u (the same for every light).
    const float3 g = 1.0 - exp(-g_SubsurfaceK2.xxx * sqrt(max(d, 0.0) * max(curvature, 0.0)));
    const float3 gC = 1.0 - exp(-g_SubsurfaceK2.yyy * sqrt(max(d, 0.0) * max(curvature, 0.0)));
    const float3 gE = 1.0 - exp(-g_SubsurfaceK2.zzz * sqrt(max(d, 0.0) * max(curvature, 0.0)));
    s.subsurfaceA = 1.0 + g_SubsurfaceK1.x * g;
    s.subsurfaceC = g_SubsurfaceK1.y * gC;
    s.subsurfaceE = g_SubsurfaceK1.z * gE;
    s.subsurfaceNormals = float3x3(normalize(normalR), normalize(normalG), normalize(normalB));
}

// A light's diffuse: the surface's own term (baseTerm, already worked out by the caller), or with subsurface
// scattering each channel's own pre-integrated term at its blurred normal, as far as the weight says.
float3 DiffuseLight(LightingSurface s, float3 l, float3 baseTerm)
{
    if (s.subsurface <= 0.0)
        return baseTerm;
    const float3 nDotL = mul(s.subsurfaceNormals, l);
    const float3 scattered = max(s.subsurfaceA * saturate(nDotL) + s.subsurfaceC + s.subsurfaceE * nDotL, 0.0);
    return lerp(baseTerm, scattered, s.subsurface);
}

// ---------------------------------------------------------------------------------------------- our lobe's albedo
// The GGX lobe's albedo at n.v and roughness (brdf_table.hlsl, bilinear, sqrt(n.v) across): x E (F = 1), y E5, z C82.
float4 BrdfAlbedo(float nDotV, float roughness)
{
    const float2 p = float2(sqrt(saturate(nDotV)), saturate(roughness)) * 15.0;
    const int2 i = min((int2)p, 14);
    const float2 f = p - i;
    const float4 a = lerp(g_BrdfAlbedo[i.y * 16 + i.x], g_BrdfAlbedo[i.y * 16 + i.x + 1], f.x);
    const float4 c = lerp(g_BrdfAlbedo[(i.y + 1) * 16 + i.x], g_BrdfAlbedo[(i.y + 1) * 16 + i.x + 1], f.x);
    return lerp(a, c, f.y);
}

// Its cosine weighted means over every direction at a roughness: x E - E5, y E5, z E.
float4 BrdfAlbedoMean(float roughness)
{
    const float y = saturate(roughness) * 15.0;
    const int j = min((int)y, 14);
    return lerp(g_BrdfAlbedoMean[j], g_BrdfAlbedoMean[j + 1], y - j);
}

// The whole albedo of a dielectric specular of F0 from one direction (e: BrdfAlbedo there), the light lost to one
// bounce put back: what the diffuse under it does not get.
float LayerAlbedo(float f0, float4 e)
{
    return saturate((f0 * (e.x - e.y) + e.y) * (1.0 + f0 * (1.0 / max(e.x, 0.05) - 1.0)));
}

// The same, its cosine weighted mean over every direction.
float LayerAlbedoMean(float f0, float roughness)
{
    const float4 m = BrdfAlbedoMean(roughness);
    return saturate((f0 * m.x + m.y) * (1.0 + f0 * (1.0 / max(m.z, 0.05) - 1.0)));
}

// EON's parts (OpenPBR, as MaterialX's mx_oren_nayar_compensated_diffuse has them): Fujii's Oren-Nayar, its albedo
// from one direction and its mean, for a white surface of roughness sigma.
static const float kFujii1 = 0.5 - 2.0 / (3.0 * 3.14159265);
static const float kFujii2 = 2.0 / 3.0 - 28.0 / (15.0 * 3.14159265);

float FonAlbedo(float mu, float sigma)
{
    const float a = 1.0 / (1.0 + kFujii1 * sigma);
    const float s = sqrt(saturate(1.0 - mu * mu));
    const float g = s * (acos(clamp(mu, -1.0, 1.0)) - s * mu) + (2.0 / 3.0) * ((s / max(mu, 1e-4)) * (1.0 - s * s * s) - s);
    return a + sigma * a * g * 0.31830989;
}

float FonAlbedoMean(float sigma)
{
    const float a = 1.0 / (1.0 + kFujii1 * sigma);
    return a * (1.0 + kFujii2 * sigma);
}

// The specular as a material sets it: the dielectric's IOR (1.5: 4% straight back), a level that scales that and a
// tint that colours it; for a metal the tint is its F82 colour, the colour its edges take (OpenPBR's specular_color).
void SpecularLayer(inout LightingSurface s, float3 albedo, float metalness, float ior, float level, float3 tint)
{
    const float r = (ior - 1.0) / (ior + 1.0);
    const float3 f0 = saturate(r * r * level * tint);
    s.layerF0 = dot(f0, 1.0 / 3.0);
    s.specularColor = lerp(f0, albedo, metalness);
    // F82: the metal's Schlick at cos 1/7 (82 degrees), tinted there and nowhere else.
    const float mu = 1.0 / 7.0;
    const float3 schlick = albedo + (1.0 - albedo) * pow(1.0 - mu, 5.0);
    s.f82 = metalness * schlick * (1.0 - tint) / (mu * pow(1.0 - mu, 6.0));
}

// What a diffuse under the specular takes of a light that is the same from every direction (the ambient), per unit
// of its colour: EON's albedo toward the view (its light between facets put back in the colour's own way), less what
// the specular layer reflects toward the view. 1 with Valve's shading.
float3 DiffuseAlbedoWeight(LightingSurface s)
{
    if (g_bValveShading)
        return 1.0;
    const float3 toCamera = -normalize(WorldPosition(s.positionWs) - g_vCameraPositionWs);
    const float mu = max(dot(s.normalWs, toCamera), 1e-4);
    const float sigma = s.diffuseRoughness;
    const float eAvg = FonAlbedoMean(sigma);
    const float3 rho = saturate(s.diffuseColor);
    const float3 msShare = rho * eAvg / max(1.0 - rho * (1.0 - eAvg), 1e-4);
    return lerp(msShare, 1.0, FonAlbedo(mu, sigma)) * (1.0 - LayerAlbedo(s.layerF0, BrdfAlbedo(mu, s.roughness)));
}

// Anisotropy as Blender's Principled BSDF has it (node_principled_bsdf.osl): the highlights stretch along the
// tangent, turned about the normal by rotation (0..1 = a full turn). The light's GGX takes alpha_x = r^2 / aspect
// along the tangent and alpha_y = r^2 * aspect across it, aspect = sqrt(1 - 0.9 * anisotropic).
void Anisotropy(inout LightingSurface s, float amount, float rotation, float3 tangentWs)
{
    s.anisotropic = saturate(amount);
    const float3 n = s.normalWs;
    float3 t = normalize(tangentWs - n * dot(n, tangentWs));
    float sn, cs;
    sincos(rotation * 6.28318531, sn, cs);
    s.anisoTangentWs = normalize(t * cs + cross(n, t) * sn);
}

// Sheen (Blender's Principled BSDF and Filament have this one too): the Charlie distribution (Estevez and Kulla
// 2017) with Neubelt's visibility, a soft light at grazing angles that velvet, cloth and dust have. It lies on top:
// what is under it is dimmed by the light it takes (its directional albedo, sheen_table.hlsl).
void Sheen(inout LightingSurface s, float3 color, float roughness)
{
    s.sheen = max(color, 0.0);
    s.sheenRoughness = max(roughness, 0.07);
}

// Glitter: mirror flakes in the surface (the caller lays them out and gives this pixel's flake). Where there is one
// (amount 1) it covers most of what is under it (the flakes lie in a clear binder, so some of the colour under them
// still shows: else a flake that catches no light reads as a black speck); each flake has its own normal, so
// each catches the light from its own direction and they sparkle one by one as the eye or the light moves.
void Glitter(inout LightingSurface s, float amount, float3 normalWs, float roughness, float3 f0)
{
    s.glitter = saturate(amount);
    s.glitterNormalWs = normalize(normalWs);
    s.glitterRoughness = max(roughness, 0.02);
    s.glitterF0 = saturate(f0);
}

// Holographic foil: a metal foil pressed with fine grooves, a diffraction grating. Light leaving it in the first
// order bends along the grooves' direction (across the grooves) by sin = wavelength / spacing, each wavelength by
// its own angle, so a white light or a bright part of the environment fans out into a rainbow. rotation turns the
// grooves about the normal (0..1 = a full turn) from the mesh's uv tangent.
void Holographic(inout LightingSurface s, float amount, float spacing, float roughness, float rotation, float3 tangentWs)
{
    s.holographic = saturate(amount);
    const float3 n = s.normalWs;
    const float3 t = normalize(tangentWs - n * dot(n, tangentWs));
    float sn, cs;
    sincos(rotation * 6.28318531, sn, cs);
    s.holoDirWs = normalize(t * cs + cross(n, t) * sn);
    s.holoSpacing = max(spacing, 100.0);
    s.holoRoughness = max(roughness, 0.01);
}

// A clear coat (Blender's Principled BSDF and Filament have the same model): a second, dielectric GGX layer on top,
// with its own roughness and normal (the mesh's, smooth over the normal map, unless the material says otherwise);
// what is under it is dimmed by the light the coat reflects and tinted by the coat.
void Clearcoat(inout LightingSurface s, float amount, float roughness, float ior, float3 tint, float3 normalWs)
{
    s.coat = saturate(amount);
    const float3 n = normalize(normalWs);
    s.coatNormalWs = n;
    // As the base's lobe (LitSurface): how fast the coat's normal turns across the pixel, its own normal map's bumps
    // too, widens it, or an orange peel finer than a pixel sparkles under a near mirror coat.
    const float3 dx = ddx_coarse(n), dy = ddy_coarse(n);
    const float variance = 0.25 * (dot(dx, dx) + dot(dy, dy));
    const float a = max(roughness, 0.045);
    s.coatRoughness = sqrt(sqrt(saturate(a * a * a * a + min(2.0 * variance, 0.18))));
    const float r = (ior - 1.0) / (ior + 1.0);
    s.coatF0 = r * r;
    s.coatTint = tint;
}

// vr_complex's choice: the direct light takes the occlusion only as far as the material says
// (g_flAmbientOcclusionDirectDiffuse and g_flAmbientOcclusionDirectSpecular, 0 none, 1 all).
void DirectOcclusion(inout LightingSurface s, float diffuseAmount, float specularAmount)
{
    s.directDiffuseOcclusion = lerp(1.0, s.occlusion, diffuseAmount);
    s.directSpecularOcclusion = lerp(1.0, s.occlusion, specularAmount);
}

// Multiple scattering (Blender's Multiscatter GGX, Kulla and Conty; the form Filament uses, Fdez-Aguera's): GGX
// counts the light that leaves the surface after one bounce off its microfacets, and on a rough surface the rest
// bounces between them and comes out too. Without it rough metals come out darker than they are (up to a third of
// the light at roughness 1). E, the share one bounce gives back with F = 1, is the engine's own split sum table
// (g_tBRDFLookup, x + y after squaring, ggx_integrate_brdf_lut_schlick: GGX with Smith k = a / 2); the specular
// is scaled by 1 + F0 (1 / E - 1), so smooth surfaces (E near 1) are left as they were and dielectrics nearly so.
void SpecularEnergy(inout LightingSurface s)
{
    const float3 toCamera = -normalize(WorldPosition(s.positionWs) - g_vCameraPositionWs);
    if (!g_bValveShading)
    {
        // Ours: the albedo of our own lobe (height-correlated Smith), from our table.
        const float e = BrdfAlbedo(max(dot(toCamera, s.normalWs), 0.0), s.roughness).x;
        s.specularEnergy = 1.0 + s.specularColor * (1.0 / max(e, 0.2) - 1.0);
        return;
    }
    float2 lut = g_tBRDFLookup.SampleLevel(g_sLinearClamp, float2(max(dot(toCamera, s.normalWs), 0.0), s.roughness), 0).xy;
    lut *= lut;
    const float e = lut.x + lut.y;
    // A table that is not bound reads 0: nothing is changed then.
    s.specularEnergy = e > 0.05 ? 1.0 + s.specularColor * (1.0 / max(e, 0.25) - 1.0) : 1.0;
}

// The terms every light shares.
struct Brdf
{
    float3 n;
    float3 toCamera;
    float diffusePower;         // a Lambert raised to (1 - roughness) * 0.8 + 0.6
    float diffuseScale;         // and scaled to keep its energy
    float alpha;                // roughness squared
    float visibilityK;          // (roughness + 1)^2 / 8
    float visibilityV;
    float3 f0;
    // Anisotropy: the GGX widths along and across the tangent (both alpha when there is none).
    float anisotropic;
    float alphaX;
    float alphaY;
    float3 t;
    float3 bt;
    // Ours (not Valve's shading): n.v, the metal's F82 b, the dielectric layer's F0 and roughness, EON's roughness
    // and A, the colour of the light it puts back (rhoMs / rho, per unit of the colour), and the view's parts of
    // the layer and of that light: (1 - E_s(v)) / (1 - mean E_s), (1 - E_FON(v)) / (1 - mean E_FON).
    bool valve;
    float nDotV;
    float3 f82;
    float layerF0;
    float roughness;
    float sigma;
    float fonA;
    float3 msShare;
    float layerView;
    float msView;
};

Brdf BrdfFor(LightingSurface s)
{
    Brdf b;
    b.n = s.normalWs;
    b.toCamera = -normalize(WorldPosition(s.positionWs) - g_vCameraPositionWs);
    b.diffusePower = (1.0 - s.roughness) * 0.8 + 0.6;
    b.diffuseScale = (b.diffusePower + 1.0) * 0.5;
    b.alpha = s.roughness * s.roughness;
    b.visibilityK = (s.roughness + 1.0) * (s.roughness + 1.0) * 0.125;
    b.visibilityV = max(dot(b.n, b.toCamera), 0.0) * (1.0 - b.visibilityK) + b.visibilityK;
    b.f0 = s.specularColor;
    b.anisotropic = s.anisotropic;
    const float aspect = sqrt(1.0 - 0.9 * s.anisotropic);
    b.alphaX = max(b.alpha / aspect, 0.002);
    b.alphaY = max(b.alpha * aspect, 0.002);
    b.t = s.anisoTangentWs;
    b.bt = cross(b.n, b.t);
    b.valve = g_bValveShading;
    b.nDotV = clamp(dot(b.n, b.toCamera), 1e-4, 1.0);
    b.f82 = s.f82;
    b.layerF0 = s.layerF0;
    b.roughness = s.roughness;
    b.sigma = saturate(s.diffuseRoughness);
    b.fonA = 1.0 / (1.0 + kFujii1 * b.sigma);
    const float eAvg = FonAlbedoMean(b.sigma);
    const float3 rho = saturate(s.diffuseColor);
    b.msShare = rho * eAvg / max(1.0 - rho * (1.0 - eAvg), 1e-4);
    b.msView = max(1e-6, 1.0 - FonAlbedo(b.nDotV, b.sigma)) / max(1e-6, 1.0 - eAvg);
    b.layerView = (1.0 - LayerAlbedo(b.layerF0, BrdfAlbedo(b.nDotV, b.roughness))) /
                  max(1e-4, 1.0 - LayerAlbedoMean(b.layerF0, b.roughness));
    return b;
}

// The diffuse from one light (l, n.l above 0), per unit of the diffuse colour, n.l in. Valve's: a Lambert raised to a
// power by the roughness. Ours: EON, Fujii's Oren-Nayar A (1 + sigma s / t) with the light it loses between its
// facets put back (a share that depends on the colour, brighter colours more), under the specular layer, which
// takes what it reflects from the light on the way in and toward the view on the way out.
float3 DiffuseTerm(Brdf b, float3 l, float nDotL)
{
    if (b.valve)
        return pow(nDotL, b.diffusePower) * b.diffuseScale;
    const float mi = clamp(nDotL, 1e-4, 1.0);
    const float s = dot(l, b.toCamera) - mi * b.nDotV;
    const float st = s > 0.0 ? s / max(mi, b.nDotV) : s;
    const float single = b.fonA * (1.0 + b.sigma * st);
    const float3 multi = b.msShare * (b.msView * max(1e-6, 1.0 - FonAlbedo(mi, b.sigma)));
    const float layer = b.layerView * (1.0 - LayerAlbedo(b.layerF0, BrdfAlbedo(mi, b.roughness)));
    return (single + multi) * layer * nDotL;
}

float3 SpecularTerm(Brdf b, float3 l, float nDotL)
{
    const float3 h = normalize(l + b.toCamera);
    const float nDotH = dot(h, b.n);
    float d;
    if (b.anisotropic > 0.0)
    {
        // Anisotropic GGX (no 1/pi, as the rest): 1 / (ax ay ((t.h/ax)^2 + (b.h/ay)^2 + (n.h)^2)^2). At equal
        // widths it is the isotropic term.
        const float x = dot(b.t, h) / b.alphaX, y = dot(b.bt, h) / b.alphaY;
        const float k = x * x + y * y + nDotH * nDotH;
        d = 1.0 / (b.alphaX * b.alphaY * k * k);
    }
    else
    {
        d = b.alpha / (nDotH * nDotH * (b.alpha * b.alpha - 1.0) + 1.0);
        d *= d;
    }
    const float c = saturate(dot(l, h));
    if (b.valve)
    {
        const float visibility = 4.0 * b.visibilityV * (nDotL * (1.0 - b.visibilityK) + b.visibilityK);
        const float f = pow(1.0 - c, 5.0);
        return nDotL * d / visibility * ((1.0 - b.f0) * f + b.f0);
    }
    // The height-correlated Smith visibility, G2 / (4 n.l n.v) (Heitz 2014), anisotropic when the lobe is.
    const float nl = max(nDotL, 1e-4), nv = b.nDotV;
    float visibility;
    if (b.anisotropic > 0.0)
        visibility = 0.5 / (nl * length(float3(b.alphaX * dot(b.t, b.toCamera), b.alphaY * dot(b.bt, b.toCamera), nv)) +
                            nv * length(float3(b.alphaX * dot(b.t, l), b.alphaY * dot(b.bt, l), nl)));
    else
    {
        const float a2 = b.alpha * b.alpha;
        visibility = 0.5 / (nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2));
    }
    // Schlick's Fresnel, with the metal's F82 tint taken off near grazing.
    const float m = 1.0 - c, m5 = m * m * m * m * m;
    const float3 f = b.f0 + (1.0 - b.f0) * m5 - b.f82 * c * m5 * m;
    return nDotL * d * visibility * max(f, 0.0);
}

// The highlight of a light with a size (a sphere of that radius, toLight from the surface to its centre), as Karis
// 2013 has it: the surface's own lobe toward the point of the sphere nearest the mirror direction (which spreads
// the highlight over the light's disc, flat on top), its energy brought back to the light's by (a / a')^2, a' the
// lobe widened by the light's half angle, radius / (2 d). A bigger light gives a wider, softer highlight of the same
// light. A point (0) or Valve's shading: the plain term toward the centre.
float3 SizedSpecular(Brdf b, float3 toLight, float radius, float nDotL)
{
    const float d = max(length(toLight), 1e-4);
    if (radius <= 0.0 || b.valve)
        return SpecularTerm(b, toLight / d, nDotL);
    const float3 r = reflect(-b.toCamera, b.n);
    const float3 toRay = dot(toLight, r) * r - toLight;
    const float3 nearest = toLight + toRay * saturate(radius / max(length(toRay), 1e-4));
    const float3 l = normalize(nearest);
    const float keep = b.alpha / saturate(b.alpha + radius / (2.0 * d));
    // The horizon: a sphere light sinks below it over its angular radius (sin = radius / d), not at once at its
    // centre, or a big light near the surface draws a hard line where its centre crosses the horizon.
    const float sinAngle = saturate(radius / d);
    const float above = saturate((dot(b.n, toLight / d) + sinAngle) / (2.0 * sinAngle));
    return SpecularTerm(b, l, max(dot(b.n, l), 0.0)) * (keep * keep) * (above * above * (3.0 - 2.0 * above));
}

// The clear coat's highlight from one light, in the terms SpecularTerm uses (no 1/pi): GGX on the coat's normal and
// roughness, Kelemen's visibility 1/(4 (l.h)^2) for a thin coat, Schlick's Fresnel from the coat's F0.
float CoatTerm(LightingSurface s, Brdf b, float3 l)
{
    const float nDotL = saturate(dot(s.coatNormalWs, l));
    if (nDotL <= 0.0)
        return 0.0;
    const float3 h = normalize(l + b.toCamera);
    const float nDotH = saturate(dot(s.coatNormalWs, h));
    const float lDotH = max(dot(l, h), 0.001);
    const float a = s.coatRoughness * s.coatRoughness;
    float d = a / (nDotH * nDotH * (a * a - 1.0) + 1.0);
    d *= d;
    const float f = s.coatF0 + (1.0 - s.coatF0) * pow(1.0 - lDotH, 5.0);
    return nDotL * d * f / (4.0 * lDotH * lDotH);
}

// The coat's highlight from a light with a size, as SizedSpecular has it, on the coat's own normal and roughness.
float SizedCoatTerm(LightingSurface s, Brdf b, float3 toLight, float radius)
{
    const float d = max(length(toLight), 1e-4);
    if (radius <= 0.0 || b.valve)
        return CoatTerm(s, b, toLight / d);
    const float3 r = reflect(-b.toCamera, s.coatNormalWs);
    const float3 toRay = dot(toLight, r) * r - toLight;
    const float3 nearest = toLight + toRay * saturate(radius / max(length(toRay), 1e-4));
    const float a = max(s.coatRoughness * s.coatRoughness, 1e-4);
    const float keep = a / saturate(a + radius / (2.0 * d));
    const float sinAngle = saturate(radius / d);
    const float above = saturate((dot(s.coatNormalWs, toLight / d) + sinAngle) / (2.0 * sinAngle));
    return CoatTerm(s, b, normalize(nearest)) * (keep * keep) * (above * above * (3.0 - 2.0 * above));
}

// A flake's highlight from one light, in the terms CoatTerm uses: GGX on the flake's normal and roughness, Kelemen's
// visibility, Schlick's Fresnel from the flake's F0. Light from under the surface reaches no flake.
float3 GlitterTerm(LightingSurface s, Brdf b, float3 l)
{
    const float nDotL = saturate(dot(s.glitterNormalWs, l));
    if (nDotL <= 0.0 || dot(b.n, l) <= 0.0)
        return 0.0;
    const float3 h = normalize(l + b.toCamera);
    const float nDotH = saturate(dot(s.glitterNormalWs, h));
    const float lDotH = max(dot(l, h), 0.001);
    const float a = s.glitterRoughness * s.glitterRoughness;
    float d = a / (nDotH * nDotH * (a * a - 1.0) + 1.0);
    d *= d;
    const float3 f = s.glitterF0 + (1.0 - s.glitterF0) * pow(1.0 - lDotH, 5.0);
    return nDotL * d * f / (4.0 * lDotH * lDotH);
}

// The grating's light from one light. With l and v both leaving the surface, order m goes where (l + v).g =
// m wavelength / spacing along the groove direction g, and (l + v) has nothing across it (as a mirror's): so the
// direction says which wavelength is seen (orders +1 and -1, one on each side), and the across part, spread by the
// roughness, how much. Each order carries a quarter of the light (half the foil's light is diffracted, the other
// half reflects as a mirror would, which is the surface's own specular).
float3 HoloTerm(LightingSurface s, Brdf b, float3 l)
{
    const float nDotL = saturate(dot(b.n, l));
    if (nDotL <= 0.0)
        return 0.0;
    const float3 sum = l + b.toCamera;
    const float along = dot(sum, s.holoDirWs);
    const float across = dot(sum, cross(b.n, s.holoDirWs)) / s.holoRoughness;
    const float spread = exp(-across * across) / (s.holoRoughness * 1.7724539);
    return 0.25 * nDotL * spread * Spectrum(s.holoSpacing * abs(along));
}

// The layers that shine with every light's specular: the clear coat, the glitter and the holographic foil.
void LayerLights(LightingSurface s, Brdf b, float3 l, float3 light, inout DirectLight o)
{
    if (s.coat > 0.0)
        o.coat += CoatTerm(s, b, l) * light;
    if (s.glitter > 0.0)
        o.glitter += GlitterTerm(s, b, l) * light;
    if (s.holographic > 0.0)
        o.holo += HoloTerm(s, b, l) * light;
}

// The same for a light with a size (toLight from the surface to its centre): the coat's highlight spread over it,
// the flakes' and the foil's kept to its centre (each flake is a tiny mirror of its own, a glint).
void SizedLayerLights(LightingSurface s, Brdf b, float3 toLight, float radius, float3 light, inout DirectLight o)
{
    const float3 l = normalize(toLight);
    if (s.coat > 0.0)
        o.coat += SizedCoatTerm(s, b, toLight, radius) * light;
    if (s.glitter > 0.0)
        o.glitter += GlitterTerm(s, b, l) * light;
    if (s.holographic > 0.0)
        o.holo += HoloTerm(s, b, l) * light;
}

// The sheen from one light, in the lights' terms (no 1/pi: pi D V (n.l)), before the sheen's colour.
float SheenTerm(LightingSurface s, Brdf b, float3 l)
{
    const float nDotL = saturate(dot(b.n, l));
    if (nDotL <= 0.0)
        return 0.0;
    const float nDotV = max(dot(b.n, b.toCamera), 0.001);
    const float nDotH = saturate(dot(b.n, normalize(l + b.toCamera)));
    const float inv = 1.0 / (s.sheenRoughness * s.sheenRoughness);
    const float d = (2.0 + inv) * pow(max(1.0 - nDotH * nDotH, 0.0078125), inv * 0.5) * 0.5;
    const float v = 1.0 / (4.0 * (nDotL + nDotV - nDotL * nDotV));
    return d * v * nDotL;
}

// The light list's shadow test: four bilinear comparisons, and nine weighted ones in the penumbra
// (g_vShadow3x3PCFTermC0..C3 hold the offsets and weights).
float ShadowPcf(float3 uvz)
{
    const float z = saturate(uvz.z - 0.000001);
    const float4 c0 = g_vShadow3x3PCFTermC0, c1 = g_vShadow3x3PCFTermC1, c2 = g_vShadow3x3PCFTermC2,
                 c3 = g_vShadow3x3PCFTermC3;
    float4 t;
    t.x = g_tVrShadowDepthBufferDepth.SampleCmpLevelZero(g_sShadowCompare, uvz.xy + c3.xx, z);
    t.y = g_tVrShadowDepthBufferDepth.SampleCmpLevelZero(g_sShadowCompare, uvz.xy + c3.yx, z);
    t.z = g_tVrShadowDepthBufferDepth.SampleCmpLevelZero(g_sShadowCompare, uvz.xy + c3.xy, z);
    t.w = g_tVrShadowDepthBufferDepth.SampleCmpLevelZero(g_sShadowCompare, uvz.xy + c3.yy, z);
    float shadow = dot(t, 0.25);
    if (shadow != 0.0 && shadow != 1.0)
    {
        t.x = g_tVrShadowDepthBufferDepth.SampleCmpLevelZero(g_sShadowCompare, uvz.xy + c2.wz, z);
        t.y = g_tVrShadowDepthBufferDepth.SampleCmpLevelZero(g_sShadowCompare, uvz.xy + c1.zw, z);
        t.z = g_tVrShadowDepthBufferDepth.SampleCmpLevelZero(g_sShadowCompare, uvz.xy + c1.wz, z);
        t.w = g_tVrShadowDepthBufferDepth.SampleCmpLevelZero(g_sShadowCompare, uvz.xy + c2.zw, z);
        shadow = shadow * 4.0 * c0.w + dot(t, c1.xxxx);
        shadow += g_tVrShadowDepthBufferDepth.SampleCmpLevelZero(g_sShadowCompare, uvz.xy, z) * c1.y;
    }
    return shadow;
}

float3 Project(float4x4 m, float3 p)
{
    const float4 q = mul(m, float4(p, 1.0));
    return q.xyz / q.w;
}

// The light list: S2FM's point, spot and orthographic lights (an orthographic light arrives as a directional one,
// a shadowed point light as six cone lights, one per cube face). g_vLightParams holds integer switches (x shadow,
// y cookie, z diffuse, w specular), declared int4 so they are tested as bits as Valve's code does: read as floats
// they are denormals, which the GPU takes as 0, and no light list light reached the surface.
void ListLights(LightingSurface s, Brdf b, inout DirectLight o)
{
    for (int n = 0; n < g_vNumLights.x; n++)
    {
        const float3 toLight = g_vLightPosition_flInvRadius[n].xyz - s.positionWs;
        const float d2 = dot(toLight, toLight);
        if (d2 > g_vLightFalloffParams[n].z)
            continue;
        const float3 l = toLight * rsqrt(d2);
        const float cone = dot(l, -g_vLightDirection[n].xyz) - g_vSpotLightInnerOuterConeCosines[n].y;
        if (cone <= 0.0)
            continue;
        float3 cookie;
        if (g_vLightParams[n].y)
            cookie = g_tVrLightCookieTexture.SampleLevel(g_sCookie, Project(g_matWorldToLightCookie[n], s.positionWs), 0).rgb;
        else
            cookie = saturate(cone * g_vSpotLightInnerOuterConeCosines[n].z);
        float shadow = 1.0;
        if (g_vLightParams[n].x)
        {
            const float3 uvz = Project(g_matWorldToShadow[n], s.positionWs);
            const float4 bounds = g_vSpotLightShadowBounds[n];
            const bool inside = all(uvz.xy >= bounds.xy) && all(uvz.xy <= bounds.zw) && uvz.z >= 0.0 && uvz.z <= 1.0;
            shadow = inside ? ShadowPcf(uvz) : 0.0;
        }
        if (shadow <= 0.0)
            continue;
        const float d2c = max(d2, 1.0);
        const float falloff = saturate(1.0 / dot(float2(sqrt(d2c), d2c), g_vLightFalloffParams[n].xy) - g_vLightFalloffParams[n].w);
        const float3 color = falloff * g_vLightColor[n].xyz * cookie * shadow;
        const float nDotL = max(dot(b.n, l), 0.0);
        if (g_vLightParams[n].z)
        {
            o.diffuse += DiffuseLight(s, l, DiffuseTerm(b, l, nDotL)) * color;
            if (any(s.sheen > 0.0))
                o.sheen += SheenTerm(s, b, l) * color;
        }
        if (g_vLightParams[n].w)
        {
            const float size = ListLightSize(n);
            o.specular += SizedSpecular(b, toLight, size, nDotL) * color;
            SizedLayerLights(s, b, toLight, size, color, o);
        }
    }
}

// Light list light n at a point p (world, not offset): its colour through its cone or cookie, falloff and shadow; l
// the direction toward it. For the volumes; ListLights works the same light out inline (kept as it was there, so the
// surface shaders compile to the same code).
float3 ListLightAt(int n, float3 p, out float3 l)
{
    const float3 q = p - g_vHighPrecisionLightingOffsetWs.xyz;
    const float3 toLight = g_vLightPosition_flInvRadius[n].xyz - q;
    const float d2 = dot(toLight, toLight);
    l = toLight * rsqrt(max(d2, 1e-8));
    if (d2 > g_vLightFalloffParams[n].z || !g_vLightParams[n].z)
        return 0.0;
    const float cone = dot(l, -g_vLightDirection[n].xyz) - g_vSpotLightInnerOuterConeCosines[n].y;
    if (cone <= 0.0)
        return 0.0;
    float3 cookie;
    if (g_vLightParams[n].y)
        cookie = g_tVrLightCookieTexture.SampleLevel(g_sCookie, Project(g_matWorldToLightCookie[n], q), 0).rgb;
    else
        cookie = saturate(cone * g_vSpotLightInnerOuterConeCosines[n].z);
    float shadow = 1.0;
    if (g_vLightParams[n].x)
    {
        const float3 uvz = Project(g_matWorldToShadow[n], q);
        const float4 bounds = g_vSpotLightShadowBounds[n];
        const bool inside = all(uvz.xy >= bounds.xy) && all(uvz.xy <= bounds.zw) && uvz.z >= 0.0 && uvz.z <= 1.0;
        shadow = inside ? ShadowPcf(uvz) : 0.0;
    }
    const float d2c = max(d2, 1.0);
    const float falloff = saturate(1.0 / dot(float2(sqrt(d2c), d2c), g_vLightFalloffParams[n].xy) - g_vLightFalloffParams[n].w);
    return falloff * g_vLightColor[n].xyz * cookie * shadow;
}

// The sun's cascades: sixteen comparisons around the texel.
float SunPcf(float2 uv, float z)
{
    float sum = 0.0;
    [unroll] for (int y = -2; y <= 1; y++)
        [unroll] for (int x = -2; x <= 1; x++)
            sum += g_tShadowDepthMapAtlas.SampleCmpLevelZero(g_sShadowCompare, uv, z, int2(x, y));
    return sum / 16.0;
}

float3 SunCascade(int c, float3 p)
{
    return mul(float4(p, 1.0), g_matWorldToShadowTexMatrices[c]).xyz;
}

float SunShadow(float3 p)
{
    float shadow = 1.0;
    if (g_nSunShadowCascadeCount == 1)
    {
        const float3 q = SunCascade(0, p);
        if (all(saturate(q.xy) == q.xy))
            shadow = SunPcf(q.xy * g_vCascadeAtlasUVOffsets[0].zw + g_vCascadeAtlasUVOffsets[0].xy, q.z);
    }
    else
    {
        int c = -1;
        for (int k = 0; k < g_nSunShadowCascadeCount; k++)
        {
            const float2 q = SunCascade(k, p).xy;
            if (all(saturate(q) == q))
            {
                c = k;
                break;
            }
        }
        if (c >= 0)
        {
            const float3 q = SunCascade(c, p);
            // Near a cascade's edge it blends into the next one.
            const float2 e = 1.0 - saturate((abs(q.xy - 0.5) - g_vSunTexParams2.z) * g_vSunTexParams2.w);
            const float blend = 1.0 - e.x * e.y;
            shadow = SunPcf(q.xy * g_vCascadeAtlasUVOffsets[c].zw + g_vCascadeAtlasUVOffsets[c].xy, q.z);
            if (blend > 0.0)
            {
                float next = 1.0;
                if (c < g_nSunShadowCascadeCount - 1)
                {
                    const float3 r = SunCascade(c + 1, p);
                    next = SunPcf(r.xy * g_vCascadeAtlasUVOffsets[c + 1].zw + g_vCascadeAtlasUVOffsets[c + 1].xy, r.z);
                }
                shadow = lerp(shadow, next, blend);
            }
        }
    }
    // Fades out with distance from the camera.
    const float fade = saturate((length(p - g_vCameraPositionWs) - g_vSunTexParams3.x) * g_vSunTexParams3.y);
    return lerp(shadow, 1.0, fade);
}

// The sun's light at a point for what needs no soft shadow (falling rain and snow): the first cascade holding it, one
// filtered comparison.
float3 SunLightQuick(float3 p)
{
    if (g_nSunShadowCascadeCount <= 0)
        return 0.0;
    float shadow = 1.0;
    for (int k = 0; k < g_nSunShadowCascadeCount; k++)
    {
        const float3 q = SunCascade(k, p);
        if (all(saturate(q.xy) == q.xy))
        {
            shadow = g_tShadowDepthMapAtlas.SampleCmpLevelZero(g_sShadowCompare,
                                                               q.xy * g_vCascadeAtlasUVOffsets[k].zw + g_vCascadeAtlasUVOffsets[k].xy, q.z);
            break;
        }
    }
    const float fade = saturate((length(p - g_vCameraPositionWs) - g_vSunTexParams3.x) * g_vSunTexParams3.y);
    return g_vSunLightColor.xyz * lerp(shadow, 1.0, fade);
}

// The sun's light at a point (world position, not offset): its colour times its shadow there; 0 without a sun.
float3 SunLightAt(float3 p)
{
    if (g_nSunShadowCascadeCount <= 0)
        return 0.0;
    return g_vSunLightColor.xyz * SunShadow(p);
}

void Sun(LightingSurface s, Brdf b, inout DirectLight o)
{
    // Subsurface scattering carries light a little past the terminator, so it keeps the sun there too.
    if (g_nSunShadowCascadeCount <= 0 || (dot(g_vSunLightDir, b.n) <= 0.0 && s.subsurface <= 0.0))
        return;
    const float shadow = SunShadow(WorldPosition(s.positionWs));
    if (shadow <= 0.0)
        return;
    const float3 l = g_vSunLightDir;
    const float nDotL = max(dot(b.n, l), 0.0);
    const float3 color = g_vSunLightColor.xyz * shadow;
    o.diffuse += DiffuseLight(s, l, DiffuseTerm(b, l, nDotL)) * color;
    o.specular += SpecularTerm(b, l, nDotL) * color;
    LayerLights(s, b, l, color, o);
    if (any(s.sheen > 0.0))
        o.sheen += SheenTerm(s, b, l) * color;
}

// A projected light's footprint: a square with corners rounded by roundness, softened over spotFalloff.
float SfmWindow(float2 xy, float spotFalloff, float roundness)
{
    const float m = max(abs(xy.x), abs(xy.y));
    if (m > 1.0)
        return 0.0;
    const float f = saturate(spotFalloff) * 0.9998 + 0.0001;
    const float r = saturate(roundness) * 0.9998 + 0.0001;
    if (min(abs(xy.x), abs(xy.y)) / m < 1.0 - r)
        return saturate(1.0 - (m - (1.0 - f)) / f);
    const float edge = 1.0 - f;
    const float2 a = abs(xy);
    const float k = r * r - 4.0 * r + 2.0;
    const float s = a.x + a.y;
    const float q = dot(a, a);
    const float outer = (s * (1.0 - r) + sqrt((1.0 - r) * s * (1.0 - r) * s - k * q)) / q;
    const float2 a2 = a / edge;
    const float s2 = a2.x + a2.y;
    const float q2 = dot(a2, a2);
    const float inner = (s2 * (1.0 - r) + sqrt((1.0 - r) * s2 * (1.0 - r) * s2 - k * q2)) / q2;
    if (edge * (2.0 - r) >= s)
        return 1.0;
    if (outer < 1.0)
        return 0.0;
    return saturate(1.0 - (1.0 - inner) / (outer - inner));
}

// A projected light's shadow: four comparisons, their offsets turned by a per pixel random angle.
float SfmShadow(float3 uvz, float slice, float filterSize)
{
    const float angle = frac(sin(dot(uvz.xy + uvz.z, float2(12.9898, 78.233))) * 43758.5469) * 3.14159265;
    float sn, cs;
    sincos(angle, sn, cs);
    const float4 d01 = float4(g_SFMLight_ShadowSampleDelta0, g_SFMLight_ShadowSampleDelta1) * filterSize;
    const float4 d23 = float4(g_SFMLight_ShadowSampleDelta2, g_SFMLight_ShadowSampleDelta3) * filterSize;
    const float2x2 turn = float2x2(cs, -sn, sn, cs);
    float sum = 0.0;
    sum += g_tSFMShadowDepthTexture.SampleCmpLevelZero(g_sShadowCompare, float3(uvz.xy + mul(turn, d01.xy), slice), uvz.z);
    sum += g_tSFMShadowDepthTexture.SampleCmpLevelZero(g_sShadowCompare, float3(uvz.xy + mul(turn, d01.zw), slice), uvz.z);
    sum += g_tSFMShadowDepthTexture.SampleCmpLevelZero(g_sShadowCompare, float3(uvz.xy + mul(turn, d23.xy), slice), uvz.z);
    sum += g_tSFMShadowDepthTexture.SampleCmpLevelZero(g_sShadowCompare, float3(uvz.xy + mul(turn, d23.zw), slice), uvz.z);
    return sum * 0.25;
}

// S2FM's projected light i at a point p (world): its colour through its window, its falloff, shadow and cookie; l the
// direction toward it. For the volumes; the same light as SfmLights works out inline (kept as it was there, so the
// surface shaders compile to the same code).
float3 SfmLightAt(int i, float3 p, out float3 l)
{
    const float4 clip = mul(float4(p, 1.0), g_SFMLight_ViewProj[i]);
    const float3 proj = clip.xyz / clip.w;
    const float3 toLight = g_SFMLight_Position_Roundness[i].xyz - p;
    const float d2 = dot(toLight, toLight);
    l = toLight * rsqrt(d2);
    const float4 atten = g_SFMLight_Attenuation_CookieTexIndex[i];
    const float4 depth = g_SFMLight_DepthParams_SpotFalloff[i];
    const float invFar = 1.0 / depth.z;
    const float d2c = max(d2, 8.0);
    float falloff = saturate(1.0 / dot(atten.yz, float2(sqrt(d2c), d2c)) - dot(atten.yz, float2(invFar, invFar * invFar)));
    // Only inside its frustum, near plane to far: past the far plane (Far Z) it lights nothing, as the engine's own
    // volumetrics end there; without that test it lit clouds a mountain's distance away.
    if (dot(toLight, -g_SFMLight_Direction_ShadowFilterSize[i].xyz) <= 0.0 || proj.z <= 0.0 || proj.z >= 1.0)
        falloff = 0.0;
    const float window = SfmWindow(proj.xy, depth.w, g_SFMLight_Position_Roundness[i].w);
    const float3 uvz = proj * float3(0.5, -0.5, 1.0) + float3(0.5, 0.5, 0.0);
    const float4 color = g_SFMLight_Color_ShadowTexIndex[i];
    float shadow = color.w >= 0.0 ? SfmShadow(uvz, color.w, g_SFMLight_Direction_ShadowFilterSize[i].w) : 1.0;
    const float ambient = g_SFMLight_PadXYZ_AmbientIntensity[i].w;
    shadow = shadow * (1.0 - ambient) + ambient;
    const float4 gobo = g_SFMLight_GoboTexCoords[i];
    float3 c = color.xyz;
    if (gobo.z * gobo.w != 0.0)
        c *= g_tSFMCookieTextureArray.SampleLevel(g_sLinearClamp, float3(uvz.xy * gobo.zw + gobo.xy, atten.w), 0).rgb;
    return max(c * shadow * falloff * window, 0.0);
}

// S2FM's projected lights (its own buffer, up to 32). Valve's shaders give them a plain Lambert diffuse only; with
// g_bProjectedLightSpecular they take the light list's diffuse and specular terms.
void SfmLights(LightingSurface s, Brdf b, inout DirectLight o)
{
    if (!g_bIsSFM)
        return;
    const float3 p = WorldPosition(s.positionWs);
    for (float n = 0.0; n < g_SFMLight_Count; n += 1.0)
    {
        const int i = (int)n;
        const float4 clip = mul(float4(p, 1.0), g_SFMLight_ViewProj[i]);
        const float3 proj = clip.xyz / clip.w;
        const float3 toLight = g_SFMLight_Position_Roundness[i].xyz - p;
        const float d2 = dot(toLight, toLight);
        const float3 l = toLight * rsqrt(d2);
        const float4 atten = g_SFMLight_Attenuation_CookieTexIndex[i];
        const float4 depth = g_SFMLight_DepthParams_SpotFalloff[i];
        const float invFar = 1.0 / depth.z;
        const float d2c = max(d2, 8.0);
        float falloff = saturate(1.0 / dot(atten.yz, float2(sqrt(d2c), d2c)) - dot(atten.yz, float2(invFar, invFar * invFar)));
        if (dot(toLight, -g_SFMLight_Direction_ShadowFilterSize[i].xyz) <= 0.0 || proj.z <= 0.0)
            falloff = 0.0;
        const float window = SfmWindow(proj.xy, depth.w, g_SFMLight_Position_Roundness[i].w);
        const float3 uvz = proj * float3(0.5, -0.5, 1.0) + float3(0.5, 0.5, 0.0);
        const float4 color = g_SFMLight_Color_ShadowTexIndex[i];
        float shadow = color.w >= 0.0 ? SfmShadow(uvz, color.w, g_SFMLight_Direction_ShadowFilterSize[i].w) : 1.0;
        const float ambient = g_SFMLight_PadXYZ_AmbientIntensity[i].w;
        shadow = shadow * (1.0 - ambient) + ambient;
        const float4 gobo = g_SFMLight_GoboTexCoords[i];
        float3 c = color.xyz;
        if (gobo.z * gobo.w != 0.0)
            c *= g_tSFMCookieTextureArray.SampleLevel(g_sLinearClamp, float3(uvz.xy * gobo.zw + gobo.xy, atten.w), 0).rgb;
        const float3 light = max(c * shadow * falloff * window, 0.0);
        const float nDotL = saturate(dot(l, b.n));
        if (g_bProjectedLightSpecular)
        {
            o.diffuse += DiffuseLight(s, l, DiffuseTerm(b, l, nDotL)) * light;
            o.specular += SizedSpecular(b, toLight, b.valve ? 0.0 : ProjectedLightSize(g_SFMLight_Position_Roundness[i].xyz),
                                        nDotL) * light;
        }
        else
        {
            o.diffuse += DiffuseLight(s, l, b.valve ? (float3)nDotL : DiffuseTerm(b, l, nDotL)) * light;
        }
        // The coat, glitter, foil and sheen are ours, not Valve's: they shine under every light, projected ones too.
        SizedLayerLights(s, b, toLight, ProjectedLightSize(g_SFMLight_Position_Roundness[i].xyz), light, o);
        if (any(s.sheen > 0.0))
            o.sheen += SheenTerm(s, b, l) * light;
    }
}

#if D_BAKED_LIGHTING_FROM_PROBE
float3 ProbePosition(float3 p)
{
    return mul(g_matLightProbeVolumeWorldToLocal, float4(p, 1.0));
}

// The map's baked lights that keep a dynamic shadow (in two cascades, or one map with bounds).
float BakedShadow(float3 positionWs)
{
    const float4 p = float4(positionWs, 1.0);
    if (g_vBakedLightCascadeThreshold[0].w != 0.0)
    {
        const float c0 = max(abs(dot(g_vBakedLightCascadeThreshold[0], p)), abs(dot(g_vBakedLightCascadeThreshold[1], p)));
        const float c1 = max(abs(dot(g_vBakedLightCascadeThreshold[2], p)), abs(dot(g_vBakedLightCascadeThreshold[3], p)));
        const float w0 = smoothstep(0.0, 1.0, saturate((c0 - 1.0) / (g_vBakedLightShadowStrengthVignette.y - 1.0)));
        const float w1 = smoothstep(0.0, 1.0, saturate((c1 - 1.0) / (g_vBakedLightShadowStrengthVignette.z - 1.0)));
        float shadow = (1.0 - w0) * (1.0 - w1);
        if (w0 != 0.0)
            shadow += w0 * ShadowPcf(Project(g_matWorldToBakedLightShadow[0], positionWs));
        if ((1.0 - w0) * w1 != 0.0)
            shadow += (1.0 - w0) * w1 * ShadowPcf(Project(g_matWorldToBakedLightShadow[1], positionWs));
        return shadow;
    }
    const float3 uvz = Project(g_matWorldToBakedLightShadow[0], positionWs);
    const float4 bounds = g_vBakedLightShadowBounds;
    const bool inside = all(uvz.xy >= bounds.xy) && all(uvz.xy <= bounds.zw) && uvz.z >= 0.0 && uvz.z <= 1.0;
    return inside ? ShadowPcf(uvz) : 1.0;
}

// Up to four of the map's baked lights reach this probe (the sun of a baked map among them): a plain Lambert and
// Valve's specular, no distance falloff (the strength carries it).
void BakedLights(LightingSurface s, Brdf b, inout DirectLight o)
{
    const float3 p = WorldPosition(s.positionWs);
    const float3 local = ProbePosition(p);
    const int4 index = int4(g_tLightProbeVolumeTextureDirectLightIndices.SampleLevel(g_sPointClamp, local, 0) * 255.0);
    float4 strength = g_tLightProbeVolumeTextureDirectLightScalars.SampleLevel(g_sBilinearClamp, local, 0);
    strength *= strength;
    const float4 shadowed = (index == g_vBakedLightShadowIntParams.xxxx) ? g_vBakedLightShadowStrengthVignette.x : 0.0;
    if (any(strength * shadowed != 0.0))
        strength *= shadowed * (BakedShadow(s.positionWs) - 1.0) + 1.0;
    [loop] for (int k = 0; k < 4; k++)
    {
        if (strength[k] <= 0.0001)
            continue;
        const float4 color = g_vBakedLightColor[index[k]];
        const float3 l = normalize(g_vBakedLightPosition[index[k]].xyz - p);
        const float nDotL = max(dot(b.n, l), 0.0);
        o.diffuse += DiffuseLight(s, l, b.valve ? (float3)nDotL : DiffuseTerm(b, l, nDotL)) * color.xyz * strength[k];
        if (any(s.sheen > 0.0))
            o.sheen += SheenTerm(s, b, l) * color.xyz * strength[k];
        if (color.w != 0.0)
        {
            o.specular += SpecularTerm(b, l, nDotL) * color.xyz * strength[k];
            LayerLights(s, b, l, color.xyz * strength[k], o);
        }
    }
}
#endif

// Every light that reaches the surface directly, plus the tools ambient (the scene's ambient colour in S2FM).
DirectLight DirectLighting(LightingSurface s)
{
    const Brdf b = BrdfFor(s);
    DirectLight o;
    o.diffuse = 0.0;
    o.specular = 0.0;
    o.coat = 0.0;
    o.sheen = 0.0;
    o.glitter = 0.0;
    o.holo = 0.0;
    ListLights(s, b, o);
    Sun(s, b, o);
    SfmLights(s, b, o);
    o.diffuse += g_vToolsAmbientLightingVr.xyz;
#if D_BAKED_LIGHTING_FROM_PROBE
    BakedLights(s, b, o);
#endif
    return o;
}

// The ambient light from the probe volume at a point (world) for a normal n: an ambient cube (six colours, one per
// axis direction) weighted by n. Zero for an object that takes no probe.
float3 AmbientAt(float3 p, float3 n)
{
#if D_BAKED_LIGHTING_FROM_PROBE
    const float3 local = ProbePosition(p);
    const float3 uvw = clamp(float3(local.xy, local.z / 6.0), g_vLightProbeVolumeLayer0TextureMin.xyz,
                             g_vLightProbeVolumeLayer0TextureMax.xyz);
    const float3 face = float3(n.x > 0.0 ? 0.0 : 0.5, n.y > 0.0 ? 1.0 / 6.0 : 2.0 / 3.0, n.z > 0.0 ? 1.0 / 3.0 : 5.0 / 6.0);
    float3 x, y, z;
    if (g_vLightProbeVolumeColor.w != 0.0)
    {
        x = g_tLightProbeVolumeTexture.SampleLevel(g_sPointClamp, uvw + float3(0, 0, face.x), 0).rgb;
        y = g_tLightProbeVolumeTexture.SampleLevel(g_sPointClamp, uvw + float3(0, 0, face.y), 0).rgb;
        z = g_tLightProbeVolumeTexture.SampleLevel(g_sPointClamp, uvw + float3(0, 0, face.z), 0).rgb;
    }
    else
    {
        x = g_tLightProbeVolumeTexture.SampleLevel(g_sBilinearClamp, uvw + float3(0, 0, face.x), 0).rgb;
        y = g_tLightProbeVolumeTexture.SampleLevel(g_sBilinearClamp, uvw + float3(0, 0, face.y), 0).rgb;
        z = g_tLightProbeVolumeTexture.SampleLevel(g_sBilinearClamp, uvw + float3(0, 0, face.z), 0).rgb;
    }
    const float3 w = n * n;
    return (w.x * x + w.y * y + w.z * z) * g_vLightProbeVolumeColor.xyz;
#else
    return 0.0;
#endif
}

// The same for a surface (kept written out, so the surface shaders compile to the same code).
float3 MapAmbientDiffuse(LightingSurface s)
{
#if D_BAKED_LIGHTING_FROM_PROBE
    const float3 local = ProbePosition(WorldPosition(s.positionWs));
    const float3 uvw = clamp(float3(local.xy, local.z / 6.0), g_vLightProbeVolumeLayer0TextureMin.xyz,
                             g_vLightProbeVolumeLayer0TextureMax.xyz);
    const float3 n = s.normalWs;
    const float3 face = float3(n.x > 0.0 ? 0.0 : 0.5, n.y > 0.0 ? 1.0 / 6.0 : 2.0 / 3.0, n.z > 0.0 ? 1.0 / 3.0 : 5.0 / 6.0);
    float3 x, y, z;
    if (g_vLightProbeVolumeColor.w != 0.0)
    {
        x = g_tLightProbeVolumeTexture.SampleLevel(g_sPointClamp, uvw + float3(0, 0, face.x), 0).rgb;
        y = g_tLightProbeVolumeTexture.SampleLevel(g_sPointClamp, uvw + float3(0, 0, face.y), 0).rgb;
        z = g_tLightProbeVolumeTexture.SampleLevel(g_sPointClamp, uvw + float3(0, 0, face.z), 0).rgb;
    }
    else
    {
        x = g_tLightProbeVolumeTexture.SampleLevel(g_sBilinearClamp, uvw + float3(0, 0, face.x), 0).rgb;
        y = g_tLightProbeVolumeTexture.SampleLevel(g_sBilinearClamp, uvw + float3(0, 0, face.y), 0).rgb;
        z = g_tLightProbeVolumeTexture.SampleLevel(g_sBilinearClamp, uvw + float3(0, 0, face.z), 0).rgb;
    }
    const float3 w = n * n;
    return (w.x * x + w.y * y + w.z * z) * g_vLightProbeVolumeColor.xyz;
#else
    return 0.0;
#endif
}

// The ambient light at the surface's normal: the map's, mixed toward the sky's as far as Sky Light says.
float3 AmbientDiffuse(LightingSurface s)
{
    const float3 map = MapAmbientDiffuse(s);
    const float sky = SkyLightShare();
    return sky > 0.0 ? lerp(map, SkyLightAt(s.normalWs, kSkyLightDiffuse), sky) : map;
}

// The ambient the surface's diffuse takes: with subsurface scattering, each channel's at that channel's blurred
// normal, as far as the weight says.
float3 SurfaceAmbient(LightingSurface s)
{
    const float3 ambient = AmbientDiffuse(s);
    if (s.subsurface <= 0.0)
        return ambient;
    LightingSurface c = s;
    float3 scattered;
    c.normalWs = s.subsurfaceNormals[0];
    scattered.r = AmbientDiffuse(c).r;
    c.normalWs = s.subsurfaceNormals[1];
    scattered.g = AmbientDiffuse(c).g;
    c.normalWs = s.subsurfaceNormals[2];
    scattered.b = AmbientDiffuse(c).b;
    return lerp(ambient, scattered, s.subsurface);
}

// The light the environment map the engine chose for the object sends from direction r: box projected, blurred
// by roughness and, in S2FM, scaled toward the ambient's brightness (g_bCubemapNormalization).
float3 MapEnvironmentLight(LightingSurface s, float3 r, float roughness, float3 ambientDiffuse)
{
    const float3 p = WorldPosition(s.positionWs);
    const float3 n = s.normalWs;
    const uint index = s.envMap >> 8;
    const float slice = (float)(s.envMap & 255);
    float3 local;
    if (g_vEnvironmentMapColorRotated[index].w != 0.0)
    {
        local = mul(g_matEnvMapWorldToLocal[index], float4(p, 1.0));
        r = mul((float3x3)g_matEnvMapWorldToLocal[index], r);
    }
    else
        local = p - g_vEnvironmentMapProxySphere[index].xyz;
    const float3 t = max((g_vEnvironmentMapBoxMaxs[index].xyz - local) / r, (g_vEnvironmentMapBoxMins[index].xyz - local) / r);
    const float3 hit = r * abs(min(min(t.x, t.y), t.z)) + local;
    const float blur = sqrt(roughness);
    const float3 dir = normalize(lerp(hit, n, blur));
    float3 env = g_tEnvironmentMap.SampleLevel(g_sLinearWrap, float4(dir, slice), blur * g_vEnvironmentMapSizeConstants.y).rgb;
    env *= g_vEnvironmentMapColorRotated[index].xyz;
    if (g_bCubemapNormalization)
    {
        const float scale = dot(ambientDiffuse, float3(0.2126, 0.7152, 0.0722)) /
                            dot(float4(n, 1.0), g_vEnvironmentMapNormalizationSH[index]);
        env *= min(scale, max(roughness * g_vCubemapNormalizationParams.x + g_vCubemapNormalizationParams.y, 1.0));
    }
    return env;
}

// The same, mixed toward the sky's reflection as far as Sky Light says: the sky is at infinity (no box projection),
// its mip by the roughness it was prefiltered for, toward the lobe's dominant direction (a rough lobe's peak leans
// from the mirror direction toward the normal; Lagarde and de Rousiers 2014).
float3 EnvironmentLight(LightingSurface s, float3 r, float roughness, float3 ambientDiffuse)
{
    const float sky = SkyLightShare();
    if (sky <= 0.0)
        return MapEnvironmentLight(s, r, roughness, ambientDiffuse);
    const float a = roughness * roughness;
    const float3 dominant = normalize(lerp(s.normalWs, r, (1.0 - a) * (sqrt(1.0 - a) + a)));
    const float3 skyEnv = SkyLightAt(dominant, saturate(roughness) * kSkyLightMips);
    return sky >= 1.0 ? skyEnv : lerp(MapEnvironmentLight(s, r, roughness, ambientDiffuse), skyEnv, sky);
}

// The environment map's reflection, weighted by the split sum BRDF table.
float3 AmbientSpecular(LightingSurface s, float3 ambientDiffuse)
{
    const float3 p = WorldPosition(s.positionWs);
    const float3 v = normalize(p - g_vCameraPositionWsMultiview[0].xyz);
    const float3 n = s.normalWs;
    float3 r = reflect(v, n);
    if (s.anisotropic > 0.0)
    {
        // One environment map sample cannot stretch, so the reflection is taken off a normal bent toward the
        // stretch (Filament's anisotropic IBL): the long streak of brushed metal.
        const float3 across = cross(n, s.anisoTangentWs);
        const float3 bent = cross(cross(across, -v), across);
        r = reflect(v, normalize(lerp(n, bent, s.anisotropic)));
    }
    if (g_bValveShading)
    {
        float2 brdf = g_tBRDFLookup.SampleLevel(g_sLinearClamp, float2(max(dot(-v, n), 0.0), s.roughness), 0).xy;
        brdf *= brdf;
        return EnvironmentLight(s, r, s.roughness, ambientDiffuse) * (s.specularColor * brdf.x + brdf.y);
    }
    // Ours: our lobe's albedo with the Fresnel put together from it (Schlick's two parts, less the metal's F82 part),
    // and the reflection faded where it would come from under the mesh's own surface (a normal map can turn it
    // there; Jimenez 2016).
    const float4 e = BrdfAlbedo(max(dot(-v, n), 0.0), s.roughness);
    const float3 albedo = max(s.specularColor * (e.x - e.y) + e.y - s.f82 * e.z, 0.0);
    const float horizon = saturate(1.0 + dot(r, s.geometricNormalWs));
    return EnvironmentLight(s, r, s.roughness, ambientDiffuse) * albedo * (horizon * horizon);
}

// How much of the environment map's reflection the ambient occlusion lets through: the occlusion itself (Valve's),
// or ours, the share of a reflection's cone the occluded part of the hemisphere covers (Lagarde 2014), which leaves a
// smooth surface's reflection whole where it looks away from the occluder.
float EnvironmentOcclusion(LightingSurface s)
{
    if (g_bValveShading)
        return s.occlusion;
    const float3 toCamera = -normalize(WorldPosition(s.positionWs) - g_vCameraPositionWs);
    const float nDotV = saturate(dot(s.normalWs, toCamera));
    return saturate(pow(nDotV + s.occlusion, exp2(-16.0 * s.roughness - 1.0)) - 1.0 + s.occlusion);
}

// The sum: the diffuse colour takes the ambient (occluded, and with our shading what the specular layer over it does
// not reflect) and the direct light; the environment map is occluded, the direct light as far as the surface says.
float3 ShadeSurface(LightingSurface s, DirectLight direct)
{
    const float3 ambient = AmbientDiffuse(s);
    const float3 env = AmbientSpecular(s, ambient);
    return s.diffuseColor * (SurfaceAmbient(s) * DiffuseAlbedoWeight(s) * s.occlusion + direct.diffuse * s.directDiffuseOcclusion) +
           (direct.specular * s.directSpecularOcclusion + env * EnvironmentOcclusion(s)) * s.specularEnergy;
}

float3 ShadeSurface(LightingSurface s)
{
    return ShadeSurface(s, DirectLighting(s));
}

// The clear coat over what the surface gave (under): under is tinted and dimmed by what the coat reflects toward
// the camera, then the coat's own highlights and its environment map reflection (at the coat's normal and
// roughness, F0 from its IOR) are added.
float3 CoatLayer(LightingSurface s, DirectLight direct, float3 under)
{
    if (s.coat <= 0.0)
        return under;
    const float3 toCamera = -normalize(WorldPosition(s.positionWs) - g_vCameraPositionWs);
    const float nDotV = saturate(dot(s.coatNormalWs, toCamera));
    const float fresnel = s.coatF0 + (1.0 - s.coatF0) * pow(1.0 - nDotV, 5.0);
    LightingSurface c = s;
    c.normalWs = s.coatNormalWs;
    c.roughness = s.coatRoughness;
    c.specularColor = s.coatF0;
    c.anisotropic = 0.0;                // the coat is smooth whatever is under it
    const float3 env = AmbientSpecular(c, AmbientDiffuse(s)) * s.occlusion;
    // The coat's colour is what it lets through straight down; light that crosses it at a slant goes further
    // through it and comes out deeper (Beer-Lambert over the path, bent by the coat's IOR, as Blender's coat tint).
    const float root = sqrt(saturate(s.coatF0));
    const float ior = (1.0 + root) / max(1.0 - root, 1e-3);
    const float cosInside = sqrt(saturate(1.0 - (1.0 - nDotV * nDotV) / (ior * ior)));
    const float3 through = pow(max(s.coatTint, 1e-4), 1.0 / max(cosInside, 0.05));
    return under * lerp(1.0, through, s.coat) * (1.0 - s.coat * fresnel) +
           s.coat * (direct.coat * s.directSpecularOcclusion + env);
}

// The sheen's directional albedo at n.v and its roughness, from the table (bilinear), at most 1: Charlie with
// Neubelt's visibility sends back more than it takes at grazing angles when it is smooth.
float SheenAlbedo(float nDotV, float roughness)
{
    const float2 p = saturate(float2(nDotV, roughness)) * 15.0;
    const int2 i = min((int2)p, 14);
    const float2 f = p - i;
    const float a = lerp(g_SheenAlbedo[i.y * 16 + i.x], g_SheenAlbedo[i.y * 16 + i.x + 1], f.x);
    const float c = lerp(g_SheenAlbedo[(i.y + 1) * 16 + i.x], g_SheenAlbedo[(i.y + 1) * 16 + i.x + 1], f.x);
    return min(lerp(a, c, f.y), 1.0);
}

// The sheen on top of everything (the coat included, as OpenPBR's fuzz lies over its coat): what is under it is
// dimmed by the light the sheen takes, then the sheen's light from every light and from the ambient is added.
float3 SheenLayer(LightingSurface s, DirectLight direct, float3 under)
{
    if (!any(s.sheen > 0.0))
        return under;
    const float3 toCamera = -normalize(WorldPosition(s.positionWs) - g_vCameraPositionWs);
    const float e = SheenAlbedo(saturate(dot(s.normalWs, toCamera)), s.sheenRoughness);
    const float covered = saturate(max(s.sheen.r, max(s.sheen.g, s.sheen.b)) * e);
    return under * (1.0 - covered) +
           s.sheen * (direct.sheen * s.directDiffuseOcclusion + AmbientDiffuse(s) * s.occlusion * e);
}

// The flakes over what the surface gave (under): where a flake is, it covers 70% of what is under it (Glitter) and
// reflects the lights and the environment map off its own normal.
float3 GlitterLayer(LightingSurface s, DirectLight direct, float3 under)
{
    if (s.glitter <= 0.0)
        return under;
    LightingSurface c = s;
    c.normalWs = s.glitterNormalWs;
    c.roughness = s.glitterRoughness;
    c.specularColor = s.glitterF0;
    c.anisotropic = 0.0;
    const float3 env = AmbientSpecular(c, AmbientDiffuse(s)) * s.occlusion;
    return under * (1.0 - 0.7 * s.glitter) + s.glitter * (direct.glitter * s.directSpecularOcclusion + env);
}

// The foil's rainbows from the environment map: for wavelengths across the spectrum, the directions orders +1 and
// -1 bring that wavelength from (HoloTerm's rule turned round), each looked up in the environment map.
float3 HoloEnvironment(LightingSurface s, float3 ambientDiffuse)
{
    const float3 v = -normalize(WorldPosition(s.positionWs) - g_vCameraPositionWs);
    const float3 n = s.normalWs;
    const float3 g = s.holoDirWs;
    const float3 across = cross(n, g);
    const float vg = dot(v, g), va = dot(v, across);
    float3 sum = 0.0;
    // One loop over both orders (even k: -1, odd: +1), so the environment lookup is compiled once.
    [loop] for (int k = 0; k < 16; k++)
    {
        const float l = 400.0 + 37.5 * ((k >> 1) + 0.5);
        const float lg = ((k & 1) ? l : -l) / s.holoSpacing - vg;
        const float q = 1.0 - lg * lg - va * va;
        if (q > 0.0)
            sum += Spectrum(l) * EnvironmentLight(s, lg * g - va * across + sqrt(q) * n, s.holoRoughness, ambientDiffuse);
    }
    return sum * (0.25 / 8.0);
}

// The foil over what the surface gave (under): it takes half the light from what is under it (the half that
// reflects as a mirror stays in the surface's own specular), and gives it back spread into rainbows.
float3 HoloLayer(LightingSurface s, DirectLight direct, float3 under)
{
    if (s.holographic <= 0.0)
        return under;
    const float3 rainbow = direct.holo * s.directSpecularOcclusion + HoloEnvironment(s, AmbientDiffuse(s)) * s.occlusion;
    return under * (1.0 - 0.5 * s.holographic) + s.holographic * rainbow;
}
