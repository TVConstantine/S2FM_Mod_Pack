// What touches a water (s2vfx_water) and what it leaves behind: ripples spreading from it, the floating debris pushed
// aside and drifting back, foam. Counter-Strike 2's water reads such a map from the game (its players draw into it);
// Alyx has none, so it is worked out here, for anything at all that stands in the water.
//
// The map is fixed to the world and repeats every kArea units (a water larger than that sees the same pattern again
// kArea away): kSize x kSize texels of x the water's height, y its height a step before, z how far the debris is
// cleared (1 all), w foam. After each water draw the depth before it (the scene) and the water's own depth (the
// engine draws see-through surfaces on a read-only depth view, so the water is drawn once more, depth only, into a
// copy of ours) give, for every pixel the water covers, how deep the scene is below the surface; what is less than kBand
// below it is in contact. Contacts are gathered into a map of their own (the strongest a texel), then the map takes
// as many steps as the engine's time moved (60 a second, at most 8 a frame): a change of contact pushes the surface
// (a ripple), the surface spreads by the wave equation and calms, the debris is held clear where something stands and
// drifts back over 8 s, foam fades. The time is the view's own (g_flTime), which follows S2FM's timeline:
// a frame drawn again (refinement) takes no step, a jump back or far ahead starts the map again.
//
// Splashes: where much of the water around a texel is reached in one frame (a body landing, not a leg wading), droplets
// are thrown up and out of it, faster the quicker the wet area grew. They fly under gravity on the card (kDrops kept in
// a ring, the oldest taken first), and where one falls back into the water it leaves a small ripple and foam. They are
// drawn after the water, once for each frame copy, as small lenses: the scene through them upside down, the light
// around on their rim, stretched along their path, behind what stands in front of them by the engine's depth.
//
// Wetness: the map also keeps the water's height wherever it was seen and, where something stands in it, a wet line
// a little above the surface that sinks slowly once nothing is there; droplets falling onto the scene leave wet spots
// that dry. After the water, one full screen pass finds each pixel's point from the scene's depth and makes it darker
// and shinier between the surface and the wet line and at the spots: every model, the floor and the walls alike, with
// no change to their shaders. The wetness stays where it was in the world; it does not go along with what moves.
#include "../core/core.h"
#include "water_fx.h"
#include "water_carry.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

const float kArea = 2048.0f;
const UINT kSize = 1024;
const float kBand = 8.0f;   // the debris drifts back over 8 s and foam fades over 1.5 s (in the Step shader)
const UINT kDrops = 8192;
const float kSplashSpeed = 80.0f;   // how fast (units a second) the wet area must grow for droplets
const float kDropRadius = 0.55f;
// Times closer than this to the last stepped one are that moment again (refinement's samples, Advance); a movie frame
// at 24 a second is 0.042 s, the shutter's sweep measured at 0.0102 s.
const float kSameMoment = 0.03f;

// The constants of every shader here.
#define CONSTS                                                                                                        \
    "cbuffer C : register(b0) {\n"                                                                                     \
    "    row_major float4x4 g_projToRel;\n"                                                                            \
    "    float4 g_range;     // depth min, max, target width, height\n"                                                \
    "    float4 g_camera;    // the camera in the world, the contact band\n"                                           \
    "    float4 g_map;       // area, size, step seconds, first step of the frame\n"                                   \
    "    float4 g_splash;    // frame seconds, the time, the speed for droplets, a droplet's radius\n"                 \
    "    row_major float4x4 g_worldToProj;\n"                                                                          \
    "    float4 g_camOff;\n"                                                                                           \
    "    float4 g_viewport;  // x, y, width, height\n"                                                                 \
    "};\n"                                                                                                             \
    "struct Drop { float3 pos; float life; float3 vel; float surface; };\n"                                           \
    "// A spot where a droplet landed, kept in the texel under it: the repeat of the world it is of as one number, and\n" \
    "// where in the texel it landed (a 256th of the texel each way) as another.\n"                                      \
    "float SpotTile(float2 xy) { const float2 t = floor(xy / g_map.x) + 2048.0; return t.x + t.y * 4096.0; }\n"      \
    "float SpotPlace(float2 xy) {\n"                                                                                   \
    "    const float2 f = floor(frac(xy / (g_map.x / g_map.y)) * 255.0);\n"                                           \
    "    return f.x + f.y * 256.0;\n"                                                                                  \
    "}\n"

const char kCs[] =
    CONSTS
    "Texture2D<float> g_pre : register(t0);\n"
    "#ifdef POST_MS\n"
    "Texture2DMS<float> g_post : register(t1);\n"
    "#else\n"
    "Texture2D<float> g_post : register(t1);\n"
    "#endif\n"
    "Texture2D<float4> g_state : register(t0);\n"
    "// A texel's contact: how deep (the low 16 bits, 0 none) and whether the water over it was seen (bit 16). A texel\n"
    "// not seen in a frame (behind a model standing in front of the water, out of the view) keeps the frame before's:\n"
    "// what the camera does not see has not changed, and the camera moving makes no ripples.\n"
    "#define SEEN 0x10000u\n"
    "Texture2D<uint> g_contact : register(t1);\n"
    "Texture2D<uint> g_contactBefore : register(t2);\n"
    "RWTexture2D<uint> g_contactOut : register(u0);\n"
    "float Depth(uint c) { return (c & 0xFFFFu) / 65535.0; }\n"
    "RWBuffer<uint> g_count : register(u1);      // water pixels, touching, all, depth changed\n"
    "RWTexture2D<float4> g_stateOut : register(u0);\n"
    "RWTexture2D<float4> g_surfaceOut : register(u2);   // where a texel's contact was in the world, the water's height\n"
    "Texture2D<float4> g_surface : register(t3);\n"
    "Texture2D<uint> g_impact : register(t4);           // droplets fallen back a frame before\n"
    "Texture2D<float4> g_carriedAt : register(t6);      // the moving models' normals a frame before (w 1 on one)\n"
    "RWStructuredBuffer<Drop> g_drops : register(u1);\n"
    "RWBuffer<uint> g_ring : register(u2);\n"
    "RWTexture2D<uint> g_impactOut : register(u3);\n"
    "// Wetness: the water's height where it was seen this frame; kept, the last height known (x) and how high things\n"
    "// standing in it are wet (y); spots where droplets hit something, at what height (x) and how wet (y).\n"
    "// The map repeats every area units, so each of these keeps which repeat of the world (zw, its tile: the world's\n"
    "// x and y over the area, floored) its value is of; a reader takes a value only of its own tile, or a water seen\n"
    "// once would be known again every area units all over a large map.\n"
    "RWTexture2D<float4> g_levelOut : register(u3);   // height, tile\n"
    "Texture2D<float4> g_levelIn : register(t5);\n"
    "RWTexture2D<float4> g_wet : register(u3);       // height, wet line, tile\n"
    "RWTexture2D<float4> g_spots : register(u4);     // height, how wet, SpotTile, SpotPlace\n"
    "RWBuffer<uint> g_stepCount : register(u5);   // the first step's counts (4 on): reached, fresh, splashing, droplets\n"
    "float2 Tile(float2 xy) { return floor(xy / g_map.x); }\n"
    "float Hash(uint2 p, uint k) {\n"
    "    uint h = p.x * 1973u + p.y * 9277u + k * 26699u + asuint(g_splash.y) * 7919u;\n"
    "    h = (h ^ 61u) ^ (h >> 16); h *= 9u; h ^= h >> 4; h *= 0x27d4eb2du; h ^= h >> 15;\n"
    "    return (h & 0xffffffu) / 16777216.0;\n"
    "}\n"
    "// A pixel of the target and its depth, to the world: by the viewport the engine drew through, which need not be\n"
    "// the whole target. The engine draws the scene into a part of the depth range (0.05 to 0.95) and the 3D skybox and\n"
    "// sky below it; a depth outside the viewport's range is not of this view, and gives a point too far to matter.\n"
    "float3 World(float2 px, float d) {\n"
    "    const float z = (d - g_range.x) / (g_range.y - g_range.x);\n"
    "    if (!(z > 1e-6 && z <= 1.0)) return g_camera.xyz + 1e30;\n"
    "    float2 ndc = ((px + 0.5 - g_viewport.xy) / g_viewport.zw) * 2.0 - 1.0;\n"
    "    ndc.y = -ndc.y;\n"
    "    const float4 v = mul(float4(ndc, z, 1.0), g_projToRel);\n"
    "    return v.xyz / v.w + g_camera.xyz;\n"
    "}\n"
    "bool InView(float2 px) { return all(px >= g_viewport.xy) && all(px < g_viewport.xy + g_viewport.zw); }\n"
    "// The floor under each texel: the lowest point of the scene ever seen through a water there, as a number that\n"
    "// orders as the float -z does (the lowest the largest, 0 none yet), and the repeat of the world it is of. What\n"
    "// stands in shallow water is told from the floor by it: the floor itself is less than the contact band below the\n"
    "// surface and would be in contact everywhere, and each texel of it coming into view would be a contact new to it.\n"
    "uint Ord(float f) { const uint u = asuint(f); return (u & 0x80000000u) ? ~u : u | 0x80000000u; }\n"
    "float Unord(uint k) { return asfloat((k & 0x80000000u) ? k & 0x7FFFFFFFu : ~k); }\n"
    "// The repeat as one number (a typed unordered view reads back one channel only), 0 none.\n"
    "uint TileKey(float2 xy) { const float2 t = floor(xy / g_map.x) + 2048.0; return (uint)t.x + (uint)t.y * 4096u + 1u; }\n"
    "RWTexture2D<uint> g_floorOut : register(u0);\n"
    "RWTexture2D<uint> g_floorTileOut : register(u1);\n"
    "Texture2D<uint> g_floorIn : register(t2);\n"
    "Texture2D<uint> g_floorTileIn : register(t3);\n"
    "// The scene's point under the water at a pixel of the target, or false (not the water's pixel, the sky below).\n"
    "bool Under(uint2 px, out float3 w, out float3 s, out float dw, out float ds) {\n"
    "    const float d0 = g_pre.Load(int3(px, 0));\n"
    "#ifdef POST_MS\n"
    "    const float d1 = g_post.Load(px, 0);\n"
    "#else\n"
    "    const float d1 = g_post.Load(int3(px, 0));\n"
    "#endif\n"
    "    w = World(px, d1);\n"
    "    s = World(px, d0);\n"
    "    dw = length(w - g_camera.xyz);\n"
    "    ds = length(s - g_camera.xyz);\n"
    "    return dw < ds - 0.01 && ds < 1e5 && all(isfinite(w)) && all(isfinite(s));\n"
    "}\n"
    "// Before the detection: the floor kept lower where this frame sees lower.\n"
    "[numthreads(8, 8, 1)] void Floor(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= (uint)g_range.z || id.y >= (uint)g_range.w || !InView(id.xy)) return;\n"
    "    float3 w, s;\n"
    "    float dw, ds;\n"
    "    if (!Under(id.xy, w, s, dw, ds)) return;\n"
    "    const uint2 t = (uint2)(frac(s.xy / g_map.x) * g_map.y) % (uint)g_map.y;\n"
    "    const uint tile = TileKey(s.xy);\n"
    "    if (g_floorTileOut[t] != tile) {   // another repeat of the world here now: its own floor\n"
    "        g_floorTileOut[t] = tile;\n"
    "        uint was;\n"
    "        InterlockedExchange(g_floorOut[t], Ord(-s.z), was);\n"
    "    } else {\n"
    "        InterlockedMax(g_floorOut[t], Ord(-s.z));\n"
    "    }\n"
    "}\n"
    "[numthreads(8, 8, 1)] void Detect(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= (uint)g_range.z || id.y >= (uint)g_range.w || !InView(id.xy)) return;\n"
    "    const float d0 = g_pre.Load(int3(id.xy, 0));\n"
    "#ifdef POST_MS\n"
    "    const float d1 = g_post.Load(id.xy, 0);\n"
    "#else\n"
    "    const float d1 = g_post.Load(int3(id.xy, 0));\n"
    "#endif\n"
    "    InterlockedAdd(g_count[2], 1);\n"
    "    if (d1 != d0) InterlockedAdd(g_count[3], 1);\n"
    "    // Nearer or farther by the distance from the camera, whichever way the depth is stored (the engine tests\n"
    "    // GREATER: near is the larger value).\n"
    "    const float3 w = World(id.xy, d1), s = World(id.xy, d0);\n"
    "    const float dw = length(w - g_camera.xyz), ds = length(s - g_camera.xyz);\n"
    "    if (!(dw < ds - 0.01)) {\n"
    "        // Not the water's pixel: the scene seen with no water in front of it. A point under a water is only ever\n"
    "        // seen through it, so one lower than the height kept for its texel says that water is gone (moved, scaled\n"
    "        // down): marked here (-2e9, its height), let go by the Step.\n"
    "        if (ds < 1e5 && all(isfinite(s)))\n"
    "            g_levelOut[(uint2)(frac(s.xy / g_map.x) * g_map.y) % (uint)g_map.y] = float4(-2e9, s.z, Tile(s.xy));\n"
    "        return;\n"
    "    }\n"
    "    InterlockedAdd(g_count[0], 1);\n"
    "    if (any(!isfinite(w))) return;                    // no point (the sky)\n"
    "    g_levelOut[(uint2)(frac(w.xy / g_map.x) * g_map.y) % (uint)g_map.y] = float4(w.z, 0.0, Tile(w.xy));   // its height\n"
    "    // Seen: the texel under the water here (where a contact would be), or the surface's own over the sky.\n"
    "    const float2 under = ds > 1e5 || any(!isfinite(s)) ? w.xy : s.xy;\n"
    "    InterlockedOr(g_contactOut[(uint2)(frac(under / g_map.x) * g_map.y) % (uint)g_map.y], SEEN);\n"
    "    if (ds > 1e5) return;                              // the sky below: nothing there\n"
    "    const float below = w.z - s.z;\n"
    "    if (below <= 0.0 || below > g_camera.w) return;\n"
    "    const uint2 t = (uint2)(frac(s.xy / g_map.x) * g_map.y) % (uint)g_map.y;\n"
    "    // Only what stands above the floor: none where the floor is not known yet, none on the floor itself.\n"
    "    const uint floorKey = g_floorIn[t];\n"
    "    if (floorKey == 0u || g_floorTileIn[t] != TileKey(s.xy) || s.z < -Unord(floorKey) + FLOOR_MARGIN) return;\n"
    "    InterlockedMax(g_contactOut[t], SEEN | (uint)max((1.0 - below / g_camera.w) * 65535.0, 1.0));\n"
    "    g_surfaceOut[t] = float4(s.xy, w.z, 1.0);\n"
    "    InterlockedAdd(g_count[1], 1);\n"
    "}\n"
    "// After the detection (or in a frame without the water): a texel not seen keeps the frame before's contact, as not\n"
    "// seen this frame.\n"
    "[numthreads(8, 8, 1)] void Keep(uint3 id : SV_DispatchThreadID) {\n"
    "    if (!(g_contactOut[id.xy] & SEEN)) g_contactOut[id.xy] = g_contactBefore[id.xy] & 0xFFFFu;\n"
    "}\n"
    "// What has stood in each texel lately: the highest contact seen there, sinking TOP_FALL a second while the texel is\n"
    "// seen with nothing in it (what moved has gone); -1 never seen. A contact makes waves only where it rises above it:\n"
    "// a wall or a model standing still in the water is no push however its depth, seen through the water, wavers with\n"
    "// the view (the point of it nearest the surface that a pixel happens to see), and neither is a texel seen for the\n"
    "// first time. Nor does it sink while something stands in a texel beside it: a wall's face lies on the line between\n"
    "// two rows of texels and falls into one or the other as its depth rounds, so either is seen empty now and then.\n"
    "Texture2D<float> g_topIn : register(t7);\n"
    "RWTexture2D<float> g_topOut : register(u0);\n"
    "// A rise under RISE_MIN is none: a wall's point nearest the surface creeps up a little at a time as the camera\n"
    "// comes closer (the pixels fall nearer the waterline), a few hundred texels a frame; something coming in rises more.\n"
    "// And only in a texel seen the frame before as well: one the view has just come to (at the screen's edge, a sharp\n"
    "// turn) shows what stood there all along, not what came in.\n"
    "bool SeenBefore(uint2 q) { return (g_contactBefore[q] & SEEN) != 0u; }\n"
    "float Rise(uint2 q) {\n"
    "    const float top = g_topIn[q], r = Depth(g_contact[q]) - top;\n"
    "    return top < 0.0 || r < RISE_MIN || !SeenBefore(q) ? 0.0 : r;\n"
    "}\n"
    "// Reached now where nothing has stood lately.\n"
    "bool Fresh(uint2 q) {\n"
    "    const float top = g_topIn[q];\n"
    "    return top >= 0.0 && top < 0.02 && Depth(g_contact[q]) > 0.0 && SeenBefore(q);\n"
    "}\n"
    "// After the steps of a frame: the texels seen this frame.\n"
    "[numthreads(8, 8, 1)] void Top(uint3 id : SV_DispatchThreadID) {\n"
    "    const uint cn = g_contact[id.xy];\n"
    "    if (!(cn & SEEN)) return;\n"
    "    const int n = (int)g_map.y;\n"
    "    float beside = 0.0;   // the most in contact of the 3 x 3 around, this one too\n"
    "    [unroll] for (int y = -1; y <= 1; y++)\n"
    "    [unroll] for (int x = -1; x <= 1; x++)\n"
    "        beside = max(beside, Depth(g_contact[uint2((int(id.x) + x + n) % n, (int(id.y) + y + n) % n)]));\n"
    "    const float c0 = Depth(cn), top = g_topOut[id.xy];\n"
    "    g_topOut[id.xy] = top < 0.0 ? beside : c0 > 0.0 ? max(c0, top) : beside > 0.0 ? top\n"
    "                    : max(top - g_splash.x * TOP_FALL, 0.0);\n"
    "}\n"
    "[numthreads(8, 8, 1)] void Step(uint3 id : SV_DispatchThreadID) {\n"
    "    const int n = (int)g_map.y;\n"
    "    const float4 s = g_state[id.xy];\n"
    "    // The four around (the map repeats), and the contact and its rise, widened over the 3 x 3 around (a contact is\n"
    "    // a texel or two: a push that narrow rings at the texel's own size).\n"
    "    float4 around = 0.0;\n"
    "    float c = 0.0, rise = 0.0;\n"
    "    const float c0 = Depth(g_contact[id.xy]);\n"
    "    [unroll] for (int y = -1; y <= 1; y++)\n"
    "    [unroll] for (int x = -1; x <= 1; x++) {\n"
    "        const uint2 q = uint2((int(id.x) + x + n) % n, (int(id.y) + y + n) % n);\n"
    "        c = max(c, Depth(g_contact[q]));\n"
    "        rise = max(rise, Rise(q));\n"
    "        if (abs(x) + abs(y) == 1) around += g_state[q];\n"
    "    }\n"
    "    around *= 0.25;\n"
    "    const float push = g_map.w > 0.5 ? rise : 0.0;              // for the foam\n"
    "    const float push0 = g_map.w > 0.5 ? Rise(id.xy) : 0.0;     // for the ripples\n"
    "    const bool fresh0 = Fresh(id.xy);\n"
    "    if (g_map.w > 0.5 && Depth(g_contact[id.xy]) > 0.0 && g_topIn[id.xy] >= 0.0 && g_topIn[id.xy] < 0.02) {\n"
    "        InterlockedAdd(g_stepCount[4], 1);   // reached where nothing stood lately\n"
    "        if (fresh0) InterlockedAdd(g_stepCount[5], 1);   // and seen the frame before\n"
    "    }\n"
    "    const float fallen = g_map.w > 0.5 ? (float)g_impact[id.xy] : 0.0;   // droplets come down here\n"
    "    // Splashes: a texel reached in this frame, where much of the water around was reached with it.\n"
    "    if (g_map.w > 0.5 && fresh0) {\n"
    "        float fresh = 0.0, wet = 0.0;\n"
    "        float2 middle = 0.0;\n"
    "        [unroll] for (int y = -2; y <= 2; y++)\n"
    "        [unroll] for (int x = -2; x <= 2; x++) {\n"
    "            const uint2 q = uint2((int(id.x) + x + n) % n, (int(id.y) + y + n) % n);\n"
    "            if (Fresh(q)) fresh += 1.0;\n"
    "            if (Depth(g_contact[q]) > 0.0) { middle += float2(x, y); wet += 1.0; }\n"
    "        }\n"
    "        // How fast the wet area grew, in units a second: the share of the 5 x 5 texels reached, over their width.\n"
    "        const float speed = fresh / 25.0 * 5.0 * g_map.x / g_map.y / max(g_splash.x, 1e-3);\n"
    "        const float amount = saturate((speed - g_splash.z) / g_splash.z);\n"
    "        const float4 surface = g_surface[id.xy];\n"
    "        if (amount > 0.0 && surface.w > 0.0) {\n"
    "            const float2 away = wet > 0.0 ? -middle / wet : 0.0;   // out of the wet middle\n"
    "            const uint count = (uint)(amount * 9.0 + 1.0 + Hash(id.xy, 0) * 2.0);\n"
    "            InterlockedAdd(g_stepCount[6], 1);\n"
    "            InterlockedAdd(g_stepCount[7], count);\n"
    "            for (uint k = 0; k < count; k++) {\n"
    "                uint slot;\n"
    "                InterlockedAdd(g_ring[0], 1, slot);\n"
    "                const float r1 = Hash(id.xy, k * 4 + 1), r2 = Hash(id.xy, k * 4 + 2), r3 = Hash(id.xy, k * 4 + 3);\n"
    "                float sa, ca;\n"
    "                sincos(r1 * 6.2831853, sa, ca);\n"
    "                const float2 dir = normalize(away + float2(ca, sa) * 0.8 + 1e-4);\n"
    "                // Launched at 60 to 300 units a second (some 0.3 to 1.5 s in the air), faster the harder it came in.\n"
    "                const float up = (100.0 + min(speed * 0.4, 200.0)) * (0.6 + 0.4 * r2);\n"
    "                Drop d;\n"
    "                d.pos = float3(surface.xy + (float2(r2, r3) - 0.5) * 2.0, surface.z + 0.5);\n"
    "                d.life = 1.5 + 1.5 * r3;\n"
    "                d.vel = float3(dir * up * (0.2 + 0.5 * r3), up);\n"
    "                d.surface = surface.z;\n"
    "                g_drops[slot % DROPS] = d;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    // Wading: where something moves on through the water (its contact rising above what stood there lately, never\n"
    "    // a wall or a model standing still), now and then a droplet is kicked up off its front, more the higher it rises.\n"
    "    if (push0 > 0.0) {\n"
    "        const float4 surface = g_surface[id.xy];\n"
    "        if (surface.w > 0.0 && Hash(id.xy, 40) < saturate(push0 * 2.0) * WADE) {\n"
    "            uint slot;\n"
    "            InterlockedAdd(g_ring[0], 1, slot);\n"
    "            InterlockedAdd(g_stepCount[7], 1);\n"
    "            const float r1 = Hash(id.xy, 41), r2 = Hash(id.xy, 42), r3 = Hash(id.xy, 43);\n"
    "            float sa, ca;\n"
    "            sincos(r1 * 6.2831853, sa, ca);\n"
    "            const float up = (70.0 + 60.0 * r2) * (0.7 + 0.6 * saturate(push0 * 2.0));\n"
    "            Drop d;\n"
    "            d.pos = float3(surface.xy + float2(ca, sa) * (0.5 + r3), surface.z + 0.5);\n"
    "            d.life = 1.0 + r3;\n"
    "            d.vel = float3(float2(ca, sa) * up * (0.2 + 0.4 * r3), up);\n"
    "            d.surface = surface.z;\n"
    "            g_drops[slot % DROPS] = d;\n"
    "        }\n"
    "    }\n"
    "    // The wave equation, calming.\n"
    "    float h = s.x + (s.x - s.y) * 0.985 + 0.12 * (around.x - s.x);\n"
    "    h = h * 0.995 + push0 * 0.6 - fallen * 0.05;\n"
    "    // The debris held clear where something stands, softened into the texels around, drifting back over 8 s.\n"
    "    const float clear = max(lerp(s.z, around.z, 0.3) - g_map.z / 8.0, c > 0.0 ? 1.0 : 0.0);\n"
    "    // Foam: churned up where something moves, spreading a little to the texels around, fading over some 3.5 s.\n"
    "    const float foam = max(lerp(s.w, around.w, 0.2) * exp(-g_map.z / 3.5), max(push, fallen * 0.15));\n"
    "    g_stateOut[id.xy] = float4(clamp(h, -4.0, 4.0), s.x, saturate(clear), saturate(foam));\n"
    "    // Wet: whatever stands in the water is wet to WET_BAND above the surface (the water creeping up, the small\n"
    "    // waves); once out of it the wet line sinks by WET_DRY units a second. Spots dry over WET_SPOT seconds.\n"
    "    const float4 seen = g_levelIn[id.xy];\n"
    "    float level = seen.x;\n"
    "    float2 tile = seen.zw;\n"
    "    const float4 surface = g_surface[id.xy];\n"
    "    if (c0 > 0.0 && surface.w > 0.0 && surface.z > level) { level = surface.z; tile = Tile(surface.xy); }\n"
    "    float4 wet = g_wet[id.xy];\n"
    "    // Seen dry below the water's height kept here (a camera above it, this repeat of the world): no water now.\n"
    "    if (seen.x < -1.5e9 && wet.x > -1e8 && seen.y < wet.x - 1.0 && g_camera.z > wet.x && all(seen.zw == wet.zw))\n"
    "        wet.x = -1e9;\n"
    "    if (level > -1e8) {\n"
    "        if (any(wet.zw != tile)) wet.y = -1e9;   // another repeat of the world now: its own line\n"
    "        wet.xzw = float3(level, tile);\n"
    "    }\n"
    "    wet.y = c > 0.0 && wet.x > -1e8 ? max(wet.y, wet.x + WET_BAND) : wet.y - g_map.z * WET_DRY;\n"
    "    g_wet[id.xy] = wet;\n"
    "    const float4 spot = g_spots[id.xy];\n"
    "    if (spot.y > 0.0) g_spots[id.xy] = float4(spot.x, max(spot.y - g_map.z / WET_SPOT, 0.0), spot.zw);\n"
    "}\n"
    "// The droplets fly: gravity (386 units a second squared, an inch a unit), a little drag; one that falls back into\n"
    "// its water is gone and counted where it came down.\n"
    "[numthreads(64, 1, 1)] void Fly(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x == 0) g_stepCount[11] = g_ring[0];\n"
    "    Drop d = g_drops[id.x];\n"
    "    if (d.life <= 0.0) return;\n"
    "    InterlockedAdd(g_stepCount[8], 1);\n"
    "    const float dt = g_splash.x;\n"
    "    d.vel.z -= 386.0 * dt;\n"
    "    d.vel *= exp(-0.4 * dt);\n"
    "    d.pos += d.vel * dt;\n"
    "    d.life -= dt;\n"
    "    const uint2 t = (uint2)(frac(d.pos.xy / g_map.x) * g_map.y) % (uint)g_map.y;\n"
    "    if (d.pos.z < d.surface && d.vel.z < 0.0) {\n"
    "        d.life = 0.0;\n"
    "        InterlockedAdd(g_impactOut[t], 1);\n"
    "        InterlockedAdd(g_stepCount[9], 1);\n"
    "    } else if (d.vel.z < 0.0) {\n"
    "        // Falling onto something of the scene (a leg, the floor beside the water): a fast one bounces once, up off\n"
    "        // it, slower; one that has bounced or falls slowly stays, a wet spot where it struck (the scene's point).\n"
    "        const float4 clip = mul(float4(d.pos, 1.0) + g_camOff, g_worldToProj);\n"
    "        const float2 ndc = clip.xy / max(clip.w, 1e-4);\n"
    "        const float2 px = g_viewport.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * g_viewport.zw;\n"
    "        if (clip.w > 0.0 && InView(px) && all(px < g_range.zw)) {\n"
    "            const float3 s = World(floor(px), g_pre.Load(int3(px, 0)));\n"
    "            const float behind = length(d.pos - g_camera.xyz) - length(s - g_camera.xyz);\n"
    "            if (behind > 0.0 && behind < 4.0) {\n"
    "                InterlockedAdd(g_stepCount[10], 1);\n"
    "                if (length(d.vel) > 80.0 && d.life > 0.5) {\n"
    "                    d.pos = s + normalize(g_camera.xyz - s) * 0.5;\n"
    "                    d.vel = float3(d.vel.xy * 0.5, abs(d.vel.z) * 0.25);\n"
    "                    d.life = 0.5;\n"
    "                } else {\n"
    "                    d.life = 0.0;\n"
    "                    // A spot is kept in the world: on a model that moves it would be left hanging in the air as it\n"
    "                    // walks on; such a model has its own wetness (water_carry.cpp's normals there, a frame before).\n"
    "                    const uint2 ts = (uint2)(frac(s.xy / g_map.x) * g_map.y) % (uint)g_map.y;\n"
    "                    if (!(g_carriedAt.Load(int3(px, 0)).w > 0.5))\n"
    "                        g_spots[ts] = float4(s.z, 1.0, SpotTile(s.xy), SpotPlace(s.xy));\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    g_drops[id.x] = d;\n"
    "}\n";

// The droplets drawn: six corners a droplet, a quad facing the camera, stretched along its path over a 1/60 s
// shutter; a dead droplet's corners fall outside the view.
const char kDraw[] =
    CONSTS
    "StructuredBuffer<Drop> g_dropsIn : register(t0);\n"
    "Texture2D<float4> g_copy : register(t1);\n"
    "SamplerState g_linear : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : UV; nointerpolation float3 centre : CENTRE; };\n"
    "float4 Clip(float3 p) { return mul(float4(p, 1.0) + g_camOff, g_worldToProj); }\n"
    "V Vs(uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "    V o = (V)0;\n"
    "    o.pos = float4(0.0, 0.0, -1.0, 1.0);\n"
    "    const Drop d = g_dropsIn[iid];\n"
    "    if (d.life <= 0.0) return o;\n"
    "    const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };\n"
    "    const float2 c = corners[vid];\n"
    "    const float3 toCamera = normalize(g_camera.xyz - d.pos);\n"
    "    const float speed = length(d.vel);\n"
    "    float3 along = speed > 1.0 ? d.vel / speed : float3(0.0, 0.0, 1.0);\n"
    "    float3 across = cross(along, toCamera);\n"
    "    if (dot(across, across) < 1e-6) across = cross(float3(1.0, 0.0, 0.0), toCamera);\n"
    "    across = normalize(across);\n"
    "    along = cross(toCamera, across);\n"
    "    const float r = g_splash.w * (0.6 + 0.8 * frac(iid * 0.618034)) * (((uint)g_splash.z & 256u) ? 8.0 : 1.0);\n"
    "    const float stretch = r + speed * (0.5 / 60.0);\n"
    "    o.pos = Clip(d.pos + along * c.y * stretch + across * c.x * r);\n"
    "    o.uv = float2(c.x, c.y);\n"
    "    const float4 m = Clip(d.pos);\n"
    "    const float2 ndc = m.xy / max(m.w, 1e-4);\n"
    "    // The middle in pixels, and how much of the droplet's light is left by the stretch and as it dies.\n"
    "    o.centre = float3(g_viewport.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * g_viewport.zw,\n"
    "                      saturate(d.life * 4.0) * r / stretch);\n"
    "    return o;\n"
    "}\n"
    "float4 Ps(V i) : SV_Target {\n"
    "    const float d2 = dot(i.uv, i.uv);\n"
    "    if (d2 > 1.0) discard;\n"
    "    if ((uint)g_splash.z & 256u) return float4(1.0, 0.0, 1.0, 1.0);   // \"dropdebug\"\n"
    "    const float3 n = float3(i.uv, sqrt(1.0 - d2));\n"
    "    float2 size;\n"
    "    float mips;\n"
    "    g_copy.GetDimensions(0, size.x, size.y, mips);\n"
    "    // Through it the scene upside down, as a droplet's lens shows it; on its rim the light around it, blurred.\n"
    "    const float2 through = i.centre.xy + (i.centre.xy - i.pos.xy) * 0.8;\n"
    "    const float3 seen = g_copy.SampleLevel(g_linear, clamp(through / size, 0.0, 1.0), 0.0).rgb;\n"
    "    const float3 around = g_copy.SampleLevel(g_linear, clamp(i.centre.xy / size, 0.0, 1.0), max(mips - 4.0, 0.0)).rgb;\n"
    "    const float fresnel = 0.02 + 0.98 * pow(1.0 - n.z, 5.0);\n"
    "    float3 c = lerp(seen * 0.9, around * 1.5, fresnel);\n"
    "    c += around * 6.0 * pow(saturate(dot(n, normalize(float3(-0.35, 0.45, 0.82)))), 60.0);\n"
    "    return float4(c, saturate((1.0 - d2) * 5.0) * i.centre.z);\n"
    "}\n"
    "// Wetness, over the whole picture: each pixel's scene point found from the scene's depth; from the water's surface\n"
    "// up to the wet line (the map's, with a ragged edge) and at droplets' spots it is darker and shinier: two\n"
    "// outputs, what is added and what the picture is multiplied by (dual source blending). Under the surface the\n"
    "// pixel shows the water now, which is left alone.\n"
    "Texture2D<float> g_sceneDepth : register(t2);\n"
    "Texture2D<float4> g_wetMap : register(t3);    // height, wet line, tile (of the world's repeats)\n"
    "Texture2D<float4> g_spotMap : register(t4);   // height, how wet, SpotTile, SpotPlace\n"
    "Texture2D<float2> g_carryMap : register(t5);  // what the moving models carry, and their beads (water_carry.cpp)\n"
    "Texture2D<float4> g_carryNormal : register(t6);   // and their smooth normals, w 1 where there is one\n"
    "float H(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
    "float Noise(float2 p) {\n"
    "    const float2 i = floor(p);\n"
    "    float2 f = frac(p);\n"
    "    f = f * f * (3.0 - 2.0 * f);\n"
    "    return lerp(lerp(H(i), H(i + float2(1, 0)), f.x), lerp(H(i + float2(0, 1)), H(i + 1.0), f.x), f.y);\n"
    "}\n"
    "// The scene's point at a pixel of the target, by its depth (far away where there is none: the sky).\n"
    "float3 ScenePoint(float2 px) {\n"
    "    const float d = g_sceneDepth.Load(int3(px, 0));\n"
    "    const float z = (d - g_range.x) / (g_range.y - g_range.x);\n"
    "    if (!(z > 1e-6 && z <= 1.0)) return g_camera.xyz + 1e6;\n"
    "    float2 ndc = (px + 0.5 - g_viewport.xy) / g_viewport.zw * 2.0 - 1.0;\n"
    "    ndc.y = -ndc.y;\n"
    "    const float4 r = mul(float4(ndc, z, 1.0), g_projToRel);\n"
    "    return abs(r.w) > 1e-9 ? r.xyz / r.w + g_camera.xyz : g_camera.xyz + 1e6;\n"
    "}\n"
    "float4 FullVs(uint vid : SV_VertexID) : SV_Position {\n"
    "    const float2 uv = float2((vid << 1) & 2, vid & 2);\n"
    "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n"
    "struct Wet { float4 add : SV_Target0; float4 mul : SV_Target1; };\n"
    "Wet WetPs(float4 pos : SV_Position) {\n"
    "    Wet o;\n"
    "    o.add = 0.0;\n"
    "    o.mul = 1.0;\n"
    "    const float d = g_sceneDepth.Load(int3(pos.xy, 0));\n"
    "    float2 ndc = (pos.xy - g_viewport.xy) / g_viewport.zw * 2.0 - 1.0;   // by the engine's viewport\n"
    "    ndc.y = -ndc.y;\n"
    "    const float4 r = mul(float4(ndc, (d - g_range.x) / (g_range.y - g_range.x), 1.0), g_projToRel);\n"
    "    float3 s = r.xyz / r.w + g_camera.xyz;\n"
    "    // The far plane, or a depth outside this view's part of the range (the 3D skybox and the sky): no point here.\n"
    "    const float zv = (d - g_range.x) / (g_range.y - g_range.x);\n"
    "    const bool sky = !(zv > 1e-6 && zv <= 1.0) || !(abs(r.w) > 1e-9) || any(!isfinite(s));\n"
    "    if (sky) s = g_camera.xyz + 1e6;\n"
    "    float3 n = cross(ddx(s), ddy(s));\n"
    "    const float3 v = normalize(s - g_camera.xyz);\n"
    "    n = dot(n, n) > 1e-12 ? normalize(n) : -v;\n"
    "    if (dot(n, v) > 0.0) n = -n;\n"
    "    // On a model taken for the carried wetness, its own normal (its material's normal map on it) rather than the\n"
    "    // depth's facets; one the map turned away from the camera is laid back flat to it, not flipped (a flip leaves a\n"
    "    // hard line that slides with the camera).\n"
    "    const float4 sn = g_carryNormal.Load(int3(pos.xy, 0));\n"
    "    if (sn.w > 0.5) {\n"
    "        n = sn.xyz;\n"
    "        const float away = dot(n, v);\n"
    "        if (away > 0.0) n = normalize(n - v * (away + 0.01));\n"
    "    }\n"
    "    if (sky || length(s - g_camera.xyz) > 1e5) return o;\n"
    "    // The map around: the water's height some 8 units each way (where it was seen), and the wet line 2 units each way\n"
    "    // (where something stands: wider, a floor flush with a shallow water would be wet all round it); each only of\n"
    "    // this repeat of the world.\n"
    "    const int size = (int)g_map.y;\n"
    "    const float2 t = s.xy / g_map.x * g_map.y;\n"
    "    const float texel = g_map.x / g_map.y;\n"
    "    float level = -1e9, top = -1e9;\n"
    "    [unroll] for (int y = -1; y <= 1; y++)\n"
    "    [unroll] for (int x = -1; x <= 1; x++) {\n"
    "        const float4 far = g_wetMap[((int2)floor(t + float2(x, y) * 4.0) % size + size) % size];\n"
    "        if (all(far.zw == floor((s.xy + float2(x, y) * 4.0 * texel) / g_map.x))) level = max(level, far.x);\n"
    "        const float4 near = g_wetMap[((int2)floor(t + float2(x, y)) % size + size) % size];\n"
    "        if (all(near.zw == floor((s.xy + float2(x, y) * texel) / g_map.x))) top = max(top, near.y);\n"
    "    }\n"
    "    float wet = 0.0;\n"
    "    if (level > -1e8 && s.z > level - 0.5) {\n"
    "        const float edge = top + (Noise(s.xy * 0.35 + s.z * 0.15) - 0.5) * 3.0;\n"
    "        // The line is on what stands in the water (its sides); a floor facing up beside it is not reached.\n"
    "        wet = saturate((edge - s.z) / 1.5) * saturate((1.0 - n.z) * 2.5);\n"
    "    }\n"
    "    // Droplets landed on the scene: a round bead where each struck, a dome the reflection bends over; on a wall it\n"
    "    // runs down as it dries, a streak growing from where it struck and narrowing. The texels around, one each.\n"
    "    const float upright = saturate(1.0 - abs(n.z));\n"
    "    float bead = 0.0;\n"
    "    float2 beadTilt = 0.0;\n"
    "    [unroll] for (int by = -1; by <= 1; by++)\n"
    "    [unroll] for (int bx = -1; bx <= 1; bx++) {\n"
    "        const float2 cell = floor(t) + float2(bx, by);\n"
    "        const float4 sp = g_spotMap[((int2)cell % size + size) % size];\n"
    "        const float2 f = float2(fmod(sp.w, 256.0), floor(sp.w / 256.0)) / 255.0;\n"
    "        const float2 c = (cell + f) * texel;\n"
    "        if (!(sp.y > 0.0) || sp.z != SpotTile(c)) continue;\n"
    "        const float r = 0.9 * (0.6 + 0.4 * sp.y);           // drying smaller\n"
    "        const float streak = (1.0 - sp.y) * 6.0 * upright;  // run down a wall over its life\n"
    "        const float below = sp.x - s.z;\n"
    "        const float along = below < 0.0 ? -below : max(below - streak, 0.0);\n"
    "        const float across = length(s.xy - c) / (1.0 - 0.4 * saturate(below / max(streak, 1e-3)));\n"
    "        const float shape = smoothstep(r, r * 0.55, length(float2(across, along))) * saturate(sp.y * 3.0);\n"
    "        if (shape > bead) { bead = shape; beadTilt = (s.xy - c) / r; }\n"
    "    }\n"
    "    if (bead > 0.0) {\n"
    "        wet = max(wet, bead);\n"
    "        n = normalize(n + float3(beadTilt, 0.0) * 1.4 * bead);\n"
    "    }\n"
    "    // A moving model's own wetness, where it is not under the water now.\n"
    "    const float2 carriedAll = g_carryMap.Load(int3(pos.xy, 0));\n"
    "    const float carried = carriedAll.x;\n"
    "    if (!(level > -1e8 && s.z < level - 0.5)) {\n"
    "        wet = max(wet, carried);\n"
    "        // A bead of water on it looks as one on the scene does: water standing up off the surface.\n"
    "        bead = max(bead, carriedAll.y);\n"
    "        wet = max(wet, carriedAll.y);\n"
    "    }\n"
    "    if ((uint)g_splash.z & 128u) {   // \"copydebug\": the picture this pass reads as what lies under each pixel\n"
    "        float2 dsize;\n"
    "        float dmips;\n"
    "        g_copy.GetDimensions(0, dsize.x, dsize.y, dmips);\n"
    "        o.mul = 0.0;\n"
    "        o.add = float4(g_copy.SampleLevel(g_linear, clamp(pos.xy / dsize, 0.0, 1.0), 0.0).rgb, 0.0);\n"
    "        return o;\n"
    "    }\n"
    "    if ((uint)g_splash.z & 64u) {   // \"modeldebug\": where a moving model is taken (red), carried wet (green), wet (blue)\n"
    "        o.mul = float4(0.3, 0.3, 0.3, 1.0);\n"
    "        o.add = float4(sn.w > 0.5 ? 0.6 : 0.0, carried, wet, 0.0);\n"
    "        return o;\n"
    "    }\n"
    "    if ((uint)g_splash.z & 8u) {   // \"beaddebug\": the beads alone, blue over the dimmed picture\n"
    "        o.mul = float4(0.3, 0.3, 0.3, 1.0);\n"
    "        o.add = float4(0.0, 0.0, carriedAll.y, 0.0);\n"
    "        return o;\n"
    "    }\n"
    "    if (g_splash.x > 0.5) {   // the test view: wet red, carried green, the water's height known or a bead blue\n"
    "        o.mul = float4(0.3, 0.3, 0.3, 1.0);\n"
    "        // Beads on a model bright blue over all of it.\n"
    "        o.add = float4(wet, carried, max(level > -1e8 && wet <= 0.0 ? 0.35 : 0.0, carriedAll.y * 3.0), 0.0);\n"
    "        return o;\n"
    "    }\n"
    "    if (wet <= 0.0) return o;\n"
    "    // Darker (the water fills the surface's pores), and glossy: the scene mirrored in it, some even face on, all of\n"
    "    // it at grazing angles; and what already shines on it shines harder.\n"
    "    float2 csize;\n"
    "    float mips;\n"
    "    g_copy.GetDimensions(0, csize.x, csize.y, mips);\n"
    "    const float2 cuv = clamp(pos.xy / csize, 0.0, 1.0);\n"
    "    const float3 around = g_copy.SampleLevel(g_linear, cuv, max(mips - 4.0, 0.0)).rgb;\n"
    "    const float3 own = g_copy.SampleLevel(g_linear, cuv, 0.0).rgb;\n"
    "    // The mirrored view ray 32 units on, found on the screen; going over to the light around as it nears the\n"
    "    // screen's edge or the camera's plane, never all at once (a jump there flickers as the view moves).\n"
    "    // Where no mirror can be read, the light around this pixel: near it (not the whole screen's, which with a bright\n"
    "    // sky greys a model seen from afar into something see-through), and no brighter than the surface itself.\n"
    "    const float3 luma0 = float3(0.2126, 0.7152, 0.0722);\n"
    "    const float3 nearby = g_copy.SampleLevel(g_linear, cuv, max(mips - 6.0, 0.0)).rgb;\n"
    "    const float3 local = nearby * min(1.0, dot(own, luma0) * 1.2 / max(dot(nearby, luma0), 1e-3));\n"
    "    float3 mirrored = local;\n"
    "    float keepMirror = 0.0;   // how much of it is the mirror's (the rest the light around)\n"
    "    const float4 rc = mul(float4(s + reflect(v, n) * 32.0, 1.0) + g_camOff, g_worldToProj);\n"
    "    if (rc.w > 1e-3) {\n"
    "        const float2 rn = rc.xy / rc.w;\n"
    "        const float2 rp = g_viewport.xy + float2(rn.x * 0.5 + 0.5, 0.5 - rn.y * 0.5) * g_viewport.zw;\n"
    "        const float2 edge = saturate((1.0 - abs(rn)) / 0.15);\n"
    "        // Seen from afar the 32 units are a few pixels on the screen: what is read there is what lies right behind\n"
    "        // the model, not what it mirrors (it would look see-through, its thin parts gone); the light around then.\n"
    "        const float near = saturate((length(rp - pos.xy) - 6.0) / 24.0);\n"
    "        // And only where the scene there is about as far as the mirrored point: where it is much farther, the ray\n"
    "        // went past the model into the open and what is read is what stands behind it (the model looks see-through).\n"
    "        const float3 there = ScenePoint(clamp(rp, g_viewport.xy, g_viewport.xy + g_viewport.zw - 1.0));\n"
    "        const float3 mirroredPoint = s + reflect(v, n) * 32.0;\n"
    "        const float found = saturate(1.0 - abs(length(there - g_camera.xyz) - length(mirroredPoint - g_camera.xyz)) / 32.0);\n"
    "        const float keep = edge.x * edge.y * saturate(rc.w / 8.0) * near * found;\n"
    "        keepMirror = keep;\n"
    "        if (keep > 0.0)\n"
    "            mirrored = lerp(local, g_copy.SampleLevel(g_linear, clamp(rp / csize, 0.0, 1.0), 2.5).rgb, keep);\n"
    "    }\n"
    "    // The water film's own reflection: Schlick's, from water's index of refraction (1.333 against air, 2% face on);\n"
    "    // what it reflects is taken from what lies beneath. A film's is held under 35%: the mirror here is the screen's,\n"
    "    // not the world's, and a full one at grazing angles takes the model's own colour away. A bead (a droplet) has its\n"
    "    // whole, and a highlight where its dome faces up towards the light around.\n"
    "    const float f0 = (1.333 - 1.0) * (1.333 - 1.0) / ((1.333 + 1.0) * (1.333 + 1.0));\n"
    "    const float fresnel = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(n, -v)), 5.0);\n"
    "    const float f = lerp(min(fresnel, 0.35), min(fresnel, 0.6), bead);\n"
    "    const float3 luma = float3(0.2126, 0.7152, 0.0722);\n"
    "    const float lit = dot(own, luma), lit0 = dot(around, luma);\n"
    "    const float3 shine = own * saturate((lit - lit0 * 1.3) / max(lit0, 1e-3)) * 1.5;\n"
    "    const float3 highlight = around * 4.0 * pow(saturate(dot(reflect(v, n), normalize(float3(0.2, 0.3, 1.0)))), 48.0) * bead;\n"
    "    // The test words' switches (g_splash.z, bits): 1 no mirror (the light around instead), 2 no light around (black\n"
    "    // where there is no mirror), 4 no shine.\n"
    "    const uint off = (uint)g_splash.z;\n"
    "    float3 reflected = mirrored;\n"
    "    if (off & 2u) reflected -= local * (1.0 - keepMirror);\n"
    "    if (off & 1u) reflected = (off & 2u) ? 0.0 : local;\n"
    "    const float3 shone = (off & 4u) ? 0.0 : shine;\n"
    "    // On a moving model nothing of the screen: what is read around it there is what stands behind it, so the model\n"
    "    // looks see-through, a layer that slides as it moves. Its own colour darker, its own shine harder, and on a bead\n"
    "    // its own colour lighter towards the rim (the drop's lens) and the highlight.\n"
    "    const bool onModel = sn.w > 0.5;\n"
    "    const float fr = onModel ? 0.0 : f;\n"
    "    const float3 lens = onModel ? own * 0.5 * fresnel * bead : 0.0;\n"
    "    o.mul = float4((1.0 - 0.6 * wet * (1.0 - 0.6 * bead)) * (1.0 - fr * wet), 1.0, 1.0, 1.0).xxxw;\n"
    "    o.add = float4((reflected * fr + shone) * wet + highlight + lens, 0.0);\n"
    "    if (off & 16u) o.add = 0.0;   // \"wetnoadd\": nothing added\n"
    "    if (off & 32u) o.mul = 1.0;   // \"wetnomul\": nothing darkened\n"
    "    // Never a value that is not a number: the engine's exposure and bloom average the picture, and one such pixel\n"
    "    // darkens the whole frame.\n"
    "    if (any(!isfinite(o.add)) || any(!isfinite(o.mul))) { o.add = 0.0; o.mul = 1.0; }\n"
    "    return o;\n"
    "}\n";

struct Consts {
    float projToRel[16];
    float range[4];
    float camera[4];
    float map[4];
    float splash[4];
    float worldToProj[16];
    float camOff[4];
    float viewport[4];
};

ID3D11ComputeShader* g_detect = nullptr;
ID3D11ComputeShader* g_detectMs = nullptr;   // the water's depth multisampled
ID3D11ComputeShader* g_step = nullptr;
ID3D11ComputeShader* g_fly = nullptr;
ID3D11ComputeShader* g_keep = nullptr;   // a texel not seen this frame keeps its contact
ID3D11ComputeShader* g_floorCs = nullptr;     // the floor under each texel kept, before the detection
ID3D11ComputeShader* g_floorCsMs = nullptr;   // the water's depth multisampled
ID3D11ComputeShader* g_topCs = nullptr;       // what has stood in each texel lately, after the steps
// Where each texel's contact last was in the world, droplets fallen back this frame, the droplets and their ring.
ID3D11Texture2D* g_surface = nullptr;
ID3D11ShaderResourceView* g_surfaceSrv = nullptr;
ID3D11UnorderedAccessView* g_surfaceUav = nullptr;
ID3D11Texture2D* g_impact = nullptr;
ID3D11ShaderResourceView* g_impactSrv = nullptr;
ID3D11UnorderedAccessView* g_impactUav = nullptr;
ID3D11Buffer* g_drops = nullptr;
ID3D11ShaderResourceView* g_dropsSrv = nullptr;
ID3D11UnorderedAccessView* g_dropsUav = nullptr;
ID3D11Buffer* g_ring = nullptr;
ID3D11UnorderedAccessView* g_ringUav = nullptr;
// Wetness: the water's height seen this frame, the kept height and wet line, droplets' spots; and its full screen
// pass, which adds one output and multiplies by the other, with no depth test.
struct Map {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
};
Map g_level, g_wet, g_spots;
// The floor under each texel and the repeat of the world it is of (the Floor shader): never cleared with the map, the
// floor of the world does not move when the timeline starts again.
Map g_floor, g_floorTile;
// What has stood in each texel lately (the Top shader), -1 never seen: kept like the floor.
Map g_top;
ID3D11VertexShader* g_fullVs = nullptr;
ID3D11PixelShader* g_wetPs = nullptr;
ID3D11BlendState* g_wetBlend = nullptr;
ID3D11DepthStencilState* g_noDepth = nullptr;
// Drawing them: the shaders, an alpha blend into the colour alone, no culling, the engine's depth
// test without writing, and the frame copy they were last drawn with.
ID3D11VertexShader* g_dropVs = nullptr;
ID3D11PixelShader* g_dropPs = nullptr;
ID3D11BlendState* g_dropBlend = nullptr;
ID3D11RasterizerState* g_dropRaster = nullptr;
ID3D11SamplerState* g_dropSampler = nullptr;
ID3D11DepthStencilState* g_dropDss = nullptr;
uint64_t g_drawnGen = 0;
// Counts of the last detect pass (the water's pixels, those in contact), read back for the log every ten seconds.
ID3D11Buffer* g_count = nullptr;
ID3D11UnorderedAccessView* g_countUav = nullptr;
ID3D11Buffer* g_countRead = nullptr;
bool g_countPending = false;
UINT g_lastCounts[12] = {};
unsigned long long g_splashCounts[7] = {};
UINT g_ringFirst = 0;   // the ring's total when first read   // the first steps' counts (g_lastCounts 4..7) added up as read
// The engine's depth state for the water and ours made from it with the depth written, for the redraw below.
ID3D11DepthStencilState* g_engineDss = nullptr;
UINT g_engineStencilRef = 0;
ID3D11DepthStencilState* g_writeDss = nullptr;
ID3D11DepthStencilState* g_writeFrom = nullptr;
// The water's depth: the scene's copied into a depth of ours, and the water drawn again into it, depth only.
ID3D11Texture2D* g_wd = nullptr;
ID3D11DepthStencilView* g_wdDsv = nullptr;
ID3D11ShaderResourceView* g_wdSrv = nullptr;
D3D11_TEXTURE2D_DESC g_wdDesc = {};
ID3D11Buffer* g_cb = nullptr;
bool g_broken = false;
ID3D11Texture2D* g_state[2] = {};
ID3D11ShaderResourceView* g_stateSrv[2] = {};
ID3D11UnorderedAccessView* g_stateUav[2] = {};
ID3D11Texture2D* g_contact[2] = {};
ID3D11ShaderResourceView* g_contactSrv[2] = {};
ID3D11UnorderedAccessView* g_contactUav[2] = {};
int g_cur = 0;          // the state the water reads
int g_contactNow = 0;   // the contact map this frame writes
// The view of the water draw about to be made.
Consts g_view = {};
float g_time = 0.0f;
bool g_settle = true;     // the next contacts found are taken as they are: no push, no splash
bool g_hadWater = false;  // the last step found what touches a water
bool g_viewOk = false;
float g_lastTime = -1.0f;
long long g_steps = 0, g_resets = 0;
bool g_wetDebug = false;   // "wetdebug": the wetness pass shows its maps instead
// The frame's scene, noted at its opaque draws, for the wetness when no water is in the view (a wet model the camera
// turned to, the water behind it): the last opaque draw's PerView, viewport and targets before the passes after the
// scene. Whether the wetness was drawn this frame already (after a water), and the frames stepped without one.
// The frame's opaque draws by their camera: the 3D skybox is drawn through a camera of its own (a scaled down world),
// after the world's, so the last draw's camera can be the skybox's; the scene's is the one with the most draws.
struct NoteView {
    float pv[204];
    D3D11_VIEWPORT viewport;
    bool hasViewport;
    int draws;
};
NoteView g_noteViews[4];
int g_noteViewCount = 0;
float g_notePv[204];
D3D11_VIEWPORT g_noteViewport = {};
bool g_noteHasViewport = false;
UINT g_noteW = 0, g_noteH = 0;
bool g_noted = false;
uint32_t g_noteBindGen = ~0u;
bool g_noteSkip = false;   // the targets bound are not the scene's (another tool's window)
ID3D11RenderTargetView* g_noteRtv = nullptr;
ID3D11Texture2D* g_noteDepth = nullptr;
bool g_wetThisFrame = false;
long long g_dryFrames = 0, g_lateDraws = 0;
bool g_noEnd = false;   // "noend": no wetness at the frame's end
unsigned g_wetOff = 0;  // "wetnomirror" 1, "wetnoaround" 2, "wetnoshine" 4: parts of the wet look left out
int g_trace = 0;        // "wettrace": the next events of the wetness's frame logged one by one
// The views waters were drawn in over the last ten seconds, for the log. A view is its target's size, its viewport and
// its depth range, not where its camera is (one camera moving is one view): where its camera was first and last, how
// many draws, how many of them stepped the map (only one view should).
struct WaterKey {
    UINT rtW, rtH;
    float viewport[4];
    float range[2];
};
bool SameKey(const WaterKey& a, const WaterKey& b) { return !memcmp(&a, &b, sizeof(a)); }
struct WaterView {
    WaterKey key;
    float first[3], last[3];
    int draws, steps;
};
std::vector<WaterView> g_waterViews;
int g_waterView = -1;   // the one of the draw about to be made

}  // namespace

bool Inverse4x4(const float* m, float* out) {
    double inv[16], a[16];
    for (int i = 0; i < 16; ++i) a[i] = m[i];
    inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-30) return false;
    for (int i = 0; i < 16; ++i) out[i] = (float)(inv[i] / det);
    return true;
}

namespace {

template <class T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }

// No water seen anywhere yet, nothing wet.
void ClearWetness(ID3D11DeviceContext* ctx) {
    const float none[4] = { -1e9f, -1e9f, -1e9f, -1e9f };
    const float zero[4] = {};
    ctx->ClearUnorderedAccessViewFloat(g_level.uav, none);
    ctx->ClearUnorderedAccessViewFloat(g_wet.uav, none);
    ctx->ClearUnorderedAccessViewFloat(g_spots.uav, zero);
}

bool Ensure(ID3D11DeviceContext* ctx) {
    if (g_broken) return false;
    if (g_detect && g_detectMs && g_step && g_fly && g_keep && g_floorCs && g_floorCsMs && g_topCs && g_dropPs && g_wetPs
        && g_cb && g_state[1] && g_contact[1] && g_ringUav && g_dropSampler && g_spots.uav && g_floorTile.uav && g_top.uav
        && g_noDepth)
        return true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    const char* entries[12] = { "Detect", "Detect", "Step", "Fly", "Vs", "Ps", "FullVs", "WetPs", "Keep", "Floor", "Floor",
                                "Top" };
    const char* targets[12] = { "cs_5_0", "cs_5_0", "cs_5_0", "cs_5_0", "vs_5_0", "ps_5_0", "vs_5_0", "ps_5_0", "cs_5_0",
                                "cs_5_0", "cs_5_0", "cs_5_0" };
    char drops[16];
    sprintf_s(drops, "%u", kDrops);
    // The wet line: WET_BAND units above the water, sinking WET_DRY units a second once out; spots dry in WET_SPOT s.
    // A contact is something FLOOR_MARGIN units or more above the floor under it (the depth's own unevenness below).
    // What has stood in a texel lately sinks TOP_FALL contact a second (a full one in two seconds) while seen empty; a
    // contact pushes the water where it rises RISE_MIN above it (0.1 of the band, 0.8 units).
    // Wading kicks a droplet up off a texel whose contact rises, at most WADE of the time (at a rise of half the band).
    const D3D_SHADER_MACRO plain[] = { { "DROPS", drops }, { "WET_BAND", "4.0" }, { "WET_DRY", "0.5" }, { "WET_SPOT", "20.0" },
                                       { "FLOOR_MARGIN", "1.5" }, { "TOP_FALL", "0.5" }, { "RISE_MIN", "0.1" }, { "WADE", "0.35" },
                                       { nullptr, nullptr } };
    const D3D_SHADER_MACRO ms[] = { { "DROPS", drops }, { "WET_BAND", "4.0" }, { "WET_DRY", "0.5" }, { "WET_SPOT", "20.0" },
                                    { "FLOOR_MARGIN", "1.5" }, { "TOP_FALL", "0.5" }, { "RISE_MIN", "0.1" }, { "WADE", "0.35" },
                                    { "POST_MS", "1" }, { nullptr, nullptr } };
    for (int i = 0; i < 12 && !g_broken; ++i) {
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        const bool cs = i < 4 || i >= 8;
        const char* src = cs ? kCs : kDraw;
        const size_t len = cs ? sizeof(kCs) - 1 : sizeof(kDraw) - 1;
        HRESULT hr = D3DCompile(src, len, "water_fx", i == 1 || i == 10 ? ms : plain, nullptr, entries[i], targets[i], 0, 0,
                                &code, &err);
        if (SUCCEEDED(hr) && code) {
            const void* b = code->GetBufferPointer();
            const SIZE_T n = code->GetBufferSize();
            if (i == 0) hr = dev->CreateComputeShader(b, n, nullptr, &g_detect);
            else if (i == 1) hr = dev->CreateComputeShader(b, n, nullptr, &g_detectMs);
            else if (i == 2) hr = dev->CreateComputeShader(b, n, nullptr, &g_step);
            else if (i == 3) hr = dev->CreateComputeShader(b, n, nullptr, &g_fly);
            else if (i == 4) hr = dev->CreateVertexShader(b, n, nullptr, &g_dropVs);
            else if (i == 5) hr = dev->CreatePixelShader(b, n, nullptr, &g_dropPs);
            else if (i == 6) hr = dev->CreateVertexShader(b, n, nullptr, &g_fullVs);
            else if (i == 7) hr = dev->CreatePixelShader(b, n, nullptr, &g_wetPs);
            else if (i == 8) hr = dev->CreateComputeShader(b, n, nullptr, &g_keep);
            else if (i == 9) hr = dev->CreateComputeShader(b, n, nullptr, &g_floorCs);
            else if (i == 10) hr = dev->CreateComputeShader(b, n, nullptr, &g_floorCsMs);
            else hr = dev->CreateComputeShader(b, n, nullptr, &g_topCs);
        }
        if (FAILED(hr) || !code) {
            Log("[WATER] the %s shader could not be made: %s\n", entries[i], err ? (const char*)err->GetBufferPointer() : "");
            g_broken = true;
        }
        if (code) code->Release();
        if (err) err->Release();
    }
    if (!g_broken) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = d.Height = kSize;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_surface)) || FAILED(dev->CreateShaderResourceView(g_surface, nullptr, &g_surfaceSrv))
            || FAILED(dev->CreateUnorderedAccessView(g_surface, nullptr, &g_surfaceUav))) g_broken = true;
        d.Format = DXGI_FORMAT_R32_UINT;
        if (!g_broken && (FAILED(dev->CreateTexture2D(&d, nullptr, &g_impact))
                          || FAILED(dev->CreateShaderResourceView(g_impact, nullptr, &g_impactSrv))
                          || FAILED(dev->CreateUnorderedAccessView(g_impact, nullptr, &g_impactUav)))) g_broken = true;
        D3D11_BUFFER_DESC b = {};
        b.ByteWidth = kDrops * 32;
        b.Usage = D3D11_USAGE_DEFAULT;
        b.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        b.StructureByteStride = 32;
        if (!g_broken && (FAILED(dev->CreateBuffer(&b, nullptr, &g_drops))
                          || FAILED(dev->CreateShaderResourceView(g_drops, nullptr, &g_dropsSrv))
                          || FAILED(dev->CreateUnorderedAccessView(g_drops, nullptr, &g_dropsUav)))) g_broken = true;
        D3D11_BUFFER_DESC rb = {};
        rb.ByteWidth = 16;
        rb.Usage = D3D11_USAGE_DEFAULT;
        rb.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ru = {};
        ru.Format = DXGI_FORMAT_R32_UINT;
        ru.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ru.Buffer.NumElements = 4;
        if (!g_broken && (FAILED(dev->CreateBuffer(&rb, nullptr, &g_ring)) || FAILED(dev->CreateUnorderedAccessView(g_ring, &ru, &g_ringUav))))
            g_broken = true;
        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN
                                                   | D3D11_COLOR_WRITE_ENABLE_BLUE;
        D3D11_RASTERIZER_DESC rs = {};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        rs.MultisampleEnable = TRUE;
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (!g_broken && (FAILED(dev->CreateBlendState(&bl, &g_dropBlend)) || FAILED(dev->CreateRasterizerState(&rs, &g_dropRaster))
                          || FAILED(dev->CreateSamplerState(&sd, &g_dropSampler)))) g_broken = true;
        D3D11_BLEND_DESC wb = {};
        wb.RenderTarget[0].BlendEnable = TRUE;
        wb.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        wb.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
        wb.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        wb.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        wb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        wb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        wb.RenderTarget[0].RenderTargetWriteMask = bl.RenderTarget[0].RenderTargetWriteMask;
        D3D11_DEPTH_STENCIL_DESC nd = {};
        if (!g_broken && (FAILED(dev->CreateBlendState(&wb, &g_wetBlend)) || FAILED(dev->CreateDepthStencilState(&nd, &g_noDepth))))
            g_broken = true;
        // Each with the tile of the world's repeat its value is of (zw); the floor's tile in a map of its own.
        const DXGI_FORMAT formats[6] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                         DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT,
                                         DXGI_FORMAT_R32_FLOAT };
        Map* maps[6] = { &g_level, &g_wet, &g_spots, &g_floor, &g_floorTile, &g_top };
        for (int i = 0; i < 6 && !g_broken; ++i) {
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = d.Height = kSize;
            d.MipLevels = d.ArraySize = 1;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            d.Format = formats[i];
            if (FAILED(dev->CreateTexture2D(&d, nullptr, &maps[i]->tex)) || FAILED(dev->CreateShaderResourceView(maps[i]->tex, nullptr, &maps[i]->srv))
                || FAILED(dev->CreateUnorderedAccessView(maps[i]->tex, nullptr, &maps[i]->uav))) g_broken = true;
        }
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Consts);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (!g_broken && FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb))) g_broken = true;
    for (int i = 0; i < 2 && !g_broken; ++i) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = d.Height = kSize;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_state[i])) || FAILED(dev->CreateShaderResourceView(g_state[i], nullptr, &g_stateSrv[i]))
            || FAILED(dev->CreateUnorderedAccessView(g_state[i], nullptr, &g_stateUav[i]))) g_broken = true;
        d.Format = DXGI_FORMAT_R32_UINT;
        if (!g_broken && (FAILED(dev->CreateTexture2D(&d, nullptr, &g_contact[i]))
                          || FAILED(dev->CreateShaderResourceView(g_contact[i], nullptr, &g_contactSrv[i]))
                          || FAILED(dev->CreateUnorderedAccessView(g_contact[i], nullptr, &g_contactUav[i])))) g_broken = true;
    }
    dev->Release();
    if (g_broken) {
        Log("[WATER] the water's effects map could not be made\n");
        return false;
    }
    const float zero[4] = {};
    const UINT zeroU[4] = {};
    for (int i = 0; i < 2; ++i) {
        ctx->ClearUnorderedAccessViewFloat(g_stateUav[i], zero);
        ctx->ClearUnorderedAccessViewUint(g_contactUav[i], zeroU);
    }
    ctx->ClearUnorderedAccessViewFloat(g_surfaceUav, zero);
    ctx->ClearUnorderedAccessViewUint(g_impactUav, zeroU);
    ctx->ClearUnorderedAccessViewUint(g_dropsUav, zeroU);
    ctx->ClearUnorderedAccessViewUint(g_ringUav, zeroU);
    ctx->ClearUnorderedAccessViewUint(g_floor.uav, zeroU);
    ctx->ClearUnorderedAccessViewUint(g_floorTile.uav, zeroU);
    const float never[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    ctx->ClearUnorderedAccessViewFloat(g_top.uav, never);
    ClearWetness(ctx);
    Log("[WATER] the water's effects map is made: %u x %u texels over %.0f units, repeating\n", kSize, kSize, kArea);
    return true;
}

}  // namespace

void WaterFxDebug(bool on) { g_wetDebug = on; }

void WaterFxNoEnd(bool off) { g_noEnd = off; }

void WaterFxLookOff(unsigned parts) { g_wetOff = parts; }

void WaterFxTrace(bool arm) {
    static bool armed = false;
    if (arm && !armed) g_trace = 40;
    armed = arm;
}

bool WaterFxTracing() {
    if (g_trace <= 0) return false;
    --g_trace;   // one of the lines asked for
    return true;
}

namespace {

// A view for the shaders here from the engine's PerView floats, the viewport drawn through (null: the whole target)
// and the target's size; and its time. False when it has none to go by.
bool ViewOf(const float* pv, const D3D11_VIEWPORT* vp, UINT rtW, UINT rtH, Consts* view, float* time) {
    float m[16];
    if (!rtW || !rtH || !Inverse4x4(&pv[PV_MAT_WORLD_TO_PROJ_MV], m)) return false;
    float eye[3];
    for (int c = 0; c < 3; ++c) eye[c] = pv[PV_CAMERA_POSITION_WS + c] + pv[PV_WORLD_TO_CAM_OFF_MV + c];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) m[r * 4 + c] -= m[r * 4 + 3] * eye[c];
    memcpy(view->projToRel, m, sizeof(m));
    memcpy(view->worldToProj, &pv[PV_MAT_WORLD_TO_PROJ_MV], sizeof(view->worldToProj));
    memcpy(view->camOff, &pv[PV_WORLD_TO_CAM_OFF_MV], sizeof(view->camOff));
    view->viewport[0] = vp ? vp->TopLeftX : 0.0f;
    view->viewport[1] = vp ? vp->TopLeftY : 0.0f;
    view->viewport[2] = vp ? vp->Width : (float)rtW;
    view->viewport[3] = vp ? vp->Height : (float)rtH;
    view->range[0] = vp ? vp->MinDepth : 0.0f;
    view->range[1] = vp ? vp->MaxDepth : 1.0f;
    view->range[2] = (float)rtW;
    view->range[3] = (float)rtH;
    for (int c = 0; c < 3; ++c) view->camera[c] = pv[PV_CAMERA_POSITION_WS + c];
    view->camera[3] = kBand;
    *time = pv[PV_TIME];
    return std::isfinite(*time);
}

WaterKey KeyOf(const Consts& v) {
    WaterKey k = {};
    k.rtW = (UINT)v.range[2];
    k.rtH = (UINT)v.range[3];
    memcpy(k.viewport, v.viewport, sizeof(k.viewport));
    k.range[0] = v.range[0];
    k.range[1] = v.range[1];
    return k;
}

}  // namespace

ID3D11ShaderResourceView* WaterFxBefore(ID3D11DeviceContext* ctx, ContextState* st) {
    g_viewOk = false;
    if (!Ensure(ctx)) return nullptr;
    ctx->OMGetDepthStencilState(&g_engineDss, &g_engineStencilRef);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    if (g_engineDss) g_engineDss->GetDesc(&dd);
    else {
        dd.DepthEnable = TRUE;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
    }
    if (!g_writeDss || g_writeFrom != g_engineDss) {
        static bool said = false;
        if (!said) {
            said = true;
            Log("[WATER] the engine draws the water with depth test %d, write %d, function %d, on a read-only depth view; "
                "it is drawn again into a depth of ours for its effects\n",
                (int)dd.DepthEnable, (int)dd.DepthWriteMask, (int)dd.DepthFunc);
        }
        if (g_writeDss) { g_writeDss->Release(); g_writeDss = nullptr; }
        if (g_dropDss) { g_dropDss->Release(); g_dropDss = nullptr; }
        D3D11_DEPTH_STENCIL_DESC w = dd;
        w.DepthEnable = TRUE;
        w.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        // The droplets: the engine's depth test, nothing written, no stencil.
        D3D11_DEPTH_STENCIL_DESC t = {};
        t.DepthEnable = TRUE;
        t.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        t.DepthFunc = dd.DepthFunc;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            dev->CreateDepthStencilState(&w, &g_writeDss);
            dev->CreateDepthStencilState(&t, &g_dropDss);
            dev->Release();
        }
        g_writeFrom = g_engineDss;
    }
    float pv[204];
    D3D11_VIEWPORT vp[1] = {};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, vp);
    if (CorePerView(st, pv) && ViewOf(pv, nvp ? &vp[0] : nullptr, st->rtW, st->rtH, &g_view, &g_time)) {
        g_viewOk = true;
        WaterCarryWaterSeen(st->rtW, st->rtH);
        const WaterKey key = KeyOf(g_view);
        g_waterView = -1;
        for (size_t i = 0; i < g_waterViews.size() && g_waterView < 0; ++i)
            if (SameKey(g_waterViews[i].key, key)) g_waterView = (int)i;
        if (g_waterView < 0 && g_waterViews.size() < 8) {
            WaterView w = {};
            w.key = key;
            memcpy(w.first, g_view.camera, sizeof(w.first));
            g_waterViews.push_back(w);
            g_waterView = (int)g_waterViews.size() - 1;
        }
        if (g_waterView >= 0) {
            WaterView& w = g_waterViews[g_waterView];
            memcpy(w.last, g_view.camera, sizeof(w.last));
            ++w.draws;
        }
    }
    return g_stateSrv[g_cur];
}

namespace {

// The water just drawn, drawn again into a depth of ours that starts as a copy of the scene's (the engine binds its
// own read-only for see-through surfaces): depth only, no pixel shader, with the engine's depth test and the depth
// written. Its view, or null.
ID3D11ShaderResourceView* WaterDepth(ID3D11DeviceContext* ctx, const ContextState* st) {
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    ID3D11Texture2D* tex = nullptr;
    if (dsv) {
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        if (res) { res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex); res->Release(); }
    }
    ID3D11ShaderResourceView* out = nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    if (tex) tex->GetDesc(&d);
    const bool fits = tex && g_writeDss && (d.Format == DXGI_FORMAT_R24G8_TYPELESS || d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT)
                      && d.ArraySize == 1 && d.MipLevels == 1;
    if (fits && (!g_wd || g_wdDesc.Width != d.Width || g_wdDesc.Height != d.Height
                 || g_wdDesc.SampleDesc.Count != d.SampleDesc.Count || g_wdDesc.SampleDesc.Quality != d.SampleDesc.Quality)) {
        if (g_wdSrv) { g_wdSrv->Release(); g_wdSrv = nullptr; }
        if (g_wdDsv) { g_wdDsv->Release(); g_wdDsv = nullptr; }
        if (g_wd) { g_wd->Release(); g_wd = nullptr; }
        D3D11_TEXTURE2D_DESC c = d;
        c.Format = DXGI_FORMAT_R24G8_TYPELESS;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        c.CPUAccessFlags = 0;
        c.MiscFlags = 0;
        const bool ms = d.SampleDesc.Count > 1;
        D3D11_DEPTH_STENCIL_VIEW_DESC vd = {};
        vd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        vd.ViewDimension = ms ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sd.ViewDimension = ms ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev || FAILED(dev->CreateTexture2D(&c, nullptr, &g_wd)) || FAILED(dev->CreateDepthStencilView(g_wd, &vd, &g_wdDsv))
            || FAILED(dev->CreateShaderResourceView(g_wd, &sd, &g_wdSrv))) {
            if (g_wdSrv) { g_wdSrv->Release(); g_wdSrv = nullptr; }
            if (g_wdDsv) { g_wdDsv->Release(); g_wdDsv = nullptr; }
            if (g_wd) { g_wd->Release(); g_wd = nullptr; }
            Log("[WATER] a depth for the water could not be made (%ux%u, %u samples)\n", d.Width, d.Height, d.SampleDesc.Count);
        } else {
            g_wdDesc = d;
        }
        if (dev) dev->Release();
    }
    if (fits && g_wd) {
        ctx->CopyResource(g_wd, tex);
        ID3D11PixelShader* ps = nullptr;
        ID3D11ClassInstance* psCi[256];
        UINT psCiN = 256;
        ctx->PSGetShader(&ps, psCi, &psCiN);
        ID3D11DepthStencilState* dss = nullptr;
        UINT ref = 0;
        ctx->OMGetDepthStencilState(&dss, &ref);
        ctx->OMSetRenderTargets(0, nullptr, g_wdDsv);
        ctx->OMSetDepthStencilState(g_writeDss, ref);
        ctx->PSSetShader(nullptr, nullptr, 0);
        if (CoreRedraw(ctx, st)) out = g_wdSrv;
        ctx->PSSetShader(ps, psCi, psCiN);
        ctx->OMSetDepthStencilState(dss, ref);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        if (ps) ps->Release();
        for (UINT i = 0; i < psCiN; ++i) if (psCi[i]) psCi[i]->Release();
        if (dss) dss->Release();
    }
    if (tex) tex->Release();
    if (dsv) dsv->Release();
    for (auto* r : rtvs) if (r) r->Release();
    return out;
}

// The wetness and the droplets drawn into the engine's target, over the water just drawn, once for each frame copy (a
// second water in the same view draws them not again); everything the draws change is put back.
void DrawAfterWater(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* copy, uint64_t copyGen,
                    ID3D11ShaderResourceView* sceneDepth) {
    if (!copy || copyGen == g_drawnGen || !g_dropDss || g_broken) return;
    g_drawnGen = copyGen;
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    // The map's area and size for the wetness (the view holds neither); the frame seconds, not read here, say
    // whether the test view is on.
    Consts c = g_view;
    c.map[0] = kArea;
    c.map[1] = (float)kSize;
    c.splash[0] = g_wetDebug ? 1.0f : 0.0f;
    c.splash[2] = (float)g_wetOff;   // the test words' switches of the wet look
    c.splash[3] = kDropRadius;
    ctx->UpdateSubresource(g_cb, 0, nullptr, &c, 0, 0);

    D3D11_PRIMITIVE_TOPOLOGY topology;
    ctx->IAGetPrimitiveTopology(&topology);
    ID3D11InputLayout* layout = nullptr;
    ctx->IAGetInputLayout(&layout);
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    ID3D11GeometryShader* gs = nullptr;
    ctx->GSGetShader(&gs, nullptr, nullptr);
    ID3D11HullShader* hs = nullptr;
    ctx->HSGetShader(&hs, nullptr, nullptr);
    ID3D11DomainShader* ds = nullptr;
    ctx->DSGetShader(&ds, nullptr, nullptr);
    ID3D11PixelShader* ps = nullptr;
    ID3D11ClassInstance* psCi[256];
    UINT psCiN = 256;
    ctx->PSGetShader(&ps, psCi, &psCiN);
    ID3D11ShaderResourceView* vsSrv = nullptr;
    ctx->VSGetShaderResources(0, 1, &vsSrv);
    ID3D11Buffer* vsCb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &vsCb);
    ID3D11ShaderResourceView* psSrv[7] = {};
    ctx->PSGetShaderResources(0, 7, psSrv);
    ID3D11Buffer* psCb = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &psCb);
    ID3D11SamplerState* psSampler = nullptr;
    ctx->PSGetSamplers(0, 1, &psSampler);
    ID3D11RasterizerState* rs = nullptr;
    ctx->RSGetState(&rs);
    ID3D11BlendState* blend = nullptr;
    float factor[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&blend, factor, &mask);
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&dss, &ref);

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g_dropVs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_dropPs, nullptr, 0);
    ctx->VSSetShaderResources(0, 1, &g_dropsSrv);
    ctx->VSSetConstantBuffers(0, 1, &g_cb);
    ID3D11ShaderResourceView* in[2] = { nullptr, copy };
    ctx->PSSetShaderResources(0, 2, in);
    ctx->PSSetSamplers(0, 1, &g_dropSampler);
    ctx->RSSetState(g_dropRaster);
    const float noFactor[4] = {};
    ctx->OMSetBlendState(g_dropBlend, noFactor, 0xffffffff);
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    if (sceneDepth && rtvs[0]) {
        ID3D11ShaderResourceView* carried = nullptr;
        ID3D11ShaderResourceView* carriedNormal = nullptr;
        WaterCarryScreen(ctx, sceneDepth, &carried, &carriedNormal);
        ctx->OMSetRenderTargets(1, rtvs, dsv);
        ctx->VSSetShader(g_fullVs, nullptr, 0);
        ctx->PSSetShader(g_wetPs, nullptr, 0);
        ID3D11ShaderResourceView* wetIn[7] = { nullptr, copy, sceneDepth, g_wet.srv, g_spots.srv, carried, carriedNormal };
        ctx->PSSetShaderResources(0, 7, wetIn);
        ctx->PSSetConstantBuffers(0, 1, &g_cb);
        ctx->OMSetBlendState(g_wetBlend, noFactor, 0xffffffff);
        ctx->OMSetDepthStencilState(g_noDepth, 0);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* noIn[7] = {};
        ctx->PSSetShaderResources(0, 7, noIn);
        ctx->PSSetShaderResources(0, 2, in);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        ctx->VSSetShader(g_dropVs, nullptr, 0);
        ctx->PSSetShader(g_dropPs, nullptr, 0);
        ctx->OMSetBlendState(g_dropBlend, noFactor, 0xffffffff);
    }
    for (auto* r : rtvs) if (r) r->Release();
    if (dsv) dsv->Release();
    ctx->OMSetDepthStencilState(g_dropDss, 0);
    ctx->DrawInstanced(6, kDrops, 0, 0);

    ctx->IASetPrimitiveTopology(topology);
    ctx->IASetInputLayout(layout);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->GSSetShader(gs, nullptr, 0);
    ctx->HSSetShader(hs, nullptr, 0);
    ctx->DSSetShader(ds, nullptr, 0);
    ctx->PSSetShader(ps, psCi, psCiN);
    ctx->VSSetShaderResources(0, 1, &vsSrv);
    ctx->VSSetConstantBuffers(0, 1, &vsCb);
    ctx->PSSetShaderResources(0, 7, psSrv);
    ctx->PSSetConstantBuffers(0, 1, &psCb);
    if (psCb) psCb->Release();
    ctx->PSSetSamplers(0, 1, &psSampler);
    ctx->RSSetState(rs);
    ctx->OMSetBlendState(blend, factor, mask);
    ctx->OMSetDepthStencilState(dss, ref);
    if (layout) layout->Release();
    if (vs) vs->Release();
    if (gs) gs->Release();
    if (hs) hs->Release();
    if (ds) ds->Release();
    if (ps) ps->Release();
    for (UINT i = 0; i < psCiN; ++i) if (psCi[i]) psCi[i]->Release();
    if (vsSrv) vsSrv->Release();
    if (vsCb) vsCb->Release();
    for (auto* s : psSrv) if (s) s->Release();
    if (psSampler) psSampler->Release();
    if (rs) rs->Release();
    if (blend) blend->Release();
    if (dss) dss->Release();
    t_CorePassThrough = pass;
}

}  // namespace

namespace {

// The map's step for the time moved since the last (none when the same moment is drawn again; a fresh start, after
// which there is nothing to draw, when the time went back or jumped). With a water's draw (waterSt) what touches it is
// found first; without one (no water in the view this frame) nothing touches it, and it all calms and dries. g_view is
// the view and g_time the time. False when there is nothing to draw.
bool Advance(ID3D11DeviceContext* ctx, const ContextState* waterSt, ID3D11ShaderResourceView* sceneDepth) {
    float dt = g_lastTime < 0.0f ? 0.0f : g_time - g_lastTime;
    // The same moment drawn again: no step. S2FM's refinement draws a frame's samples over the shutter (its motion
    // blur), the time sweeping forward a few ten thousandths of a second at a time across about a hundredth and back;
    // each sample stepped as a moment of its own made the models' small moves contacts new to the water, thrown with
    // the speed of their tiny time, and each sweep back started the map afresh.
    if (g_lastTime >= 0.0f && std::fabs(dt) < kSameMoment) return true;
    const bool reset = g_lastTime < 0.0f || dt < 0.0f || dt > 2.0f;
    g_lastTime = g_time;
    if (reset) {
        const float zero[4] = {};
        const UINT zeroU[4] = {};
        for (int i = 0; i < 2; ++i) {
            ctx->ClearUnorderedAccessViewFloat(g_stateUav[i], zero);
            ctx->ClearUnorderedAccessViewUint(g_contactUav[i], zeroU);
        }
        ctx->ClearUnorderedAccessViewUint(g_impactUav, zeroU);
        ctx->ClearUnorderedAccessViewUint(g_dropsUav, zeroU);
        ClearWetness(ctx);
        WaterCarryClear(ctx);
        g_settle = true;
        ++g_resets;
        return false;
    }
    // Our own calls: the core passes them straight through to the card.
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    ID3D11ShaderResourceView* water = waterSt ? WaterDepth(ctx, waterSt) : nullptr;
    if (waterSt && !water) { t_CorePassThrough = pass; return false; }
    const int steps = (std::min)(8, (std::max)(1, (int)std::lround(dt * 60.0f)));
    if (water && g_waterView >= 0 && g_waterView < (int)g_waterViews.size()) ++g_waterViews[g_waterView].steps;
    if (!water) ++g_dryFrames;

    // What the engine had in the compute stage, put back after.
    ID3D11ComputeShader* oldCs = nullptr;
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv[8] = {};
    ctx->CSGetShaderResources(0, 8, oldSrv);
    ID3D11UnorderedAccessView* oldUav[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
    ctx->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, oldUav);
    ID3D11Buffer* oldCb = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &oldCb);
    ID3D11ShaderResourceView* none[8] = {};
    ID3D11UnorderedAccessView* noUavs[6] = {};

    // Contacts of this frame.
    const UINT zeroU[4] = {};
    ctx->ClearUnorderedAccessViewUint(g_contactUav[g_contactNow], zeroU);
    Consts c = g_view;
    c.map[0] = kArea;
    c.map[1] = (float)kSize;
    c.map[2] = dt / (float)steps;
    c.splash[0] = dt;
    c.splash[1] = g_time;
    c.splash[2] = kSplashSpeed;
    c.splash[3] = kDropRadius;
    ctx->UpdateSubresource(g_cb, 0, nullptr, &c, 0, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    const float none4[4] = { -1e9f, -1e9f, -1e9f, -1e9f };
    ctx->ClearUnorderedAccessViewFloat(g_level.uav, none4);
    // What touches the water drawn this frame (none without one).
    if (water) {
        ID3D11ShaderResourceView* depths[2] = { sceneDepth, water };
        ctx->CSSetShaderResources(0, 2, depths);
        if (!g_count) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = sizeof(g_lastCounts);
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = DXGI_FORMAT_R32_UINT;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = sizeof(g_lastCounts) / sizeof(UINT);
            D3D11_BUFFER_DESC rd = bd;
            rd.Usage = D3D11_USAGE_STAGING;
            rd.BindFlags = 0;
            rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (dev && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_count))) {
                dev->CreateUnorderedAccessView(g_count, &ud, &g_countUav);
                dev->CreateBuffer(&rd, nullptr, &g_countRead);
            }
            if (dev) dev->Release();
        }
        if (g_countUav) ctx->ClearUnorderedAccessViewUint(g_countUav, zeroU);
        // The floor first, with what this frame sees; the detection then reads it.
        const bool ms = g_wdDesc.SampleDesc.Count > 1;
        ID3D11UnorderedAccessView* floorUavs[2] = { g_floor.uav, g_floorTile.uav };
        ctx->CSSetUnorderedAccessViews(0, 2, floorUavs, nullptr);
        ctx->CSSetShader(ms ? g_floorCsMs : g_floorCs, nullptr, 0);
        ctx->Dispatch(((UINT)c.range[2] + 7) / 8, ((UINT)c.range[3] + 7) / 8, 1);
        ctx->CSSetUnorderedAccessViews(0, 2, noUavs, nullptr);
        ID3D11ShaderResourceView* floors[2] = { g_floor.srv, g_floorTile.srv };
        ctx->CSSetShaderResources(2, 2, floors);
        ID3D11UnorderedAccessView* detectUavs[4] = { g_contactUav[g_contactNow], g_countUav, g_surfaceUav, g_level.uav };
        ctx->CSSetUnorderedAccessViews(0, 4, detectUavs, nullptr);
        ctx->CSSetShader(ms ? g_detectMs : g_detect, nullptr, 0);
        ctx->Dispatch(((UINT)c.range[2] + 7) / 8, ((UINT)c.range[3] + 7) / 8, 1);
        ctx->CSSetUnorderedAccessViews(0, 5, noUavs, nullptr);
        ctx->CSSetShaderResources(0, 6, none);
    }
    // What was not seen keeps the frame before's contact (all of it in a frame without the water).
    {
        ID3D11ShaderResourceView* before[3] = { nullptr, nullptr, g_contactSrv[1 - g_contactNow] };
        ctx->CSSetShaderResources(0, 3, before);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_contactUav[g_contactNow], nullptr);
        ctx->CSSetShader(g_keep, nullptr, 0);
        ctx->Dispatch(kSize / 8, kSize / 8, 1);
        ctx->CSSetUnorderedAccessViews(0, 5, noUavs, nullptr);
        ctx->CSSetShaderResources(0, 3, none);
    }

    // The steps; the first throws the splashes and takes the droplets fallen back the frame before. Not on the first
    // contacts after a fresh start or after frames without the water: everything standing in it would be new to it,
    // a ring and a burst of droplets round every model (the timeline stopped and started again).
    const bool settle = g_settle || !g_hadWater;
    g_hadWater = water != nullptr;
    if (water) g_settle = false;
    ctx->CSSetShader(g_step, nullptr, 0);
    for (int i = 0; i < steps; ++i) {
        c.map[3] = i == 0 && !settle ? 1.0f : 0.0f;
        ctx->UpdateSubresource(g_cb, 0, nullptr, &c, 0, 0);
        ID3D11ShaderResourceView* in[8] = { g_stateSrv[g_cur], g_contactSrv[g_contactNow], g_contactSrv[1 - g_contactNow],
                                            g_surfaceSrv, g_impactSrv, g_level.srv, nullptr, g_top.srv };
        ctx->CSSetShaderResources(0, 8, in);
        ID3D11UnorderedAccessView* out[6] = { g_stateUav[1 - g_cur], g_dropsUav, g_ringUav, g_wet.uav, g_spots.uav,
                                              water ? g_countUav : nullptr };
        ctx->CSSetUnorderedAccessViews(0, 6, out, nullptr);
        ctx->Dispatch(kSize / 8, kSize / 8, 1);
        ctx->CSSetUnorderedAccessViews(0, 6, noUavs, nullptr);
        ctx->CSSetShaderResources(0, 8, none);
        g_cur = 1 - g_cur;
    }
    // What has stood in each texel lately, from the texels seen this frame (after the steps, which read it as it was).
    {
        ID3D11ShaderResourceView* in[2] = { nullptr, g_contactSrv[g_contactNow] };
        ctx->CSSetShaderResources(0, 2, in);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_top.uav, nullptr);
        ctx->CSSetShader(g_topCs, nullptr, 0);
        ctx->Dispatch(kSize / 8, kSize / 8, 1);
        ctx->CSSetUnorderedAccessViews(0, 1, noUavs, nullptr);
        ctx->CSSetShaderResources(0, 2, none);
    }
    g_contactNow = 1 - g_contactNow;
    g_steps += steps;

    // The droplets fly the frame's time, those falling back counted for the next frame's first step.
    ctx->ClearUnorderedAccessViewUint(g_impactUav, zeroU);
    ctx->CSSetShader(g_fly, nullptr, 0);
    ID3D11UnorderedAccessView* fly[6] = { nullptr, g_dropsUav, g_ringUav, g_impactUav, g_spots.uav,
                                          water ? g_countUav : nullptr };
    ctx->CSSetUnorderedAccessViews(0, 6, fly, nullptr);
    ctx->CSSetShaderResources(0, 1, &sceneDepth);
    ID3D11ShaderResourceView* carriedAt = WaterCarryLastNormal();
    ctx->CSSetShaderResources(6, 1, &carriedAt);
    ctx->Dispatch(kDrops / 64, 1, 1);
    ctx->CSSetUnorderedAccessViews(0, 6, noUavs, nullptr);
    // The detection's counts: copied now and then, read once the card has them (a frame or more later).
    if (water && g_countRead) {
        if (g_countPending) {
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(ctx->Map(g_countRead, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
                memcpy(g_lastCounts, m.pData, sizeof(g_lastCounts));
                for (int k = 0; k < 7; ++k) g_splashCounts[k] += g_lastCounts[4 + k];
                if (!g_ringFirst) g_ringFirst = g_lastCounts[11] ? g_lastCounts[11] : 1;
                ctx->Unmap(g_countRead, 0);
                g_countPending = false;
            }
        } else {
            ctx->CopyResource(g_countRead, g_count);
            g_countPending = true;
        }
    }

    ctx->CSSetShaderResources(0, 7, none);
    // The skinned models' vertices wet or drying by the map just stepped.
    WaterCarryStep(ctx, g_wet.srv, kArea, (float)kSize, dt, g_time, g_dropsUav, g_ringUav);

    const UINT keep[D3D11_PS_CS_UAV_REGISTER_COUNT] = { (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1,
                                                        (UINT)-1, (UINT)-1 };
    ctx->CSSetShader(oldCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 8, oldSrv);
    ctx->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, oldUav, keep);
    ctx->CSSetConstantBuffers(0, 1, &oldCb);
    if (oldCs) oldCs->Release();
    for (auto* s : oldSrv) if (s) s->Release();
    for (auto* u : oldUav) if (u) u->Release();
    if (oldCb) oldCb->Release();
    t_CorePassThrough = pass;

    static ULONGLONG lastSaid = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastSaid > 10000) {
        lastSaid = now;
        Log("[WATER] effects: %lld steps, %lld fresh starts so far, time %.3f s, %lld frames stepped and %lld wetness "
            "passes drawn with no water in view | last count: %u water pixels, %u touching something (of %u pixels, %u "
            "with the depth changed by the draw)\n", g_steps, g_resets, g_time, g_dryFrames, g_lateDraws,
            g_lastCounts[0], g_lastCounts[1], g_lastCounts[2], g_lastCounts[3]);
        Log("[WATER] splashes, in the frames read (about half): %llu texels reached where nothing stood lately, %llu "
            "of them seen the frame before (fresh), %llu splashing, %llu droplets thrown | droplets alive %llu (summed "
            "over frames), %llu fell into the water, %llu struck the scene | droplets thrown in all (splashes and "
            "drips) since first read: %u\n", g_splashCounts[0], g_splashCounts[1], g_splashCounts[2], g_splashCounts[3],
            g_splashCounts[4], g_splashCounts[5], g_splashCounts[6], g_lastCounts[11] - (g_ringFirst ? g_ringFirst : 0));
        for (const WaterView& w : g_waterViews)
            Log("[WATER]   a view of water: target %ux%u, viewport %.0f %.0f %.0fx%.0f depth %.3f-%.3f, camera from %.1f "
                "%.1f %.1f to %.1f %.1f %.1f, %d draws, %d of them stepped the map\n", w.key.rtW, w.key.rtH,
                w.key.viewport[0], w.key.viewport[1], w.key.viewport[2], w.key.viewport[3], w.key.range[0],
                w.key.range[1], w.first[0], w.first[1], w.first[2], w.last[0], w.last[1], w.last[2], w.draws, w.steps);
        g_waterViews.clear();
        g_waterView = -1;
    }
    return true;
}

}  // namespace

void WaterFxAfter(ID3D11DeviceContext* ctx, const ContextState* st, ID3D11ShaderResourceView* sceneDepth,
                  ID3D11ShaderResourceView* copy, uint64_t copyGen) {
    if (g_engineDss) { g_engineDss->Release(); g_engineDss = nullptr; }
    WaterCarryWaterDone();
    if (g_trace > 0) {
        --g_trace;
        Log("[WATER] trace: a water's draw: its view %d, scene depth %d, copy generation %llu (the wetness last drawn "
            "with %llu), time %.4f\n", (int)g_viewOk, sceneDepth ? 1 : 0, (unsigned long long)copyGen,
            (unsigned long long)g_drawnGen, g_time);
    }
    if (!g_viewOk || !sceneDepth || g_broken) return;
    g_viewOk = false;
    // A water drawn this frame: the wetness is its (once for each frame copy), not the frame end's as well.
    g_wetThisFrame = true;
    if (Advance(ctx, st, sceneDepth)) DrawAfterWater(ctx, copy, copyGen, sceneDepth);
}

void WaterFxSceneDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    // Nothing to keep up before a water was first drawn (the maps are made then), and only the scene's opaque draws:
    // into a target of the size the water was drawn into, with a depth of that size too (the shadows' draws write
    // depth with a pixel shader as well, into a 16384 x 16384 atlas: copying that each frame is a second a frame).
    if (g_broken || !g_cb || g_lastTime < 0.0f || !st || st->deferred || !st->ps || !st->depthWrites) return;
    if (st->rtW != (UINT)g_view.range[2] || st->rtH != (UINT)g_view.range[3] || st->dsW != st->rtW || st->dsH != st->rtH)
        return;
    if (st->rtBindGen != g_noteBindGen) {
        g_noteBindGen = st->rtBindGen;
        Drop(g_noteRtv);
        Drop(g_noteDepth);
        g_noteSkip = CoreFrameForeign(st);
        if (!g_noteSkip) {
            ID3D11DepthStencilView* dsv = nullptr;
            ctx->OMGetRenderTargets(1, &g_noteRtv, &dsv);
            if (dsv) {
                ID3D11Resource* res = nullptr;
                dsv->GetResource(&res);
                if (res) { res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&g_noteDepth); res->Release(); }
                dsv->Release();
            }
            D3D11_TEXTURE2D_DESC dd = {};
            if (g_noteDepth) g_noteDepth->GetDesc(&dd);
            if (dd.Width != st->rtW || dd.Height != st->rtH) Drop(g_noteDepth);
            if (!g_noteRtv || !g_noteDepth) {
                Drop(g_noteRtv);
                Drop(g_noteDepth);
                g_noteSkip = true;
            }
        }
    }
    float pv[204];
    if (g_noteSkip || !CorePerView(st, pv)) return;
    // The viewport as well: the sky is drawn through the scene's camera but with its depth range closed up on the far
    // plane, where no point can be found from the depth (a draw with no depth range is not the scene's).
    D3D11_VIEWPORT vp = {};
    UINT n = 1;
    ctx->RSGetViewports(&n, &vp);
    if (n && !(vp.MaxDepth > vp.MinDepth)) return;
    if (!g_noted) g_noteViewCount = 0;
    NoteView* v = nullptr;
    for (int i = 0; i < g_noteViewCount && !v; ++i)
        if (!memcmp(&g_noteViews[i].pv[PV_MAT_WORLD_TO_PROJ_MV], &pv[PV_MAT_WORLD_TO_PROJ_MV], 64)
            && !memcmp(&g_noteViews[i].viewport, &vp, sizeof(vp)) && g_noteViews[i].hasViewport == (n > 0))
            v = &g_noteViews[i];
    if (!v) {
        if (g_noteViewCount == 4) return;
        v = &g_noteViews[g_noteViewCount++];
        v->draws = 0;
    }
    memcpy(v->pv, pv, sizeof(pv));
    v->viewport = vp;
    v->hasViewport = n > 0;
    ++v->draws;
    g_noteW = st->rtW;
    g_noteH = st->rtH;
    g_noted = true;
}

bool WaterFxSceneNoted() { return g_noted; }

bool WaterFxWetPending() { return g_noted && !g_wetThisFrame && g_noteRtv && g_noteDepth && !g_broken && !g_noEnd; }

void WaterFxSceneTargets(ID3D11RenderTargetView** rtv, ID3D11Texture2D** depth) {
    *rtv = g_noteRtv;
    *depth = g_noteDepth;
}

void WaterFxFrameEnd(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* copy, uint64_t copyGen,
                     ID3D11ShaderResourceView* sceneDepth) {
    const bool pending = WaterFxWetPending() && copy && sceneDepth;
    if (g_trace > 0) {
        --g_trace;
        Log("[WATER] trace: frame end: noted %d (%d cameras), a water drew it %d, targets %d/%d, copy %d, depth %d, "
            "time %.4f (last stepped %.4f) -> %s\n", (int)g_noted, g_noteViewCount, (int)g_wetThisFrame,
            g_noteRtv ? 1 : 0, g_noteDepth ? 1 : 0, copy ? 1 : 0, sceneDepth ? 1 : 0, g_time, g_lastTime,
            pending ? "drawn" : "not drawn");
    }
    g_noted = false;
    g_wetThisFrame = false;
    // The scene's camera: the one most of the frame's opaque draws were drawn through.
    const NoteView* best = nullptr;
    for (int i = 0; i < g_noteViewCount; ++i)
        if (!best || g_noteViews[i].draws > best->draws) best = &g_noteViews[i];
    static ULONGLONG lastSaid = 0;
    const ULONGLONG now = GetTickCount64();
    if (pending && now - lastSaid > 10000) {
        lastSaid = now;
        for (int i = 0; i < g_noteViewCount; ++i) {
            const NoteView& w = g_noteViews[i];
            Log("[WATER]   the scene with no water in view, a camera of its draws: at %.1f %.1f %.1f, viewport %.0f %.0f "
                "%.0fx%.0f depth %.3f to %.3f, %d draws%s\n", w.pv[PV_CAMERA_POSITION_WS], w.pv[PV_CAMERA_POSITION_WS + 1],
                w.pv[PV_CAMERA_POSITION_WS + 2], w.viewport.TopLeftX, w.viewport.TopLeftY, w.viewport.Width,
                w.viewport.Height, w.viewport.MinDepth, w.viewport.MaxDepth, w.draws, &w == best ? " (taken)" : "");
        }
    }
    g_noteViewCount = 0;
    if (!best) return;
    memcpy(g_notePv, best->pv, sizeof(g_notePv));
    g_noteViewport = best->viewport;
    g_noteHasViewport = best->hasViewport;
    if (!pending || !ViewOf(g_notePv, g_noteHasViewport ? &g_noteViewport : nullptr, g_noteW, g_noteH, &g_view, &g_time))
        return;
    WaterCarryWaterDone();
    WaterCarrySceneSeen(g_noteW, g_noteH);
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    // The scene's target bound for the wetness, and what the engine had put back after: its targets, its viewports
    // and every shader resource the binding may have taken (a texture bound as a target is unbound wherever it was
    // being read, and the engine's pass after the scene reads the scene).
    // The pass's targets with the unordered views bound beside them (a post process pass writes its own buffers that
    // way; binding a target alone unbinds them, and the pass then runs without them).
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11UnorderedAccessView* omUav[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv, 0,
                                                   D3D11_PS_CS_UAV_REGISTER_COUNT, omUav);
    UINT targets = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) if (rtvs[i]) targets = i + 1;
    UINT uavs = 0;
    for (UINT i = targets; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) if (omUav[i]) uavs = i + 1 - targets;
    if (uavs) {
        static bool said = false;
        if (!said) {
            said = true;
            Log("[WATER] the pass after the scene has %u unordered views bound with its targets; kept for it\n", uavs);
        }
    }
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&nvp, vps);
    ID3D11ShaderResourceView* srv[3][32] = {};
    ctx->PSGetShaderResources(0, 32, srv[0]);
    ctx->VSGetShaderResources(0, 32, srv[1]);
    ctx->CSGetShaderResources(0, 32, srv[2]);

    ctx->OMSetRenderTargets(1, &g_noteRtv, nullptr);
    if (g_noteHasViewport) ctx->RSSetViewports(1, &g_noteViewport);
    if (Advance(ctx, nullptr, sceneDepth)) {
        DrawAfterWater(ctx, copy, copyGen, sceneDepth);
        ++g_lateDraws;
    }

    if (uavs) {
        const UINT keep[D3D11_PS_CS_UAV_REGISTER_COUNT] = { (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1,
                                                            (UINT)-1, (UINT)-1 };
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(targets, rtvs, dsv, targets, uavs, &omUav[targets], keep);
    } else {
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    }
    for (auto* u : omUav) if (u) u->Release();
    ctx->RSSetViewports(nvp, vps);
    ctx->PSSetShaderResources(0, 32, srv[0]);
    ctx->VSSetShaderResources(0, 32, srv[1]);
    ctx->CSSetShaderResources(0, 32, srv[2]);
    for (auto* r : rtvs) if (r) r->Release();
    if (dsv) dsv->Release();
    for (auto& stage : srv)
        for (auto* s : stage) if (s) s->Release();
    t_CorePassThrough = pass;
}
