// The noises s2vfx_weather builds its clouds and precipitation from: two hashes and the baked, tileable 3D noises
// (tests/weather_noise.py) kept as 2D atlases of z slices. A PS block declares g_tShapeNoise, g_tDetailNoise and
// g_sVolumeNoise (they carry material settings) and includes this after them.

float Hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float3 Hash33(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}

// A tileable 3D noise kept as a 2D atlas of z slices: n voxels a side, each slice a tile of n + 2 texels (the slice and
// a wrapped border), cols tiles a row, row 0 at the top. p in volumes (it repeats every 1): the two nearest slices
// read bilinearly and blended.
float4 Atlas(Texture2D t, float3 p, float n, float cols, float2 invSize)
{
    p = frac(p);
    const float z = p.z * n - 0.5;
    const float zf = floor(z);
    const float w = z - zf;
    const float z0 = zf < 0.0 ? n - 1.0 : zf;
    const float z1 = z0 + 1.0 >= n ? 0.0 : z0 + 1.0;
    const float2 inTile = 1.0 + p.xy * n;
    const float tile = n + 2.0;
    const float2 a = (float2(fmod(z0, cols), floor(z0 / cols)) * tile + inTile) * invSize;
    const float2 b = (float2(fmod(z1, cols), floor(z1 / cols)) * tile + inTile) * invSize;
    return lerp(t.SampleLevel(g_sVolumeNoise, a, 0), t.SampleLevel(g_sVolumeNoise, b, 0), w);
}

// Shape: 126^3, RGBA (Perlin-Worley, Worley at 6, 12 and 24 cells), 16 slices a row.
float4 ShapeNoise(float3 p)
{
    return Atlas(g_tShapeNoise, p, 126.0, 16.0, float2(1.0 / 2048.0, 1.0 / 1024.0));
}

// Detail: 30^3, RGB (Worley at 4, 8 and 16 cells), 8 slices a row.
float3 DetailNoise(float3 p)
{
    return Atlas(g_tDetailNoise, p, 30.0, 8.0, float2(1.0 / 256.0, 1.0 / 128.0)).rgb;
}
