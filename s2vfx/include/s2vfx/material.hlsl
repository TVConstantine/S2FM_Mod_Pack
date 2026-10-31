// A material's textures, as the game's vr_complex reads them: the texture coordinates and the colour's tint, normal maps (with the roughness in their third
// channel), the normal map's scale, detail textures (Mod2X, Overlay, Normals, Overlay and Normals) and the flexes'
// wrinkle maps (squish and stretch colour, normal and ambient occlusion). Functions only: the shader declares its
// textures (examples/lit.vfx has them all, with the same names, channels and formats as vr_complex).

// Set by the engine (a tools switch): shade with the vertex normal.
bool g_bDisableNormalMapping;

// A colour the artist picks, as Valve's shaders take it: g_vColorTint's expression in vr_simple and vr_complex is
// SrgbGammaToLinear(this) (function 31 of the expression bytecode), so 0.8 in the material is 0.604 in the shader.
float3 SrgbGammaToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

// The mesh's uv as vr_complex moves it (its Texture Coordinates group): scaled, moved, then scrolled with time.
float2 TexCoordTransform(float2 uv, float2 scale, float2 offset, float2 scrollSpeed)
{
    return uv * scale + offset + frac(scrollSpeed * g_flTime);
}

// The colour's tint as vr_complex makes it: the material's tint (an sRGB colour) times the object's colour (S2FM's
// model colour) as far as the model tint amount says, laid on only where the tint mask is white.
float3 ModelTint(float3 materialTint, float3 objectTint, float amount, float mask)
{
    return lerp(1.0, SrgbGammaToLinear(materialTint) * lerp(1.0, objectTint, amount), mask);
}

// A normal map texel: hemi-octahedral in red and green (HemiOctIsoRoughness_RG_B), a tangent space normal.
float3 DecodeNormal(float2 rg)
{
    const float2 a = float2(rg.x + rg.y - 1.003922, rg.x - rg.y);       // the first is 256/255 off, as Valve's is
    return normalize(float3(a, 1.0 - abs(a.x) - abs(a.y)));
}

// A tangent space normal in the world, in the frame the vertex stage hands over: x along the tangent, y against the
// bitangent, z along the normal.
float3 NormalToWorld(PixelInput i, float3 n)
{
    if (g_bDisableNormalMapping)
        return normalize(i.vNormalWs.xyz);
    const float3 t = normalize(i.vTangentWs), b = normalize(i.vBitangentWs), z = normalize(i.vNormalWs.xyz);
    return normalize(n.x * t - n.y * b + n.z * z);
}

// The normal map's strength: 0 flat, 1 as painted (g_flNormalMapScaleFactor).
float3 ScaleNormal(float3 n, float scale)
{
    return normalize(lerp(float3(0.0, 0.0, 1.0), n, scale));
}

// ---------------------------------------------------------------------------------------------- detail textures
// The detail texture's uv: the mesh's uv scaled, turned about the texture's centre and moved.
float2 DetailUv(float2 uv, float2 scale, float2 offset, float rotationDegrees)
{
    float s, c;
    sincos(radians(rotationDegrees), s, c);
    const float2 p = uv * scale - 0.5;
    return float2(c * p.x - s * p.y, s * p.x + c * p.y) + 0.5 + offset;
}

// How much of the detail shows: the mask's red, raised to blendToFull, times blendFactor.
float DetailAmount(float mask, float blendFactor, float blendToFull)
{
    return max(mask, blendToFull) * blendFactor;
}

// Mod2X: the detail (stored centred on 0.5, Mod2XCenter) doubles or darkens the colour.
float3 DetailMod2X(float3 albedo, float3 detail, float amount)
{
    return albedo * lerp(1.0, detail * 1.992157, amount);
}

// Overlay: Photoshop's overlay of the detail on the colour, in gamma space.
float3 DetailOverlay(float3 albedo, float3 detail, float amount)
{
    const float3 d = detail * 0.996078;
    const float3 base = pow(albedo, 1.0 / 2.2);
    const float3 over = base <= 0.5 ? 2.0 * d * base : 1.0 - 2.0 * (1.0 - d) * (1.0 - base);
    return lerp(albedo, pow(over, 2.2), amount);
}

// Normals: the detail normal (its strength 0 to 1) laid on the normal map's, as far as amount says.
float3 DetailNormal(float3 n, float3 detail, float strength, float amount)
{
    float3 d = lerp(float3(0.0, 0.0, 1.0), detail, strength);
    d = lerp(float3(0.0, 0.0, 1.0), d, amount);
    return normalize(float3(n.xy * d.z + n.z * d.xy, n.z * d.z));
}

// ---------------------------------------------------------------------------------------------- self illumination
// vr_complex's glow, added after the lighting and before the fog. The mask scrolls with time; its colour is the tint
// scaled as vr_complex's g_vSelfIllumTintScaled expression does (2^brightness * scale * SrgbGammaToLinear(tint)),
// then taken toward the surface's own colour by albedoFactor.
float3 SelfIllum(Texture2D mask, SamplerState s, float2 uv, float3 albedo, float3 tint, float brightness, float scale,
                 float2 scrollSpeed, float albedoFactor)
{
    const float2 at = frac(scrollSpeed * g_flTime) + uv;
    const float3 glow = mask.Sample(s, at).rgb * (exp2(brightness) * scale * SrgbGammaToLinear(tint));
    return lerp(glow, glow * albedo, albedoFactor);
}

// ---------------------------------------------------------------------------------------------- wrinkles
// The flexes' wrinkle amount at the pixel: below 0 squish, above 0 stretch. The material can hold it at a value
// (g_bWrinkleOverride, g_flWrinkleOverride) to paint the maps.
float Wrinkle(PixelInput i, bool held, float value)
{
    return held ? value : clamp(i.vNormalWs.w, -1.0, 1.0);
}

// A texel blended toward its squish or stretch version by the wrinkle amount.
float4 WrinkleBlend(float4 base, float4 squish, float4 stretch, float wrinkle)
{
    return lerp(base, wrinkle < 0.0 ? squish : stretch, abs(wrinkle));
}
