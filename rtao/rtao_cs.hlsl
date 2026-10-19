// RTAO's compute shaders (rtao_d3d12.cpp). Built with dxc into headers by build_core.cmd:
//   ToWorld  cs_6_0: the captured vertices (clip space, as the engine's vertex shaders wrote them) to the rays' space;
//   Ao       cs_6_5: the AO of every pixel of the view, inline ray queries against the frame's structure.
//
// The rays' space is the engine's own "world plus the camera offset" (its vertex shaders add g_vWorldToCameraOffset
// before they project): the inverse of the frame's world to projection matrix takes a clip position there, and takes
// a pixel and its depth there too, so the structure and the pixels agree whatever the camera's jitter.

cbuffer Frame : register(b0)
{
    row_major float4x4 ProjToRays;  // inverse of the frame's world to projection matrix
    float4 Eye;                     // xyz: the camera in the rays' space
    float4 Ao;                      // x: reach (units), y: height above the plane an occluder needs (units),
                                    // z: strength (a factor on the occlusion), w: rays a pixel
    float4 View;                    // x, y: the view's size in pixels; z: a seed that changes every frame; w: debug
    float4 Range;                   // x, y: the viewport's depth range (S2FM: 0.05 to 0.95); z: vertices to convert;
                                    // w: runs
    float4 Planes[6];               // the camera's frustum in the rays' space, normals inward, unit long
};

// --- ToWorld: a triangle a thread. The capture holds the camera's triangles and those of the views the engine draws
// before it (the shadows', "offscreen"), each in its own clip space; a run says which view a stretch of it was drawn
// with. Another view's triangle is put on one point (left out, its place kept for a refit) where any corner of it is
// inside the camera's frustum, and where it lies wholly beyond the AO's reach from it. The camera draws whole objects,
// so whatever reaches into its frustum it has already; the shadows' copies of those, kept where they only crossed the
// frustum's sides, darkened a wall's foot by 0.18 (not where the camera's copy is: cause not found).
struct ViewRec
{
    row_major float4x4 ClipToRays;
    float3 Delta;   // added after the division: from the view's world plus offset to the camera's
    uint Cull;
};
StructuredBuffer<float4> Captured : register(t0);
RWStructuredBuffer<float3> Positions : register(u0);
StructuredBuffer<ViewRec> Views : register(t5);
StructuredBuffer<uint3> Runs : register(t6);   // x: first vertex, y: view, z: 1 smooth normals captured; in order

// The run a captured vertex is in.
uint RunOf(uint vertex)
{
    uint lo = 0, hi = (uint)Range.w;
    while (hi - lo > 1) {
        const uint mid = (lo + hi) / 2;
        if (Runs[mid].x <= vertex) lo = mid; else hi = mid;
    }
    return lo;
}

float3 ToRays(float4 clip, ViewRec v)
{
    const float4 h = mul(clip, v.ClipToRays);
    // A vertex behind the camera still has a place in the world: w is the projection's, not the point's.
    return ((abs(h.w) > 1e-20f) ? h.xyz / h.w : float3(0.0f, 0.0f, 0.0f)) + v.Delta;
}

[numthreads(256, 1, 1)]
void ToWorld(uint3 id : SV_DispatchThreadID)
{
    const uint first = id.x * 3;
    if (first + 3 > (uint)Range.z) return;
    const ViewRec v = Views[Runs[RunOf(first)].y];
    float3 p[3];
    [unroll] for (uint k = 0; k < 3; ++k) p[k] = ToRays(Captured[first + k], v);
    if (v.Cull) {
        // Each vertex: the planes it is inside of.
        uint3 planesIn = uint3(0, 0, 0);
        bool beyond = false;
        [unroll] for (uint i = 0; i < 6; ++i) {
            const float3 d = float3(dot(Planes[i].xyz, p[0]), dot(Planes[i].xyz, p[1]), dot(Planes[i].xyz, p[2]))
                           + Planes[i].w;
            planesIn += uint3(d >= 0.0f);
            beyond = beyond || all(d < -Ao.x);
        }
        if (any(planesIn == 6u) || beyond) p[1] = p[2] = p[0];
    }
    [unroll] for (uint k = 0; k < 3; ++k) Positions[first + k] = p[k];
}

// --- Ao
#ifdef AO_KERNEL
RaytracingAccelerationStructure Scene : register(t1);
Texture2D<float> Depth : register(t2);
RWTexture2D<float4> Out : register(u1);
// The cut outs (rtao_capture.cpp): their uv (xy) and vertex alpha (z) at each captured vertex, a record for each of the
// structure's geometries (x first vertex, y mask, z reference as float bits, w 1 a cut out | 2 times the vertex alpha),
// and the masks of their alpha.
StructuredBuffer<float3> CutUv : register(t3);
// The vertices' smooth normals, in the rays' space (the vertex shaders' own, rtao_capture.cpp), where a run says so.
StructuredBuffer<float3> Normals : register(t7);
StructuredBuffer<uint4> Geometry : register(t4);
Texture2D<float> Masks[32] : register(t0, space1);
SamplerState Linear : register(s0);

uint WangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16);
    seed *= 9u;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15);
    return seed;
}

// A scrambled Sobol sequence (Burley 2020), so the samples a pixel gets over
// the frames S2FM's refinement sums fill each other's gaps. The state is the sample's index shifted up eight bits with
// a dimension counter in the low eight.
static const uint kSobolDir[2][32] = {
    { 0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u, 0x08000000u, 0x04000000u, 0x02000000u, 0x01000000u, 0x00800000u, 0x00400000u, 0x00200000u, 0x00100000u, 0x00080000u, 0x00040000u, 0x00020000u, 0x00010000u, 0x00008000u, 0x00004000u, 0x00002000u, 0x00001000u, 0x00000800u, 0x00000400u, 0x00000200u, 0x00000100u, 0x00000080u, 0x00000040u, 0x00000020u, 0x00000010u, 0x00000008u, 0x00000004u, 0x00000002u, 0x00000001u },
    { 0x80000000u, 0xC0000000u, 0xA0000000u, 0xF0000000u, 0x88000000u, 0xCC000000u, 0xAA000000u, 0xFF000000u, 0x80800000u, 0xC0C00000u, 0xA0A00000u, 0xF0F00000u, 0x88880000u, 0xCCCC0000u, 0xAAAA0000u, 0xFFFF0000u, 0x80008000u, 0xC000C000u, 0xA000A000u, 0xF000F000u, 0x88008800u, 0xCC00CC00u, 0xAA00AA00u, 0xFF00FF00u, 0x80808080u, 0xC0C0C0C0u, 0xA0A0A0A0u, 0xF0F0F0F0u, 0x88888888u, 0xCCCCCCCCu, 0xAAAAAAAAu, 0xFFFFFFFFu }
};

uint LaineKarras(uint x, uint seed)
{
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}

uint NestedUniformScramble(uint x, uint seed) { return reversebits(LaineKarras(reversebits(x), seed)); }

uint SobolDim(uint index, uint d)
{
    if (d == 0u) return reversebits(index);
    uint x = 0u;
    [loop] for (uint b = 0u; index != 0u && b < 32u; ++b, index >>= 1)
    {
        if ((index & 1u) != 0u) x ^= kSobolDir[1][b];
    }
    return x;
}

float RandomFloat(inout uint state, uint2 pix)
{
    const uint dim = state & 0xFFu;
    const uint n = state >> 8;
    state = (state & 0xFFFFFF00u) | ((dim + 1u) & 0xFFu);
    const uint pixelSeed = WangHash(pix.x * 1973u + pix.y * 9277u + 26699u);
    const uint seed = WangHash(pixelSeed ^ ((dim >> 1) * 0x9E3779B9u));
    const uint d = dim & 1u;
    const uint index = NestedUniformScramble(n, seed);
    const uint x = NestedUniformScramble(SobolDim(index, d), WangHash(seed ^ ((d + 1u) * 0x85EBCA6Bu)));
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

float3 CosineHemisphere(float3 normal, inout uint rng, uint2 pix)
{
    const float u1 = RandomFloat(rng, pix);
    const float u2 = RandomFloat(rng, pix);
    const float r = sqrt(u1);
    const float theta = 6.2831853f * u2;
    const float3 up = abs(normal.z) < 0.99f ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 t = normalize(cross(up, normal));
    const float3 b = cross(normal, t);
    return normalize(t * (r * cos(theta)) + b * (r * sin(theta)) + normal * sqrt(max(0.0f, 1.0f - u1)));
}

// Whether a cut out's triangle blocks a ray where it was struck: its alpha there against its reference, or, for the
// see-through ones (reference below zero), against chance, so they block with the chance of their alpha and S2FM's
// refinement averages that into their shade.
bool Blocks(uint geometry, uint primitive, float2 bary, float chance)
{
    const uint4 g = Geometry[geometry];
    if ((g.w & 1u) == 0u) return true;
    const uint v = g.x + primitive * 3u;
    const float3 w = float3(1.0f - bary.x - bary.y, bary.x, bary.y);
    const float3 at = CutUv[v] * w.x + CutUv[v + 1u] * w.y + CutUv[v + 2u] * w.z;
    float alpha = Masks[NonUniformResourceIndex(g.y)].SampleLevel(Linear, at.xy, 0.0f);
    if ((g.w & 2u) != 0u) alpha *= at.z;
    const float reference = asfloat(g.z);
    return alpha >= (reference >= 0.0f ? reference : chance);
}

// The nearest surface along a ray that blocks it, or below zero where there is none.
float Closest(RayDesc ray, float chance)
{
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
    [loop] while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
            && Blocks(q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), chance))
            q.CommitNonOpaqueTriangleHit();
    }
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? q.CommittedRayT() : -1.0f;
}

// The same, with which triangle it was and where on it.
float ClosestHit(RayDesc ray, float chance, out uint geometry, out uint primitive, out float2 bary)
{
    geometry = 0u;
    primitive = 0u;
    bary = float2(0.0f, 0.0f);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
    [loop] while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
            && Blocks(q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), chance))
            q.CommitNonOpaqueTriangleHit();
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return -1.0f;
    geometry = q.CommittedGeometryIndex();
    primitive = q.CommittedPrimitiveIndex();
    bary = q.CommittedTriangleBarycentrics();
    return q.CommittedRayT();
}

// A pixel of the view in the rays' space; sky where the engine drew nothing (its depth cleared to either end of the
// range) or outside the view.
float3 Position(int2 p, out bool sky)
{
    const float2 size = View.xy;
    if (any(p < 0) || any((float2)p >= size))
    {
        sky = true;
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float span = max(Range.y - Range.x, 1e-6f);
    const float z = (Depth.Load(int3(p, 0)) - Range.x) / span;
    sky = !(z > 1e-6f && z < 1.0f - 1e-6f);
    float2 d = (((float2)p + 0.5f) / size) * 2.0f - 1.0f;
    d.y = -d.y;
    const float4 w = mul(float4(d.x, d.y, z, 1.0f), ProjToRays);
    return w.xyz / w.w;
}

// Where the camera's ray through a pixel meets the structure, looked for only near where the depth puts it: 24 bits
// of depth far off and at a grazing angle are units apart from one pixel to the next, and normals taken from them
// point anywhere. The structure has the surface exactly; where it has nothing near, the first surface it has behind,
// up to half as far again (an edge between two surfaces); past that, the depth.
float3 SurfaceHit(int2 p, out bool sky, out bool hit, out uint geometry, out uint primitive, out float2 bary)
{
    hit = false;
    geometry = 0u;
    primitive = 0u;
    bary = float2(0.0f, 0.0f);
    const float3 approx = Position(p, sky);
    if (sky) return approx;
    const float3 o = Eye.xyz;
    const float3 v = approx - o;
    const float d = length(v);
    if (d < 1e-3f) return approx;
    const float3 dir = v / d;
    const float window = max(4.0f, d * 0.02f);
    RayDesc r;
    r.Origin = o;
    r.Direction = dir;
    r.TMin = max(d - window, 0.0f);
    r.TMax = d + window;
    // A cut out counts as the surface where its alpha is half or more, as the engine's depth mostly has it.
    const float t = ClosestHit(r, 0.5f, geometry, primitive, bary);
    if (t >= 0.0f) { hit = true; return o + dir * t; }
    RayDesc behind = r;
    behind.TMin = r.TMax;
    behind.TMax = d * 1.5f + window;
    const float tb = ClosestHit(behind, 0.5f, geometry, primitive, bary);
    if (tb >= 0.0f) { hit = true; return o + dir * tb; }
    return approx;
}

float3 Surface(int2 p, out bool sky)
{
    bool hit;
    uint g, prim;
    float2 b;
    return SurfaceHit(p, sky, hit, g, prim, b);
}

// The smooth normal on a triangle the camera's ray met, from its corners' (the vertex shader's own normals, as the
// engine lights it), turned to the same side as the face; the face's own where none was captured or it disagrees.
float3 SmoothNormal(bool hit, uint geometry, uint primitive, float2 bary, float3 face)
{
    if (!hit) return face;
    const uint v = Geometry[geometry].x + primitive * 3u;
    if ((Runs[RunOf(v)].z & 1u) == 0u) return face;
    const float3 w = float3(1.0f - bary.x - bary.y, bary.x, bary.y);
    float3 s = Normals[v] * w.x + Normals[v + 1u] * w.y + Normals[v + 2u] * w.z;
    if (!(dot(s, s) > 1e-8f)) return face;
    s = normalize(s);
    if (dot(s, face) < 0.0f) s = -s;   // a two sided surface seen from its back
    return dot(s, face) > 0.3f ? s : face;
}


[numthreads(8, 8, 1)]
void AoMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    if (any((float2)p >= View.xy)) return;
    bool sky, hit;
    uint hitGeometry, hitPrimitive;
    float2 hitBary;
    const float3 P = SurfaceHit(int2(p), sky, hit, hitGeometry, hitPrimitive, hitBary);
    if (sky)
    {
        Out[p] = float4(1.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    // The surface's normal from its neighbours, each axis from the nearer side, so an edge in front of or behind the
    // pixel does not bend it.
    bool sr, sl, su, sd;
    const float3 r = Surface(int2(p) + int2(1, 0), sr) - P;
    const float3 l = P - Surface(int2(p) - int2(1, 0), sl);
    const float3 u = Surface(int2(p) + int2(0, 1), su) - P;
    const float3 dn = P - Surface(int2(p) - int2(0, 1), sd);
    const float3 dx = (sr || (!sl && dot(l, l) < dot(r, r))) ? l : r;
    const float3 dy = (su || (!sd && dot(dn, dn) < dot(u, u))) ? dn : u;
    const float3 toEye = Eye.xyz - P;
    float3 n = cross(dx, dy);
    n = (dot(n, n) > 1e-12f) ? normalize(n) : normalize(toEye);
    if (dot(n, toEye) < 0.0f) n = -n;
    // The rays go round the smooth normal, as the engine's light does, so a model's faces do not show in its AO;
    // they stay above the face itself (a ray under it is turned back up), and the bias is measured from the face.
    const float3 ns = SmoothNormal(hit, hitGeometry, hitPrimitive, hitBary, n);

    // The engine's own SSAO (ssao_scalable_ambient_obscurance), traced: an occluder at t along a
    // direction at cosine c to the normal rises t * c above the plane and counts only past the bias, so the ray starts
    // at bias / c; it weighs 1 - (t / reach)^2; the AO is exp(-1.4 * strength * the mean weight), the engine's
    // (1 - strength * occlusion)^1.4 while that is small, without its hard stop at black.
    uint rng = ((uint)View.z) << 8;
    const uint rays = (uint)clamp(Ao.w, 1.0f, 16.0f);
    const float reach = max(Ao.x, 0.01f);
    const float bias = max(Ao.y, 0.0f);
    const float lift = max(0.02f, length(toEye) * 1e-5f);
    float occlusion = 0.0f;
    [loop] for (uint i = 0; i < rays; ++i)
    {
        float3 dir = CosineHemisphere(ns, rng, p);
        const float under = dot(dir, n);
        if (under < 0.01f) dir = normalize(dir - (2.0f * under - 0.02f) * n);
        const float start = (bias > 0.0f) ? bias / max(dot(dir, n), 1e-3f) : 0.0f;
        if (start >= reach) continue;
        RayDesc ray;
        ray.Origin = P + n * lift;
        ray.Direction = dir;
        ray.TMin = start;
        ray.TMax = reach;
        const float t = Closest(ray, RandomFloat(rng, p));
        if (t >= 0.0f)
        {
            const float x = t / reach;
            occlusion += 1.0f - x * x;
        }
    }
    occlusion /= (float)rays;
    const float ao = exp(-1.4f * max(Ao.z, 0.0f) * occlusion);
    // "debug": the normal beside the AO, to look at.
    Out[p] = View.w > 0.5f ? float4(ao, ns * 0.5f + 0.5f) : float4(ao, 0.0f, 0.0f, 0.0f);
}
#endif
