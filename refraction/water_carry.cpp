// Wetness carried on what moves. The live wetness of water_fx.cpp stays where it was in the world; a character that
// walks out of the water would leave its wet legs behind. So every skinned model's draw, and every single object one
// moved by its own transform (a prop of one bone), while a water is in the view, is drawn once more with stream output
// (a geometry shader made from its vertex shader's bytecode, as RTAO's capture does): each vertex where the engine put
// it, skinning and morphs applied by the engine itself, and its smooth normal (the game's vertex shaders hand it on as
// TEXCOORD1, in the world).
//
// Each vertex keeps how far below its wet line it is, in units, from frame to frame. Near the water (its known height
// around) the line is WET_LINE above the surface; out of it the line sinks, slowly at its top and faster where it is
// deep, so a model dries from the top down over about a minute. A height is linear over a triangle, so the line stays
// a clean line however large the triangles are. The vertices are drawn into two screen sized maps where they lie in
// the scene's depth: how wet (with a soft edge) and the smooth normal, which the full screen wetness pass takes with the
// world's wetness, its reflection turned by the normal.
//
// A model is known again by its draw: its buffers and range, and which of the draws alike it was in the frame (two
// copies of one model may trade their wetness when the engine swaps their order). No water drawn for 2 s: nothing is
// taken, and a draw costs one compare.
#include "water_carry.h"
#include "water_fx.h"
#include "../common/shader_table.h"

#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

long long CorePresents();   // core_hooks.cpp

namespace {

const UINT64 kBudget = 128ull << 20;   // all the models' buffers together
const ULONGLONG kForget = 10000;       // a model not drawn for 10 s is let go
const UINT kRigidMost = 30000;         // a single object of more vertices is the map's, which does not move
const float kDry = -100.0f;            // a vertex far below nothing: dry

const char kCarry[] =
    "cbuffer K : register(b0) {\n"
    "    row_major float4x4 g_clipToRel;   // the draw's clip space to the world moved by the camera offset\n"
    "    float4 g_off;                     // the camera offset\n"
    "    float4 g_k;                       // vertices, seconds, the map's area, its size\n"
    "    float4 g_k2;                      // the time, drips on, a single object, the frame's bytes a vertex\n"
    "};\n"
    "Buffer<float4> g_pos : register(t0);\n"
    "Texture2D<float4> g_wetMap : register(t1);   // height, wet line, tile (water_fx.cpp: the map repeats)\n"
    "ByteAddressBuffer g_frame : register(t2);    // the vertices' normal first (when the stride is 12 or more)\n"
    "RWBuffer<float> g_vwet : register(u0);\n"
    "RWBuffer<uint> g_wetCount : register(u1);   // the frame's wet vertices, for the log\n"
    "// Where each vertex was the frame before (w 1 once known), for its speed; and water_fx.cpp's droplets, their ring.\n"
    "struct Drop { float3 pos; float life; float3 vel; float surface; };\n"
    "RWBuffer<float4> g_prev : register(u3);\n"
    "RWStructuredBuffer<Drop> g_drops : register(u4);\n"
    "RWBuffer<uint> g_ring : register(u5);\n"
    "// How long each vertex keeps beads of water on it (1 under the water, down to 0 over BEAD_TIME seconds out of it).\n"
    "RWBuffer<float> g_beadTime : register(u6);\n"
    "float Rand(uint a, uint b) {\n"
    "    uint h = a * 1973u + b * 9277u;\n"
    "    h = (h ^ 61u) ^ (h >> 16); h *= 9u; h ^= h >> 4; h *= 0x27d4eb2du; h ^= h >> 15;\n"
    "    return (h & 0xffffffu) / 16777216.0;\n"
    "}\n"
    "// A single object's first vertex where it was first seen (xyz), and whether it has moved since (w: -1 not seen yet,\n"
    "// 0 still, 1 moved); then where it was the frame before. One never moved is the map's: it keeps the world's\n"
    "// wetness alone. A jump of more than 50 units in a frame is another object of the same geometry (two tiles of a\n"
    "// floor, one drawn as the other leaves the view), not a move: it starts again from there.\n"
    "RWBuffer<float4> g_origin : register(u2);\n"
    "[numthreads(64, 1, 1)] void Carry(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= (uint)g_k.x) return;\n"
    "    const float4 r = mul(g_pos[id.x], g_clipToRel);\n"
    "    const float3 p = r.xyz / r.w - g_off.xyz;\n"
    "    if (any(!isfinite(p))) return;\n"
    "    // The water's height known around (some 8 units each way), as the full screen pass reads it.\n"
    "    const int size = (int)g_k.w;\n"
    "    const float2 t = p.xy / g_k.z * g_k.w;\n"
    "    float level = -1e9;\n"
    "    [unroll] for (int y = -1; y <= 1; y++)\n"
    "    [unroll] for (int x = -1; x <= 1; x++) {\n"
    "        const int2 q = ((int2)floor(t + float2(x, y) * 4.0) % size + size) % size;\n"
    "        const float4 w = g_wetMap[q];\n"
    "        if (all(w.zw == floor((p.xy + float2(x, y) * 4.0 * g_k.z / g_k.w) / g_k.z))) level = max(level, w.x);\n"
    "    }\n"
    "    if (id.x == 0) {\n"
    "        const float4 o = g_origin[0];\n"
    "        const float3 before = g_origin[1].xyz;\n"
    "        if (o.w < -0.5 || distance(before, p) > 50.0) g_origin[0] = float4(p, 0.0);\n"
    "        else if (o.w < 0.5 && distance(o.xyz, p) > 1.0) g_origin[0] = float4(o.xyz, 1.0);\n"
    "        g_origin[1] = float4(p, 0.0);\n"
    "    }\n"
    "    // How far below its wet line the vertex is: the line sinks WET_SINK units a second at its top and WET_DRAIN of\n"
    "    // the depth below it more, and a vertex under the water lifts it to WET_LINE above the surface (the vertices\n"
    "    // just above the water are wet by the line between them and those below, not by being near it).\n"
    "    float d = g_vwet[id.x];\n"
    "    d -= g_k.y * (WET_SINK + WET_DRAIN * max(d, 0.0));\n"
    "    if (level > -1e8 && p.z < level) d = max(d, level + WET_LINE - p.z);\n"
    "    d = max(d, -100.0);\n"
    "    g_vwet[id.x] = d;\n"
    "    if (d > -1.0) InterlockedAdd(g_wetCount[0], 1);\n"
    "    // Beads: full while the vertex is in the water, going over BEAD_TIME seconds once out of it.\n"
    "    g_beadTime[id.x] = level > -1e8 && p.z < level ? 1.0 : max(g_beadTime[id.x] - g_k.y / BEAD_TIME, 0.0);\n"
    "    // Drips: now and then from a wet vertex out of the water, more from one facing down, one deeper below its wet\n"
    "    // line and one moving, thrown off at its speed; falling into the water below or onto the scene (water_fx.cpp's\n"
    "    // droplets fly them). Not from a single object never moved (the map's).\n"
    "    const float4 prev = g_prev[id.x];\n"
    "    float3 vel = prev.w > 0.5 && g_k.y > 0.0 ? (p - prev.xyz) / g_k.y : 0.0;\n"
    "    if (dot(vel, vel) > 2000.0 * 2000.0) vel = 0.0;   // a jump, not a speed\n"
    "    g_prev[id.x] = float4(p, 1.0);\n"
    "    const bool still = g_k2.z > 0.5 && g_origin[0].w < 0.5;\n"
    "    if (g_k2.y > 0.5 && g_k.y > 0.0 && d > 0.0 && p.z > level && !still) {\n"
    "        float3 n = 0.0;\n"
    "        if (g_k2.w >= 12.0) n = asfloat(g_frame.Load3(id.x * (uint)g_k2.w));\n"
    "        if (dot(n, n) > 1e-6) n = normalize(n);\n"
    "        const float rate = DRIP_RATE * (0.15 + saturate(-n.z)) * saturate(d / 2.0) * (1.0 + length(vel) / 60.0);\n"
    "        const uint seed = asuint(g_k2.x) ^ ((uint)g_k.x * 2654435761u);\n"
    "        if (Rand(id.x, seed) < rate * g_k.y) {\n"
    "            uint slot;\n"
    "            InterlockedAdd(g_ring[0], 1, slot);\n"
    "            Drop drip;\n"
    "            drip.pos = p + n * 0.3;\n"
    "            drip.vel = vel * 0.9 + (float3(Rand(id.x, seed + 1), Rand(id.x, seed + 2), 0.0) - float3(0.5, 0.5, 0.0)) * 8.0;\n"
    "            drip.life = 3.0;\n"
    "            drip.surface = level > -1e8 ? level : -1e9;\n"
    "            g_drops[slot % DROPS] = drip;\n"
    "        }\n"
    "    }\n"
    "}\n"
    "// The model's drawing: bytes a vertex of its frame (12: the normal; 44: the normal, tangent, bitangent and uv; 0:\n"
    "// none), whether its material's normal map is bound, a single object, the beads' test view.\n"
    "cbuffer D : register(b1) { uint g_stride; uint g_hasMap; uint g_rigid; uint g_beadView; };\n"
    "Buffer<float> g_vwetIn : register(t1);\n"
    "Buffer<float4> g_originIn : register(t3);\n"
    "Buffer<float> g_beadTimeIn : register(t4);\n"
    "struct V {\n"
    "    float4 pos : SV_Position;\n"
    "    float below : BELOW;\n"
    "    float beads : BEADS;\n"
    "    float3 n : NORMAL;\n"
    "    float3 t : TANGENT;\n"
    "    float3 b : BINORMAL;\n"
    "    float2 uv : UV;\n"
    "};\n"
    "V CarryVs(uint vid : SV_VertexID) {\n"
    "    V o = (V)0;\n"
    "    o.pos = g_pos[vid];\n"
    "    const bool still = g_rigid && g_originIn[0].w < 0.5;   // a single object never moved: dry\n"
    "    o.below = still ? -100.0 : g_vwetIn[vid];\n"
    "    o.beads = still ? -1.0 : g_beadTimeIn[vid];\n"
    "    if (g_stride >= 12) o.n = asfloat(g_frame.Load3(vid * g_stride));\n"
    "    if (g_stride >= 44) {\n"
    "        o.t = asfloat(g_frame.Load3(vid * g_stride + 12));\n"
    "        o.b = asfloat(g_frame.Load3(vid * g_stride + 24));\n"
    "        o.uv = asfloat(g_frame.Load2(vid * g_stride + 36));\n"
    "    }\n"
    "    return o;\n"
    "}\n"
    "// Only where the vertices lie in the scene's depth: what stands in front of the model keeps its own. How wet, with\n"
    "// a soft edge 4 units wide at the line; the normal, or none: the material's own where its normal map is known,\n"
    "// rebuilt as the game's pixel shaders do (octahedral, unfolded from 256/255; t.x T - t.y B + t.z N).\n"
    "Texture2D<float4> g_normalMap : register(t1);\n"
    "SamplerState g_wrap : register(s0);\n"
    "Texture2D<float> g_depth : register(t0);\n"
    "// How wet (x) and how much of a bead of water (y), each by its largest; the normal.\n"
    "struct Out { float2 wet : SV_Target0; float4 n : SV_Target1; };\n"
    "// Beads of water left on the model by the water (as Counter-Strike 2's characters carry them): a tiling texture of\n"
    "// round droplets in its uv, so they stay on it as it moves, xy their dome's slope and w how high each stands. They\n"
    "// go as the vertex's bead time runs out: the small and low ones first, the largest last.\n"
    "Texture2D<float4> g_beadTex : register(t2);\n"
    "Out CarryPs(V i) {\n"
    "    const float d = g_depth.Load(int3(i.pos.xy, 0));\n"
    "    // The scene's depth is one sample, not the pixel's middle: on a slope seen edge on it differs by up to the\n"
    "    // depth's change across a pixel. Nothing more: these are the engine's own vertices, so where the model is what\n"
    "    // is seen the two depths are the same. A share of the depth itself (it was 0.2%) is several units in the world\n"
    "    // with the engine's reversed depth and a short near plane: the far arm behind the body passed, and its wetness\n"
    "    // and normal drew its outline on the body (seen in the shot camera). A hundred thousandth stays (single pixels\n"
    "    // inside the model fell out without it; 5 units behind are some 2e-5 even 500 units away).\n"
    "    if (abs(d - i.pos.z) > max(1e-5, fwidth(i.pos.z) * 2.0)) discard;\n"
    "    Out o;\n"
    "    o.wet = float2(smoothstep(-1.0, 3.0, i.below), 0.0);\n"
    "    float3 n = i.n;\n"
    "    const float l = length(n);\n"
    "    if (l > 0.5 && g_stride >= 44 && g_hasMap) {\n"
    "        const float2 s = g_normalMap.Sample(g_wrap, i.uv).xy;\n"
    "        float3 t = float3(s.x + s.y - 1.003922, s.x - s.y, 0.0);\n"
    "        t.z = 1.0 - abs(t.x) - abs(t.y);\n"
    "        t = normalize(t);\n"
    "        n = t.x * i.t - t.y * i.b + t.z * (n / l);\n"
    "    }\n"
    "    // (\"beaddebug\" too only on what has moved: the map's single objects never carry any.)\n"
    "    if (l > 0.5 && g_stride >= 44 && (i.beads > 0.0 || (g_beadView && i.beads >= 0.0))) {\n"
    "        const float4 bead = g_beadTex.Sample(g_wrap, i.uv * BEAD_TILE);\n"
    "        // \"beaddebug\": every bead, whatever its time.\n"
    "        const float left = g_beadView ? 1.0 : i.beads;\n"
    "        // The low ones (the small beads, each one's rim) never: a sheet of them reads as a layer of plastic.\n"
    "        const float shown = saturate((bead.w - (1.0 - left) - 0.4) * 4.0);\n"
    "        if (shown > 0.0) {\n"
    "            o.wet.y = shown;\n"
    "            const float2 slope = bead.xy * 2.0 - 1.0;\n"
    "            n = normalize(n / max(length(n), 1e-4) + (slope.x * i.t - slope.y * i.b) * shown);\n"
    "        }\n"
    "    }\n"
    "    const float m = length(n);\n"
    "    o.n = l > 0.5 && m > 1e-4 ? float4(n / m, 1.0) : 0.0;\n"
    "    return o;\n"
    "}\n";

struct Obj {
    ID3D11Buffer* pos = nullptr;
    ID3D11ShaderResourceView* posSrv = nullptr;
    ID3D11Buffer* nrm = nullptr;   // the vertices' frame (stride bytes a vertex); null when the shader hands on none
    ID3D11ShaderResourceView* nrmSrv = nullptr;
    UINT stride = 0;
    ID3D11ShaderResourceView* map = nullptr;   // the material's normal map this frame, held until the next
    ID3D11Buffer* wet = nullptr;
    ID3D11ShaderResourceView* wetSrv = nullptr;
    ID3D11UnorderedAccessView* wetUav = nullptr;
    ID3D11Buffer* prev = nullptr;     // where each vertex was the frame before, for its speed (the drips)
    ID3D11UnorderedAccessView* prevUav = nullptr;
    ID3D11Buffer* beadTime = nullptr;   // how long each vertex keeps its beads of water (1 in the water, to 0 out of it)
    ID3D11UnorderedAccessView* beadTimeUav = nullptr;
    ID3D11ShaderResourceView* beadTimeSrv = nullptr;
    ID3D11Buffer* origin = nullptr;   // where its first vertex was first seen, and whether it has moved
    ID3D11ShaderResourceView* originSrv = nullptr;
    ID3D11UnorderedAccessView* originUav = nullptr;
    UINT vertices = 0;
    UINT64 bytes = 0;
    bool rigid = false;
    float clipToRel[16] = {};
    float off[4] = {};
    uint64_t frame = 0;
    long long presents = 0;   // the presents made before it was taken
    ULONGLONG tick = 0;
};
// The geometry alone, not the shaders: the engine picks another variant as the lights on a model change (one walking
// out of the water into the sun), and the model is still the same one.
using Key = std::tuple<void*, void*, UINT, UINT, INT, UINT, int>;   // vb, ib, start, count, base, instances, the n-th alike
std::map<Key, Obj> g_objs;
std::map<Key, int> g_alike;   // this frame's draws of each kind so far
// Single objects drawn more than once a frame (the map's tiles and parts repeated): the engine draws them in another
// order as the view moves, so which is which cannot be followed; they are left to the world's wetness for good.
std::map<Key, bool> g_repeated;
ULONGLONG g_noRoomUntil = 0;   // the budget was full: no model made until then (else they are let go and made again)
UINT64 g_bytes = 0;

// What a vertex shader is to the carried wetness: skinned (BLENDINDICES among its inputs), or one object moved by its
// own transform (TEXCOORD13, the transform texture's channel), or neither; what of the frame it hands the pixel shader
// (stride: 44 the normal TEXCOORD1, tangent TEXCOORD6, bitangent TEXCOORD7 and uv TEXCOORD2; 12 the normal alone; 0
// none); and its stream output.
struct VsInfo {
    bool skinned = false, rigid = false;
    UINT stride = 0;
    ID3D11GeometryShader* so = nullptr;
};
std::unordered_map<ID3D11VertexShader*, VsInfo> g_vs;

bool g_off = false, g_broken = false;
ULONGLONG g_waterTick = 0;
UINT g_rtW = 0, g_rtH = 0;   // the last water's target
// The targets waters were drawn into in the last 2 s (S2FM's viewports may each have their own size).
struct Seen { UINT w = 0, h = 0; ULONGLONG tick = 0; };
Seen g_seen[4];
bool g_newFrame = true;
uint64_t g_frame = 0;
long long g_taken = 0, g_full = 0, g_made = 0;
// How many of the frame's vertices are wet, counted by the step and read back now and then for the log.
ID3D11Buffer* g_count = nullptr;
ID3D11UnorderedAccessView* g_countUav = nullptr;
ID3D11Buffer* g_countRead = nullptr;
bool g_countPending = false;
UINT g_wetVertices = 0;

ID3D11ComputeShader* g_cs = nullptr;
ID3D11VertexShader* g_vsDraw = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11BlendState* g_blend = nullptr;   // the wetness by its largest, the normal as drawn
ID3D11Buffer* g_drawCb = nullptr;      // a model's clip space to the world, frame stride, normal map, the time
bool g_beadView = false;               // "beaddebug": every bead shown, whatever its time
// The beads' texture: made here at the start (round droplets at random, wrapping), not taken from a game.
ID3D11Texture2D* g_beadTex = nullptr;
ID3D11ShaderResourceView* g_beadSrv = nullptr;
ID3D11SamplerState* g_wrap = nullptr;  // the normal maps read with
// The two screen maps: how wet, and the smooth normal.
struct Screen {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
};
Screen g_screen[2];
UINT g_screenW = 0, g_screenH = 0;

template <class T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }

void Free(Obj& o) {
    g_bytes -= o.bytes;
    o.bytes = 0;
    Drop(o.posSrv);
    Drop(o.pos);
    Drop(o.nrmSrv);
    Drop(o.nrm);
    Drop(o.map);
    o.stride = 0;
    Drop(o.wetUav);
    Drop(o.wetSrv);
    Drop(o.wet);
    Drop(o.originUav);
    Drop(o.originSrv);
    Drop(o.origin);
    Drop(o.prevUav);
    Drop(o.prev);
    Drop(o.beadTimeSrv);
    Drop(o.beadTimeUav);
    Drop(o.beadTime);
}

void Unseen(ID3D11DeviceContext* ctx, Obj& o);

// The beads' texture: 256 x 256, wrapping, some 900 round droplets at random (1.5 to 9 texels across the radius, the
// large ones fewer), each a dome: rg its slope (0.5 flat), a how high it stands, scaled by its size so the small ones go
// first and each shrinks to its middle as it goes. Its mips by averaging.
bool MakeBeads(ID3D11Device* dev) {
    const int n = 256;
    std::vector<float> height(n * n, 0.0f), slopeX(n * n, 0.0f), slopeY(n * n, 0.0f);
    uint32_t seed = 12345u;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f; };
    for (int k = 0; k < 900; ++k) {
        const float cx = rnd() * n, cy = rnd() * n;
        const float r = 1.5f + 7.5f * rnd() * rnd() * rnd();
        const int reach = (int)std::ceil(r);
        for (int dy = -reach; dy <= reach; ++dy)
            for (int dx = -reach; dx <= reach; ++dx) {
                const int px = (int)std::floor(cx) + dx, py = (int)std::floor(cy) + dy;
                const float ox = px + 0.5f - cx, oy = py + 0.5f - cy;
                const float d2 = (ox * ox + oy * oy) / (r * r);
                if (d2 >= 1.0f) continue;
                const float w = std::sqrt(1.0f - d2) * (0.3f + 0.7f * r / 9.0f);
                const int i = ((py % n + n) % n) * n + ((px % n + n) % n);
                if (w <= height[i]) continue;
                height[i] = w;
                slopeX[i] = ox / r;
                slopeY[i] = oy / r;
            }
    }
    // Every level, the largest first, each by averaging the one above.
    std::vector<std::vector<uint8_t>> levels;
    std::vector<float> h = height, sx = slopeX, sy = slopeY;
    for (int size = n; size >= 1; size /= 2) {
        std::vector<uint8_t> px(size * size * 4);
        for (int i = 0; i < size * size; ++i) {
            px[i * 4 + 0] = (uint8_t)std::lround((sx[i] * 0.5f + 0.5f) * 255.0f);
            px[i * 4 + 1] = (uint8_t)std::lround((sy[i] * 0.5f + 0.5f) * 255.0f);
            px[i * 4 + 2] = 0;
            px[i * 4 + 3] = (uint8_t)std::lround(std::min(h[i], 1.0f) * 255.0f);
        }
        levels.push_back(px);
        if (size == 1) break;
        const int half = size / 2;
        std::vector<float> h2(half * half), sx2(half * half), sy2(half * half);
        for (int y = 0; y < half; ++y)
            for (int x = 0; x < half; ++x) {
                const int a = (y * 2) * size + x * 2, b = a + 1, c = a + size, d = c + 1;
                h2[y * half + x] = (h[a] + h[b] + h[c] + h[d]) * 0.25f;
                sx2[y * half + x] = (sx[a] + sx[b] + sx[c] + sx[d]) * 0.25f;
                sy2[y * half + x] = (sy[a] + sy[b] + sy[c] + sy[d]) * 0.25f;
            }
        h.swap(h2);
        sx.swap(sx2);
        sy.swap(sy2);
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = n;
    d.MipLevels = (UINT)levels.size();
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
    for (size_t m = 0; m < levels.size(); ++m) {
        init[m].pSysMem = levels[m].data();
        init[m].SysMemPitch = (UINT)(std::max(n >> m, 1) * 4);
    }
    return SUCCEEDED(dev->CreateTexture2D(&d, init.data(), &g_beadTex))
           && SUCCEEDED(dev->CreateShaderResourceView(g_beadTex, nullptr, &g_beadSrv));
}

bool Ensure(ID3D11Device* dev) {
    if (g_broken) return false;
    if (g_cs && g_vsDraw && g_ps && g_cb && g_blend && g_drawCb && g_wrap && g_beadSrv) return true;
    const char* entries[3] = { "Carry", "CarryVs", "CarryPs" };
    const char* targets[3] = { "cs_5_0", "vs_5_0", "ps_5_0" };
    // The wet line 2 units above the water; sinking 0.3 units a second at its top and 4% of the depth below it more:
    // a model wet to 40 units below its line is dry in some 45 s, its top edge going first.
    // Drips: a wet vertex facing down sheds one with a chance of 0.0015 * 1.15 a second, one facing aside 0.15 of that
    // (each vertex is three or more of the stream's, one for each triangle it is in): some 20 a second from a wet
    // character. DROPS: water_fx.cpp's ring of droplets.
    // Beads: kept 18 s once out of the water; their texture 12 times over the uv (a character's body is about one uv).
    const D3D_SHADER_MACRO macros[] = { { "WET_LINE", "2.0" }, { "WET_SINK", "0.3" }, { "WET_DRAIN", "0.04" },
                                        { "DRIP_RATE", "0.0015" }, { "DROPS", "8192" }, { "BEAD_TIME", "18.0" },
                                        { "BEAD_TILE", "12.0" }, { nullptr, nullptr } };
    for (int i = 0; i < 3 && !g_broken; ++i) {
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT hr = D3DCompile(kCarry, sizeof(kCarry) - 1, "water_carry", macros, nullptr, entries[i], targets[i], 0, 0,
                                &code, &err);
        if (SUCCEEDED(hr) && code) {
            const void* b = code->GetBufferPointer();
            const SIZE_T n = code->GetBufferSize();
            if (i == 0) hr = dev->CreateComputeShader(b, n, nullptr, &g_cs);
            else if (i == 1) hr = dev->CreateVertexShader(b, n, nullptr, &g_vsDraw);
            else hr = dev->CreatePixelShader(b, n, nullptr, &g_ps);
        }
        if (FAILED(hr) || !code) {
            Log("[WATER] the carried wetness's %s shader could not be made: %s\n", entries[i],
                err ? (const char*)err->GetBufferPointer() : "");
            g_broken = true;
        }
        if (code) code->Release();
        if (err) err->Release();
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 112;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_BLEND_DESC bl = {};
    bl.IndependentBlendEnable = TRUE;
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
    bl.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_BUFFER_DESC db = bd;
    db.ByteWidth = 16;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (!g_broken && (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb)) || FAILED(dev->CreateBlendState(&bl, &g_blend))
                      || FAILED(dev->CreateBuffer(&db, nullptr, &g_drawCb)) || FAILED(dev->CreateSamplerState(&sd, &g_wrap))
                      || !MakeBeads(dev)))
        g_broken = true;
    if (g_broken) Log("[WATER] the carried wetness could not be made\n");
    return !g_broken;
}

// How many float components the vertex shader's output TEXCOORDn has (0: none, or not floats).
int Components(ID3D11ShaderReflection* refl, UINT index) {
    D3D11_SHADER_DESC sd = {};
    refl->GetDesc(&sd);
    for (UINT i = 0; i < sd.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        refl->GetOutputParameterDesc(i, &p);
        if (!p.SemanticName || _stricmp(p.SemanticName, "TEXCOORD") || p.SemanticIndex != index) continue;
        if (p.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32) return 0;
        int n = 0;
        for (int c = 0; c < 4; ++c) n += (p.Mask >> c) & 1;
        return n;
    }
    return 0;
}

const VsInfo& InfoOf(ID3D11Device* dev, ID3D11VertexShader* vs) {
    auto it = g_vs.find(vs);
    if (it != g_vs.end()) return it->second;
    VsInfo info;
    std::vector<uint8_t> code(1 << 20);
    UINT bytes = (UINT)code.size();
    ID3D11ShaderReflection* refl = nullptr;
    if (CoreShaderBytecode(vs, code.data(), &bytes)
        && SUCCEEDED(D3DReflect(code.data(), bytes, IID_ID3D11ShaderReflection, (void**)&refl)) && refl) {
        D3D11_SHADER_DESC sd = {};
        refl->GetDesc(&sd);
        bool transform = false;
        for (UINT i = 0; i < sd.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC p = {};
            refl->GetInputParameterDesc(i, &p);
            if (!p.SemanticName) continue;
            if (!_stricmp(p.SemanticName, "BLENDINDICES")) info.skinned = true;
            if (!_stricmp(p.SemanticName, "TEXCOORD") && p.SemanticIndex == 13) transform = true;
        }
        info.rigid = !info.skinned && transform;
        // The frame as the game's pixel shaders take it (vr_skin, vr_complex: the normal map's uv in TEXCOORD2, the
        // normal TEXCOORD1, tangent TEXCOORD6 and bitangent TEXCOORD7, all in the world).
        const bool normal = Components(refl, 1) >= 3;
        const bool frame = normal && Components(refl, 6) >= 3 && Components(refl, 7) >= 3 && Components(refl, 2) == 2;
        info.stride = frame ? 44 : normal ? 12 : 0;
        refl->Release();
        if (info.skinned || info.rigid) {
            D3D11_SO_DECLARATION_ENTRY decl[5] = {};
            decl[0].SemanticName = "SV_POSITION";
            decl[0].ComponentCount = 4;
            const UINT order[4] = { 1, 6, 7, 2 };
            for (int i = 0; i < 4; ++i) {
                decl[1 + i].SemanticName = "TEXCOORD";
                decl[1 + i].SemanticIndex = order[i];
                decl[1 + i].ComponentCount = order[i] == 2 ? 2 : 3;
                decl[1 + i].OutputSlot = 1;
            }
            // The whole frame, or the normal alone, or the position alone, whichever the runtime takes first.
            HRESULT hr = E_FAIL;
            for (UINT stride = info.stride; FAILED(hr); stride = stride == 44 ? 12 : 0) {
                const UINT strides[2] = { 16, stride };
                const UINT entries = stride == 44 ? 5 : stride == 12 ? 2 : 1;
                hr = dev->CreateGeometryShaderWithStreamOutput(code.data(), bytes, decl, entries, strides, stride ? 2 : 1,
                                                               D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &info.so);
                info.stride = stride;
                if (!stride) break;
            }
            if (FAILED(hr)) {
                Log("[WATER] no stream output from a vertex shader of a %s model (0x%08X): it carries no wetness\n",
                    info.skinned ? "skinned" : "single object", (unsigned)hr);
                info.so = nullptr;
            }
        }
    }
    return g_vs.emplace(vs, info).first->second;
}

// Room for a model's vertices: the buffers made, dry.
bool Make(ID3D11Device* dev, ID3D11DeviceContext* ctx, Obj& o, UINT vertices, UINT stride) {
    const UINT64 need = (UINT64)vertices * (40 + stride);
    // Over the budget: the models not drawn this frame let go, the longest unseen first.
    while (g_bytes + need > kBudget) {
        auto oldest = g_objs.end();
        for (auto it = g_objs.begin(); it != g_objs.end(); ++it)
            if (it->second.frame != g_frame && it->second.pos && (oldest == g_objs.end() || it->second.tick < oldest->second.tick))
                oldest = it;
        if (oldest == g_objs.end()) return false;
        Free(oldest->second);
        g_objs.erase(oldest);
    }
    D3D11_BUFFER_DESC b = {};
    b.ByteWidth = vertices * 16;
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.NumElements = vertices;
    bool ok = SUCCEEDED(dev->CreateBuffer(&b, nullptr, &o.pos)) && SUCCEEDED(dev->CreateShaderResourceView(o.pos, &sv, &o.posSrv));
    if (ok && stride) {
        // Read as raw bytes: stride bytes a vertex.
        D3D11_BUFFER_DESC nb = b;
        nb.ByteWidth = vertices * stride;
        nb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SHADER_RESOURCE_VIEW_DESC rv = {};
        rv.Format = DXGI_FORMAT_R32_TYPELESS;
        rv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        rv.BufferEx.NumElements = vertices * stride / 4;
        rv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ok = SUCCEEDED(dev->CreateBuffer(&nb, nullptr, &o.nrm)) && SUCCEEDED(dev->CreateShaderResourceView(o.nrm, &rv, &o.nrmSrv));
        o.stride = stride;
    }
    b.ByteWidth = vertices * 4;
    b.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer.NumElements = vertices;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&b, nullptr, &o.wet)) && SUCCEEDED(dev->CreateShaderResourceView(o.wet, &sv, &o.wetSrv))
         && SUCCEEDED(dev->CreateUnorderedAccessView(o.wet, &uv, &o.wetUav));
    // Where each vertex was the frame before: unknown (w 0) until its first step.
    b.ByteWidth = vertices * 16;
    D3D11_UNORDERED_ACCESS_VIEW_DESC pu = uv;
    pu.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&b, nullptr, &o.prev)) && SUCCEEDED(dev->CreateUnorderedAccessView(o.prev, &pu, &o.prevUav));
    // How long each vertex keeps its beads: none to begin with.
    D3D11_BUFFER_DESC tb = b;
    tb.ByteWidth = vertices * 4;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&tb, nullptr, &o.beadTime))
         && SUCCEEDED(dev->CreateUnorderedAccessView(o.beadTime, &uv, &o.beadTimeUav))
         && SUCCEEDED(dev->CreateShaderResourceView(o.beadTime, &sv, &o.beadTimeSrv));
    b.ByteWidth = 32;   // where it was first seen, and the frame before
    sv.Format = uv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sv.Buffer.NumElements = uv.Buffer.NumElements = 2;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&b, nullptr, &o.origin))
         && SUCCEEDED(dev->CreateShaderResourceView(o.origin, &sv, &o.originSrv))
         && SUCCEEDED(dev->CreateUnorderedAccessView(o.origin, &uv, &o.originUav));
    o.vertices = vertices;
    o.bytes = need;
    g_bytes += need;
    if (!ok) { Free(o); return false; }
    const float dry[4] = { kDry, kDry, kDry, kDry };
    ctx->ClearUnorderedAccessViewFloat(o.wetUav, dry);
    Unseen(ctx, o);
    return true;
}

// Not seen yet: its first vertex taken afresh at its next step, and no vertex's speed known.
void Unseen(ID3D11DeviceContext* ctx, Obj& o) {
    const float none[4] = { 0.0f, 0.0f, 0.0f, -1.0f };
    const float unknown[4] = {};
    if (o.originUav) ctx->ClearUnorderedAccessViewFloat(o.originUav, none);
    if (o.prevUav) ctx->ClearUnorderedAccessViewFloat(o.prevUav, unknown);
    if (o.beadTimeUav) ctx->ClearUnorderedAccessViewFloat(o.beadTimeUav, unknown);
}

}  // namespace

void WaterCarryOff(bool off) { g_off = off; }

void WaterCarrySceneSeen(UINT rtW, UINT rtH) {
    const ULONGLONG now = GetTickCount64();
    g_rtW = rtW;
    g_rtH = rtH;
    Seen* slot = &g_seen[0];
    for (Seen& s : g_seen) {
        if (s.w == rtW && s.h == rtH) { slot = &s; break; }
        if (s.tick < slot->tick) slot = &s;
    }
    *slot = Seen{ rtW, rtH, now };
}

void WaterCarryWaterSeen(UINT rtW, UINT rtH) {
    g_waterTick = GetTickCount64();
    WaterCarrySceneSeen(rtW, rtH);
}

void WaterCarryWaterDone() { g_newFrame = true; }

void WaterCarryDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    if (g_off || g_broken || !g_waterTick || !st || st->deferred || !st->vs || !st->ps) return;
    const ULONGLONG now = GetTickCount64();
    // A water in the view in the last 2 s, or a model still wet (the camera turned from the water).
    if (now - g_waterTick > 2000 && g_wetVertices == 0) return;
    bool sized = false;
    for (const Seen& s : g_seen) sized = sized || (s.w == st->rtW && s.h == st->rtH && now - s.tick <= 2000);
    const ContextState::Draw& d = st->draw;
    if (!sized || d.indirect || d.count < 3) return;
    auto known = g_vs.find(st->vs);
    if (known != g_vs.end() && !known->second.so) return;
    const UINT vertices = (d.count / 3) * 3 * (d.instances ? d.instances : 1);
    if (known != g_vs.end() && known->second.rigid && (d.instances != 1 || vertices > kRigidMost)) return;
    if (CoreFrameForeign(st)) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    const VsInfo& info = InfoOf(dev, st->vs);
    if (!info.so || (info.rigid && (d.instances != 1 || vertices > kRigidMost)) || !Ensure(dev)) { dev->Release(); return; }
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topo);
    ID3D11HullShader* hs = nullptr;
    ctx->HSGetShader(&hs, nullptr, nullptr);
    ID3D11GeometryShader* gs = nullptr;
    ctx->GSGetShader(&gs, nullptr, nullptr);
    const bool plain = topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && !hs && !gs;
    if (hs) hs->Release();
    if (gs) gs->Release();
    float pv[204];
    float inv[16];
    if (!plain || !CorePerView(st, pv) || !Inverse4x4(&pv[PV_MAT_WORLD_TO_PROJ_MV], inv)) { dev->Release(); return; }

    if (g_newFrame) {
        // The draws from here on are the next frame's; the models long unseen let go.
        g_newFrame = false;
        g_alike.clear();
        ++g_frame;
        for (auto it = g_objs.begin(); it != g_objs.end();) {
            if (now - it->second.tick > kForget) { Free(it->second); it = g_objs.erase(it); }
            else ++it;
        }
    }
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    ctx->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
    Key key(vb, ib, d.start, d.count, d.base, d.instances, 0);
    if (vb) vb->Release();
    if (ib) ib->Release();
    if (info.rigid && g_repeated.count(key)) { dev->Release(); return; }
    const int nth = g_alike[key]++;
    if (info.rigid && nth > 0) {
        // A single object drawn again this frame: its first, taken already, let go too.
        g_repeated[key] = true;
        auto first = g_objs.find(key);
        if (first != g_objs.end()) { Free(first->second); g_objs.erase(first); }
        dev->Release();
        return;
    }
    std::get<6>(key) = nth;
    auto found = g_objs.find(key);
    const bool fits = found != g_objs.end() && found->second.pos && found->second.vertices == vertices
                      && found->second.stride == info.stride;
    if (!fits && now < g_noRoomUntil) { dev->Release(); return; }
    Obj& o = g_objs[key];
    if (!fits) {
        ++g_made;
        Free(o);
        if (!Make(dev, ctx, o, vertices, info.stride)) {
            g_objs.erase(key);
            g_noRoomUntil = now + 2000;
            if (g_full++ == 0) Log("[WATER] the carried wetness's %u MB are full: a model is left dry\n", (unsigned)(kBudget >> 20));
            dev->Release();
            return;
        }
    }
    dev->Release();
    memcpy(o.clipToRel, inv, sizeof(inv));
    memcpy(o.off, &pv[PV_WORLD_TO_CAM_OFF_MV], 12);
    o.rigid = info.rigid;
    o.frame = g_frame;
    o.presents = CorePresents();
    o.tick = now;
    // The material's normal map (g_tNormal, by the shader table), for the wet film's normal.
    Drop(o.map);
    uint8_t reg = 0;
    uint16_t at = 0;
    if (o.stride == 44 && st->psLayout >= 0 && ShaderDbFind(st->psLayout, "g_tNormal", &reg, &at) && reg == SDB_REG_TEXTURE
        && (at & 0xFF) < 32 && (o.map = st->psSrv[at & 0xFF]) != nullptr)
        o.map->AddRef();

    // The draw again, its vertices streamed out into the model's buffers; what was bound there put back.
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    ID3D11GeometryShader* oldGs = nullptr;
    ID3D11ClassInstance* oldCi[256];
    UINT oldCiN = 256;
    ctx->GSGetShader(&oldGs, oldCi, &oldCiN);
    ID3D11Buffer* oldSo[2] = {};
    ctx->SOGetTargets(2, oldSo);
    ID3D11Buffer* targets[2] = { o.pos, o.nrm };
    const UINT zero[2] = {};
    ctx->GSSetShader(info.so, nullptr, 0);
    ctx->SOSetTargets(o.nrm ? 2 : 1, targets, zero);
    CoreRedraw(ctx, st);
    const UINT append[2] = { (UINT)-1, (UINT)-1 };
    ctx->SOSetTargets(2, oldSo, append);
    ctx->GSSetShader(oldGs, oldCi, oldCiN);
    for (ID3D11Buffer* b : oldSo) if (b) b->Release();
    if (oldGs) oldGs->Release();
    for (UINT i = 0; i < oldCiN; ++i) if (oldCi[i]) oldCi[i]->Release();
    t_CorePassThrough = pass;
    ++g_taken;
}

void WaterCarryStep(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* wetMap, float area, float size, float dt,
                    float time, ID3D11UnorderedAccessView* drops, ID3D11UnorderedAccessView* ring) {
    if (g_off || g_broken || !g_cs) return;
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    if (!g_count) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 4;
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
    const UINT zeroU[4] = {};
    if (g_countUav) ctx->ClearUnorderedAccessViewUint(g_countUav, zeroU);
    ctx->CSSetShader(g_cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    UINT models = 0, rigid = 0, vertices = 0, framed = 0, mapped = 0;
    for (auto& kv : g_objs) {
        Obj& o = kv.second;
        if (o.frame != g_frame || !o.pos) continue;
        float k[28] = {};
        memcpy(k, o.clipToRel, 64);
        memcpy(k + 16, o.off, 12);
        k[20] = (float)o.vertices;
        k[21] = dt;
        k[22] = area;
        k[23] = size;
        k[24] = time;
        k[25] = drops && ring ? 1.0f : 0.0f;
        k[26] = o.rigid ? 1.0f : 0.0f;
        k[27] = o.nrm ? (float)o.stride : 0.0f;
        ctx->UpdateSubresource(g_cb, 0, nullptr, k, 0, 0);
        ID3D11ShaderResourceView* in[3] = { o.posSrv, wetMap, o.nrmSrv };
        ctx->CSSetShaderResources(0, 3, in);
        ID3D11UnorderedAccessView* out[7] = { o.wetUav, g_countUav, o.originUav, o.prevUav, drops, ring, o.beadTimeUav };
        ctx->CSSetUnorderedAccessViews(0, 7, out, nullptr);
        ctx->Dispatch((o.vertices + 63) / 64, 1, 1);
        ++models;
        if (o.rigid) ++rigid;
        if (o.nrm && o.stride == 44) ++framed;
        if (o.map) ++mapped;
        vertices += o.vertices;
    }
    ID3D11ShaderResourceView* none[3] = {};
    ID3D11UnorderedAccessView* noUav[7] = {};
    ctx->CSSetShaderResources(0, 3, none);
    ctx->CSSetUnorderedAccessViews(0, 7, noUav, nullptr);
    // The wet count: copied now and then, read once the card has it (a frame or more later).
    if (g_countRead) {
        if (g_countPending) {
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(ctx->Map(g_countRead, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
                g_wetVertices = *(const UINT*)m.pData;
                ctx->Unmap(g_countRead, 0);
                g_countPending = false;
            }
        } else {
            ctx->CopyResource(g_countRead, g_count);
            g_countPending = true;
        }
    }
    t_CorePassThrough = pass;

    static ULONGLONG lastSaid = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastSaid > 10000) {
        lastSaid = now;
        Log("[WATER] carried wetness: %u models (%u single objects, %u with uv and tangents for beads, %u with their "
            "normal map), %u vertices this frame (%u wet), %u kept (%.1f MB), %lld draws taken and %lld models made in "
            "10 s, %u repeated single objects left to the world's wetness\n", models, rigid, framed, mapped, vertices,
            g_wetVertices, (unsigned)g_objs.size(), g_bytes / 1048576.0, g_taken, g_made, (unsigned)g_repeated.size());
        g_taken = 0;
        g_made = 0;
    }
}

void WaterCarryClear(ID3D11DeviceContext* ctx) {
    const float dry[4] = { kDry, kDry, kDry, kDry };
    for (auto& kv : g_objs) {
        if (kv.second.wetUav) ctx->ClearUnorderedAccessViewFloat(kv.second.wetUav, dry);
        Unseen(ctx, kv.second);
    }
}

bool WaterCarryScreen(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* sceneDepth, ID3D11ShaderResourceView** wet,
                      ID3D11ShaderResourceView** normal) {
    *wet = *normal = nullptr;
    if (g_off || g_broken || !g_ps || !sceneDepth) return false;
    // The frame runs from one water's draw to the next, so a model drawn after the water (its second pass, a
    // translucent part) was taken in the frame before and is where the model was then: only those taken since the
    // last present, else the beads slide behind a moving model.
    bool any = false;
    long long newest = 0;
    for (auto& kv : g_objs) {
        if (kv.second.frame != g_frame || !kv.second.pos) continue;
        any = true;
        newest = std::max(newest, kv.second.presents);
    }
    if (!any) return false;
    if (!g_screen[0].tex || g_screenW != g_rtW || g_screenH != g_rtH) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        const DXGI_FORMAT formats[2] = { DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT };
        bool ok = dev != nullptr;
        for (int i = 0; i < 2; ++i) {
            Screen& s = g_screen[i];
            Drop(s.srv);
            Drop(s.rtv);
            Drop(s.tex);
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = g_rtW;
            d.Height = g_rtH;
            d.MipLevels = d.ArraySize = 1;
            d.Format = formats[i];
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &s.tex)) && SUCCEEDED(dev->CreateRenderTargetView(s.tex, nullptr, &s.rtv))
                 && SUCCEEDED(dev->CreateShaderResourceView(s.tex, nullptr, &s.srv));
        }
        if (dev) dev->Release();
        if (!ok) {
            for (Screen& s : g_screen) { Drop(s.srv); Drop(s.rtv); Drop(s.tex); }
            return false;
        }
        g_screenW = g_rtW;
        g_screenH = g_rtH;
    }
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    ID3D11BlendState* blend = nullptr;
    float factor[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&blend, factor, &mask);
    ID3D11VertexShader* vs = nullptr;
    ID3D11ClassInstance* vsCi[256];
    UINT vsCiN = 256;
    ctx->VSGetShader(&vs, vsCi, &vsCiN);
    ID3D11PixelShader* ps = nullptr;
    ID3D11ClassInstance* psCi[256];
    UINT psCiN = 256;
    ctx->PSGetShader(&ps, psCi, &psCiN);
    ID3D11ShaderResourceView* vsSrv[5] = {};
    ctx->VSGetShaderResources(0, 5, vsSrv);
    ID3D11ShaderResourceView* psSrv[3] = {};
    ctx->PSGetShaderResources(0, 3, psSrv);
    ID3D11Buffer* vsCb = nullptr;
    ctx->VSGetConstantBuffers(1, 1, &vsCb);
    ID3D11Buffer* psCb = nullptr;
    ctx->PSGetConstantBuffers(1, 1, &psCb);
    ID3D11SamplerState* psSampler = nullptr;
    ctx->PSGetSamplers(0, 1, &psSampler);
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ctx->IAGetPrimitiveTopology(&topo);
    ID3D11InputLayout* layout = nullptr;
    ctx->IAGetInputLayout(&layout);

    const float none[4] = {};
    ID3D11RenderTargetView* mine[2] = { g_screen[0].rtv, g_screen[1].rtv };
    ctx->ClearRenderTargetView(mine[0], none);
    ctx->ClearRenderTargetView(mine[1], none);
    ctx->OMSetRenderTargets(2, mine, nullptr);
    ctx->OMSetBlendState(g_blend, none, 0xffffffff);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g_vsDraw, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->VSSetConstantBuffers(1, 1, &g_drawCb);
    ctx->PSSetConstantBuffers(1, 1, &g_drawCb);
    ctx->PSSetSamplers(0, 1, &g_wrap);
    ctx->PSSetShaderResources(0, 1, &sceneDepth);
    ctx->PSSetShaderResources(2, 1, &g_beadSrv);
    for (auto& kv : g_objs) {
        Obj& o = kv.second;
        if (o.frame != g_frame || !o.pos || o.presents != newest) continue;
        const UINT k[4] = { o.nrm ? o.stride : 0u, o.map ? 1u : 0u, o.rigid ? 1u : 0u, g_beadView ? 1u : 0u };
        ctx->UpdateSubresource(g_drawCb, 0, nullptr, k, 0, 0);
        ID3D11ShaderResourceView* in[5] = { o.posSrv, o.wetSrv, o.nrmSrv, o.originSrv, o.beadTimeSrv };
        ctx->VSSetShaderResources(0, 5, in);
        ctx->PSSetShaderResources(1, 1, &o.map);
        ctx->Draw(o.vertices, 0);
    }

    ID3D11ShaderResourceView* noSrv[5] = {};
    ctx->VSSetShaderResources(0, 5, noSrv);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    ctx->OMSetBlendState(blend, factor, mask);
    ctx->VSSetShader(vs, vsCi, vsCiN);
    ctx->PSSetShader(ps, psCi, psCiN);
    ctx->VSSetShaderResources(0, 5, vsSrv);
    ctx->PSSetShaderResources(0, 3, psSrv);
    ctx->VSSetConstantBuffers(1, 1, &vsCb);
    ctx->PSSetConstantBuffers(1, 1, &psCb);
    ctx->PSSetSamplers(0, 1, &psSampler);
    ctx->IASetPrimitiveTopology(topo);
    ctx->IASetInputLayout(layout);
    if (vsCb) vsCb->Release();
    if (psCb) psCb->Release();
    if (psSampler) psSampler->Release();
    for (auto* r : rtvs) if (r) r->Release();
    if (dsv) dsv->Release();
    if (blend) blend->Release();
    if (vs) vs->Release();
    for (UINT i = 0; i < vsCiN; ++i) if (vsCi[i]) vsCi[i]->Release();
    if (ps) ps->Release();
    for (UINT i = 0; i < psCiN; ++i) if (psCi[i]) psCi[i]->Release();
    for (auto* s : vsSrv) if (s) s->Release();
    for (auto* s : psSrv) if (s) s->Release();
    if (layout) layout->Release();
    t_CorePassThrough = pass;
    *wet = g_screen[0].srv;
    *normal = g_screen[1].srv;
    return true;
}

ID3D11ShaderResourceView* WaterCarryLastNormal() { return g_off || g_broken ? nullptr : g_screen[1].srv; }

void WaterCarryBeadView(bool on) { g_beadView = on; }
