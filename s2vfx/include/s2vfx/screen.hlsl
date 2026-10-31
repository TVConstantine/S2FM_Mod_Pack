// Screen space helpers for a pixel shader that reads the scene behind it: the frame copy and the scene's depth, both
// made by the mod pack for any shader that declares them. Include after common.hlsl in a PS block.

SamplerState g_sScreenClamp < Filter(MIN_MAG_MIP_LINEAR); AddressU(CLAMP); AddressV(CLAMP); >;
Texture2D g_tFrameBufferCopyTexture;
Texture2D g_tSceneDepth;

// A pixel kept inside the viewport (the copies hold the whole target; the viewport is a part of it).
float2 InViewport(float2 pixel)
{
    return clamp(pixel, g_vViewportOffset + 0.5, g_vViewportOffset + g_vViewportSize - 0.5);
}

// The distance from the camera along direction to the scene's depth at a pixel (far away where nothing is drawn).
float SceneDistance(float2 pixel, float3 camera, float3 direction)
{
    const float stored = g_tSceneDepth.Load(int3(InViewport(pixel), 0)).r;
    const float d = (stored - g_flViewportMinZ) / (g_flViewportMaxZ - g_flViewportMinZ);
    const float4 c = mul(g_matWorldToProjectionMultiview[0], float4(camera, 1.0) + g_vWorldToCameraOffsetMultiview[0]);
    const float4 r = mul(g_matWorldToProjectionMultiview[0], float4(direction, 0.0));
    const float t = (c.z - d * c.w) / (d * r.w - r.z);
    return t > 0.0 && t < 1e6 ? t : 1e6;
}

// The scene behind at a pixel, blurred over about radius pixels (the mip that wide, eight points round a ring).
float3 SceneBehind(float2 pixel, float radius)
{
    float2 size;
    float mips;
    g_tFrameBufferCopyTexture.GetDimensions(0, size.x, size.y, mips);
    const float2 inv = 1.0 / size;
    const float lod = clamp(log2(max(radius * 0.5, 1.0)), 0.0, mips - 1.0);
    float3 sum = g_tFrameBufferCopyTexture.SampleLevel(g_sScreenClamp, InViewport(pixel) * inv, lod).rgb;
    if (radius < 1.0)
        return sum;
    [unroll] for (int k = 0; k < 8; k++)
    {
        float s, c;
        sincos(k * 0.785398, s, c);
        sum += g_tFrameBufferCopyTexture.SampleLevel(g_sScreenClamp, InViewport(pixel + float2(c, s) * radius * 0.6) * inv, lod).rgb;
    }
    return sum / 9.0;
}
