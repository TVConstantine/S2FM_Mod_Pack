// The ReShade tool's stage points. S2FM draws a view's frame on the
// engine's immediate context in this order: the opaque surfaces (the AO readers, pixel shaders with
// g_tDynamicAmbientOcclusion), the sky, fog and see-through surfaces, the bloom, post_process into a half float
// target, then while refining sfm_accumulation adds that into its sum, and copytexture makes the shown 8-bit picture.
// The stages:
//   A  at the first draw of a pass after the opaque surfaces, on the target the AO readers drew into;
//   B  after post_process draws, on its target;
//   C  after the copytexture that follows post_process (or sfm_accumulation), on its 8-bit target.
// "probe" on the line of s2fm_reshade_debug.txt (beside the DLL) writes what the stage points found for 8 presents.
#include "../core/core.h"
#include "../common/shader_table.h"
#include "../common/sfm_doc.h"
#include "rs_stage_api.h"

#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// The main proxy's helpers, on the core.
#define t_DxrStagePassThrough t_CorePassThrough
namespace {
struct StageDrawInfo {
    int psLayout = -1, psCombo = -1;
    UINT rtW = 0, rtH = 0;
    DXGI_FORMAT rtFormat = DXGI_FORMAT_UNKNOWN;
    bool blend = false;
    uint8_t blendSrc = 0, blendDst = 0;
    ID3D11ShaderResourceView* const* psSrv = nullptr;
};
bool StageDrawInfoOf(ContextState* st, StageDrawInfo* out) {
    if (!st || !out) return false;
    out->psLayout = st->psLayout;
    out->psCombo = st->psCombo;
    out->rtW = st->rtW;
    out->rtH = st->rtH;
    out->rtFormat = st->rtFormat;
    out->blend = st->blend;
    out->blendSrc = st->blendSrc;
    out->blendDst = st->blendDst;
    out->psSrv = st->psSrv;
    return true;
}
// The slot the bound pixel shader reads a texture parameter at, by its name, or -1.
int StageTextureSlot(ContextState* st, const char* name) {
    uint8_t reg = 0;
    uint16_t offset = 0;
    return st && st->psLayout >= 0 && ShaderDbFind(st->psLayout, name, &reg, &offset) && reg == SDB_REG_TEXTURE
               ? (int)(offset & 0xFF) : -1;
}
// The PerView of the draw about to be made, when it is the w x h view's: its world to projection matrix and the
// camera's tone map scalars.
bool StagePerViewProjection(ContextState* st, UINT w, UINT h, float worldToProj[16], float* toneMapLinear,
                            float* toneMapGamma) {
    float pv[204];
    if (!CorePerView(st, pv)) return false;
    const float vw = pv[PV_VIEWPORT_SIZE], vh = pv[PV_VIEWPORT_SIZE + 1];
    if (!std::isfinite(vw) || !std::isfinite(vh) || (UINT)(vw + 0.5f) != w || (UINT)(vh + 0.5f) != h) return false;
    memcpy(worldToProj, &pv[PV_MAT_WORLD_TO_PROJ_MV], 16 * sizeof(float));
    if (toneMapLinear) *toneMapLinear = pv[PV_TONEMAP_LINEAR];
    if (toneMapGamma) *toneMapGamma = pv[PV_TONEMAP_GAMMA];
    return true;
}
inline bool DxrFrameForeign(ContextState* st) { return CoreFrameForeign(st); }
inline bool DxrSwapChainIsS2fm(IDXGISwapChain* sc) { return CoreSwapChainIsS2fm(sc); }
inline bool DxrExportActive() { return CoreExportActive(); }
}  // namespace

// --- The core (tools/s2fm_reshade_core.dll) registers its stage callback here (rs_stage_api.h).
static std::atomic<DxrStageCallback> g_stageCb{nullptr};
static std::atomic<void*> g_stageUser{nullptr};
static std::atomic<int> g_stageCalls[3] = {}, g_stageDrew[3] = {};

extern "C" int __cdecl DxrStages_ApiVersion() { return DXR_STAGES_API_VERSION; }

// The panel changed something the engine does not know of: S2FM draws the view again (common/sfm_doc.cpp).
bool SfmRedraw();
extern "C" int __cdecl DxrStages_Redraw() { return SfmRedraw() ? 1 : 0; }

// The open session's name (the core keeps a preset for each), "" when none is open.
extern "C" int __cdecl DxrStages_SessionName(char* out, int size) {
    if (!out || size <= 0) return 0;
    out[0] = 0;
    double seconds = 0.0, fps = 24.0;
    return SfmPlayhead(&seconds, &fps, out, size) ? 1 : 0;
}

extern "C" int __cdecl DxrStages_Register(DxrStageCallback cb, void* user) {
    g_stageUser.store(user);
    g_stageCb.store(cb);
    Log("[RESHADE] stage callback %s\n", cb ? "registered" : "removed");
    return 1;
}

namespace {

// What a pixel shader is to the stages, by its name in the shader table, kept per layout.
enum Kind : int8_t {
    KIND_UNKNOWN = -1, KIND_OTHER = 0, KIND_AFTER_OPAQUE, KIND_POST, KIND_ACCUM, KIND_COPY,
};
const char* const kAfterNames[] = { "sky", "sfm_volumetrics_frustum", "downsample_depth", "gaussian_bloom_blur" };

std::vector<int8_t> g_kind;          // by layout
std::vector<int8_t> g_aoReader;      // by layout: -1 not asked, 0 no, 1 reads g_tDynamicAmbientOcclusion
std::vector<int8_t> g_depthSlot;     // by layout: slot of g_tDynamicAmbientOcclusionDepth, or -1; -2 not asked

Kind KindOf(int layout) {
    if (layout < 0) return KIND_OTHER;
    if ((size_t)layout >= g_kind.size()) g_kind.resize((size_t)layout + 1, KIND_UNKNOWN);
    int8_t& k = g_kind[(size_t)layout];
    if (k != KIND_UNKNOWN) return (Kind)k;
    const char* name = ShaderDbName(layout);
    k = KIND_OTHER;
    if (name && *name) {
        if (!strcmp(name, "post_process")) k = KIND_POST;
        else if (!strcmp(name, "sfm_accumulation")) k = KIND_ACCUM;
        else if (!strcmp(name, "copytexture")) k = KIND_COPY;
        else for (const char* a : kAfterNames) if (!strcmp(name, a)) k = KIND_AFTER_OPAQUE;
    }
    return (Kind)k;
}

bool IsAoReader(ContextState* st, int layout) {
    if (layout < 0) return false;
    if ((size_t)layout >= g_aoReader.size()) {
        g_aoReader.resize((size_t)layout + 1, -1);
        g_depthSlot.resize((size_t)layout + 1, -2);
    }
    int8_t& r = g_aoReader[(size_t)layout];
    if (r < 0) {
        r = StageTextureSlot(st, "g_tDynamicAmbientOcclusion") >= 0 ? 1 : 0;
        g_depthSlot[(size_t)layout] = (int8_t)StageTextureSlot(st, "g_tDynamicAmbientOcclusionDepth");
    }
    return r == 1;
}

// --- The switch file and the probe's state.
std::mutex g_m;
std::string g_line;
bool g_probeWanted = false;
int g_probeLeft = 0;                 // presents still to record
std::vector<std::string> g_probeBlocks;

// --- One present's stage points (render thread only, the immediate context).
struct PresentState {
    bool aoReaderSeen = false;
    bool aFound = false, postSeen = false, bFound = false, cFound = false;
    bool accumSeen = false;
    int lone = 0;                    // copytextures with no post_process before them
    ID3D11RenderTargetView* sceneRtv = nullptr;
    ID3D11ShaderResourceView* depthSrv = nullptr;
    ID3D11DepthStencilView* sceneDsv = nullptr;   // the first AO reader's depth target (MakeViewZ)
    float minDepth = 0.0f, maxDepth = 1.0f;
    float proj[16] = {};
    bool projValid = false;
    float toneMapLinear = -1.0f, toneMapGamma = -1.0f;
    bool recFromStart = false;       // the probe was on when this present began
    bool viewZMade = false;          // the view distance was made from this present's depth
    bool clockRead = false;
    double seconds = 0.0, fps = 24.0;
    std::string block;               // the probe's text for this present
    std::map<const void*, int> ids;  // resource -> short number in the probe's text
};
PresentState g_ps;
// What a draw needs after it: its kind and the target it drew into (B and C).
thread_local Kind t_afterKind = KIND_OTHER;
thread_local bool t_afterPending = false;
// Refinement: a sfm_accumulation draw without blending begins a run.
int g_sampleIndex = 0;
bool g_refining = false;

// --- The view distance every stage of a present gets (rs_stage_api.h viewZ): the scene's depth undone through
// the PerView projection. The depth target the AO readers draw into, copied once the opaque surfaces are done
// (for example 1280x720 R24G8, 8 samples; the first sample is taken). It was the AO readers' t9 (R24, the
// picture at its top left), which the engine makes from its depth prepass and which lacks some models
// (characters among them). t9 stays the way when the target cannot be copied.
// Clip z and w are both linear in the view distance, so zndc = a + b / z: z = b / (zndc - a), with
// zndc = (d - minDepth) / (maxDepth - minDepth). Sky and the clear (1.0, above the 0.05 to 0.95 range) are 0.
const char kViewZCs[] = R"(
#ifdef MULTISAMPLED
Texture2DMS<float> Depth : register(t0);
#define DEPTH_AT(p) Depth.Load(p, 0)
#else
Texture2D<float> Depth : register(t0);
#define DEPTH_AT(p) Depth[p]
#endif
RWTexture2D<float> ViewZ : register(u0);
cbuffer C : register(b0) { float a; float b; float minD; float rcpRange; uint width; uint height; float2 pad; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    const float zndc = (DEPTH_AT(id.xy) - minD) * rcpRange;
    const float z = b / (zndc - a);
    ViewZ[id.xy] = (zndc > 1.000001 || zndc < -0.000001 || !(z > 0.0)) ? 0.0 : z;
}
)";
ID3D11ComputeShader* g_vzCs = nullptr;
ID3D11ComputeShader* g_vzCsMs = nullptr;
// The scene's depth target copied so it can be read, and how many view distances came from it or from t9.
ID3D11Texture2D* g_sdTex = nullptr;
ID3D11ShaderResourceView* g_sdSrv = nullptr;
D3D11_TEXTURE2D_DESC g_sdDesc = {};
std::atomic<int> g_viewZScene{0}, g_viewZT9{0};
// "t9depth" on the switch: the view distance from t9 as before, for comparing.
bool g_t9Depth = false;
ID3D11Buffer* g_vzCb = nullptr;
ID3D11Texture2D* g_vzTex = nullptr;
ID3D11UnorderedAccessView* g_vzUav = nullptr;
ID3D11ShaderResourceView* g_vzSrv = nullptr;
UINT g_vzW = 0, g_vzH = 0;
bool g_vzBroken = false;

// a and b of zndc = a + b / z from the world to projection matrix: its z and w rows, or columns, whichever
// pair is parallel (the probe found columns).
bool ProjectionAB(const float* m, double* a, double* b) {
    double best = -1.0;
    for (int asColumns = 0; asColumns < 2; ++asColumns) {
        double r2[4], r3[4];
        for (int k = 0; k < 4; ++k) {
            r2[k] = asColumns ? m[k * 4 + 2] : m[2 * 4 + k];
            r3[k] = asColumns ? m[k * 4 + 3] : m[3 * 4 + k];
        }
        const double d33 = r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2];
        const double d23 = r2[0] * r3[0] + r2[1] * r3[1] + r2[2] * r3[2];
        const double d22 = r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2];
        if (d33 <= 0.0 || d22 <= 0.0) continue;
        const double c = std::fabs(d23 / std::sqrt(d33 * d22));
        if (c > best) {
            best = c;
            *a = d23 / d33;
            *b = r2[3] - *a * r3[3];
        }
    }
    return best > 0.999;
}

// The scene's depth target into g_sdTex (its typeless family, readable), or null when it cannot be: the view to
// read it through, and whether it has more than one sample.
ID3D11ShaderResourceView* SceneDepthCopy(ID3D11DeviceContext* ctx, ID3D11Device* dev, UINT w, UINT h, bool* ms) {
    if (!g_ps.sceneDsv || !g_vzCsMs) return nullptr;
    ID3D11Resource* res = nullptr;
    g_ps.sceneDsv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    if (res) res->Release();
    if (!tex) return nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    DXGI_FORMAT family = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN;
    switch (d.Format) {
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
        family = DXGI_FORMAT_R24G8_TYPELESS; view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT:
        family = DXGI_FORMAT_R32_TYPELESS; view = DXGI_FORMAT_R32_FLOAT; break;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        family = DXGI_FORMAT_R32G8X24_TYPELESS; view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM:
        family = DXGI_FORMAT_R16_TYPELESS; view = DXGI_FORMAT_R16_UNORM; break;
    default: break;
    }
    bool ok = family != DXGI_FORMAT_UNKNOWN && d.ArraySize == 1 && d.Width >= w && d.Height >= h;
    if (ok && (!g_sdTex || g_sdDesc.Width != d.Width || g_sdDesc.Height != d.Height || g_sdDesc.Format != d.Format
               || g_sdDesc.SampleDesc.Count != d.SampleDesc.Count || g_sdDesc.SampleDesc.Quality != d.SampleDesc.Quality
               || g_sdDesc.MipLevels != d.MipLevels)) {
        if (g_sdTex) { g_sdTex->Release(); g_sdTex = nullptr; }
        if (g_sdSrv) { g_sdSrv->Release(); g_sdSrv = nullptr; }
        D3D11_TEXTURE2D_DESC c = d;
        c.Format = family;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        c.CPUAccessFlags = 0;
        c.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = view;
        if (d.SampleDesc.Count > 1) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
        }
        ok = SUCCEEDED(dev->CreateTexture2D(&c, nullptr, &g_sdTex)) && SUCCEEDED(dev->CreateShaderResourceView(g_sdTex, &sd, &g_sdSrv));
        g_sdDesc = d;
        Log("[RESHADE] the scene's depth target: %ux%u, format %u, %u samples; %s\n", d.Width, d.Height, (UINT)d.Format,
            d.SampleDesc.Count, ok ? "the view distance is made from it" : "COULD NOT be copied, the view distance keeps t9");
        if (!ok) {
            if (g_sdTex) { g_sdTex->Release(); g_sdTex = nullptr; }
            if (g_sdSrv) { g_sdSrv->Release(); g_sdSrv = nullptr; }
        }
    }
    ok = ok && g_sdTex && g_sdSrv;
    if (ok) ctx->CopyResource(g_sdTex, tex);
    tex->Release();
    *ms = d.SampleDesc.Count > 1;
    return ok ? g_sdSrv : nullptr;
}

bool MakeViewZ(ID3D11DeviceContext* ctx, UINT w, UINT h) {
    if (g_vzBroken || (!g_ps.depthSrv && !g_ps.sceneDsv) || !g_ps.projValid || !w || !h) return false;
    double a = 0.0, b = 0.0;
    if (!ProjectionAB(g_ps.proj, &a, &b)) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (!g_vzCs) {
        ID3DBlob* code = nullptr;
        if (FAILED(D3DCompile(kViewZCs, sizeof(kViewZCs) - 1, "reshade_viewz", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                              &code, nullptr)) || !code
            || FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_vzCs))) {
            g_vzBroken = true;
            Log("[RESHADE] the view distance shader could not be made\n");
        }
        if (code) code->Release();
        // The scene's depth target has more than one sample a pixel; without this one t9 stays the depth.
        code = nullptr;
        const D3D_SHADER_MACRO ms[] = { { "MULTISAMPLED", "1" }, { nullptr, nullptr } };
        if (!g_vzBroken && SUCCEEDED(D3DCompile(kViewZCs, sizeof(kViewZCs) - 1, "reshade_viewz_ms", ms, nullptr, "main",
                                                "cs_5_0", 0, 0, &code, nullptr)) && code)
            dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_vzCsMs);
        if (code) code->Release();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (!g_vzBroken && FAILED(dev->CreateBuffer(&bd, nullptr, &g_vzCb))) g_vzBroken = true;
    }
    if (!g_vzBroken && (!g_vzTex || g_vzW != w || g_vzH != h)) {
        if (g_vzSrv) { g_vzSrv->Release(); g_vzSrv = nullptr; }
        if (g_vzUav) { g_vzUav->Release(); g_vzUav = nullptr; }
        if (g_vzTex) { g_vzTex->Release(); g_vzTex = nullptr; }
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_vzTex))) {
            dev->CreateUnorderedAccessView(g_vzTex, nullptr, &g_vzUav);
            dev->CreateShaderResourceView(g_vzTex, nullptr, &g_vzSrv);
        }
        g_vzW = w;
        g_vzH = h;
    }
    bool ms = false;
    ID3D11ShaderResourceView* src = (!g_vzBroken && g_vzUav && !g_t9Depth) ? SceneDepthCopy(ctx, dev, w, h, &ms) : nullptr;
    dev->Release();
    if (!src) { src = g_ps.depthSrv; ms = false; }
    if (g_vzBroken || !g_vzUav || !g_vzSrv || !src) return false;
    (src == g_ps.depthSrv ? g_viewZT9 : g_viewZScene).fetch_add(1, std::memory_order_relaxed);
    struct { float a, b, minD, rcpRange; UINT width, height; float pad[2]; } c = {
        (float)a, (float)b, g_ps.minDepth, 1.0f / (g_ps.maxDepth - g_ps.minDepth), w, h, { 0, 0 } };
    ctx->UpdateSubresource(g_vzCb, 0, nullptr, &c, 0, 0);
    ID3D11ComputeShader* oldCs = nullptr;
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv = nullptr;
    ctx->CSGetShaderResources(0, 1, &oldSrv);
    ID3D11UnorderedAccessView* oldUav = nullptr;
    ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
    ID3D11Buffer* oldCb = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &oldCb);
    t_DxrStagePassThrough = true;
    ctx->CSSetShader(ms ? g_vzCsMs : g_vzCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &src);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_vzUav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g_vzCb);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11UnorderedAccessView* none = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    const UINT keep = (UINT)-1;
    ctx->CSSetShader(oldCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &oldSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
    ctx->CSSetConstantBuffers(0, 1, &oldCb);
    t_DxrStagePassThrough = false;
    if (oldCs) oldCs->Release();
    if (oldSrv) oldSrv->Release();
    if (oldUav) oldUav->Release();
    if (oldCb) oldCb->Release();
    return true;
}

// "viewz" with "dump" on the switch: the view distance written as a float picture (s2fm_reshade_viewz.pfm).
bool g_viewZDumpPending = false;
void DumpViewZ(ID3D11DeviceContext* ctx) {
    if (!g_vzTex) return;
    D3D11_TEXTURE2D_DESC d = {};
    g_vzTex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11Texture2D* st = nullptr;
    if (dev && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &st)) && st) {
        ctx->CopyResource(st, g_vzTex);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE* f = nullptr;
            if (_wfopen_s(&f, CorePath(L"s2fm_reshade_viewz.pfm").c_str(), L"wb") == 0 && f) {
                fprintf(f, "Pf\n%u %u\n-1.0\n", d.Width, d.Height);
                for (UINT y = d.Height; y-- > 0;)   // PFM rows run bottom to top
                    fwrite((const uint8_t*)m.pData + (size_t)y * m.RowPitch, 4, d.Width, f);
                fclose(f);
            }
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    if (dev) dev->Release();
}

// The core: loaded by the proxy when the switch says "core" (later: when the remembered state says effects
// are on), from the game's tools folder beside the other engine tools.
bool g_wantCore = false, g_coreTried = false;
// "dump" on the switch: the shown picture, after stage C, written to s2fm_reshade_picture.ppm once the line has
// stood for 1.5 s.
bool g_dumpPending = false;
ULONGLONG g_dumpAt = 0;

// Counters for the 10 s log line.
std::atomic<int> g_playheadReads{0}, g_playheadFails{0};
double g_lastSeconds = 0.0;
std::atomic<int> g_presentsWithPost{0}, g_presentsA{0}, g_presentsB{0}, g_presentsC{0}, g_presentsLone{0};

void ReleaseRef(IUnknown*& p) { if (p) { p->Release(); p = nullptr; } }

bool Recording() { return g_probeLeft > 0; }

void ResetPresent() {
    IUnknown* r = g_ps.sceneRtv; ReleaseRef(r);
    IUnknown* d = g_ps.depthSrv; ReleaseRef(d);
    IUnknown* z = g_ps.sceneDsv; ReleaseRef(z);
    g_ps = PresentState();
    g_ps.recFromStart = Recording();
}

int IdOf(const void* res) {
    if (!res) return 0;
    auto it = g_ps.ids.find(res);
    if (it != g_ps.ids.end()) return it->second;
    const int id = (int)g_ps.ids.size() + 1;
    g_ps.ids[res] = id;
    return id;
}

void Append(const char* fmt, ...) {
    char b[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    g_ps.block += b;
}

std::string DescribeTex(ID3D11Resource* res) {
    if (!res) return "none";
    ID3D11Texture2D* t = nullptr;
    char b[160];
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t) {
        sprintf_s(b, "r%d (not a 2D texture)", IdOf(res));
        return b;
    }
    D3D11_TEXTURE2D_DESC d = {};
    t->GetDesc(&d);
    t->Release();
    sprintf_s(b, "r%d %ux%u fmt %u samples %u bind 0x%x", IdOf(res), d.Width, d.Height, (UINT)d.Format,
              d.SampleDesc.Count, d.BindFlags);
    return b;
}

std::string DescribeView(ID3D11View* v) {
    if (!v) return "none";
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    std::string s = DescribeTex(r);
    if (r) r->Release();
    char b[48];
    DXGI_FORMAT vf = DXGI_FORMAT_UNKNOWN;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    if (SUCCEEDED(v->QueryInterface(__uuidof(ID3D11RenderTargetView), (void**)&rtv)) && rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC d; rtv->GetDesc(&d); vf = d.Format; rtv->Release();
    } else if (SUCCEEDED(v->QueryInterface(__uuidof(ID3D11ShaderResourceView), (void**)&srv)) && srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d; srv->GetDesc(&d); vf = d.Format; srv->Release();
    } else if (SUCCEEDED(v->QueryInterface(__uuidof(ID3D11DepthStencilView), (void**)&dsv)) && dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC d; dsv->GetDesc(&d); vf = d.Format; dsv->Release();
    }
    sprintf_s(b, " view fmt %u", (UINT)vf);
    return s + b;
}

// --- Read back (probe only): a texture's texels on a grid, resolved first when it has samples. Returns
// floats, 4 per texel (depth formats give the depth in x), and the grid's size.
float HalfToFloat(uint16_t h) { return DirectX::PackedVector::XMConvertHalfToFloat(h); }

bool ReadGrid(ID3D11DeviceContext* ctx, ID3D11Resource* res, DXGI_FORMAT viewFormat, UINT w, UINT h, int gx, int gy,
              std::vector<float>& out) {
    ID3D11Texture2D* tex = nullptr;
    if (!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) return false;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11Texture2D* single = nullptr;
    ID3D11Texture2D* stage = nullptr;
    bool ok = false;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.MiscFlags = 0;
    ID3D11Resource* from = tex;
    if (d.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = sd;
        rd.Usage = D3D11_USAGE_DEFAULT;
        rd.BindFlags = 0;
        rd.CPUAccessFlags = 0;
        if (dev && SUCCEEDED(dev->CreateTexture2D(&rd, nullptr, &single)) && single) {
            ctx->ResolveSubresource(single, 0, tex, 0, viewFormat != DXGI_FORMAT_UNKNOWN ? viewFormat : d.Format);
            from = single;
        } else {
            from = nullptr;
        }
    }
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (from && dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &stage)) && stage) {
        ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, from, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
            out.assign((size_t)gx * gy * 4, 0.0f);
            const DXGI_FORMAT f = d.Format;
            for (int j = 0; j < gy; ++j) {
                for (int i = 0; i < gx; ++i) {
                    const UINT x = (UINT)((i + 0.5) * w / gx), y = (UINT)((j + 0.5) * h / gy);
                    const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                    float* o = &out[((size_t)j * gx + i) * 4];
                    switch (f) {
                    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: {
                        const uint16_t* p = (const uint16_t*)(row + (size_t)x * 8);
                        for (int c = 0; c < 4; ++c) o[c] = HalfToFloat(p[c]);
                        break;
                    }
                    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: {
                        memcpy(o, row + (size_t)x * 16, 16);
                        break;
                    }
                    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
                    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
                        const uint8_t* p = row + (size_t)x * 4;
                        for (int c = 0; c < 4; ++c) o[c] = (float)p[c];   // raw bytes, as stored
                        break;
                    }
                    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: {
                        const uint32_t v = *(const uint32_t*)(row + (size_t)x * 4);
                        o[0] = (float)(v & 0xFFFFFF) / 16777215.0f;
                        break;
                    }
                    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT: {
                        memcpy(o, row + (size_t)x * 4, 4);
                        break;
                    }
                    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: {
                        memcpy(o, row + (size_t)x * 8, 4);
                        break;
                    }
                    default:
                        o[0] = o[1] = o[2] = o[3] = NAN;
                    }
                }
            }
            ctx->Unmap(stage, 0);
            ok = true;
        }
    }
    if (stage) stage->Release();
    if (single) single->Release();
    if (dev) dev->Release();
    tex->Release();
    return ok;
}

DXGI_FORMAT ViewFormatOf(ID3D11View* v) {
    DXGI_FORMAT vf = DXGI_FORMAT_UNKNOWN;
    ID3D11RenderTargetView* rtv = nullptr;
    if (v && SUCCEEDED(v->QueryInterface(__uuidof(ID3D11RenderTargetView), (void**)&rtv)) && rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC d; rtv->GetDesc(&d); vf = d.Format; rtv->Release();
    }
    return vf;
}

void TexSize(ID3D11Resource* res, UINT* w, UINT* h) {
    *w = *h = 0;
    ID3D11Texture2D* t = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) {
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); *w = d.Width; *h = d.Height; t->Release();
    }
}

// A few pixels of a target, for the block: the means over a 64x36 grid and four points.
void AppendPixels(ID3D11DeviceContext* ctx, const char* what, ID3D11View* view, UINT w, UINT h) {
    if (!view) return;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    std::vector<float> g;
    if (ReadGrid(ctx, res, ViewFormatOf(view), w, h, 64, 36, g)) {
        double s[4] = {};
        for (size_t i = 0; i < g.size(); i += 4) for (int c = 0; c < 4; ++c) s[c] += g[i + c];
        const double n = g.size() / 4.0;
        Append("  %s mean %.4g %.4g %.4g %.4g | at (16,9) %.4g %.4g %.4g | (32,18) %.4g %.4g %.4g | (48,27) %.4g %.4g %.4g\n",
               what, s[0] / n, s[1] / n, s[2] / n, s[3] / n,
               g[(9 * 64 + 16) * 4], g[(9 * 64 + 16) * 4 + 1], g[(9 * 64 + 16) * 4 + 2],
               g[(18 * 64 + 32) * 4], g[(18 * 64 + 32) * 4 + 1], g[(18 * 64 + 32) * 4 + 2],
               g[(27 * 64 + 48) * 4], g[(27 * 64 + 48) * 4 + 1], g[(27 * 64 + 48) * 4 + 2]);
    } else {
        Append("  %s: could not be read back\n", what);
    }
    if (res) res->Release();
}

// The view distance a depth value means: clip z and w are both linear in the view distance, so
// zndc = a + b / z, with a and b from the projection's z and w rows (or columns).
void AppendProjection() {
    if (!g_ps.projValid) { Append("  projection: no PerView found\n"); return; }
    const float* m = g_ps.proj;
    for (int asColumns = 0; asColumns < 2; ++asColumns) {
        float r2[4], r3[4];
        for (int k = 0; k < 4; ++k) {
            r2[k] = asColumns ? m[k * 4 + 2] : m[2 * 4 + k];
            r3[k] = asColumns ? m[k * 4 + 3] : m[3 * 4 + k];
        }
        const double d33 = (double)r3[0] * r3[0] + (double)r3[1] * r3[1] + (double)r3[2] * r3[2];
        const double d23 = (double)r2[0] * r3[0] + (double)r2[1] * r3[1] + (double)r2[2] * r3[2];
        const double d22 = (double)r2[0] * r2[0] + (double)r2[1] * r2[1] + (double)r2[2] * r2[2];
        const double cosine = (d33 > 0 && d22 > 0) ? d23 / std::sqrt(d33 * d22) : 0.0;
        const double a = d33 > 0 ? d23 / d33 : 0.0;
        const double b = r2[3] - a * r3[3];
        Append("  projection as %s: z.w %g, w.w %g, cos %.6f, a %.9g, b %.9g -> z at zndc 0: %.6g, at 1: %.6g\n",
               asColumns ? "columns" : "rows", r2[3], r3[3], cosine, a, b, a != 0 ? -b / a : INFINITY,
               (1.0 - a) != 0 ? b / (1.0 - a) : INFINITY);
    }
}

void AppendDepth(ID3D11DeviceContext* ctx) {
    if (!g_ps.depthSrv) { Append("  depth (AO readers' t9): none bound\n"); return; }
    ID3D11Resource* res = nullptr;
    g_ps.depthSrv->GetResource(&res);
    UINT w = 0, h = 0;
    TexSize(res, &w, &h);
    Append("  depth (AO readers' t9): %s, viewport depth range %g to %g\n", DescribeView(g_ps.depthSrv).c_str(),
           g_ps.minDepth, g_ps.maxDepth);
    Append("  the view distance is made from %s: %s\n", g_ps.sceneDsv && g_sdSrv && !g_t9Depth ? "the scene's depth target" : "t9",
           DescribeView(g_ps.sceneDsv).c_str());
    // The picture's part of it (the top left, the picture's size) and the whole.
    std::vector<float> g;
    if (ReadGrid(ctx, res, DXGI_FORMAT_UNKNOWN, w, h, 96, 54, g)) {
        float mn = 1e30f, mx = -1e30f;
        int ones = 0, zeros = 0, n = 0;
        for (size_t i = 0; i < g.size(); i += 4) {
            const float v = g[i];
            if (!(v == v)) continue;
            ++n;
            if (v >= 1.0f) ++ones; else if (v <= 0.0f) ++zeros;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        Append("  depth over the whole texture: min %.7f max %.7f, %d of %d at 1, %d at 0\n", mn, mx, ones, n, zeros);
        // Columns of the grid, left to right, on the middle row: where the picture ends.
        std::string row;
        for (int i = 0; i < 96; i += 6) {
            char b[24];
            sprintf_s(b, " %.5f", g[(27 * 96 + i) * 4]);
            row += b;
        }
        Append("  depth along the middle row (every 6th of 96):%s\n", row.c_str());
    } else {
        Append("  depth: could not be read back\n");
    }
    if (res) res->Release();
}

void ReadSwitch() {
    static ULONGLONG lastLook = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastLook < 250) return;
    lastLook = now;
    std::string line;
    FILE* f = nullptr;
    if (_wfopen_s(&f, CorePath(L"s2fm_reshade_debug.txt").c_str(), L"r") == 0 && f) {
        char b[400] = {};
        if (fgets(b, sizeof(b), f)) line = b;
        fclose(f);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    }
    if (line == g_line) return;
    g_line = line;
    Log("[RESHADE] switch: \"%s\"\n", line.c_str());
    const bool probe = line.find("probe") != std::string::npos;
    if (probe && !g_probeWanted) {
        g_probeLeft = 8;
        g_probeBlocks.clear();
    }
    g_probeWanted = probe;
    if (line.find("core") != std::string::npos) g_wantCore = true;
    g_dumpPending = line.find("dump") != std::string::npos;
    g_viewZDumpPending = g_dumpPending && line.find("viewz") != std::string::npos;
    g_t9Depth = line.find("t9depth") != std::string::npos;
    // "dumpnow": at the next stage C (S2FM stops drawing once refinement is done, so a test that stands still
    // asks for it before moving the playhead).
    g_dumpAt = GetTickCount64() + (line.find("dumpnow") != std::string::npos ? 0 : 1500);
}

// Whether the core keeps a preset for some session (its s2fm_reshade\state.ini says Enabled=1): then the
// proxy loads it without waiting for the panel, so a session's effects are on as soon as it is open.
bool CoreWanted() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&CoreWanted, &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) *slash = 0;
    std::wstring state = std::wstring(path) + L"\\s2fm_reshade\\state.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, state.c_str(), L"r") != 0 || !f) return false;
    char line[256];
    bool on = false;
    while (fgets(line, sizeof(line), f)) if (!strncmp(line, "Enabled=1", 9)) on = true;
    fclose(f);
    return on;
}

void LoadCore() {
    g_coreTried = true;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&LoadCore, &self);
    wchar_t dir[MAX_PATH] = {};
    GetModuleFileNameW(self, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;     // game\bin\win64
    std::wstring core = std::wstring(dir) + L"\\tools\\s2fm_reshade_core.dll";
    HMODULE m = LoadLibraryW(core.c_str());
    if (!m) { Log("[RESHADE] the core was not loaded from %ls (%lu)\n", core.c_str(), GetLastError()); return; }
    typedef int (*Start_Fn)(const char*);
    Start_Fn start = (Start_Fn)GetProcAddress(m, "RsCore_Start");
    char dirUtf8[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, dir, -1, dirUtf8, sizeof(dirUtf8), nullptr, nullptr);
    const int ok = start ? start(dirUtf8) : 0;
    Log("[RESHADE] the core loaded from %ls, started: %d\n", core.c_str(), ok);
}

// The shown picture (an 8-bit target) written whole to a PPM, for the tests.
void DumpPicture(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT w, UINT h) {
    ID3D11Texture2D* tex = nullptr;
    if (!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) return;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11Texture2D* st = nullptr;
    if (dev && d.SampleDesc.Count == 1 && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &st)) && st) {
        ctx->CopyResource(st, tex);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE* f = nullptr;
            if (_wfopen_s(&f, CorePath(L"s2fm_reshade_picture.ppm").c_str(), L"wb") == 0 && f) {
                fprintf(f, "P6\n%u %u\n255\n", w, h);
                std::vector<uint8_t> row((size_t)w * 3);
                const bool bgr = d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM
                              || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                for (UINT y = 0; y < h; ++y) {
                    const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                    for (UINT x = 0; x < w; ++x) {
                        row[x * 3 + 0] = src[x * 4 + (bgr ? 2 : 0)];
                        row[x * 3 + 1] = src[x * 4 + 1];
                        row[x * 3 + 2] = src[x * 4 + (bgr ? 0 : 2)];
                    }
                    fwrite(row.data(), 1, row.size(), f);
                }
                fclose(f);
                Log("[RESHADE] picture dumped (%ux%u), playhead %.4f s at %.2f fps, sample %d\n", w, h, g_ps.seconds,
                    g_ps.fps, g_sampleIndex);
            }
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    if (dev) dev->Release();
    tex->Release();
}

// A stage, handed to the core: the engine's texture, how it holds its colour, and the frame's facts.
void CallStage(ID3D11DeviceContext* ctx, int stage, ID3D11RenderTargetView* rtv, int encoding, DXGI_FORMAT viewFormat,
               float exposure) {
    DxrStageCallback cb = g_stageCb.load(std::memory_order_acquire);
    if (!cb || !rtv) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    if (res) res->Release();
    if (!tex) return;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    DxrStageFrame f = {};
    f.size = sizeof(f);
    f.stage = stage;
    f.ctx = ctx;
    f.target = tex;
    f.viewFormat = viewFormat;
    f.encoding = encoding;
    f.width = d.Width;
    f.height = d.Height;
    f.samples = d.SampleDesc.Count;
    f.viewZ = g_ps.viewZMade && g_vzW == d.Width && g_vzH == d.Height ? g_vzSrv : nullptr;
    f.exposure = exposure > 0.0f ? exposure : 1.0f;
    f.seconds = g_ps.seconds;
    f.filmFrame = (uint32_t)std::floor(g_ps.seconds * g_ps.fps + 0.5);
    f.sampleIndex = (uint32_t)g_sampleIndex;
    f.flags = (DxrExportActive() ? DXR_STAGE_RENDERING : 0) | (g_refining ? DXR_STAGE_REFINING : 0);
    g_stageCalls[stage].fetch_add(1, std::memory_order_relaxed);
    t_DxrStagePassThrough = true;
    const int drew = cb(&f, g_stageUser.load(std::memory_order_relaxed));
    t_DxrStagePassThrough = false;
    if (drew) g_stageDrew[stage].fetch_add(1, std::memory_order_relaxed);
    tex->Release();
}

}  // namespace

static void ReshadeStagesBeforeDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    t_afterPending = false;
    if (!ctx || !st || t_DxrStagePassThrough) return;
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    if (DxrFrameForeign(st)) return;
    StageDrawInfo di;
    if (!StageDrawInfoOf(st, &di) || di.psLayout < 0) return;
    const Kind kind = KindOf(di.psLayout);
    const bool rec = Recording();

    if (!g_ps.aoReaderSeen && IsAoReader(st, di.psLayout)) {
        g_ps.aoReaderSeen = true;
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        g_ps.sceneRtv = rtv;   // the reference OMGetRenderTargets took is kept
        g_ps.sceneDsv = dsv;
        if (dsv) dsv->AddRef();
        D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&nvp, vp);
        if (nvp) { g_ps.minDepth = vp[0].MinDepth; g_ps.maxDepth = vp[0].MaxDepth; }
        g_ps.projValid = StagePerViewProjection(st, di.rtW, di.rtH, g_ps.proj, &g_ps.toneMapLinear, &g_ps.toneMapGamma);
        if (rec) {
            Append("first AO reader %s: target %s, dsv %s\n", ShaderDbName(di.psLayout),
                   DescribeView(rtv).c_str(), DescribeView(dsv).c_str());
        }
        if (dsv) dsv->Release();
    }
    // The depth: the t9 of the first AO reader that has the frame's own there. The engine binds a 1x1 stand in
    // at t9 for some draws (the characters among them), and taken from the frame's first
    // reader whatever it held, the depth was a single texel whenever such a draw came first.
    if (g_ps.aoReaderSeen && !g_ps.aFound && !g_ps.depthSrv && IsAoReader(st, di.psLayout)) {
        const int ds = g_depthSlot[(size_t)di.psLayout];
        ID3D11ShaderResourceView* d = ds >= 0 && di.psSrv ? di.psSrv[ds] : nullptr;
        if (d) {
            ID3D11Resource* r = nullptr;
            d->GetResource(&r);
            UINT w = 0, h = 0;
            TexSize(r, &w, &h);
            if (r) r->Release();
            if (w > 1 && h > 1) {
                g_ps.depthSrv = d;
                d->AddRef();
            } else if (rec) {
                Append("  AO reader %s has a %ux%u stand in at t9, not the frame's depth\n", ShaderDbName(di.psLayout), w, h);
            }
        }
    }

    if (kind == KIND_AFTER_OPAQUE || kind == KIND_POST || kind == KIND_ACCUM || kind == KIND_COPY) {
        if (g_ps.aoReaderSeen && !g_ps.aFound) {
            g_ps.aFound = true;
            if (g_stageCb.load(std::memory_order_relaxed)) {
                if (!g_ps.clockRead) {
                    g_ps.clockRead = true;
                    double sec = 0.0, fps = 24.0;
                    if (SfmPlayhead(&sec, &fps, nullptr, 0)) { g_ps.seconds = sec; g_ps.fps = fps; g_playheadReads.fetch_add(1); }
                    else g_playheadFails.fetch_add(1);
                    g_lastSeconds = g_ps.seconds;
                }
                UINT sw = 0, sh = 0;
                ID3D11Resource* sr = nullptr;
                if (g_ps.sceneRtv) { g_ps.sceneRtv->GetResource(&sr); TexSize(sr, &sw, &sh); if (sr) sr->Release(); }
                g_ps.viewZMade = MakeViewZ(ctx, sw, sh);
            }
            CallStage(ctx, DXR_STAGE_A, g_ps.sceneRtv, DXR_ENC_SCENE, DXGI_FORMAT_R16G16B16A16_FLOAT, g_ps.toneMapLinear);
            if (rec) {
                UINT w = 0, h = 0;
                ID3D11Resource* r = nullptr;
                if (g_ps.sceneRtv) { g_ps.sceneRtv->GetResource(&r); TexSize(r, &w, &h); if (r) r->Release(); }
                Append("A at %s: scene target %s\n", ShaderDbName(di.psLayout), DescribeView(g_ps.sceneRtv).c_str());
                AppendPixels(ctx, "scene (resolved, linear)", g_ps.sceneRtv, di.rtW ? di.rtW : w, di.rtH ? di.rtH : h);
                AppendDepth(ctx);
                AppendProjection();
                Append("  PerView tone map scalars: linear %g, gamma %g\n", g_ps.toneMapLinear, g_ps.toneMapGamma);
            }
        }
    }

    if (kind == KIND_POST) {
        g_ps.postSeen = true;
        if (rec) {
            Append("B post_process: D_OUTPUT_LINEAR %d, srv0 %s\n", ShaderDbComboValue(di.psCombo, "D_OUTPUT_LINEAR"),
                   DescribeView(di.psSrv ? di.psSrv[0] : nullptr).c_str());
        }
    } else if (kind == KIND_ACCUM) {
        g_ps.accumSeen = true;
        if (!di.blend) { g_sampleIndex = 0; g_refining = true; }
        else ++g_sampleIndex;
    } else if (kind == KIND_COPY) {
        if (!g_ps.postSeen) ++g_ps.lone;
    }

    // From post_process to the present: every draw, what it reads at t0 and what it writes.
    if (rec && (g_ps.postSeen || kind == KIND_POST)) {
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        const char* name = ShaderDbName(di.psLayout);
        Append("  draw %-24s writes %s | t0 %s | blend %d %u/%u\n", (name && *name) ? name : "(no table)",
               DescribeView(rtv).c_str(), DescribeView(di.psSrv ? di.psSrv[0] : nullptr).c_str(), di.blend ? 1 : 0,
               (UINT)di.blendSrc, (UINT)di.blendDst);
        if (rtv) rtv->Release();
    }

    if (kind == KIND_POST || kind == KIND_ACCUM || (kind == KIND_COPY && g_ps.postSeen)) {
        t_afterKind = kind;
        t_afterPending = true;
    }
}

static void ReshadeStagesAfterDraw(ID3D11DeviceContext* ctx) {
    if (!t_afterPending || !ctx) return;
    t_afterPending = false;
    const Kind kind = t_afterKind;
    const bool rec = Recording();
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    UINT w = 0, h = 0;
    ID3D11Resource* r = nullptr;
    if (rtv) { rtv->GetResource(&r); TexSize(r, &w, &h); if (r) r->Release(); }
    if (kind == KIND_POST) {
        g_ps.bFound = rtv != nullptr;
        CallStage(ctx, DXR_STAGE_B, rtv, DXR_ENC_LINEAR, DXGI_FORMAT_R16G16B16A16_FLOAT, 1.0f);
        if (rec) {
            Append("B target after post_process: %s\n", DescribeView(rtv).c_str());
            AppendPixels(ctx, "post_process target", rtv, w, h);
        }
    } else if (kind == KIND_COPY && !g_ps.cFound) {
        // The shown picture: an 8-bit target.
        const DXGI_FORMAT vf = ViewFormatOf(rtv);
        const bool eight = vf == DXGI_FORMAT_R8G8B8A8_UNORM || vf == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                        || vf == DXGI_FORMAT_B8G8R8A8_UNORM || vf == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if (eight) {
            g_ps.cFound = true;
            const bool bgra = vf == DXGI_FORMAT_B8G8R8A8_UNORM || vf == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            CallStage(ctx, DXR_STAGE_C, rtv, DXR_ENC_SRGB8, bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM, 1.0f);
            if (g_dumpPending && GetTickCount64() >= g_dumpAt) {
                g_dumpPending = false;
                ID3D11Resource* pic = nullptr;
                rtv->GetResource(&pic);
                DumpPicture(ctx, pic, w, h);
                if (pic) pic->Release();
                if (g_viewZDumpPending) { g_viewZDumpPending = false; DumpViewZ(ctx); }
            }
            if (rec) {
                Append("C copytexture: target %s\n", DescribeView(rtv).c_str());
                AppendPixels(ctx, "C target (bytes)", rtv, w, h);
            }
        }
    }
    if (rtv) rtv->Release();
}

static void ReshadeStagesPresent(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lk(g_m);
    ReadSwitch();
    if (!g_coreTried && !g_wantCore) {
        static ULONGLONG lastLook = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastLook >= 2000) { lastLook = now; g_wantCore = CoreWanted(); }
    }
    if (g_wantCore && !g_coreTried) LoadCore();
    // Another tool's window ends no frame of S2FM's: what S2FM's frame found stays for S2FM's own present.
    if (sc && !DxrSwapChainIsS2fm(sc)) return;
    if (g_ps.postSeen) {
        g_presentsWithPost.fetch_add(1, std::memory_order_relaxed);
        if (g_ps.aFound) g_presentsA.fetch_add(1, std::memory_order_relaxed);
        if (g_ps.bFound) g_presentsB.fetch_add(1, std::memory_order_relaxed);
        if (g_ps.cFound) g_presentsC.fetch_add(1, std::memory_order_relaxed);
        if (!g_ps.accumSeen) { g_refining = false; g_sampleIndex = 0; }
        // A present the probe came on in the middle of is not recorded.
        if (Recording() && g_ps.recFromStart) {
            ID3D11Texture2D* back = nullptr;
            std::string backText = "unknown";
            if (sc && SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
                backText = DescribeTex(back);
                back->Release();
            }
            char head[200];
            sprintf_s(head, "present %d export %d refining %d sample %d lone %d | presented back buffer %s\n",
                      9 - g_probeLeft, DxrExportActive() ? 1 : 0, g_ps.accumSeen ? 1 : 0, g_ps.accumSeen ? g_sampleIndex : 0,
                      g_ps.lone, backText.c_str());
            g_probeBlocks.push_back(head + g_ps.block);
            if (--g_probeLeft == 0) {
                FILE* f = nullptr;
                if (_wfopen_s(&f, CorePath(L"s2fm_reshade_probe.txt").c_str(), L"w") == 0 && f) {
                    for (const std::string& b : g_probeBlocks) fprintf(f, "%s", b.c_str());
                    fclose(f);
                }
                Log("[RESHADE] probe written, %zu presents\n", g_probeBlocks.size());
            }
        }
    } else if (g_ps.lone) {
        g_presentsLone.fetch_add(1, std::memory_order_relaxed);
    }
    ResetPresent();

    static ULONGLONG lastSay = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastSay >= 10000) {
        lastSay = now;
        const int p = g_presentsWithPost.exchange(0);
        if (p) {
            Log("[RESHADE] in 10 s: %d presents drew the scene, stage A found in %d, B %d, C %d; %d presents only "
                "showed the picture again | the core was called A %d B %d C %d times and drew A %d B %d C %d\n", p,
                g_presentsA.exchange(0), g_presentsB.exchange(0), g_presentsC.exchange(0), g_presentsLone.exchange(0),
                g_stageCalls[0].exchange(0), g_stageCalls[1].exchange(0), g_stageCalls[2].exchange(0),
                g_stageDrew[0].exchange(0), g_stageDrew[1].exchange(0), g_stageDrew[2].exchange(0));
            Log("[RESHADE] playhead read %d times, failed %d, last %.3f s | view distance from the scene's depth %d "
                "times, from t9 %d\n", g_playheadReads.exchange(0), g_playheadFails.exchange(0), g_lastSeconds,
                g_viewZScene.exchange(0), g_viewZT9.exchange(0));
        }
    }
}

// --- On the core.
namespace {
void FeatureBeforeDraw(ID3D11DeviceContext* ctx, ContextState* st) { ReshadeStagesBeforeDraw(ctx, st); }
void FeatureAfterDraw(ID3D11DeviceContext* ctx, ContextState*) { ReshadeStagesAfterDraw(ctx); }
const CoreFeature kFeature = { "ReShade stages", FeatureBeforeDraw, FeatureAfterDraw, nullptr, ReshadeStagesPresent,
                               nullptr, nullptr };
}  // namespace

void ReshadeStagesRegister() { CoreRegister(&kFeature); }
