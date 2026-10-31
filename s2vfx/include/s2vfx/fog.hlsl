// Source 2's fog for a model's pixel shader, carried over from the game's vr_simple: gradient fog, cubemap fog,
// volumetric fog and the spherical vignette, in that order, each as the map (or S2FM's post settings) has it.
// A PS block includes this after lighting.hlsl (or common.hlsl) and calls, last:
//     color = ApplyFog(color, i.vPositionWs, i.vPositionPs.xy);
// The material's "Fog Enabled" turns it off.

TextureCube g_tFogCubeTexture;
Texture3D g_tFogVolume;
Texture2D g_tBlueNoise;

SamplerState g_sFogClamp < Filter(MIN_MAG_MIP_LINEAR); AddressU(CLAMP); AddressV(CLAMP); AddressW(CLAMP); >;
SamplerState g_sNoise < Filter(MIN_MAG_MIP_POINT); AddressU(WRAP); AddressV(WRAP); >;

bool g_bFogEnabled < UiType(CheckBox); Default(1); UiGroup("Fog"); >;

// positionWs as PixelInput carries it (less the lighting offset), pixel the screen position (SV_Position.xy).
float3 ApplyFog(float3 color, float3 positionWs, float2 pixel)
{
    if (!g_bFogEnabled)
        return color;
    const float3 p = positionWs + g_vHighPrecisionLightingOffsetWs.xyz;
    const float3 d = p - g_vCameraPositionWsMultiview[0].xyz;

    // Gradient fog: by horizontal distance and by height.
    const float flat2 = dot(d.xy, d.xy);
    if (flat2 > g_vGradientFogCullingParams.x && p.z < g_vGradientFogCullingParams.y)
    {
        float2 f = saturate(float2(p.z, sqrt(flat2)) * g_vGradientFogBiasAndScale.wz + g_vGradientFogBiasAndScale.yx);
        f = pow(f, m_vGradientFogExponents.yx);
        color = lerp(color, g_vGradientFogColor_Opacity.xyz, f.x * f.y * g_vGradientFogColor_Opacity.w);
    }

    // Cubemap fog: the sky's colour in the view direction, blurrier when thinner.
    const float d2 = dot(d, d);
    if (d2 > g_vCubeFogCullingParams.x || p.z > g_vCubeFogCullingParams.y)
    {
        const float3 dir = normalize(mul(d, (float3x3)g_matvCubeFogSkyWsToOs));
        const float a = pow(max(sqrt(d2) * g_vCubeFog_Offset_Scale_Bias_Exponent.y + g_vCubeFog_Offset_Scale_Bias_Exponent.x, 0.0),
                            g_vCubeFog_Offset_Scale_Bias_Exponent.w);
        const float b = pow(max(p.z * g_vCubeFog_Height_Offset_Scale_Exponent_Log2Mip.y + g_vCubeFog_Height_Offset_Scale_Exponent_Log2Mip.x, 0.0),
                            g_vCubeFog_Height_Offset_Scale_Exponent_Log2Mip.z);
        const float f = max(a, b);
        const float mip = saturate(1.0 - f * g_vCubeFog_Offset_Scale_Bias_Exponent.z) * g_vCubeFog_Height_Offset_Scale_Exponent_Log2Mip.w;
        color = lerp(color, g_tFogCubeTexture.SampleLevel(g_sFogClamp, dir, mip).rgb, min(f, 1.0));
    }

    // Volumetric fog: the engine's froxel volume, read at the pixel's place with a little blue noise.
    if (g_bFogTypeEnabled.x)
    {
#if S2VFX_FOG_LEVEL_ZERO
        // A shader that fogs inside flow control (a volume, after its march) reads both at their top mip: the same for
        // the point-sampled noise, next to the same for the small fog volume.
        const float3 noise = g_tBlueNoise.SampleLevel(g_sNoise, pixel * g_vScreenSpaceDitherParams.z + g_vScreenSpaceDitherParams.xy, 0).rgb;
#else
        const float3 noise = g_tBlueNoise.Sample(g_sNoise, pixel * g_vScreenSpaceDitherParams.z + g_vScreenSpaceDitherParams.xy).rgb;
#endif
        const float3 q = p + noise * g_vVolFogDitherScaleBias.x + g_vVolFogDitherScaleBias.y;
        float3 v = mul(g_mVolFogFromWorld[0], float4(q, 1.0)).xyw;
        v.xy /= v.z;
        v = v * g_vVolFogPostWorldToFrustumScale.xyz + g_vVolFogPostWorldToFrustumBias.xyz;
#if S2VFX_FOG_LEVEL_ZERO
        const float4 fog = g_tFogVolume.SampleLevel(g_sFogClamp, float3(v.xy, sqrt(max(v.z, 0.0))), 0);
#else
        const float4 fog = g_tFogVolume.Sample(g_sFogClamp, float3(v.xy, sqrt(max(v.z, 0.0))));
#endif
        color = color * fog.a + fog.rgb;
    }

    // Spherical vignette: a colour that closes in with distance from a point.
    if (g_bFogTypeEnabled.w)
    {
        const float f = pow(saturate(g_vSphericalVignetteBiasAndScale.y * length(p - g_vSphericalVignetteOrigin_Exponent.xyz) +
                                     g_vSphericalVignetteBiasAndScale.x), g_vSphericalVignetteOrigin_Exponent.w);
        color = lerp(color, g_vSphericalVignetteColor_Opacity.xyz, f * g_vSphericalVignetteColor_Opacity.w);
    }
    return color;
}

// The fog over a see-through surface that shows the scene behind it from the frame copy, which carries its own fog
// already: the surface's own light (own: its reflections, its own colour) takes the fog as any surface's does, what it
// lets through (passing, as the copy has it) is left as it is, and the fog's own light is added only as far as the
// surface hides what is behind it (cover, 0..1). Every part of the fog is a blend, so ApplyFog(c) is c k + f and this
// is passing + own k + f cover.
float3 ApplyFogSeeThrough(float3 own, float3 passing, float cover, float3 positionWs, float2 pixel)
{
    const float3 fogLight = ApplyFog(0.0, positionWs, pixel);
    return passing + ApplyFog(own, positionWs, pixel) - fogLight * (1.0 - saturate(cover));
}
