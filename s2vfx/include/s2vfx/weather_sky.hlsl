// The sky light for s2vfx_weather, written from scratch for this project: an S2FM light pointed straight down over
// the scene (orthographic, its shadow on) stands for the open sky. Its shadow map says what a point has above it: lit,
// it is open to the sky and the precipitation reaches it; shadowed, it is under a roof. Read as a height map seen from
// above, it also says where the hollows are and which surfaces are the tops. A PS block includes this after
// lighting.hlsl (Project, the light list and its shadow atlas).

SamplerState g_sSkyPoint < Filter(MIN_MAG_MIP_POINT); AddressU(CLAMP); AddressV(CLAMP); >;

// The sky light: of the light list's lights with a shadow that shine within about 10 degrees of straight down, the
// one with the widest cone (an orthographic light before a spot); -1 none.
int SkyLight()
{
    int sky = -1;
    float widest = 2.0;
    for (int n = 0; n < g_vNumLights.x; n++)
    {
        const float outer = g_vSpotLightInnerOuterConeCosines[n].y;
        if (g_vLightParams[n].x && g_vLightDirection[n].z < -0.985 && outer < widest)
        {
            sky = n;
            widest = outer;
        }
    }
    return sky;
}

// Where a point (world, not offset) falls in sky light n's map: uv in the atlas, its depth there; inside: within the
// light's own part of the atlas and its depth range.
float3 SkyUvz(int n, float3 p, out bool inside)
{
    const float3 uvz = Project(g_matWorldToShadow[n], p - g_vHighPrecisionLightingOffsetWs.xyz);
    const float4 bounds = g_vSpotLightShadowBounds[n];
    inside = all(uvz.xy >= bounds.xy) && all(uvz.xy <= bounds.zw) && uvz.z >= 0.0 && uvz.z <= 1.0;
    return uvz;
}

// How open a point is to the sky: 1 open (and with no sky light, or outside its map), 0 under cover.
float SkyOpen(int n, float3 p)
{
    if (n < 0)
        return 1.0;
    bool inside;
    const float3 uvz = SkyUvz(n, p, inside);
    if (!inside)
        return 1.0;
    return g_tVrShadowDepthBufferDepth.SampleCmpLevelZero(g_sShadowCompare, uvz.xy, saturate(uvz.z - 0.000001));
}

// The same for a surface point p (normal up its normal), soft: nine filtered comparisons a texel and a half apart. The
// map's texels are big (an orthographic light over a whole set), and one comparison draws a roof's edge on the ground
// as steps; for the ground, not falling rain. The points compared lie on p's own slope, 2 units over it: compared at
// p's height, a dome's points were under its own higher parts round them (dry along a sphere's top). Half open counts
// as open: the spread would otherwise reach out past what covers the ground, a dry ring round everything standing on
// it.
float SkyOpenSoft(int n, float3 p, float3 normal)
{
    if (n < 0)
        return 1.0;
    bool inside;
    const float3 uvz = SkyUvz(n, p, inside);
    if (!inside)
        return 1.0;
    const float4 bounds = g_vSpotLightShadowBounds[n];
    float2 size;
    g_tVrShadowDepthBufferDepth.GetDimensions(size.x, size.y);
    // A texel and a half in world units, and the slope's rise per unit across (steep faces held to about 73 degrees).
    bool unused;
    const float texel = 100.0 / max(length(SkyUvz(n, p + float3(100.0, 0.0, 0.0), unused).xy - uvz.xy) * size.x, 1e-6);
    const float spacing = 1.5 * texel;
    const float2 rise = -normal.xy / max(normal.z, 0.3);
    float open = 0.0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
        {
            const float2 across = float2(x, y) * spacing;
            const float3 q = SkyUvz(n, p + float3(across, dot(rise, across) + 2.0), unused);
            open += g_tVrShadowDepthBufferDepth.SampleCmpLevelZero(g_sShadowCompare, clamp(q.xy, bounds.xy, bounds.zw),
                                                                   saturate(q.z - 0.000001));
        }
    return smoothstep(0.0, 0.5, open / 9.0);
}

// The map round a point p: x how far p's surface lies below the lowest of its surroundings radius out (world units;
// above 0 a hollow, a basin lower than all round it; against their average, the ground at the foot of a box or a step
// was a hollow, and puddles lined everything standing on it), y how far p lies below the topmost surface right above
// it (0: p is the top). 0 with no sky light or outside its map.
float2 SkyHeights(int n, float3 p, float radius)
{
    if (n < 0)
        return 0.0;
    bool inside;
    const float3 uvz = SkyUvz(n, p, inside);
    if (!inside)
        return 0.0;
    const float4 bounds = g_vSpotLightShadowBounds[n];
    // World units of height for a unit of the map's depth (the light shines down its z), and the steps in the map of
    // radius world units along the world's x and y.
    bool unused;
    const float3 above = SkyUvz(n, p + float3(0.0, 0.0, radius), unused);
    const float perDepth = radius / max(abs(uvz.z - above.z), 1e-9);
    const float2 ux = SkyUvz(n, p + float3(radius, 0.0, 0.0), unused).xy - uvz.xy;
    const float2 uy = SkyUvz(n, p + float3(0.0, radius, 0.0), unused).xy - uvz.xy;
    // Filtered reads: a point read draws the texels' steps into the puddles and the snow's edge.
    const float here = g_tVrShadowDepthBufferDepth.SampleLevel(g_sBilinearClamp, uvz.xy, 0).r;
    // The light shines down, so the lowest surface is the deepest in its map.
    float lowest = 0.0;
    [unroll] for (int k = 0; k < 8; k++)
    {
        float s, c;
        sincos(k * 0.785398, s, c);
        lowest = max(lowest, g_tVrShadowDepthBufferDepth.SampleLevel(g_sBilinearClamp, clamp(uvz.xy + ux * c + uy * s, bounds.xy, bounds.zw), 0).r);
    }
    // p's own height, not the map's there: by an object's foot the filtered read takes in its top.
    return float2((uvz.z - lowest) * perDepth, max(uvz.z - here, 0.0) * perDepth);
}
