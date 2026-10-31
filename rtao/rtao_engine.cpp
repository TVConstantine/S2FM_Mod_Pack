// RTAO, the engine's side: our traced AO over S2FM's own viewports, with the rays of rtao_d3d12.cpp against the
// geometry of rtao_capture.cpp.
//
// What the engine has: every opaque shader that lights by the probes (vr_complex, vr_simple, vr_skin and the rest)
// samples a screen sized dynamic AO texture (g_tDynamicAmbientOcclusion), with a single sample depth beside it
// (g_tDynamicAmbientOcclusionDepth, t9). Alyx never fills it. These draws are the AO readers.
//
// One frame of the view, in the engine's order: the AO readers (the opaque surfaces), then the sky, the fogs and
// see-through surfaces (sfm_volumetrics_frustum), the bloom, post_process, S2FM's refinement sum (sfm_accumulation),
// copytexture. Here:
//   - at the frame's first AO reader: the camera (its PerView), t9 copied into a depth slot, the target the reader
//     draws into kept as the one to put the AO on, the fogs' inputs kept, the capture opened;
//   - after every AO reader: it again with stream output (rtao_capture.cpp); a see-through draw into the picture
//     before the AO is drawn once more into a target of what such draws add, which the AO is not to darken;
//   - at the first draw of a pass after the opaque surfaces: the scene's own depth (every opaque surface, the
//     characters too, which t9 lacks) over the t9 copy, the rays sent (our queue waits on the GPU for the capture and
//     the copy), the engine's context waits on the GPU for the AO, and the picture is multiplied by it with one full
//     screen triangle: dest x AO, plus what the fogs add there and the see-through draws added, times (1 - AO).
//
// The camera's SSAORadius, SSAOBias and SSAOStrength drive it (S2FM's document, common/sfm_doc.h), meaning what they
// mean to the engine's own SSAO (see rtao_cs.hlsl).
//
// Test words, on the first line of s2fm_rtao_debug.txt beside the DLL (read four times a second):
//   off           no AO;
//   raysN         rays a pixel, 1 to 16 (8 when not said);
//   radiusN strengthN biasN   the camera's values overridden;
//   t9depth       the rays from t9 alone, as the engine has it, not the scene's own depth;
//   nofog         the AO over the fogs too; fogonly: only what the fogs add, to see them;
//   debug         the normal beside the AO (in the dump);
//   measure       each presented picture's mean, per line, into s2fm_rtao_out.txt; with "dump" also the AO, the
//                 depth and the presented picture once a line (s2fm_rtao_ao.pgm, _depth.pgm, _picture.ppm);
//   views         the views the engine draws (the camera's, the shadows'...) logged for the next presents;
//   offscreen     the other views' triangles too (the shadows', rtao_capture.cpp), for what is off the screen: off
//                 by default, its cost is too high for what it gives (docs/rtao.md).
#include "rtao.h"
#include "rtao_api.h"
#include "../core/core.h"
#include "../common/sfm_doc.h"
#include "../common/shader_table.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::recursive_mutex g_m;
std::atomic<bool> g_on{true};
bool g_broken = false;
// The panel's settings (s2fm_rtao.ini beside the DLL), and the test words that override them: on is the saved switch
// unless the words say "off"; the rays are the words' "raysN" when they say it.
int g_savedEnabled = 1, g_savedRays = 8;
bool g_switchOff = false;
int g_switchRays = 0;
// For the panel: the AO put on, in all and when last, and why it is not running (the engine's side; rtao_d3d12.cpp
// keeps its own).
std::atomic<long long> g_mulTotal{0};
std::atomic<ULONGLONG> g_lastMultiplyAt{0};
char g_why[256] = "";

// --- Words from the switch file.
std::string g_line = "\x01";   // nothing read yet
int g_rays = 8;

void ApplySettings() {
    g_on.store(g_savedEnabled && !g_switchOff, std::memory_order_relaxed);
    g_rays = g_switchRays ? g_switchRays : g_savedRays;
}

void LoadSettings() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, CorePath(L"s2fm_rtao.ini").c_str(), L"r") == 0 && f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "Enabled=", 8)) g_savedEnabled = atoi(line + 8) ? 1 : 0;
            if (!strncmp(line, "Rays=", 5)) {
                const int n = atoi(line + 5);
                if (n >= 1 && n <= 16) g_savedRays = n;
            }
        }
        fclose(f);
    }
    ApplySettings();
    Log("[RTAO] settings: %s, %d rays a pixel", g_savedEnabled ? "on" : "off", g_savedRays);
}

void SaveSettings() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, CorePath(L"s2fm_rtao.ini").c_str(), L"w") == 0 && f) {
        fprintf(f, "[RTAO]\nEnabled=%d\nRays=%d\n", g_savedEnabled, g_savedRays);
        fclose(f);
    }
}
float g_radiusAsked = 0.0f, g_strengthAsked = -1.0f, g_biasAsked = -1.0f;
bool g_t9Depth = false, g_noFogFix = false, g_fogOnly = false, g_debug = false, g_noRefit = false, g_regrow = false;
bool g_measure = false, g_dumpWanted = false, g_dumpDone = false, g_pictureDumped = false;
ULONGLONG g_dumpAfter = 0;

// --- The camera's sliders, read from S2FM's document every quarter second. S2FM's defaults until then.
float g_camRadius = 30.0f, g_camBias = 0.5f, g_camStrength = 2.0f;
bool g_camRead = false;
char g_camName[64] = "";
ULONGLONG g_camReadAt = 0;

// --- The view.
std::atomic<IDXGISwapChain*> g_viewSwapChain{nullptr};
std::atomic<bool> g_readerSincePresent{false};
std::map<IDXGISwapChain*, ULONGLONG> g_scenePresentAt;
bool g_firstReaderDone = false;   // this frame's first AO reader has drawn
bool g_mulPending = false;        // the AO is still to go on this frame
thread_local bool t_reader = false;

struct Camera {
    float worldToProj[16];
    float position[3];
    float offset[3];
};
Camera g_cam = {};
bool g_camValid = false;

// --- The two depth and AO slots, shared with our D3D12 device.
struct Slot {
    ID3D11Texture2D* depth11 = nullptr;
    ID3D11UnorderedAccessView* depthUav = nullptr;
    ID3D11ShaderResourceView* depthSrv = nullptr;
    ID3D12Resource* depth12 = nullptr;
    ID3D11Texture2D* ao11 = nullptr;
    ID3D11ShaderResourceView* aoSrv = nullptr;
    ID3D12Resource* ao12 = nullptr;
    UINT64 busyUntil = 0;
    bool hasAo = false;
    float eye[3] = {}, forward[3] = {};   // the camera the AO was traced for
};
Slot g_slots[2];
UINT g_slotW = 0, g_slotH = 0;
int g_newestSlot = -1, g_showSlot = -1, g_depthSlot = -1;
UINT64 g_capturedValue = 0, g_aoValue = 0;
UINT g_seed = 0;
// t9 (the engine's AO depth), its size, and the viewport's depth range taken at the first reader.
ID3D11ShaderResourceView* g_t9 = nullptr;
UINT g_t9W = 0, g_t9H = 0;
bool g_depthFromT9 = false;   // this frame's slot got t9's depth at the first reader
bool g_noT9 = false;          // "not9": t9 ignored, as in a scene of models alone (for the tests)
bool g_remakeSlots = false;   // the slots are made again at the size asked for
float g_minDepth = 0.0f, g_maxDepth = 1.0f;
ID3D11ComputeShader* g_copyCs = nullptr;
ID3D11ComputeShader* g_copyMsCs = nullptr;

// --- Counts for the 10 s line and the out file.
std::atomic<long long> g_skyDraws{0};   // 3D sky draws that read the AO, left out, for the 10 s line
bool g_withSky = false;   // "withsky": the 3D sky traced too, as before
std::atomic<long long> g_blendedReaders{0};   // blended AO readers left out of the rays, for the 10 s line
bool g_solidBlended = false;   // "solidblended": blended AO readers traced as solid
std::atomic<long long> g_notTraced10{0};   // frames not traced in time, for the 10 s line
std::atomic<long long> g_staleSkipped{0};   // frames without AO: the last one was traced for another camera
std::atomic<long long> g_frames{0}, g_traced{0}, g_missed{0}, g_mulDone{0}, g_fogFrames{0}, g_transDraws{0},
    g_sceneDepthCopies{0};

template <typename T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }

int TextureSlot(int layout, const char* name) {
    uint8_t reg = 0;
    uint16_t offset = 0;
    return layout >= 0 && ShaderDbFind(layout, name, &reg, &offset) && reg == SDB_REG_TEXTURE ? (int)(offset & 0xFF) : -1;
}

bool TexSize(ID3D11ShaderResourceView* v, UINT* w, UINT* h) {
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    bool ok = false;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d = {};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        *w = d.Width;
        *h = d.Height;
        ok = true;
    }
    r->Release();
    return ok;
}

// A 4x4 inverted (row vectors), false when it cannot be.
bool Inverse(const float* m, float* out) {
    double a[16], inv[16];
    for (int i = 0; i < 16; ++i) a[i] = m[i];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-30) return false;
    for (int i = 0; i < 16; ++i) out[i] = (float)(inv[i] / det);
    for (int i = 0; i < 16; ++i) if (!std::isfinite(out[i])) return false;
    return true;
}

// The camera of the draw about to be made, from its PerView, when it is the w x h view's.
bool CameraOf(const ContextState* st, UINT w, UINT h, Camera* out) {
    float pv[204];
    if (!CorePerView(st, pv)) return false;
    const float vw = pv[PV_VIEWPORT_SIZE], vh = pv[PV_VIEWPORT_SIZE + 1];
    if (!std::isfinite(vw) || !std::isfinite(vh) || (UINT)(vw + 0.5f) != w || (UINT)(vh + 0.5f) != h) return false;
    memcpy(out->worldToProj, &pv[PV_MAT_WORLD_TO_PROJ_MV], sizeof(out->worldToProj));
    for (int i = 0; i < 3; ++i) {
        out->position[i] = pv[PV_CAMERA_POSITION_WS + i];
        out->offset[i] = pv[PV_WORLD_TO_CAM_OFF_MV + i];
        if (!std::isfinite(out->position[i]) || !std::isfinite(out->offset[i])) return false;
    }
    return true;
}

ID3D11ComputeShader* CompileCs(ID3D11Device* dev, const char* src, const char* name) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    if (SUCCEEDED(D3DCompile(src, strlen(src), name, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err)) && code)
        dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
    if (err) {
        Log("[RTAO] %s: %s", name, (const char*)err->GetBufferPointer());
        err->Release();
    }
    if (code) code->Release();
    return cs;
}

ID3D11Texture2D* SharedTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D* t = nullptr;
    return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) ? t : nullptr;
}

void ReleaseSlots() {
    for (Slot& a : g_slots) {
        Drop(a.depth11); Drop(a.depthUav); Drop(a.depthSrv); Drop(a.depth12);
        Drop(a.ao11); Drop(a.aoSrv); Drop(a.ao12);
        a = Slot();
    }
    g_slotW = g_slotH = 0;
    g_newestSlot = g_showSlot = g_depthSlot = -1;
    RtaoSlotsChanged();
}

// D3D12, the slots at t9's size and the copy shaders. False gives RTAO up for the session, said once.
// The slots at least as big as needW x needH (t9's size, or without t9 the scene's).
bool EnsureResources(ID3D11DeviceContext* ctx, UINT needW, UINT needH) {
    if (g_broken) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    bool ok = RtaoD3D12Start(dev);
    const char* what = "D3D12";
    if (ok && !g_copyCs) {
        g_copyCs = CompileCs(dev,
            "Texture2D<float> src : register(t0);\nRWTexture2D<float> dst : register(u0);\n"
            "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { dst[id.xy] = src.Load(int3(id.xy, 0)); }\n",
            "rtao_depth_copy");
        // The scene's depth when it has more than one sample a pixel: the first sample's.
        g_copyMsCs = CompileCs(dev,
            "Texture2DMS<float> src : register(t0);\nRWTexture2D<float> dst : register(u0);\n"
            "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { dst[id.xy] = src.Load(int2(id.xy), 0); }\n",
            "rtao_depth_copy_ms");
        ok = g_copyCs != nullptr;
        what = "the depth copy shader";
    }
    const UINT haveW = g_remakeSlots ? 0 : g_slotW, haveH = g_remakeSlots ? 0 : g_slotH;
    const UINT wantW = needW > haveW ? needW : haveW, wantH = needH > haveH ? needH : haveH;
    if (g_remakeSlots && ok) g_remakeSlots = false;
    if (ok && (g_slotW != wantW || g_slotH != wantH)) {
        // A new size: what the card still traces from the old slots finishes first, briefly.
        for (int i = 0; i < 50 && g_aoValue && !RtaoAoDone(g_aoValue); ++i) Sleep(10);
        if (g_aoValue && !RtaoAoDone(g_aoValue)) {
            ok = false;
            what = "the old slots (still being traced)";
        } else {
            ReleaseSlots();
            for (Slot& a : g_slots) {
                a.depth11 = SharedTexture(dev, wantW, wantH, DXGI_FORMAT_R32_FLOAT);
                a.ao11 = SharedTexture(dev, wantW, wantH, DXGI_FORMAT_R16G16B16A16_FLOAT);
                ok = ok && a.depth11 && a.ao11
                  && SUCCEEDED(dev->CreateUnorderedAccessView(a.depth11, nullptr, &a.depthUav))
                  && SUCCEEDED(dev->CreateShaderResourceView(a.depth11, nullptr, &a.depthSrv))
                  && SUCCEEDED(dev->CreateShaderResourceView(a.ao11, nullptr, &a.aoSrv))
                  && (a.depth12 = RtaoOpenTexture(a.depth11)) != nullptr
                  && (a.ao12 = RtaoOpenTexture(a.ao11)) != nullptr;
            }
            if (ok) {
                g_slotW = wantW;
                g_slotH = wantH;
                Log("[RTAO] depth and AO slots %ux%u made and shared", g_slotW, g_slotH);
            } else {
                what = "the shared slots";
                ReleaseSlots();
            }
        }
    }
    dev->Release();
    if (!ok) {
        g_broken = true;
        Log("[RTAO] RTAO stands down for this session: %s could not be made", what);
        sprintf_s(g_why, "%s could not be made", what);
    }
    return ok;
}

// A depth into the slot, everything the copy changes put back.
void CopyDepth(ID3D11DeviceContext* ctx, int k, UINT w, UINT h, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* src) {
    ID3D11ComputeShader* oldCs = nullptr;
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv = nullptr;
    ctx->CSGetShaderResources(0, 1, &oldSrv);
    ID3D11UnorderedAccessView* oldUav = nullptr;
    ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    ID3D11UnorderedAccessView* none = nullptr;
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &src);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_slots[k].depthUav, nullptr);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    const UINT keep = (UINT)-1;
    ctx->CSSetShader(oldCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &oldSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
    t_CorePassThrough = pass;
    if (oldCs) oldCs->Release();
    if (oldSrv) oldSrv->Release();
    if (oldUav) oldUav->Release();
}

// --- The scene's own depth (every opaque surface; t9 lacks the characters), copied where the AO goes on.
ID3D11DepthStencilView* g_sceneDsv = nullptr;
ID3D11RenderTargetView* g_sceneRtv = nullptr;
UINT g_sceneW = 0, g_sceneH = 0;
ID3D11Texture2D* g_sdTex = nullptr;
ID3D11ShaderResourceView* g_sdSrv = nullptr;
D3D11_TEXTURE2D_DESC g_sdDesc = {};

bool CopySceneDepth(ID3D11DeviceContext* ctx, int k, UINT w, UINT h) {
    if (!g_sceneDsv) return false;
    ID3D11Resource* res = nullptr;
    g_sceneDsv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    if (res) res->Release();
    if (!tex) return false;
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
    const bool ms = d.SampleDesc.Count > 1;
    bool ok = family != DXGI_FORMAT_UNKNOWN && d.ArraySize == 1 && d.Width >= w && d.Height >= h && (!ms || g_copyMsCs);
    if (ok && (!g_sdTex || g_sdDesc.Width != d.Width || g_sdDesc.Height != d.Height || g_sdDesc.Format != d.Format
               || g_sdDesc.SampleDesc.Count != d.SampleDesc.Count || g_sdDesc.SampleDesc.Quality != d.SampleDesc.Quality
               || g_sdDesc.MipLevels != d.MipLevels)) {
        Drop(g_sdTex);
        Drop(g_sdSrv);
        D3D11_TEXTURE2D_DESC c = d;
        c.Format = family;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        c.CPUAccessFlags = 0;
        c.MiscFlags = 0;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = view;
        if (ms) sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
        }
        ok = dev && SUCCEEDED(dev->CreateTexture2D(&c, nullptr, &g_sdTex))
          && SUCCEEDED(dev->CreateShaderResourceView(g_sdTex, &sd, &g_sdSrv));
        if (dev) dev->Release();
        g_sdDesc = d;
        Log("[RTAO] the scene's depth target: %ux%u, format %u, %u samples; %s", d.Width, d.Height, (UINT)d.Format,
            d.SampleDesc.Count, ok ? "copied for the rays" : "COULD NOT be copied, the rays keep t9");
        if (!ok) { Drop(g_sdTex); Drop(g_sdSrv); }
    }
    ok = ok && g_sdTex && g_sdSrv;
    if (ok) {
        ctx->CopyResource(g_sdTex, tex);
        CopyDepth(ctx, k, w, h, ms ? g_copyMsCs : g_copyCs, g_sdSrv);
        g_sceneDepthCopies.fetch_add(1, std::memory_order_relaxed);
    }
    tex->Release();
    return ok;
}

// --- The fogs' inputs, from the frame's first AO reader: its pixel shader's b1 (the camera at register 40), b3 (the
// view's fog constants) and b6 (the blue noise's placing at register 1313), and its fog volume, blue noise and fog
// cube. Every opaque shader that reads the AO has them there.
ID3D11Buffer* g_fogCb[3] = {};
ID3D11ShaderResourceView* g_fogTex[3] = {};
bool g_fogReady = false;

void FogCapture(const ContextState* st) {
    for (ID3D11Buffer*& b : g_fogCb) Drop(b);
    for (ID3D11ShaderResourceView*& t : g_fogTex) Drop(t);
    g_fogReady = false;
    const int cbs[3] = { 1, 3, 6 };
    const char* const names[3] = { "g_tFogVolume", "g_tBlueNoise", "g_tFogCubeTexture" };
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        g_fogCb[i] = st->psCb[cbs[i]];
        if (g_fogCb[i]) g_fogCb[i]->AddRef();
        ok = ok && g_fogCb[i];
        const int slot = TextureSlot(st->psLayout, names[i]);
        if (slot >= 0 && slot < 32) g_fogTex[i] = st->psSrv[slot];
        if (g_fogTex[i]) g_fogTex[i]->AddRef();
        ok = ok && g_fogTex[i];
    }
    UINT bytes[3] = {};
    for (int i = 0; i < 3; ++i) {
        D3D11_BUFFER_DESC bd = {};
        if (g_fogCb[i]) g_fogCb[i]->GetDesc(&bd);
        bytes[i] = bd.ByteWidth;
    }
    ok = ok && bytes[0] >= 41 * 16 && bytes[1] >= 37 * 16 && bytes[2] >= 1314 * 16;
    static int s_said = -1;
    if (s_said != (int)ok) {
        s_said = (int)ok;
        Log("[RTAO] the fogs' inputs from %s: buffers b1 %u, b3 %u, b6 %u bytes; fog volume %s, blue noise %s, fog cube "
            "%s; %s", ShaderDbName(st->psLayout), bytes[0], bytes[1], bytes[2], g_fogTex[0] ? "yes" : "no",
            g_fogTex[1] ? "yes" : "no", g_fogTex[2] ? "yes" : "no",
            ok ? "the AO goes on with the fogs kept out" : "the AO goes on over the fogs");
    }
    g_fogReady = ok;
}

// The scene target as it is before the AO goes on, one sample a pixel (FogKept).
ID3D11Texture2D* g_beforeTex = nullptr;
ID3D11ShaderResourceView* g_beforeSrv = nullptr;
D3D11_TEXTURE2D_DESC g_beforeDesc = {};

ID3D11ShaderResourceView* PictureBefore(ID3D11DeviceContext* ctx) {
    ID3D11Resource* res = nullptr;
    g_sceneRtv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    if (res) res->Release();
    if (!tex) return nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    g_sceneRtv->GetDesc(&rd);
    const DXGI_FORMAT f = rd.Format != DXGI_FORMAT_UNKNOWN ? rd.Format : d.Format;
    if (!g_beforeTex || g_beforeDesc.Width != d.Width || g_beforeDesc.Height != d.Height || g_beforeDesc.Format != f) {
        Drop(g_beforeSrv);
        Drop(g_beforeTex);
        D3D11_TEXTURE2D_DESC c = {};
        c.Width = d.Width;
        c.Height = d.Height;
        c.MipLevels = 1;
        c.ArraySize = 1;
        c.Format = f;
        c.SampleDesc.Count = 1;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        const bool ok = dev && SUCCEEDED(dev->CreateTexture2D(&c, nullptr, &g_beforeTex))
                     && SUCCEEDED(dev->CreateShaderResourceView(g_beforeTex, nullptr, &g_beforeSrv));
        if (dev) dev->Release();
        g_beforeDesc = c;
        if (!ok) { Drop(g_beforeSrv); Drop(g_beforeTex); }
    }
    if (g_beforeTex) {
        if (d.SampleDesc.Count > 1) ctx->ResolveSubresource(g_beforeTex, 0, tex, 0, f);
        else if (d.Format == f && d.MipLevels == 1 && d.ArraySize == 1) ctx->CopyResource(g_beforeTex, tex);
        else ctx->CopySubresourceRegion(g_beforeTex, 0, 0, 0, 0, tex, 0, nullptr);
    }
    tex->Release();
    return g_beforeSrv;
}

// --- What the see-through draws before the AO add to the picture: their colour times their own source factor, summed
// in half floats at the scene's size, cleared at each frame's first AO reader.
ID3D11Texture2D* g_transTex = nullptr;
ID3D11RenderTargetView* g_transRtv = nullptr;
ID3D11ShaderResourceView* g_transSrv = nullptr;
UINT g_transW = 0, g_transH = 0;
bool g_transUsed = false;
ID3D11BlendState* g_transBlend[2] = {};   // 0 the source as it is, 1 times its alpha

void TransPrepare(ID3D11DeviceContext* ctx, UINT w, UINT h) {
    g_transUsed = false;
    if (g_transTex && (g_transW != w || g_transH != h)) { Drop(g_transTex); Drop(g_transRtv); Drop(g_transSrv); }
    if (!g_transTex && w && h) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_transTex))
            && SUCCEEDED(dev->CreateRenderTargetView(g_transTex, nullptr, &g_transRtv))
            && SUCCEEDED(dev->CreateShaderResourceView(g_transTex, nullptr, &g_transSrv))) {
            g_transW = w;
            g_transH = h;
        }
        for (int k = 0; k < 2 && g_transTex; ++k) {
            if (g_transBlend[k]) continue;
            D3D11_BLEND_DESC bd = {};
            bd.RenderTarget[0].BlendEnable = TRUE;
            bd.RenderTarget[0].SrcBlend = k ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN
                                                     | D3D11_COLOR_WRITE_ENABLE_BLUE;
            dev->CreateBlendState(&bd, &g_transBlend[k]);
        }
        dev->Release();
    }
    if (g_transRtv) {
        const FLOAT zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ctx->ClearRenderTargetView(g_transRtv, zero);
    }
}

// A see-through draw into the picture before the AO, just drawn: once more into what such draws add, with the
// engine's depth target and its own source factor.
void TransRedraw(ID3D11DeviceContext* ctx, ContextState* st) {
    if (!st->blend || !g_transRtv || st->rtW != g_sceneW || st->rtH != g_sceneH || st->deferred) return;
    // Only the engine's two ways of laying a surface over what is there: over it by the source's alpha, or added.
    const bool alphaOver = st->blendSrc == D3D11_BLEND_SRC_ALPHA && st->blendDst == D3D11_BLEND_INV_SRC_ALPHA;
    const bool added = (st->blendSrc == D3D11_BLEND_ONE || st->blendSrc == D3D11_BLEND_SRC_ALPHA)
                    && st->blendDst == D3D11_BLEND_ONE;
    if (!alphaOver && !added) return;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    const bool intoScene = rtvs[0] && rtvs[0] == g_sceneRtv;
    ID3D11BlendState* blend = g_transBlend[st->blendSrc == D3D11_BLEND_SRC_ALPHA ? 1 : 0];
    if (intoScene && blend) {
        ID3D11BlendState* oldBlend = nullptr;
        FLOAT factor[4];
        UINT mask = 0;
        ctx->OMGetBlendState(&oldBlend, factor, &mask);
        ctx->OMSetRenderTargets(1, &g_transRtv, dsv);
        const FLOAT one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        ctx->OMSetBlendState(blend, one, 0xFFFFFFFFu);
        CoreRedraw(ctx, st);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        ctx->OMSetBlendState(oldBlend, factor, mask);
        if (oldBlend) oldBlend->Release();
        g_transUsed = true;
        g_transDraws.fetch_add(1, std::memory_order_relaxed);
    }
    for (ID3D11RenderTargetView* v : rtvs) if (v) v->Release();
    if (dsv) dsv->Release();
}

// --- What the surfaces glow with, which the AO is not to darken (it would press self illuminated materials and their
// bloom down). The engine adds the glow to the colour after
// its lighting, so the picture holds lit + glow. A reader whose variant glows (it reads g_tSelfIllumMask) is drawn twice
// more into a target of its own, with the engine's depth: once as it is, added, and once with g_vSelfIllumTintScaled
// (the factor the shader multiplies its glow by, in the material's b0) set to zero, taken away. What is left is the
// glow, by the engine's own formula, which the pass over the picture adds back times (1 - AO).
// A glowing surface drawn before something that covers it (a character, which the engine's depth prepass lacks) is
// still in the glow's target. So each glowing draw is drawn a third time, depth only, into a depth target of our own:
// the nearest glowing surface at each pixel. The pass keeps the glow only where the scene's final depth is not nearer.
ID3D11Texture2D* g_glowTex = nullptr;          // at the scene target's size and samples
ID3D11RenderTargetView* g_glowRtv = nullptr;
ID3D11Texture2D* g_glowOne = nullptr;          // resolved, one sample
ID3D11ShaderResourceView* g_glowSrv = nullptr;
D3D11_TEXTURE2D_DESC g_glowDesc = {};
bool g_glowUsed = false;
ID3D11BlendState* g_glowBlend[2] = {};         // 0 added, 1 taken away
ID3D11DepthStencilState* g_glowDepth = nullptr;
ID3D11Buffer* g_glowCb = nullptr;              // the material's b0 with the glow's factor at zero
UINT g_glowCbBytes = 0;
std::vector<uint8_t> g_glowCbData;
std::atomic<long long> g_glowDraws{0};
ID3D11Texture2D* g_glowZTex = nullptr;          // the nearest glowing surface's depth, one sample, cleared to 1
ID3D11DepthStencilView* g_glowZDsv = nullptr;
ID3D11ShaderResourceView* g_glowZSrv = nullptr;
ID3D11DepthStencilState* g_glowZState = nullptr;
bool g_glowAll = false;      // "glowall": the glow added wherever it was drawn, as before
bool g_finalDepth = false;   // this frame's depth slot has the scene's own depth, taken after the opaque surfaces

void GlowPrepare(ID3D11DeviceContext* ctx) {
    g_glowUsed = false;
    if (!g_sceneRtv) return;
    ID3D11Resource* res = nullptr;
    g_sceneRtv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    if (res) res->Release();
    if (!tex) return;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    tex->Release();
    if (!g_glowTex || g_glowDesc.Width != d.Width || g_glowDesc.Height != d.Height
        || g_glowDesc.SampleDesc.Count != d.SampleDesc.Count || g_glowDesc.SampleDesc.Quality != d.SampleDesc.Quality) {
        Drop(g_glowRtv); Drop(g_glowTex); Drop(g_glowSrv); Drop(g_glowOne);
        Drop(g_glowZDsv); Drop(g_glowZSrv); Drop(g_glowZTex);
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC c = {};
        c.Width = d.Width;
        c.Height = d.Height;
        c.MipLevels = 1;
        c.ArraySize = 1;
        c.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        c.SampleDesc = d.SampleDesc;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = D3D11_BIND_RENDER_TARGET | (d.SampleDesc.Count > 1 ? 0 : D3D11_BIND_SHADER_RESOURCE);
        bool ok = SUCCEEDED(dev->CreateTexture2D(&c, nullptr, &g_glowTex))
               && SUCCEEDED(dev->CreateRenderTargetView(g_glowTex, nullptr, &g_glowRtv));
        if (ok && d.SampleDesc.Count > 1) {
            D3D11_TEXTURE2D_DESC o = c;
            o.SampleDesc.Count = 1;
            o.SampleDesc.Quality = 0;
            o.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ok = SUCCEEDED(dev->CreateTexture2D(&o, nullptr, &g_glowOne))
              && SUCCEEDED(dev->CreateShaderResourceView(g_glowOne, nullptr, &g_glowSrv));
        } else if (ok) {
            ok = SUCCEEDED(dev->CreateShaderResourceView(g_glowTex, nullptr, &g_glowSrv));
        }
        if (ok) {
            D3D11_TEXTURE2D_DESC z = c;
            z.Format = DXGI_FORMAT_R32_TYPELESS;
            z.SampleDesc.Count = 1;
            z.SampleDesc.Quality = 0;
            z.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
            D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
            dv.Format = DXGI_FORMAT_D32_FLOAT;
            dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = DXGI_FORMAT_R32_FLOAT;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            ok = SUCCEEDED(dev->CreateTexture2D(&z, nullptr, &g_glowZTex))
              && SUCCEEDED(dev->CreateDepthStencilView(g_glowZTex, &dv, &g_glowZDsv))
              && SUCCEEDED(dev->CreateShaderResourceView(g_glowZTex, &sv, &g_glowZSrv));
        }
        if (ok && !g_glowZState) {
            D3D11_DEPTH_STENCIL_DESC dd = {};
            dd.DepthEnable = TRUE;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            dd.DepthFunc = D3D11_COMPARISON_LESS;
            ok = SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_glowZState));
        }
        for (int k = 0; k < 2 && ok; ++k) {
            if (g_glowBlend[k]) continue;
            D3D11_BLEND_DESC bd = {};
            bd.RenderTarget[0].BlendEnable = TRUE;
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOp = k ? D3D11_BLEND_OP_REV_SUBTRACT : D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN
                                                     | D3D11_COLOR_WRITE_ENABLE_BLUE;
            ok = SUCCEEDED(dev->CreateBlendState(&bd, &g_glowBlend[k]));
        }
        if (ok && !g_glowDepth) {
            // Where the engine's depth has this very surface: the same vertex shader puts it at the same depth.
            D3D11_DEPTH_STENCIL_DESC dd = {};
            dd.DepthEnable = TRUE;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            ok = SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_glowDepth));
        }
        dev->Release();
        g_glowDesc = d;
        if (!ok) {
            Drop(g_glowRtv); Drop(g_glowTex); Drop(g_glowSrv); Drop(g_glowOne);
            Drop(g_glowZDsv); Drop(g_glowZSrv); Drop(g_glowZTex);
            Log("[RTAO] the glow's target could not be made; glowing surfaces get the AO too");
            return;
        }
    }
    const FLOAT zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ctx->ClearRenderTargetView(g_glowRtv, zero);
    ctx->ClearDepthStencilView(g_glowZDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
}

// After a reader is drawn: its glow into the glow's target, when its variant glows.
void GlowRedraw(ID3D11DeviceContext* ctx, ContextState* st) {
    if (!g_glowRtv || !st->psCb[0] || TextureSlot(st->psLayout, "g_tSelfIllumMask") < 0) return;
    uint8_t reg = 0;
    uint16_t off = 0;
    if (!ShaderDbFind(st->psLayout, "g_vSelfIllumTintScaled", &reg, &off) || reg != SDB_REG_FLOAT4) return;
    UINT bytes = 0;
    if (!CoreCbRead(st->psCb[0], 0, nullptr, 0, &bytes) || bytes < (UINT)off * 4 + 12 || bytes % 16) return;
    g_glowCbData.resize(bytes);
    if (!CoreCbRead(st->psCb[0], 0, g_glowCbData.data(), bytes)) return;
    memset(g_glowCbData.data() + (size_t)off * 4, 0, 12);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    if (!g_glowCb || g_glowCbBytes != bytes) {
        Drop(g_glowCb);
        g_glowCbBytes = 0;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = bytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_glowCb))) g_glowCbBytes = bytes;
    }
    dev->Release();
    if (!g_glowCb) return;
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    ctx->UpdateSubresource(g_glowCb, 0, nullptr, g_glowCbData.data(), 0, 0);
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    ID3D11BlendState* oldBlend = nullptr;
    FLOAT factor[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&oldBlend, factor, &mask);
    ID3D11DepthStencilState* oldDepth = nullptr;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&oldDepth, &ref);
    ID3D11Buffer* oldCb = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &oldCb);
    if (dsv) {
        const FLOAT one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        ctx->OMSetRenderTargets(1, &g_glowRtv, dsv);
        ctx->OMSetDepthStencilState(g_glowDepth, 0);
        ctx->OMSetBlendState(g_glowBlend[0], one, 0xFFFFFFFFu);
        CoreRedraw(ctx, st);
        ctx->PSSetConstantBuffers(0, 1, &g_glowCb);
        ctx->OMSetBlendState(g_glowBlend[1], one, 0xFFFFFFFFu);
        CoreRedraw(ctx, st);
        ID3D11PixelShader* oldPs = nullptr;
        ID3D11ClassInstance* psCi[256];
        UINT psCiN = 256;
        ctx->PSGetShader(&oldPs, psCi, &psCiN);
        ctx->PSSetShader(nullptr, nullptr, 0);
        ctx->OMSetRenderTargets(0, nullptr, g_glowZDsv);
        ctx->OMSetDepthStencilState(g_glowZState, 0);
        CoreRedraw(ctx, st);
        ctx->PSSetShader(oldPs, psCi, psCiN);
        if (oldPs) oldPs->Release();
        for (UINT i = 0; i < psCiN; ++i) if (psCi[i]) psCi[i]->Release();
        g_glowUsed = true;
        g_glowDraws.fetch_add(1, std::memory_order_relaxed);
    }
    ctx->PSSetConstantBuffers(0, 1, &oldCb);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    ctx->OMSetBlendState(oldBlend, factor, mask);
    ctx->OMSetDepthStencilState(oldDepth, ref);
    t_CorePassThrough = pass;
    for (ID3D11RenderTargetView* v : rtvs) if (v) v->Release();
    if (dsv) dsv->Release();
    if (oldBlend) oldBlend->Release();
    if (oldDepth) oldDepth->Release();
    if (oldCb) oldCb->Release();
}

// The glow, one sample a pixel, for the pass over the picture; null when nothing glowed this frame.
ID3D11ShaderResourceView* GlowForPass(ID3D11DeviceContext* ctx) {
    if (!g_glowUsed || !g_glowSrv) return nullptr;
    if (g_glowOne) ctx->ResolveSubresource(g_glowOne, 0, g_glowTex, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
    return g_glowSrv;
}

// --- The pass that puts the AO on the picture: dest x AO + add, one draw, the
// blend's second source the factor on what is there. add is what the see-through draws added and, with the fogs kept
// out (ADD_FOG), what the engine's fogs add at the pixel, both times (1 - AO): the opaque shaders end with the
// gradient fog, the cube fog, the volumetric fog and the height fog, each a blend towards something or an add, so run
// on black the same steps give what they add, F. Each step as vr_complex has it, with the engine's own constant
// buffers bound at the same registers.
ID3D11VertexShader* g_mulVs = nullptr;
ID3D11PixelShader* g_mulPs = nullptr;
ID3D11PixelShader* g_mulPsFog = nullptr;
ID3D11Buffer* g_mulCb = nullptr;
ID3D11SamplerState* g_mulSamp[2] = {};   // linear clamp (the fog volume and cube), point wrap (the blue noise)
ID3D11BlendState* g_mulBlend = nullptr;
ID3D11DepthStencilState* g_mulDepth = nullptr;
ID3D11RasterizerState* g_mulRaster = nullptr;
bool g_mulBroken = false;

const char kMulVs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
    "}\n";
const char kMulPs[] =
    "Texture2D<float4> g_Ao : register(t0);\n"
    "Texture2D<float4> g_SeeThrough : register(t1);\n"
    "Texture2D<float4> g_Glow : register(t7);\n"
    "Texture2D<float> g_GlowZ : register(t8);\n"
    "Texture2D<float> g_SceneZ : register(t9);\n"
    "#ifdef ADD_FOG\n"
    "Texture2D<float> g_Depth : register(t2);\n"
    "Texture3D<float4> g_FogVolume : register(t3);\n"
    "Texture2D<float4> g_BlueNoise : register(t4);\n"
    "TextureCube<float4> g_FogCube : register(t5);\n"
    "Texture2D<float4> g_Before : register(t6);\n"
    "SamplerState g_Clamp : register(s0);\n"
    "SamplerState g_Wrap : register(s1);\n"
    "cbuffer Mine : register(b0) { row_major float4x4 g_ProjToRel; float4 g_Range; float4 g_Mode; };\n"
    "cbuffer PerViewPs : register(b1) { float4 cb1[41]; };\n"
    "cbuffer PerViewVr : register(b3) { float4 cb3[37]; };\n"
    "cbuffer Lighting : register(b6) { float4 cb6[1314]; };\n"
    // The fog switches (b3 register 0, x and w) are integers, 1 and not 1.0: read as floats they are denormals.
    "float Pow(float x, float y) { return exp2(log2(x) * y); }\n"
    "float3 FogAdds(float2 pix) {\n"
    "    const float span = max(g_Range.y - g_Range.x, 1e-6f);\n"
    "    const float z = (g_Depth.Load(int3(pix, 0)) - g_Range.x) / span;\n"
    "    if (!(z > 1e-6f && z < 1.0f - 1e-6f)) return float3(0.0f, 0.0f, 0.0f);\n"
    "    float2 d = (pix / g_Range.zw) * 2.0f - 1.0f;\n"
    "    d.y = -d.y;\n"
    "    const float4 h = mul(float4(d, z, 1.0f), g_ProjToRel);\n"
    "    const float3 rel = h.xyz / h.w;\n"
    "    const float3 wp = rel + cb1[40].xyz;\n"
    "    float3 c = float3(0.0f, 0.0f, 0.0f);\n"
    "    const float dxy2 = dot(rel.xy, rel.xy);\n"
    "    if (cb3[13].x < dxy2 && wp.z < cb3[13].y) {\n"
    "        const float2 g = saturate(cb3[10].wz * float2(wp.z, sqrt(dxy2)) + cb3[10].yx);\n"
    "        const float f = Pow(g.x, cb3[11].y) * Pow(g.y, cb3[11].x) * cb3[12].w;\n"
    "        c += f * (cb3[12].xyz - c);\n"
    "    }\n"
    "    const float d2 = dot(rel, rel);\n"
    "    if (cb3[20].x < d2 || cb3[20].y < wp.z) {\n"
    "        float3 dir = float3(dot(rel, cb3[16].xyz), dot(rel, cb3[17].xyz), dot(rel, cb3[18].xyz));\n"
    "        dir *= rsqrt(dot(dir, dir));\n"
    "        float f = max(Pow(max(sqrt(d2) * cb3[14].y + cb3[14].x, 0.0f), cb3[14].w),\n"
    "                      Pow(max(wp.z * cb3[15].y + cb3[15].x, 0.0f), cb3[15].z));\n"
    "        const float lod = saturate(1.0f - f * cb3[14].z) * cb3[15].w;\n"
    "        const float3 cube = g_FogCube.SampleLevel(g_Clamp, dir, lod).xyz;\n"
    "        f = min(f, 1.0f);\n"
    "        c += f * (cube - c);\n"
    "    }\n"
    "    if (asuint(cb3[0].x) != 0u) {\n"
    "        const float2 nuv = pix * cb6[1313].z + cb6[1313].xy;\n"
    "        const float3 p = wp + (g_BlueNoise.Sample(g_Wrap, nuv).xyz * cb3[25].x + cb3[25].y);\n"
    "        float3 q = p.x * cb3[28].xyw + p.y * cb3[29].xyw + p.z * cb3[30].xyw + cb3[31].xyw;\n"
    "        q.xy /= q.z;\n"
    "        q = q * cb3[26].xyz + cb3[27].xyz;\n"
    "        q.z = sqrt(max(q.z, 0.0f));\n"
    "        const float4 v = g_FogVolume.Sample(g_Clamp, q);\n"
    "        c = c * v.w + v.xyz;\n"
    "    }\n"
    "    if (asuint(cb3[0].w) != 0u) {\n"
    "        const float f = Pow(saturate(cb3[21].y * length(wp - cb3[22].xyz) + cb3[21].x), cb3[22].w) * cb3[23].w;\n"
    "        c += f * (cb3[23].xyz - c);\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    // Whether the engine put the fogs on this pixel: a material can have them off, and then what they would add is not
    // in the picture. Where they are on, the picture is never below what they add.
    "float3 FogKept(float2 pix) {\n"
    "    const float3 f = max(FogAdds(pix), 0.0f);\n"
    "    const float3 before = g_Before.Load(int3(pix, 0)).rgb;\n"
    "    return any(before < f * 0.97f - 1e-3f) ? float3(0.0f, 0.0f, 0.0f) : f;\n"
    "}\n"
    "#endif\n"
    "struct Out { float4 add : SV_Target0; float4 mul : SV_Target1; };\n"
    "Out main(float4 pos : SV_Position) {\n"
    "    const float4 v = g_Ao.Load(int3(pos.xy, 0));\n"
    "    float3 glow = max(g_Glow.Load(int3(pos.xy, 0)).rgb, 0.0f);\n"
    // Unbound (no depth this frame) both read 0 and the glow is kept.
    "    if (g_SceneZ.Load(int3(pos.xy, 0)) < g_GlowZ.Load(int3(pos.xy, 0)) - 1e-6f) glow = float3(0.0f, 0.0f, 0.0f);\n"
    "    Out o;\n"
    "    o.add = float4((g_SeeThrough.Load(int3(pos.xy, 0)).rgb + glow) * (1.0f - v.r), 0.0f);\n"
    "#ifdef ADD_FOG\n"
    "    if (v.r < 1.0f) o.add.rgb += FogKept(pos.xy) * (1.0f - v.r);\n"
    "#endif\n"
    "    o.mul = float4(v.r, v.r, v.r, 1.0f);\n"
    "#ifdef ADD_FOG\n"
    "    if (g_Mode.x > 0.5f) { o.add = float4(FogKept(pos.xy), 0.0f); o.mul = float4(0.0f, 0.0f, 0.0f, 1.0f); }\n"
    "#endif\n"
    "    return o;\n"
    "}\n";

bool EnsureMultiply(ID3D11DeviceContext* ctx) {
    if (g_mulBroken) return false;
    if (g_mulVs && g_mulPs && g_mulBlend && g_mulDepth && g_mulRaster) return true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    bool ok = true;
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    if (SUCCEEDED(D3DCompile(kMulVs, sizeof(kMulVs) - 1, "rtao_mul_vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code,
                             &err)) && code)
        ok = SUCCEEDED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_mulVs));
    else ok = false;
    Drop(code);
    if (err) { Log("[RTAO] the multiply's vertex shader: %s", (const char*)err->GetBufferPointer()); Drop(err); }
    for (int kind = 0; kind < 2; ++kind) {
        const D3D_SHADER_MACRO fog[] = { { "ADD_FOG", "1" }, { nullptr, nullptr } };
        ID3D11PixelShader** out = kind ? &g_mulPsFog : &g_mulPs;
        if (SUCCEEDED(D3DCompile(kMulPs, sizeof(kMulPs) - 1, "rtao_mul_ps", kind ? fog : nullptr, nullptr, "main",
                                 "ps_5_0", 0, 0, &code, &err)) && code) {
            const bool made = SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out));
            if (!kind) ok = ok && made;   // without the fog's version the AO goes on over the fog too
        } else if (!kind) {
            ok = false;
        }
        Drop(code);
        if (err) { Log("[RTAO] the multiply's pixel shader: %s", (const char*)err->GetBufferPointer()); Drop(err); }
    }
    D3D11_BUFFER_DESC cd = {};
    cd.ByteWidth = 96;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&cd, nullptr, &g_mulCb);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    dev->CreateSamplerState(&sd, &g_mulSamp[0]);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    dev->CreateSamplerState(&sd, &g_mulSamp[1]);
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN
                                             | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &g_mulBlend));
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_mulDepth));
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_mulRaster));
    dev->Release();
    if (!ok) {
        g_mulBroken = true;
        Log("[RTAO] the pass over the picture could not be made; the AO stays off the picture");
    }
    return ok;
}

void MultiplyNow(ID3D11DeviceContext* ctx) {
    if (g_showSlot < 0 || !g_slots[g_showSlot].aoSrv || !g_sceneRtv || !EnsureMultiply(ctx)) return;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ctx->IAGetPrimitiveTopology(&topo);
    ID3D11InputLayout* il = nullptr;
    ctx->IAGetInputLayout(&il);
    ID3D11VertexShader* vs = nullptr;
    ID3D11ClassInstance* vsCi[256];
    UINT vsCiN = 256;
    ctx->VSGetShader(&vs, vsCi, &vsCiN);
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
    ID3D11ShaderResourceView* srv0[10] = {};
    ctx->PSGetShaderResources(0, 10, srv0);
    ID3D11Buffer* cb0[7] = {};
    ctx->PSGetConstantBuffers(0, 7, cb0);
    ID3D11SamplerState* smp0[2] = {};
    ctx->PSGetSamplers(0, 2, smp0);
    ID3D11RasterizerState* rs = nullptr;
    ctx->RSGetState(&rs);
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&nvp, vps);
    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4];
    UINT sm = 0;
    ctx->OMGetBlendState(&bs, bf, &sm);
    ID3D11DepthStencilState* dss = nullptr;
    UINT sref = 0;
    ctx->OMGetDepthStencilState(&dss, &sref);
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);

    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g_mulVs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    const Slot& shown = g_slots[g_showSlot];
    float projToRel[16];
    bool fog = !g_debug && !g_noFogFix && g_mulPsFog && g_mulCb && g_mulSamp[0] && g_mulSamp[1] && g_fogReady
            && g_camValid && shown.depthSrv && g_slotW >= g_sceneW && g_slotH >= g_sceneH
            && Inverse(g_cam.worldToProj, projToRel);
    ID3D11ShaderResourceView* before = fog ? PictureBefore(ctx) : nullptr;
    fog = fog && before;
    if (fog) {
        // The inverse gives a point in the camera's offset space; less the eye there, it is the point less the camera,
        // which the shaders add their camera (b1 register 40) to.
        float eye[3];
        for (int c = 0; c < 3; ++c) eye[c] = g_cam.position[c] + g_cam.offset[c];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) projToRel[r * 4 + c] -= projToRel[r * 4 + 3] * eye[c];
        float consts[24] = {};
        memcpy(consts, projToRel, sizeof(projToRel));
        consts[16] = g_minDepth;
        consts[17] = g_maxDepth;
        consts[18] = (float)g_sceneW;
        consts[19] = (float)g_sceneH;
        consts[20] = g_fogOnly ? 1.0f : 0.0f;
        ctx->UpdateSubresource(g_mulCb, 0, nullptr, consts, 0, 0);
        ID3D11Buffer* cbs[7] = { g_mulCb, g_fogCb[0], nullptr, g_fogCb[1], nullptr, nullptr, g_fogCb[2] };
        ctx->PSSetConstantBuffers(0, 7, cbs);
        ctx->PSSetSamplers(0, 2, g_mulSamp);
        g_fogFrames.fetch_add(1, std::memory_order_relaxed);
    }
    ctx->PSSetShader(fog ? g_mulPsFog : g_mulPs, nullptr, 0);
    ID3D11ShaderResourceView* glow = g_debug ? nullptr : GlowForPass(ctx);
    ID3D11ShaderResourceView* sceneZ = glow && !g_glowAll && g_finalDepth && g_depthSlot >= 0 && g_glowZSrv
                                     ? g_slots[g_depthSlot].depthSrv : nullptr;
    ID3D11ShaderResourceView* mine[10] = { shown.aoSrv, g_transUsed ? g_transSrv : nullptr, fog ? shown.depthSrv : nullptr,
                                           fog ? g_fogTex[0] : nullptr, fog ? g_fogTex[1] : nullptr,
                                           fog ? g_fogTex[2] : nullptr, fog ? before : nullptr, glow,
                                           sceneZ ? g_glowZSrv : nullptr, sceneZ };
    ctx->PSSetShaderResources(0, 10, mine);
    ctx->RSSetState(g_mulRaster);
    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)g_sceneW;
    vp.Height = (FLOAT)g_sceneH;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    const FLOAT one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ctx->OMSetBlendState(g_mulBlend, one, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(g_mulDepth, 0);
    ctx->OMSetRenderTargets(1, &g_sceneRtv, nullptr);
    CoreRawDraw(ctx, 3);

    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    ctx->OMSetDepthStencilState(dss, sref);
    ctx->OMSetBlendState(bs, bf, sm);
    ctx->RSSetViewports(nvp, vps);
    ctx->RSSetState(rs);
    ctx->PSSetShaderResources(0, 10, srv0);
    ctx->PSSetConstantBuffers(0, 7, cb0);
    ctx->PSSetSamplers(0, 2, smp0);
    ctx->PSSetShader(ps, psCi, psCiN);
    ctx->DSSetShader(ds, nullptr, 0);
    ctx->HSSetShader(hs, nullptr, 0);
    ctx->GSSetShader(gs, nullptr, 0);
    ctx->VSSetShader(vs, vsCi, vsCiN);
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(topo);
    t_CorePassThrough = pass;
    for (ID3D11RenderTargetView* v : rtvs) if (v) v->Release();
    IUnknown* rel[] = { il, vs, gs, hs, ds, ps, rs, bs, dss, dsv };
    for (IUnknown* x : rel) if (x) x->Release();
    for (ID3D11ShaderResourceView* x : srv0) if (x) x->Release();
    for (ID3D11Buffer* x : cb0) if (x) x->Release();
    for (ID3D11SamplerState* x : smp0) if (x) x->Release();
    for (UINT i = 0; i < vsCiN; ++i) if (vsCi[i]) vsCi[i]->Release();
    for (UINT i = 0; i < psCiN; ++i) if (psCi[i]) psCi[i]->Release();
    g_mulDone.fetch_add(1, std::memory_order_relaxed);
    g_mulTotal.fetch_add(1, std::memory_order_relaxed);
    g_lastMultiplyAt.store(GetTickCount64(), std::memory_order_relaxed);
}

// --- The dump: the slot's AO and depth as grey images, once a line, and their figures for the out file.
std::string g_dumpLine;

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) bits = sign;
        else {
            uint32_t m = man;
            int e = -1;
            do { m <<= 1; ++e; } while ((m & 0x400u) == 0);
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 31) bits = sign | 0x7F800000u | (man << 13);
    else bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

std::string DumpSlot(ID3D11DeviceContext* ctx, int k, UINT w, UINT h) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return " | no device";
    std::string out;
    for (int which = 0; which < 2; ++which) {
        ID3D11Texture2D* src = which == 0 ? g_slots[k].ao11 : g_slots[k].depth11;
        D3D11_TEXTURE2D_DESC d = {};
        src->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &st)) || !st) { out += " | staging failed"; continue; }
        ctx->CopyResource(st, src);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            std::vector<float> v((size_t)w * h);
            for (UINT y = 0; y < h; ++y) {
                const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                for (UINT x = 0; x < w; ++x)
                    v[(size_t)y * w + x] = which == 0 ? HalfToFloat(((const uint16_t*)row)[x * 4]) : ((const float*)row)[x];
            }
            if (which == 0 && g_debug) {
                FILE* nf = nullptr;
                if (_wfopen_s(&nf, CorePath(L"s2fm_rtao_normal.ppm").c_str(), L"wb") == 0 && nf) {
                    fprintf(nf, "P6\n%u %u\n255\n", w, h);
                    for (UINT y = 0; y < h; ++y) {
                        const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                        for (UINT x = 0; x < w; ++x) {
                            uint8_t b3[3];
                            for (int c = 0; c < 3; ++c) {
                                float f = HalfToFloat(((const uint16_t*)row)[x * 4 + 1 + c]);
                                f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
                                b3[c] = (uint8_t)(f * 255.0f + 0.5f);
                            }
                            fwrite(b3, 1, 3, nf);
                        }
                    }
                    fclose(nf);
                }
            }
            ctx->Unmap(st, 0);
            double sum = 0, lo = 1e30, hi = -1e30;
            long long below = 0, inside = 0;
            for (float f : v) {
                sum += f;
                if (f < lo) lo = f;
                if (f > hi) hi = f;
                if (f < 0.99f) ++below;
                if (f > 0.0f && f < 1.0f) ++inside;
            }
            char b[200];
            if (which == 0)
                sprintf_s(b, " | AO: min %.4f mean %.4f max %.4f, below 0.99 in %.2f%%", lo, sum / v.size(), hi,
                          100.0 * below / v.size());
            else
                sprintf_s(b, " | depth: min %.6f mean %.6f max %.6f, inside (0,1) %.2f%%", lo, sum / v.size(), hi,
                          100.0 * inside / v.size());
            out += b;
            double dlo = 1e30, dhi = -1e30;
            if (which == 1)
                for (float f : v) if (f > 0.0f && f < 1.0f) { if (f < dlo) dlo = f; if (f > dhi) dhi = f; }
            FILE* f = nullptr;
            if (_wfopen_s(&f, CorePath(which == 0 ? L"s2fm_rtao_ao.pgm" : L"s2fm_rtao_depth.pgm").c_str(), L"wb") == 0
                && f) {
                fprintf(f, "P5\n%u %u\n255\n", w, h);
                for (float x : v) {
                    float g = which == 0 ? x : (x > 0.0f && x < 1.0f && dhi > dlo ? (float)((x - dlo) / (dhi - dlo)) : 0.0f);
                    g = g < 0 ? 0 : g > 1 ? 1 : g;
                    fputc((int)(g * 255.0f + 0.5f), f);
                }
                fclose(f);
            }
        }
        st->Release();
    }
    dev->Release();
    return out;
}

// "dump": where something glows this frame, and where the pass leaves that glow out because the scene's depth is
// nearer than the glowing surface's (s2fm_rtao_glow.ppm: green kept, red left out).
bool ReadBack(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, std::vector<uint8_t>& out, UINT& pitch) {
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11Texture2D* st = nullptr;
    bool ok = dev && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &st)) && st;
    if (dev) dev->Release();
    if (!ok) return false;
    ctx->CopyResource(st, src);
    D3D11_MAPPED_SUBRESOURCE m = {};
    ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        pitch = m.RowPitch;
        out.assign((const uint8_t*)m.pData, (const uint8_t*)m.pData + (size_t)m.RowPitch * d.Height);
        ctx->Unmap(st, 0);
    }
    st->Release();
    return ok;
}

std::string GlowDump(ID3D11DeviceContext* ctx, int k, UINT w, UINT h) {
    if (!g_glowUsed || !g_glowZTex || !g_finalDepth || k < 0) return " | glow: none this frame";
    ID3D11Texture2D* glow = g_glowOne ? g_glowOne : g_glowTex;
    if (g_glowOne) ctx->ResolveSubresource(g_glowOne, 0, g_glowTex, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
    std::vector<uint8_t> gc, gz, sz;
    UINT gp = 0, zp = 0, sp = 0;
    if (!ReadBack(ctx, glow, gc, gp) || !ReadBack(ctx, g_glowZTex, gz, zp) || !ReadBack(ctx, g_slots[k].depth11, sz, sp))
        return " | glow: could not be read";
    if (w > g_glowDesc.Width) w = g_glowDesc.Width;
    if (h > g_glowDesc.Height) h = g_glowDesc.Height;
    long long glowing = 0, left = 0;
    FILE* f = nullptr;
    _wfopen_s(&f, CorePath(L"s2fm_rtao_glow.ppm").c_str(), L"wb");
    if (f) fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            const uint16_t* c = (const uint16_t*)(gc.data() + (size_t)y * gp) + x * 4;
            const float g = HalfToFloat(c[0]) + HalfToFloat(c[1]) + HalfToFloat(c[2]);
            const float zg = ((const float*)(gz.data() + (size_t)y * zp))[x];
            const float zs = ((const float*)(sz.data() + (size_t)y * sp))[x];
            uint8_t px[3] = { 0, 0, 0 };
            if (g > 1e-3f) {
                ++glowing;
                if (zs < zg - 1e-6f) { ++left; px[0] = 255; } else px[1] = 255;
            }
            if (f) fwrite(px, 1, 3, f);
        }
    if (f) fclose(f);
    char b[120];
    sprintf_s(b, " | glow: %.2f%% of the pixels, %.2f%% left out (covered)", 100.0 * glowing / ((double)w * h),
              100.0 * left / ((double)w * h));
    return b;
}

// --- The rays, at the place the AO goes on: the scene's depth over t9, our queue told to wait for it and the capture,
// the AO traced, the engine's context told to wait for it. Nothing on the CPU waits.
// The camera of this frame as a place and a direction: its position and where it looks (the projection's w column,
// which the refinement's jitter leaves alone).
void CameraKey(float eye[3], float forward[3]) {
    const float* m = g_cam.worldToProj;
    const float len = std::sqrt(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
    for (int i = 0; i < 3; ++i) {
        eye[i] = g_cam.position[i];
        forward[i] = len > 0.0f ? m[3 + 4 * i] / len : 0.0f;
    }
}

bool SameCamera(const Slot& s, const float eye[3], const float forward[3]) {
    float d2 = 0.0f, dot = 0.0f;
    for (int i = 0; i < 3; ++i) {
        d2 += (s.eye[i] - eye[i]) * (s.eye[i] - eye[i]);
        dot += s.forward[i] * forward[i];
    }
    return d2 < 8.0f * 8.0f && dot > 0.998f;
}

void Unready(const char* why);   // below

void Fire(ID3D11DeviceContext* ctx) {
    g_showSlot = -1;
    g_finalDepth = false;
    const int k = g_depthSlot;
    const UINT vertices = RtaoCaptureVertices();
    if (k < 0 || !g_camValid) { g_missed.fetch_add(1, std::memory_order_relaxed); return; }
    const UINT w = g_sceneW < g_slotW ? g_sceneW : g_slotW;
    const UINT h = g_sceneH < g_slotH ? g_sceneH : g_slotH;
    const bool sceneDepth = !g_t9Depth && CopySceneDepth(ctx, k, w, h);
    g_finalDepth = sceneDepth;
    if (!sceneDepth && !g_depthFromT9) {
        // No depth for the rays at all (no t9 and the scene's could not be copied).
        Unready("the scene's depth could not be copied and there is no t9");
        g_missed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ID3D11DeviceContext4* c4 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&c4)) && c4) {
        c4->Signal(RtaoCapturedFence11(), ++g_capturedValue);
        ctx->Flush();
        RtaoFrame f = {};
        if (Inverse(g_cam.worldToProj, f.projToRays)) {
            for (int i = 0; i < 3; ++i) f.eye[i] = g_cam.position[i] + g_cam.offset[i];
            f.reach = g_radiusAsked > 0.0f ? g_radiusAsked : g_camRadius;
            // The bias as a share of the reach, cubed: S2FM's camera hands it over 0 to 1, and the camera's default,
            // 0.5, comes out a fiftieth of the reach, about the engine's own default.
            const float biasSlider = g_biasAsked >= 0.0f ? g_biasAsked : g_camBias;
            f.bias = 0.16f * biasSlider * biasSlider * biasSlider * f.reach;
            f.strength = g_strengthAsked >= 0.0f ? g_strengthAsked : g_camStrength;
            f.rays = (UINT)g_rays;
            f.width = w;
            f.height = h;
            f.seed = ++g_seed;
            f.minDepth = g_minDepth;
            f.maxDepth = g_maxDepth;
            f.vertices = vertices;
            f.drawList = RtaoCaptureDrawList();
            const std::vector<RtaoCutout>& cuts = RtaoCaptureCutoutList();
            f.cutouts = cuts.empty() ? nullptr : cuts.data();
            f.cutoutCount = (UINT)cuts.size();
            RtaoCaptureMasks(f.masks);
            // The views: the camera's, then the others' (the shadows'), from their own world plus offset to the
            // camera's, their triangles left out where the camera has them or the AO cannot reach.
            memcpy(f.worldToClip, g_cam.worldToProj, sizeof(f.worldToClip));
            const std::vector<RtaoCaptureView>& taken = RtaoCaptureViewList();
            std::vector<RtaoView> views(1);
            memcpy(views[0].clipToRays, f.projToRays, sizeof(f.projToRays));
            for (size_t i = 1; i < taken.size(); ++i) {
                RtaoView v = {};
                if (!Inverse(taken[i].worldToProj, v.clipToRays)) {
                    // Its triangles all to one point, left out.
                    memset(v.clipToRays, 0, sizeof(v.clipToRays));
                    v.clipToRays[15] = 1.0f;
                }
                for (int j = 0; j < 3; ++j) v.delta[j] = g_cam.offset[j] - taken[i].offset[j];
                v.cull = 1;
                views.push_back(v);
            }
            const std::vector<RtaoRun>& runs = RtaoCaptureRunList();
            f.views = views.data();
            f.viewCount = (UINT)views.size();
            f.runs = runs.empty() ? nullptr : runs.data();
            f.runCount = (UINT)runs.size();
            f.debug = g_debug;
            f.noRefit = g_noRefit;
            f.regrow = g_regrow;
            f.depth = g_slots[k].depth12;
            f.out = g_slots[k].ao12;
            f.slot = k;
            if (RtaoTrace(f, g_capturedValue, g_aoValue + 1)) {
                ++g_aoValue;
                c4->Wait(RtaoAoFence11(), g_aoValue);
                g_slots[k].busyUntil = g_aoValue;
                g_slots[k].hasAo = true;
                CameraKey(g_slots[k].eye, g_slots[k].forward);
                g_newestSlot = k;
                g_showSlot = k;
                g_traced.fetch_add(1, std::memory_order_relaxed);
            }
        }
        c4->Release();
    }
    if (g_showSlot < 0) {
        // Not traced this frame: the newest the card has finished, only if it was traced from about the same camera.
        // An AO from another view (a cut, a fast move) would put that view's shapes over this one.
        g_missed.fetch_add(1, std::memory_order_relaxed);
        g_notTraced10.fetch_add(1, std::memory_order_relaxed);
        float eye[3], forward[3];
        CameraKey(eye, forward);
        const int cands[2] = { g_newestSlot, g_newestSlot >= 0 ? 1 - g_newestSlot : -1 };
        for (int c : cands) {
            if (c < 0 || !g_slots[c].hasAo || !RtaoAoDone(g_slots[c].busyUntil)) continue;
            if (!SameCamera(g_slots[c], eye, forward)) {
                g_staleSkipped.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            g_showSlot = c;
            break;
        }
    }
    if (g_dumpWanted && !g_dumpDone && g_showSlot >= 0 && GetTickCount64() >= g_dumpAfter) {
        g_dumpDone = true;
        g_dumpLine = DumpSlot(ctx, g_showSlot, w, h) + GlowDump(ctx, k, w, h);
        char rb[96];
        sprintf_s(rb, " | viewport depth range %.3f to %.3f, %u triangles", g_minDepth, g_maxDepth, vertices / 3);
        g_dumpLine += rb;
    }
}

// The passes that come after the opaque surfaces: the first draw of one of them is where the AO goes on.
bool IsAfterOpaque(const char* name) {
    static const char* const kAfter[] = { "sky", "sfm_volumetrics_frustum", "downsample_depth", "gaussian_bloom_blur",
                                          "post_process", "sfm_accumulation", "copytexture" };
    if (!name || !*name) return false;
    for (const char* a : kAfter) if (!strcmp(name, a)) return true;
    return false;
}

// t9 as an AO reader has it bound. Some draw with the engine's 1x1 stand in there (the characters, among others), so
// it is kept from whichever reader has the real one.
void NoteT9(const ContextState* st, int depthSlotIndex) {
    if (g_noT9 || depthSlotIndex < 0 || depthSlotIndex >= 32) return;
    ID3D11ShaderResourceView* depth = st->psSrv[depthSlotIndex];
    UINT w = 0, h = 0;
    if (depth && depth != g_t9 && TexSize(depth, &w, &h) && w > 1 && h > 1) {
        Drop(g_t9);
        g_t9 = depth;
        g_t9->AddRef();
        g_t9W = w;
        g_t9H = h;
    }
}

// Why a frame's first AO reader could not start the AO, said once each.
void Unready(const char* why) {
    strcpy_s(g_why, why);
    static std::map<std::string, bool> said;
    if (said[why]) return;
    said[why] = true;
    Log("[RTAO] a frame without the AO: %s", why);
}

// The frame's first AO reader: the camera, t9 into a slot, the target, the fogs' inputs, the capture opened.
void FirstReader(ID3D11DeviceContext* ctx, ContextState* st) {
    g_depthSlot = -1;
    g_mulPending = false;
    // t9, the engine's own AO depth, is only made where the map's own geometry is drawn; a scene of models alone (a
    // void, a black room) has none. The rays start from the scene's own depth anyway (CopySceneDepth); t9 is only
    // the fallback ("t9depth") and the slots' size.
    if (!g_t9 && g_t9Depth) { Unready("no t9 seen yet (t9depth)"); return; }
    if (!st->rtW || !st->rtH) return;
    if (!EnsureResources(ctx, g_t9 ? g_t9W : st->rtW, g_t9 ? g_t9H : st->rtH)) return;
    g_camValid = st->rtW && st->rtH && CameraOf(st, st->rtW, st->rtH, &g_cam);
    if (!g_camValid) { Unready("the first AO reader's PerView is not the view's"); return; }
    // The slot not shown last.
    const int k = g_newestSlot == 0 ? 1 : 0;
    const UINT w = st->rtW < g_slotW ? st->rtW : g_slotW;
    const UINT h = st->rtH < g_slotH ? st->rtH : g_slotH;
    if (!w || !h) return;
    g_depthFromT9 = g_t9 != nullptr;
    if (g_t9) CopyDepth(ctx, k, w, h, g_copyCs, g_t9);
    g_depthSlot = k;
    // The depth range the engine draws this view with (S2FM keeps 0.05 to 0.95), which its depth buffer holds.
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&nvp, vp);
    g_minDepth = nvp ? vp[0].MinDepth : 0.0f;
    g_maxDepth = nvp ? vp[0].MaxDepth : 1.0f;
    Drop(g_sceneRtv);
    Drop(g_sceneDsv);
    ctx->OMGetRenderTargets(1, &g_sceneRtv, &g_sceneDsv);
    g_sceneW = st->rtW;
    g_sceneH = st->rtH;
    FogCapture(st);
    TransPrepare(ctx, st->rtW, st->rtH);
    GlowPrepare(ctx);
    RtaoCaptureBegin(g_cam.worldToProj);
    g_mulPending = g_sceneRtv != nullptr;
}

// --- "views": which views the engine draws between two of the view's presents, said for the next three. A view is a
// world to projection matrix on the draws' PerView; each with its draws, triangles, draws without a pixel shader or
// on a deferred context, its targets, the box its frustum spans in the world and whether it came before the frame's
// first AO reader. For the off screen occluders: what the engine draws besides the camera, which the capture leaves
// out.
struct ViewCensus {
    long long draws = 0, tris = 0, noPs = 0, deferred = 0, beforeReader = 0, readers = 0;
    UINT dsW = 0, dsH = 0, rtW = 0, rtH = 0;
    int rtFmt = 0, order = 0;
    float pv[204] = {};
    std::string vs, ps;
    // The AO readers by the viewport's depth range they are drawn with (thousandths) and whether they write depth: a 3D
    // sky is drawn in a range of its own.
    struct Readers {
        long long draws = 0, tris = 0, depthWrites = 0;
        std::string ps;
    };
    std::map<std::pair<int, int>, Readers> readerRanges;
};
std::mutex g_censusM;
std::atomic<int> g_censusLeft{0};
std::map<uint64_t, ViewCensus> g_census;
long long g_censusNoView = 0, g_censusNoViewTris = 0;

void CensusDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    const ContextState::Draw& d = st->draw;
    const long long tris = d.indirect ? 0 : (long long)(d.count / 3) * (d.instances ? d.instances : 1);
    float pv[204];
    const bool hasView = CorePerView(st, pv);
    std::lock_guard<std::mutex> lk(g_censusM);
    if (!hasView) {
        ++g_censusNoView;
        g_censusNoViewTris += tris;
        return;
    }
    uint64_t h = 1469598103934665603ull;
    const uint8_t* b = (const uint8_t*)&pv[PV_MAT_WORLD_TO_PROJ_MV];
    for (int i = 0; i < 64; ++i) h = (h ^ b[i]) * 1099511628211ull;
    ViewCensus& v = g_census[h];
    if (!v.draws) {
        v.order = (int)g_census.size();
        memcpy(v.pv, pv, sizeof(pv));
        v.dsW = st->dsW;
        v.dsH = st->dsH;
        v.rtW = st->rtW;
        v.rtH = st->rtH;
        v.rtFmt = (int)st->rtFormat;
        v.vs = st->vsLayout >= 0 ? ShaderDbName(st->vsLayout) : "?";
        v.ps = st->ps ? (st->psLayout >= 0 ? ShaderDbName(st->psLayout) : "?") : "none";
    }
    ++v.draws;
    v.tris += tris;
    // Each view's vertex shaders, once each by name, disassembled beside the DLL (s2fm_rtao_view<N>_<name>.txt): what a
    // view does to the positions besides its matrix.
    static std::map<std::string, bool> dumped;
    const std::string vsName = st->vsLayout >= 0 ? ShaderDbName(st->vsLayout) : "unknown";
    const std::string key = std::to_string(v.order) + "_" + vsName;
    if (st->vs && !dumped[key] && dumped.size() < 24) {
        dumped[key] = true;
        std::vector<uint8_t> code(1 << 20);
        UINT bytes = (UINT)code.size();
        ID3DBlob* dis = nullptr;
        if (CoreShaderBytecode(st->vs, code.data(), &bytes)
            && SUCCEEDED(D3DDisassemble(code.data(), bytes, D3D_DISASM_DISABLE_DEBUG_INFO, nullptr, &dis)) && dis) {
            FILE* f = nullptr;
            const std::wstring name = L"s2fm_rtao_view" + std::to_wstring(v.order) + L"_"
                                    + std::wstring(vsName.begin(), vsName.end()) + L".txt";
            if (_wfopen_s(&f, CorePath(name.c_str()).c_str(), L"wb") == 0 && f) {
                fwrite(dis->GetBufferPointer(), 1, dis->GetBufferSize(), f);
                fclose(f);
            }
            dis->Release();
        }
    }
    if (!st->ps) ++v.noPs;
    if (st->deferred) ++v.deferred;
    if (!g_firstReaderDone) ++v.beforeReader;
    if (st->psLayout >= 0 && TextureSlot(st->psLayout, "g_tDynamicAmbientOcclusion") >= 0) {
        ++v.readers;
        D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&nvp, vp);
        const std::pair<int, int> range(nvp ? (int)std::lround(vp[0].MinDepth * 1000.0f) : -1,
                                        nvp ? (int)std::lround(vp[0].MaxDepth * 1000.0f) : -1);
        ViewCensus::Readers& r = v.readerRanges[range];
        if (!r.draws) r.ps = ShaderDbName(st->psLayout);
        ++r.draws;
        r.tris += tris;
        if (st->depthWrites) ++r.depthWrites;
    }
}

void CensusSay() {
    std::lock_guard<std::mutex> lk(g_censusM);
    if (g_census.empty()) return;
    const int left = g_censusLeft.fetch_sub(1) - 1;
    Log("[RTAO] views since the last present: %zu, and %lld draws (%lld triangles) without a PerView",
        g_census.size(), g_censusNoView, g_censusNoViewTris);
    std::vector<const ViewCensus*> views;
    for (auto& kv : g_census) views.push_back(&kv.second);
    std::sort(views.begin(), views.end(), [](const ViewCensus* a, const ViewCensus* b) { return a->order < b->order; });
    for (const ViewCensus* v : views) {
        const float* m = &v->pv[PV_MAT_WORLD_TO_PROJ_MV];
        const float* off = &v->pv[PV_WORLD_TO_CAM_OFF_MV];
        const bool ortho = std::fabs(m[3]) + std::fabs(m[7]) + std::fabs(m[11]) < 1e-6f;
        // The frustum's corners in the world (clip z at 0 and 1; a corner at infinity left out).
        float inv[16], lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        int corners = 0;
        if (Inverse(m, inv)) {
            for (int c = 0; c < 8; ++c) {
                const float p[4] = { (c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f, (c & 4) ? 1.0f : 0.0f, 1.0f };
                float w[4];
                for (int j = 0; j < 4; ++j) w[j] = p[0] * inv[j] + p[1] * inv[4 + j] + p[2] * inv[8 + j] + p[3] * inv[12 + j];
                if (std::fabs(w[3]) < 1e-9f) continue;
                bool fine = true;
                float x[3];
                for (int j = 0; j < 3; ++j) {
                    x[j] = w[j] / w[3] - off[j];
                    fine = fine && std::isfinite(x[j]) && std::fabs(x[j]) < 1e7f;
                }
                if (!fine) continue;
                for (int j = 0; j < 3; ++j) {
                    lo[j] = x[j] < lo[j] ? x[j] : lo[j];
                    hi[j] = x[j] > hi[j] ? x[j] : hi[j];
                }
                ++corners;
            }
        }
        Log("[RTAO]  view %d: %lld draws, %lld triangles, %lld AO readers, %lld without a pixel shader, %lld deferred, "
            "%lld before the first reader", v->order, v->draws, v->tris, v->readers, v->noPs, v->deferred, v->beforeReader);
        Log("[RTAO]   target %ux%u fmt %d, depth %ux%u | %s, vs %s, ps %s", v->rtW, v->rtH, v->rtFmt, v->dsW, v->dsH,
            ortho ? "orthographic" : "perspective", v->vs.c_str(), v->ps.c_str());
        for (const auto& kv : v->readerRanges)
            Log("[RTAO]   AO readers at depth range %.3f..%.3f: %lld draws, %lld triangles, %lld writing depth, first %s",
                kv.first.first / 1000.0, kv.first.second / 1000.0, kv.second.draws, kv.second.tris,
                kv.second.depthWrites, kv.second.ps.c_str());
        Log("[RTAO]   eye %.0f %.0f %.0f, viewport %.0fx%.0f, frustum box %.0f..%.0f, %.0f..%.0f, %.0f..%.0f (%d corners)",
            v->pv[PV_CAMERA_POSITION_WS], v->pv[PV_CAMERA_POSITION_WS + 1], v->pv[PV_CAMERA_POSITION_WS + 2],
            v->pv[PV_VIEWPORT_SIZE], v->pv[PV_VIEWPORT_SIZE + 1], lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], corners);
    }
    g_census.clear();
    g_censusNoView = g_censusNoViewTris = 0;
    if (left <= 0) Log("[RTAO] views: done");
}

void BeforeDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    t_reader = false;
    if (g_censusLeft.load(std::memory_order_relaxed) > 0 && !CoreFrameForeign(st)) CensusDraw(ctx, st);
    if (!g_on.load(std::memory_order_relaxed) || g_broken || st->deferred || st->psLayout < 0 || CoreFrameForeign(st))
        return;
    const int aoSlot = TextureSlot(st->psLayout, "g_tDynamicAmbientOcclusion");
    std::lock_guard<std::recursive_mutex> lk(g_m);
    if (aoSlot >= 0 && !g_withSky) {
        // A map's 3D sky is drawn with the camera's matrix but in the last part of the depth range (S2FM: 0.95 to 1,
        // the scene 0.05 to 0.95), scaled up round the map, so that it never meets the scene in the picture. In the
        // rays it would: where its huge terrain crosses the real ground the AO showed a dark line. It is left out.
        D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&nvp, vp);
        if (nvp && vp[0].MinDepth >= 0.9f) {
            g_skyDraws.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (aoSlot >= 0) {
        t_reader = true;
        g_readerSincePresent.store(true, std::memory_order_relaxed);
        NoteT9(st, TextureSlot(st->psLayout, "g_tDynamicAmbientOcclusionDepth"));
        if (!g_firstReaderDone) {
            g_firstReaderDone = true;
            FirstReader(ctx, st);
        }
        return;
    }
    if (g_mulPending && IsAfterOpaque(ShaderDbName(st->psLayout))) {
        g_mulPending = false;
        Fire(ctx);
        if (g_showSlot >= 0) MultiplyNow(ctx);
        // The next AO reader starts another frame: S2FM's refinement may draw the scene again before it presents.
        g_firstReaderDone = false;
    }
}

void AfterDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    if (!g_mulPending) {
        // Before the frame's first AO reader: a depth only draw of another view (the shadows', drawn before the
        // camera's), whose triangles may stand beside what the camera sees though the camera does not see them.
        if (!g_firstReaderDone && !st->rtW && st->dsW && !st->deferred && g_on.load(std::memory_order_relaxed)
            && !g_broken && g_slotW && !CoreFrameForeign(st)) {
            float pv[204];
            if (!CorePerView(st, pv)) return;
            // Not the camera's own (its size is the scene's).
            if ((UINT)(pv[PV_VIEWPORT_SIZE] + 0.5f) == g_sceneW && (UINT)(pv[PV_VIEWPORT_SIZE + 1] + 0.5f) == g_sceneH)
                return;
            std::lock_guard<std::recursive_mutex> lk(g_m);
            RtaoCaptureOther(ctx, st);
        }
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_m);
    if (!g_mulPending) return;
    // A blended draw (glass, an additive glow, a see-through energy mesh) is not an occluder, even when it reads the
    // AO: it is left out of the rays, and what it adds is kept out of the AO. "solidblended": traced as solid, as before.
    if (t_reader && (!st->blend || g_solidBlended)) {
        GlowRedraw(ctx, st);
        RtaoCaptureDraw(ctx, st);
    } else if (st->blend) {
        if (t_reader) g_blendedReaders.fetch_add(1, std::memory_order_relaxed);
        TransRedraw(ctx, st);
    }
}

// --- Measuring: each presented picture's mean over a line, and the view's frame interval.
struct Picture {
    UINT w = 0, h = 0, fmt = 0;
    long long n = 0;
    double sum[3] = {};
    ID3D11Texture2D* staging = nullptr;
};
std::map<IDXGISwapChain*, Picture> g_pictures;
std::vector<float> g_intervals;
LARGE_INTEGER g_lastViewPresent = {};

void Flush() {
    if (g_line.size() && g_line[0] != '\x01' && g_measure) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, CorePath(L"s2fm_rtao_out.txt").c_str(), L"a") == 0 && f) {
            fprintf(f, "== %s\n", g_line.c_str());
            fprintf(f, "  frames %lld, traced %lld, not traced %lld, AO put on %lld, with the fogs kept out %lld, "
                    "see-through draws drawn again %lld, glowing draws drawn for their glow %lld, scene depth copies %lld\n",
                    g_frames.load(), g_traced.load(), g_missed.load(), g_mulDone.load(), g_fogFrames.load(),
                    g_transDraws.load(), g_glowDraws.load(), g_sceneDepthCopies.load());
            if (!g_dumpLine.empty()) fprintf(f, "  dump%s\n", g_dumpLine.c_str());
            double ms[4];
            long long builds = 0, refits = 0;
            RtaoD3D12LineTimes(ms, &builds, &refits);
            fprintf(f, "  the card's time a frame: conversion %.2f ms, bottom structure %.2f (%lld made, %lld refitted), "
                    "top %.2f, AO %.2f\n", ms[0], ms[1], builds, refits, ms[2], ms[3]);
            if (!g_intervals.empty()) {
                std::vector<float> v = g_intervals;
                std::sort(v.begin(), v.end());
                double sum = 0.0;
                for (float x : v) sum += x;
                fprintf(f, "  frame interval of the view: %zu frames, mean %.2f ms, median %.2f, 90th %.2f, worst %.2f\n",
                        v.size(), sum / v.size(), v[v.size() / 2], v[(size_t)((v.size() - 1) * 0.9)], v.back());
            }
            for (auto& kv : g_pictures) {
                const Picture& p = kv.second;
                if (!p.n) continue;
                fprintf(f, "  swapchain %p %ux%u | %lld pictures | mean %.5f %.5f %.5f\n", (void*)kv.first, p.w, p.h, p.n,
                        p.sum[0] / p.n, p.sum[1] / p.n, p.sum[2] / p.n);
            }
            fclose(f);
        }
    }
    for (auto& kv : g_pictures) Drop(kv.second.staging);
    g_pictures.clear();
    g_intervals.clear();
    g_dumpLine.clear();
    g_dumpDone = false;
    g_pictureDumped = false;
    g_frames = g_traced = g_missed = g_mulDone = g_fogFrames = g_transDraws = g_sceneDepthCopies = 0;
    g_glowDraws = 0;
}

void MeasurePicture(IDXGISwapChain* sc) {
    Picture& p = g_pictures[sc];
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
    D3D11_TEXTURE2D_DESC d = {};
    back->GetDesc(&d);
    const bool eight = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                    || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                    || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    const bool bgr = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                  || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    ID3D11Device* dev = nullptr;
    back->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    if (dev) dev->GetImmediateContext(&ctx);
    if (eight && d.SampleDesc.Count == 1 && ctx) {
        if (!p.staging || p.w != d.Width || p.h != d.Height || p.fmt != (UINT)d.Format) {
            Drop(p.staging);
            D3D11_TEXTURE2D_DESC sd = d;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            dev->CreateTexture2D(&sd, nullptr, &p.staging);
            p.w = d.Width;
            p.h = d.Height;
            p.fmt = (UINT)d.Format;
            p.n = 0;
            p.sum[0] = p.sum[1] = p.sum[2] = 0;
        }
        if (p.staging) {
            ctx->CopyResource(p.staging, back);
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(ctx->Map(p.staging, 0, D3D11_MAP_READ, 0, &m))) {
                double s[3] = {};
                for (UINT y = 0; y < d.Height; ++y) {
                    const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                    for (UINT x = 0; x < d.Width; ++x) {
                        s[0] += row[x * 4 + (bgr ? 2 : 0)];
                        s[1] += row[x * 4 + 1];
                        s[2] += row[x * 4 + (bgr ? 0 : 2)];
                    }
                }
                if (g_dumpWanted && !g_pictureDumped && GetTickCount64() >= g_dumpAfter
                    && sc == g_viewSwapChain.load(std::memory_order_relaxed)) {
                    g_pictureDumped = true;
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, CorePath(L"s2fm_rtao_picture.ppm").c_str(), L"wb") == 0 && pf) {
                        fprintf(pf, "P6\n%u %u\n255\n", d.Width, d.Height);
                        for (UINT y = 0; y < d.Height; ++y) {
                            const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                            for (UINT x = 0; x < d.Width; ++x) {
                                const uint8_t px3[3] = { row[x * 4 + (bgr ? 2 : 0)], row[x * 4 + 1],
                                                         row[x * 4 + (bgr ? 0 : 2)] };
                                fwrite(px3, 1, 3, pf);
                            }
                        }
                        fclose(pf);
                    }
                }
                ctx->Unmap(p.staging, 0);
                const double px = (double)d.Width * d.Height * 255.0;
                for (int c = 0; c < 3; ++c) p.sum[c] += s[c] / px;
                ++p.n;
            }
        }
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    back->Release();
}

void ReadSwitch(ULONGLONG now) {
    static ULONGLONG lastLook = 0;
    if (now - lastLook < 250) return;
    lastLook = now;
    std::string line;
    FILE* f = nullptr;
    if (_wfopen_s(&f, CorePath(L"s2fm_rtao_debug.txt").c_str(), L"r") == 0 && f) {
        char b[200] = {};
        if (fgets(b, sizeof(b), f)) line = b;
        fclose(f);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    }
    if (line == g_line) return;
    Flush();
    {
        // The card's times start again with the line.
        double ms[4];
        long long builds = 0, refits = 0;
        RtaoD3D12LineTimes(ms, &builds, &refits);
    }
    g_line = line;
    g_switchOff = line.compare(0, 3, "off") == 0;
    g_measure = line.find("measure") != std::string::npos;
    g_dumpWanted = line.find("dump") != std::string::npos;
    g_t9Depth = line.find("t9depth") != std::string::npos;
    g_noFogFix = line.find("nofog") != std::string::npos;
    g_fogOnly = line.find("fogonly") != std::string::npos;
    g_debug = line.find("debug") != std::string::npos;
    g_noRefit = line.find("norefit") != std::string::npos;
    g_regrow = line.find("regrow") != std::string::npos;
    g_solidBlended = line.find("solidblended") != std::string::npos;
    g_withSky = line.find("withsky") != std::string::npos;
    g_glowAll = line.find("glowall") != std::string::npos;
    g_noT9 = line.find("not9") != std::string::npos;
    if (g_noT9 && g_t9) {
        Drop(g_t9);
        g_t9W = g_t9H = 0;
        g_remakeSlots = true;
    }
    RtaoCaptureLeaveCutouts(line.find("nocutouts") != std::string::npos);
    RtaoCaptureSolidCutouts(line.find("solidcutouts") != std::string::npos);
    RtaoCaptureFlatNormals(line.find("flatnormals") != std::string::npos);
    RtaoCaptureRegrow(g_regrow);
    RtaoCaptureLeaveOthers(line.find("offscreen") == std::string::npos);
    if (line.find("views") != std::string::npos) {
        std::lock_guard<std::mutex> lk(g_censusM);
        g_census.clear();
        g_censusNoView = g_censusNoViewTris = 0;
        g_censusLeft = 4;   // the first present only ends a part of a frame
    }
    const size_t at = line.find("rays");
    const int n = at != std::string::npos ? atoi(line.c_str() + at + 4) : 0;
    g_switchRays = n >= 1 && n <= 16 ? n : 0;
    ApplySettings();
    const size_t ra = line.find("radius");
    const float rr = ra != std::string::npos ? (float)atof(line.c_str() + ra + 6) : 0.0f;
    g_radiusAsked = rr >= 1.0f && rr <= 1024.0f ? rr : 0.0f;
    const size_t stn = line.find("strength");
    g_strengthAsked = stn != std::string::npos ? (float)atof(line.c_str() + stn + 8) : -1.0f;
    const size_t bi = line.find("bias");
    g_biasAsked = bi != std::string::npos ? (float)atof(line.c_str() + bi + 4) : -1.0f;
    g_dumpAfter = now + 2000;
    Log("[RTAO] test words: \"%s\"%s", line.c_str(), g_on.load() ? "" : " (RTAO off)");
}

void ReadCameraSsao(ULONGLONG now) {
    if (now - g_camReadAt < 250) return;
    g_camReadAt = now;
    float r = 0.0f, b = 0.0f, s = 0.0f;
    char name[64] = "";
    if (!SfmCameraSsao(&r, &b, &s, name, sizeof(name))) return;
    if (!std::isfinite(r) || !std::isfinite(b) || !std::isfinite(s)) return;
    r = r < 0.01f ? 0.01f : r;
    b = b < 0.0f ? 0.0f : b;
    s = s < 0.0f ? 0.0f : s;
    if (!g_camRead || r != g_camRadius || b != g_camBias || s != g_camStrength || strcmp(name, g_camName))
        Log("[RTAO] camera \"%s\": SSAORadius %g, SSAOBias %g, SSAOStrength %g", name, r, b, s);
    g_camRadius = r;
    g_camBias = b;
    g_camStrength = s;
    strcpy_s(g_camName, name);
    g_camRead = true;
}

void Present(IDXGISwapChain* sc) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::recursive_mutex> lk(g_m);
    ReadSwitch(now);
    // The view is the swapchain of S2FM's that presents the scene (an AO reader drew since the last present); another
    // takes over only once the view has gone a quarter second without one.
    const bool scene = sc && CoreSwapChainIsS2fm(sc) && g_readerSincePresent.exchange(false, std::memory_order_relaxed);
    if (scene) {
        g_scenePresentAt[sc] = now;
        IDXGISwapChain* view = g_viewSwapChain.load(std::memory_order_relaxed);
        if (sc != view) {
            auto it = view ? g_scenePresentAt.find(view) : g_scenePresentAt.end();
            const ULONGLONG quiet = it == g_scenePresentAt.end() ? ~0ull : now - it->second;
            if (quiet > 250) {
                g_viewSwapChain.store(sc, std::memory_order_relaxed);
                Log("[RTAO] the viewport being looked at presents through %p", (void*)sc);
            }
        }
    }
    const bool viewed = sc && sc == g_viewSwapChain.load(std::memory_order_relaxed);
    if (viewed) {
        LARGE_INTEGER q, fq;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&fq);
        if (g_lastViewPresent.QuadPart) {
            const float ms = (float)((q.QuadPart - g_lastViewPresent.QuadPart) * 1000.0 / fq.QuadPart);
            if (ms < 1000.0f && g_intervals.size() < 20000 && g_measure) g_intervals.push_back(ms);
        }
        g_lastViewPresent = q;
        g_frames.fetch_add(1, std::memory_order_relaxed);
        if (g_censusLeft.load(std::memory_order_relaxed) > 0) CensusSay();
        if (g_mulPending) {
            // The frame ended without any of the passes after the opaque surfaces.
            g_mulPending = false;
            RtaoCaptureVertices();
            g_missed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (scene || viewed) {
        g_firstReaderDone = false;
        RtaoCaptureDrop();
    }
    if (g_on.load(std::memory_order_relaxed)) ReadCameraSsao(now);
    if (g_measure && sc && CoreSwapChainIsS2fm(sc)) MeasurePicture(sc);

    static ULONGLONG lastSaid = 0;
    if (now - lastSaid > 10000) {
        lastSaid = now;
        long long traced = 0, tris = 0, taken = 0, skipped = 0, overflow = 0;
        float mb = 0.0f;
        RtaoD3D12Stats(&traced, &tris, &mb);
        RtaoCaptureStats(&taken, &skipped, &overflow);
        long long cutDraws = 0, cutTris = 0;
        RtaoCaptureCutouts(&cutDraws, &cutTris);
        if (traced || taken)
            Log("[RTAO] in 10 s: %lld frames traced, the last of %lld triangles | draws captured %lld (cut outs %lld, "
                "%lld triangles a frame), left out %lld, frames the capture ran out of room %lld | %.0f MB on the card",
                traced, tris, taken, cutDraws, traced ? cutTris / traced : 0, skipped, overflow, mb);
        const long long sky = g_skyDraws.exchange(0);
        if (sky) Log("[RTAO] in 10 s: %lld draws of the map's 3D sky left out of the rays", sky);
        const long long blended = g_blendedReaders.exchange(0);
        if (blended) Log("[RTAO] in 10 s: %lld blended draws that read the AO left out of the rays", blended);
        const long long notTraced = g_notTraced10.exchange(0), stale = g_staleSkipped.exchange(0);
        if (notTraced)
            Log("[RTAO] in 10 s: %lld frames not traced in time (the last AO shown instead), %lld of them without AO "
                "because the last one was for another camera", notTraced, stale);
        long long otherDraws = 0, otherTris = 0;
        RtaoCaptureOthers(&otherDraws, &otherTris);
        if (otherDraws)
            Log("[RTAO] other views (the shadows'): %lld draws, %lld triangles a frame", otherDraws,
                traced ? otherTris / traced : 0);
        double ms[4];
        RtaoD3D12Times(ms);
        long long builds = 0, refits = 0;
        RtaoD3D12Builds(&builds, &refits);
        if (traced)
            Log("[RTAO] the card's time a frame: conversion %.2f ms, bottom structure %.2f (%lld made, %lld refitted), top "
                "%.2f, AO %.2f", ms[0], ms[1], builds, refits, ms[2], ms[3]);
    }
}

const CoreFeature kFeature = { "RTAO", BeforeDraw, AfterDraw, nullptr, Present, nullptr, nullptr };

}  // namespace

void RtaoRegister() {
    LoadSettings();
    CoreRegister(&kFeature);
}

// --- The panel's API (rtao_api.h), exported by dxgi.def.
extern "C" int __cdecl S2fmRtao_Get(S2fmRtaoState* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    std::lock_guard<std::recursive_mutex> lk(g_m);
    out->apiVersion = S2FM_RTAO_API_VERSION;
    out->enabled = g_savedEnabled;
    out->rays = g_savedRays;
    const ULONGLONG now = GetTickCount64();
    out->running = g_on.load() && now - g_lastMultiplyAt.load() < 1000;
    const char* why12 = "";
    RtaoD3D12Info(&out->triangles, &out->megabytes, &why12);
    out->unavailable = (why12 && *why12) || g_broken;
    // The AO's frames a second, between two calls (the panel asks a few times a second).
    static ULONGLONG lastAt = 0;
    static long long lastTotal = 0;
    static float rate = 0.0f;
    const long long total = g_mulTotal.load();
    if (lastAt && now - lastAt >= 200) {
        rate = (float)(total - lastTotal) * 1000.0f / (float)(now - lastAt);
        lastAt = now;
        lastTotal = total;
    } else if (!lastAt) {
        lastAt = now;
        lastTotal = total;
    }
    out->framesPerSecond = out->running ? rate : 0.0f;
    if (!g_savedEnabled) strcpy_s(out->why, "switched off");
    else if (g_switchOff) strcpy_s(out->why, "switched off by the test words (s2fm_rtao_debug.txt)");
    else if (why12 && *why12) strcpy_s(out->why, why12);
    else if (g_broken || !out->running) strcpy_s(out->why, *g_why ? g_why : "waiting for S2FM's viewport to draw");
    return 1;
}

extern "C" void __cdecl S2fmRtao_Set(int enabled, int rays) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    g_savedEnabled = enabled ? 1 : 0;
    if (rays >= 1 && rays <= 16) g_savedRays = rays;
    ApplySettings();
    SaveSettings();
    Log("[RTAO] from the panel: %s, %d rays a pixel", g_savedEnabled ? "on" : "off", g_savedRays);
}
