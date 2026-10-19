// RTAO's geometry: every opaque draw of the view (an AO reader) drawn once more, right after the engine's, with its own
// vertex shader and input and a geometry shader made from that vertex shader's bytecode that streams SV_Position out,
// with no rasterised stream. What lands in the capture buffer is each triangle where the engine put it, skinning,
// morphs and the per object transform texture applied by the engine itself. The geometry shader is made from the
// vertex shader's whole bytecode, not its signature: the signature alone answered 0x8007000E.
//
// Cut outs: a reader whose pixel shader discards (leaves, branches, fences, hair) has holes the rays must see through.
// Its shader is read once: which input carries the uv it samples g_tColor with, and whether it multiplies that alpha by
// a vertex's own (some trees: alpha x v3.w, thrown away under 0.001). Its draw then streams that uv and
// alpha out too, into a second buffer, and g_tColor's alpha is drawn into a mask of our own each frame (a compute pass,
// 512 x 512), which the rays look up where they strike it (rtao_cs.hlsl). The variants that discard under a reference
// (F_ALPHA_TEST, g_flAlphaTestReference) block where the alpha reaches it; the see-through ones block with the chance of
// their alpha, which S2FM's refinement averages into the right shade.
//
// Off the screen ("offscreen" on the test words, off by default): the engine culls by the camera, so an object wholly
// outside its frustum casts no AO onto what is inside. Each frame it draws a shadow view first (in a test scene: one
// orthographic view round the camera, ~250 depth only draws, ~580k triangles); those draws are taken the same way, with
// their own view, before the camera's (runs of one view each). Measured there (16 rays): the right edge 0.008 darker,
// the card's AO time 7-8 ms -> 24-31 ms, 146 -> 265 MB. Too dear for what it gives, so it waits for a shot that needs it.
#include "rtao.h"
#include "../core/core.h"
#include "../common/shader_table.h"

#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

std::mutex g_m;

// --- What a pixel shader's discard is made of.
struct CutInfo {
    bool cut = false;      // it discards
    bool traced = false;   // and its alpha could be followed: the rays judge it by its mask
    int texSlot = -1;      // g_tColor's slot
    int uvReg = -1;        // the input register its uv comes in
    int alphaReg = -1;     // the input register whose w multiplies the alpha, or -1
    int refOffset = -1;    // g_flAlphaTestReference in b0 (floats), when the variant tests against it
};
std::unordered_map<ID3D11PixelShader*, CutInfo> g_cuts;

std::string Disassembly(ID3D11DeviceChild* shader) {
    std::vector<uint8_t> code(1 << 20);
    UINT bytes = (UINT)code.size();
    ID3DBlob* dis = nullptr;
    std::string text;
    if (shader && CoreShaderBytecode(shader, code.data(), &bytes)
        && SUCCEEDED(D3DDisassemble(code.data(), bytes, D3D_DISASM_DISABLE_DEBUG_INFO, nullptr, &dis)) && dis) {
        const char* t = (const char*)dis->GetBufferPointer();
        text.assign(t, t + dis->GetBufferSize());
        dis->Release();
    }
    return text;
}

const CutInfo& CutOf(const ContextState* st) {
    auto it = g_cuts.find(st->ps);
    if (it != g_cuts.end()) return it->second;
    CutInfo c;
    const std::string text = Disassembly(st->ps);
    c.cut = text.find("discard") != std::string::npos;
    if (c.cut) {
        uint8_t reg = 0;
        uint16_t off = 0;
        if (st->psLayout >= 0 && ShaderDbFind(st->psLayout, "g_tColor", &reg, &off) && reg == SDB_REG_TEXTURE)
            c.texSlot = off & 0xFF;
        // The sample of g_tColor: "sample... rB.xyzw, vU.xy.., tK.xyzw, sM".
        std::smatch m;
        std::string rest = text;
        std::string dest;
        const std::regex sample("sample[_a-z]*\\([^)]*\\)\\([^)]*\\) (r\\d+)\\.\\w+, v(\\d+)\\.\\w+, t(\\d+)\\.");
        while (c.texSlot >= 0 && std::regex_search(rest, m, sample)) {
            if (atoi(m[3].str().c_str()) == c.texSlot) {
                dest = m[1].str();
                c.uvReg = atoi(m[2].str().c_str());
                break;
            }
            rest = m.suffix().str();
        }
        // Its alpha times a vertex's: "mul rX.c, rB.w, vA.w".
        if (!dest.empty()) {
            const std::regex vertexAlpha("mul r\\d+\\.\\w, " + dest + "\\.w, v(\\d+)\\.w");
            if (std::regex_search(text, m, vertexAlpha)) c.alphaReg = atoi(m[1].str().c_str());
        }
        if (st->psCombo >= 0 && ShaderDbComboValue(st->psCombo, "F_ALPHA_TEST") > 0
            && ShaderDbFind(st->psLayout, "g_flAlphaTestReference", &reg, &off) && reg == SDB_REG_FLOAT4)
            c.refOffset = off;
        c.traced = c.uvReg >= 0;
        Log("[RTAO] a cut out pixel shader (%s): %s", st->psLayout >= 0 ? ShaderDbName(st->psLayout) : "?",
            c.traced ? "its alpha is followed" : "its alpha could not be followed, it is traced as solid");
        Log("[RTAO]   g_tColor at t%d, its uv in v%d, times v%d.w, %s", c.texSlot, c.uvReg, c.alphaReg,
            c.refOffset >= 0 ? "against g_flAlphaTestReference" : "with the chance of its alpha");
    }
    return g_cuts.emplace(st->ps, c).first->second;
}

// --- Stream output shaders, per vertex shader and what else a cut out needs from it (its uv's and alpha's registers).
std::map<std::tuple<ID3D11VertexShader*, int, int, bool>, ID3D11GeometryShader*> g_so;
std::map<std::tuple<ID3D11VertexShader*, int, int, bool>, bool> g_soFailed;

// The vertex shader's output at a register, as a stream output entry from component first: its semantic and the
// component within it. False when the register has no output there.
bool OutputAt(ID3D11ShaderReflection* refl, int reg, int component, D3D11_SO_DECLARATION_ENTRY* e) {
    D3D11_SHADER_DESC sd = {};
    refl->GetDesc(&sd);
    for (UINT i = 0; i < sd.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        refl->GetOutputParameterDesc(i, &p);
        if ((int)p.Register != reg || !(p.Mask & (1u << component))) continue;
        int low = 0;
        while (low < 4 && !(p.Mask & (1u << low))) ++low;
        e->SemanticName = p.SemanticName;
        e->SemanticIndex = p.SemanticIndex;
        e->StartComponent = (BYTE)(component - low);
        return true;
    }
    return false;
}

// The vertex shader's smooth normal: the game's vertex shaders hand the pixel shader the posed normal in world space
// as TEXCOORD1 (vr_complex, vr_skin, vr_simple and the rest; turned by the object's and the bones' cofactor matrix).
bool NormalOutput(ID3D11ShaderReflection* refl, D3D11_SO_DECLARATION_ENTRY* e) {
    D3D11_SHADER_DESC sd = {};
    refl->GetDesc(&sd);
    for (UINT i = 0; i < sd.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        refl->GetOutputParameterDesc(i, &p);
        if (!p.SemanticName || _stricmp(p.SemanticName, "TEXCOORD") != 0 || p.SemanticIndex != 1) continue;
        if ((p.Mask & 7u) != 7u || p.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32) return false;
        e->SemanticName = p.SemanticName;
        e->SemanticIndex = 1;
        e->StartComponent = 0;
        e->ComponentCount = 3;
        e->OutputSlot = 2;
        return true;
    }
    return false;
}

// Slot 0 the position (16 bytes), slot 1 a cut out's uv and alpha (12), slot 2 the smooth normal (12).
ID3D11GeometryShader* SoFor(ID3D11Device* dev, ID3D11VertexShader* vs, int uvReg, int alphaReg, bool normal) {
    const auto key = std::make_tuple(vs, uvReg, alphaReg, normal);
    auto it = g_so.find(key);
    if (it != g_so.end()) return it->second;
    if (g_soFailed.count(key)) return nullptr;
    std::vector<uint8_t> code(1 << 20);
    UINT bytes = (UINT)code.size();
    ID3D11GeometryShader* gs = nullptr;
    HRESULT hr = E_FAIL;
    ID3D11ShaderReflection* refl = nullptr;
    if (CoreShaderBytecode(vs, code.data(), &bytes)) {
        D3D11_SO_DECLARATION_ENTRY decl[4] = {};
        UINT n = 0;
        decl[n].SemanticName = "SV_POSITION";
        decl[n].ComponentCount = 4;
        decl[n].OutputSlot = 0;
        ++n;
        UINT strides[3] = { 16, 12, 12 };
        UINT slots = 1;
        bool ok = true;
        if (uvReg >= 0 || normal)
            ok = SUCCEEDED(D3DReflect(code.data(), bytes, IID_ID3D11ShaderReflection, (void**)&refl)) && refl;
        if (ok && uvReg >= 0) {
            // The uv (two components), then the vertex's alpha (one), or a gap where there is none.
            ok = ok && OutputAt(refl, uvReg, 0, &decl[n]);
            if (ok) {
                decl[n].ComponentCount = 2;
                decl[n].OutputSlot = 1;
                ++n;
                if (alphaReg >= 0 && OutputAt(refl, alphaReg, 3, &decl[n])) {
                    decl[n].ComponentCount = 1;
                } else {
                    decl[n].SemanticName = nullptr;
                    decl[n].ComponentCount = 1;
                }
                decl[n].OutputSlot = 1;
                ++n;
                slots = 2;
            }
        }
        if (ok && normal) {
            ok = NormalOutput(refl, &decl[n]);
            if (ok) {
                ++n;
                slots = 3;
            }
        }
        if (ok) hr = dev->CreateGeometryShaderWithStreamOutput(code.data(), bytes, decl, n, strides, slots,
                                                               D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs);
    }
    if (refl) refl->Release();
    if (FAILED(hr) || !gs) {
        g_soFailed[key] = true;
        if (normal) return nullptr;   // the caller asks again without the normal
        Log("[RTAO] no stream output from a vertex shader%s (0x%08X): its draws are %s", uvReg >= 0 ? " with its uv" : "",
            (unsigned)hr, uvReg >= 0 ? "traced as solid" : "left out of the rays");
        return nullptr;
    }
    g_so[key] = gs;
    return gs;
}

// --- The frame's capture. It opens at the frame's first AO reader, or before that at the first draw of another view
// (rtao_engine.cpp: the shadows' depth only draws, which come before the camera's), whose triangles go first.
bool g_open = false;       // the camera's part: from the first AO reader to the rays
bool g_pre = false;        // other views' draws taken, the camera's part not begun
bool g_leaveCutouts = false;
bool g_leaveOthers = true;   // until "offscreen" is on the test words
std::vector<RtaoCaptureView> g_views;
std::vector<RtaoRun> g_runs;
std::atomic<long long> g_otherDraws{0}, g_otherTris{0};
bool g_solidCutouts = false;
float g_worldToProj[16] = {};
ID3D11Buffer* g_buf = nullptr;      // the positions, 16 bytes a vertex
UINT g_bufBytes = 0;
ID3D11Buffer* g_uvBuf = nullptr;    // the cut outs' uv and alpha, 12 bytes a vertex, at the same vertex as its position
UINT g_uvBytes = 0;
ID3D11Buffer* g_normBuf = nullptr;  // the smooth normals, 12 bytes a vertex, at the same vertex as its position
UINT g_normBytes = 0;
bool g_flatNormals = false;         // "flatnormals": no smooth normals, the AO turned by the faces as before
UINT g_vertices = 0;
uint64_t g_drawList = 0;   // FNV-1a over each taken draw's shaders and counts
// Asked for when a frame ran out of room: the next frame's buffer is made at least this big.
UINT g_want = 64u << 20;
bool g_overflowed = false;
std::vector<RtaoCutout> g_cutouts;
std::atomic<long long> g_taken{0}, g_skipped{0}, g_overflow{0};
std::atomic<long long> g_cutoutDraws{0}, g_cutoutTris{0};

// --- The masks: g_tColor's alpha, drawn each frame into a pool of our own textures shared with D3D12.
struct Mask {
    ID3D11Texture2D* tex = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D12Resource* tex12 = nullptr;
    ID3D11Resource* of = nullptr;   // the engine's texture it was drawn from this frame (not held)
    uint64_t frame = 0;
};
Mask g_masks[RTAO_MASKS];
uint64_t g_frame = 0;
ID3D11ComputeShader* g_maskCs = nullptr;
ID3D11SamplerState* g_maskSampler = nullptr;
ID3D11Buffer* g_maskCb = nullptr;
bool g_maskBroken = false;

const char kMaskCs[] =
    "Texture2D<float4> Src : register(t0);\n"
    "SamplerState Lin : register(s0);\n"
    "RWTexture2D<unorm float> Dst : register(u0);\n"
    "cbuffer C : register(b0) { float lod; float3 pad; };\n"
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "    const float2 uv = (float2(id.xy) + 0.5f) / 512.0f;\n"
    "    Dst[id.xy] = Src.SampleLevel(Lin, uv, lod).w;\n"
    "}\n";

bool EnsureMaskShader(ID3D11Device* dev) {
    if (g_maskBroken) return false;
    if (g_maskCs) return true;
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    if (SUCCEEDED(D3DCompile(kMaskCs, sizeof(kMaskCs) - 1, "rtao_mask", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code,
                             &err)) && code)
        dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_maskCs);
    if (err) { Log("[RTAO] the mask shader: %s", (const char*)err->GetBufferPointer()); err->Release(); }
    if (code) code->Release();
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_maskSampler);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &g_maskCb);
    if (!g_maskCs || !g_maskSampler || !g_maskCb) {
        g_maskBroken = true;
        Log("[RTAO] the cut outs' masks could not be made: cut outs are traced as solid");
    }
    return !g_maskBroken;
}

// The mask of an engine texture this frame: drawn once a frame into a free one. -1 when there is none left or it
// could not be made.
int MaskOf(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv) {
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (!res) return -1;
    res->Release();   // not held: only compared within the frame, while the engine has it bound
    for (int i = 0; i < RTAO_MASKS; ++i)
        if (g_masks[i].frame == g_frame && g_masks[i].of == res) return i;
    int k = -1;
    for (int i = 0; i < RTAO_MASKS && k < 0; ++i) if (g_masks[i].frame != g_frame) k = i;
    if (k < 0) return -1;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev || !EnsureMaskShader(dev)) { if (dev) dev->Release(); return -1; }
    Mask& m = g_masks[k];
    if (!m.tex) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = d.Height = 512;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &m.tex)) || FAILED(dev->CreateUnorderedAccessView(m.tex, nullptr, &m.uav))
            || !(m.tex12 = RtaoOpenTexture(m.tex))) {
            if (m.uav) { m.uav->Release(); m.uav = nullptr; }
            if (m.tex) { m.tex->Release(); m.tex = nullptr; }
            dev->Release();
            return -1;
        }
    }
    dev->Release();
    // The source's level nearest 512 texels across.
    float lod = 0.0f;
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC td = {};
        ((ID3D11Texture2D*)res)->GetDesc(&td);
        const float across = (float)(td.Width > td.Height ? td.Width : td.Height);
        lod = across > 512.0f ? std::log2(across / 512.0f) : 0.0f;
    }
    const float cb[4] = { lod, 0.0f, 0.0f, 0.0f };
    ID3D11ComputeShader* oldCs = nullptr;
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv = nullptr;
    ctx->CSGetShaderResources(0, 1, &oldSrv);
    ID3D11UnorderedAccessView* oldUav = nullptr;
    ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
    ID3D11SamplerState* oldSampler = nullptr;
    ctx->CSGetSamplers(0, 1, &oldSampler);
    ID3D11Buffer* oldCb = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &oldCb);
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    ctx->UpdateSubresource(g_maskCb, 0, nullptr, cb, 0, 0);
    ctx->CSSetShader(g_maskCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &m.uav, nullptr);
    ctx->CSSetSamplers(0, 1, &g_maskSampler);
    ctx->CSSetConstantBuffers(0, 1, &g_maskCb);
    ctx->Dispatch(512 / 8, 512 / 8, 1);
    const UINT keep = (UINT)-1;
    ctx->CSSetShader(oldCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &oldSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
    ctx->CSSetSamplers(0, 1, &oldSampler);
    ctx->CSSetConstantBuffers(0, 1, &oldCb);
    t_CorePassThrough = pass;
    if (oldCs) oldCs->Release();
    if (oldSrv) oldSrv->Release();
    if (oldUav) oldUav->Release();
    if (oldSampler) oldSampler->Release();
    if (oldCb) oldCb->Release();
    m.of = res;
    m.frame = g_frame;
    return k;
}

}  // namespace

void RtaoCaptureLeaveCutouts(bool leave) {
    std::lock_guard<std::mutex> lk(g_m);
    g_leaveCutouts = leave;
}

void RtaoCaptureSolidCutouts(bool solid) {
    std::lock_guard<std::mutex> lk(g_m);
    g_solidCutouts = solid;
}

void RtaoCaptureFlatNormals(bool flat) {
    std::lock_guard<std::mutex> lk(g_m);
    g_flatNormals = flat;
}

void RtaoCaptureLeaveOthers(bool leave) {
    std::lock_guard<std::mutex> lk(g_m);
    g_leaveOthers = leave;
}

namespace {

// A new frame's capture: empty, the camera's view first (filled in by the rays' side).
void Reset() {
    g_vertices = 0;
    g_drawList = 1469598103934665603ull;
    g_cutouts.clear();
    g_views.assign(1, RtaoCaptureView{});
    g_runs.clear();
    ++g_frame;
    if (g_overflowed) {
        g_overflowed = false;
        g_overflow.fetch_add(1, std::memory_order_relaxed);
    }
}

// The draw again with stream output into the capture, for the view at that index.
void Take(ID3D11DeviceContext* ctx, ContextState* st, UINT view);

}  // namespace

void RtaoCaptureBegin(const float worldToProj[16]) {
    std::lock_guard<std::mutex> lk(g_m);
    memcpy(g_worldToProj, worldToProj, sizeof(g_worldToProj));
    if (!g_pre) Reset();
    g_pre = false;
    g_open = true;
}

void RtaoCaptureDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    if (!ctx || !st || st->deferred || !st->vs) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_open) return;
    Take(ctx, st, 0);
}

void RtaoCaptureOther(ID3D11DeviceContext* ctx, ContextState* st) {
    if (!ctx || !st || st->deferred || !st->vs) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (g_open || g_leaveOthers) return;
    float pv[204];
    if (!CorePerView(st, pv)) return;
    if (!g_pre) {
        Reset();
        g_pre = true;
    }
    UINT view = 0;
    for (UINT i = 1; i < g_views.size() && !view; ++i)
        if (!memcmp(g_views[i].worldToProj, &pv[PV_MAT_WORLD_TO_PROJ_MV], 64)
            && !memcmp(g_views[i].offset, &pv[PV_WORLD_TO_CAM_OFF_MV], 12))
            view = i;
    if (!view) {
        if (g_views.size() >= RTAO_VIEWS) {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        RtaoCaptureView v = {};
        memcpy(v.worldToProj, &pv[PV_MAT_WORLD_TO_PROJ_MV], 64);
        memcpy(v.offset, &pv[PV_WORLD_TO_CAM_OFF_MV], 12);
        view = (UINT)g_views.size();
        g_views.push_back(v);
    }
    const UINT before = g_vertices;
    Take(ctx, st, view);
    if (g_vertices > before) {
        g_otherDraws.fetch_add(1, std::memory_order_relaxed);
        g_otherTris.fetch_add((g_vertices - before) / 3, std::memory_order_relaxed);
    }
}

void RtaoCaptureDrop() {
    std::lock_guard<std::mutex> lk(g_m);
    g_pre = false;
}

const std::vector<RtaoCaptureView>& RtaoCaptureViewList() { return g_views; }
const std::vector<RtaoRun>& RtaoCaptureRunList() { return g_runs; }

void RtaoCaptureOthers(long long* draws, long long* triangles) {
    *draws = g_otherDraws.exchange(0, std::memory_order_relaxed);
    *triangles = g_otherTris.exchange(0, std::memory_order_relaxed);
}

namespace {

void Take(ID3D11DeviceContext* ctx, ContextState* st, UINT view) {
    const ContextState::Draw& d = st->draw;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topo);
    ID3D11HullShader* hs = nullptr;
    ctx->HSGetShader(&hs, nullptr, nullptr);
    if (hs) hs->Release();
    if (d.indirect || topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || hs || d.count < 3) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const CutInfo& cut = CutOf(st);
    if (g_leaveCutouts && cut.cut) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The camera's part drawn with the frame's camera: its triangles go to the rays' space by that camera.
    float pv[204];
    if (!view && !CorePerView(st, pv)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (int i = 0; i < 16 && !view; ++i) {
        if (std::fabs(pv[PV_MAT_WORLD_TO_PROJ_MV + i] - g_worldToProj[i]) > 1e-5f * (1.0f + std::fabs(g_worldToProj[i]))) {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    const UINT out = (d.count / 3) * 3 * (d.instances ? d.instances : 1);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    if (!g_buf || (g_vertices == 0 && g_bufBytes < g_want)) {
        ID3D11Buffer* b = nullptr;
        ID3D11Buffer* u = nullptr;
        ID3D11Buffer* nb = nullptr;
        UINT made = 0, uvMade = 0, normMade = 0;
        if (RtaoCaptureBuffer(dev, g_want, &b, &made) && RtaoUvBuffer(dev, g_want / 16 * 12, &u, &uvMade)) {
            g_buf = b;
            g_bufBytes = made;
            g_uvBuf = u;
            g_uvBytes = uvMade;
        }
        if (RtaoNormalBuffer(dev, g_want / 16 * 12, &nb, &normMade)) {
            g_normBuf = nb;
            g_normBytes = normMade;
        }
    }
    // A cut out whose alpha can be followed, and its mask.
    int mask = -1;
    if (cut.traced && !g_solidCutouts && g_uvBuf && cut.texSlot >= 0 && cut.texSlot < 32 && st->psSrv[cut.texSlot])
        mask = MaskOf(ctx, st->psSrv[cut.texSlot]);
    // The smooth normal too, for the camera's own draws (the ones the AO starts from), when its buffer has room.
    bool normal = !view && !g_flatNormals && g_normBuf && (UINT64)(g_vertices + out) * 12 <= g_normBytes;
    ID3D11GeometryShader* gs = nullptr;
    if (g_buf && mask >= 0) {
        ID3D11GeometryShader* withNormal = normal ? SoFor(dev, st->vs, cut.uvReg, cut.alphaReg, true) : nullptr;
        gs = withNormal ? withNormal : SoFor(dev, st->vs, cut.uvReg, cut.alphaReg, false);
        normal = withNormal != nullptr;
    }
    if (!gs) {
        mask = -1;
        if (g_buf && normal) gs = SoFor(dev, st->vs, -1, -1, true);
        if (!gs) {
            normal = false;
            gs = g_buf ? SoFor(dev, st->vs, -1, -1, false) : nullptr;
        }
    }
    dev->Release();
    if (!gs) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if ((UINT64)(g_vertices + out) * 16 > g_bufBytes || (mask >= 0 && (UINT64)(g_vertices + out) * 12 > g_uvBytes)) {
        // No room: the next frame's buffer is made big enough for this one's.
        g_overflowed = true;
        const UINT64 need = (UINT64)(g_vertices + out) * 16;
        const UINT64 want = need + need / 4;
        g_want = (UINT)(want > 512ull << 20 ? 512ull << 20 : want);   // 32 million vertices at most
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const UINT flags = normal ? 1u : 0u;
    if (g_runs.empty() || g_runs.back().view != view || g_runs.back().flags != flags) {
        if (g_runs.size() >= RTAO_RUNS) {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_runs.push_back(RtaoRun{ g_vertices, view, flags });
    }

    // What is bound there now, put back after.
    ID3D11GeometryShader* oldGs = nullptr;
    ID3D11ClassInstance* oldCi[256];
    UINT oldCiN = 256;
    ctx->GSGetShader(&oldGs, oldCi, &oldCiN);
    ID3D11Buffer* oldSo[3] = {};
    ctx->SOGetTargets(3, oldSo);

    ID3D11Buffer* targets[3] = { g_buf, mask >= 0 ? g_uvBuf : nullptr, normal ? g_normBuf : nullptr };
    const UINT offsets[3] = { g_vertices * 16, g_vertices * 12, g_vertices * 12 };
    ctx->GSSetShader(gs, nullptr, 0);
    ctx->SOSetTargets(normal ? 3 : (mask >= 0 ? 2 : 1), targets, offsets);
    CoreRedraw(ctx, st);
    // An offset of -1 appends; the engine had none bound (or its own, which it set itself).
    const UINT append[3] = { (UINT)-1, (UINT)-1, (UINT)-1 };
    ctx->SOSetTargets(3, oldSo, append);
    ctx->GSSetShader(oldGs, oldCi, oldCiN);
    for (ID3D11Buffer* b : oldSo) if (b) b->Release();
    if (oldGs) oldGs->Release();
    for (UINT i = 0; i < oldCiN; ++i) if (oldCi[i]) oldCi[i]->Release();
    if (mask >= 0) {
        RtaoCutout c = {};
        c.firstVertex = g_vertices;
        c.vertexCount = out;
        c.mask = (UINT)mask;
        c.reference = -1.0f;
        if (cut.refOffset >= 0) {
            float ref = 0.5f;
            if (CoreCbRead(st->psCb[0], (UINT)cut.refOffset * 4, &ref, 4)) c.reference = ref;
        }
        c.vertexAlpha = cut.alphaReg >= 0 ? 1u : 0u;
        g_cutouts.push_back(c);
    }
    g_vertices += out;
    const uint64_t parts[5] = { (uint64_t)(uintptr_t)st->vs, (uint64_t)(uintptr_t)st->ps, out, d.start, view };
    for (uint64_t v : parts)
        for (int b = 0; b < 8; ++b) g_drawList = (g_drawList ^ ((v >> (b * 8)) & 0xFF)) * 1099511628211ull;
    g_taken.fetch_add(1, std::memory_order_relaxed);
    if (cut.cut) {
        g_cutoutDraws.fetch_add(1, std::memory_order_relaxed);
        g_cutoutTris.fetch_add(out / 3, std::memory_order_relaxed);
        static std::unordered_map<ID3D11PixelShader*, bool> said;
        if (!said[st->ps] && said.size() < 24)
            Log("[RTAO] a cut out%s: %s, %u triangles, %s", view ? " of another view" : "",
                st->psLayout >= 0 ? ShaderDbName(st->psLayout) : "?", out / 3,
                mask >= 0 ? "judged by its mask" : "traced as solid");
        said[st->ps] = true;
    }
}

}  // namespace

UINT RtaoCaptureVertices() {
    std::lock_guard<std::mutex> lk(g_m);
    g_open = false;
    g_pre = false;
    return g_vertices;
}

const std::vector<RtaoCutout>& RtaoCaptureCutoutList() { return g_cutouts; }

void RtaoCaptureMasks(ID3D12Resource* out[RTAO_MASKS]) {
    std::lock_guard<std::mutex> lk(g_m);
    for (int i = 0; i < RTAO_MASKS; ++i) out[i] = g_masks[i].tex12;
}

uint64_t RtaoCaptureDrawList() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_drawList;
}

void RtaoCaptureCutouts(long long* draws, long long* triangles) {
    *draws = g_cutoutDraws.exchange(0, std::memory_order_relaxed);
    *triangles = g_cutoutTris.exchange(0, std::memory_order_relaxed);
}

void RtaoCaptureStats(long long* taken, long long* skipped, long long* overflow) {
    *taken = g_taken.exchange(0, std::memory_order_relaxed);
    *skipped = g_skipped.exchange(0, std::memory_order_relaxed);
    *overflow = g_overflow.exchange(0, std::memory_order_relaxed);
}
