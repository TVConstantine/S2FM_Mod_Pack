// Fog of our own over S2FM's viewports and renders, driven by the camera's own fog controls and independent of the
// map.
//
// S2FM's "Add Fog Controls To Camera" puts fogenable, fogcolor, fogstart, fogend, fogmaxdensity and fogexponent on the
// camera (common/sfm_doc.h); S2FM hands them to the engine, whose fog from them often does not show. Here they are
// read from the document a few times a second (so their animation is followed) and the fog is drawn by us:
//   - at the frame's first opaque draw (the first that reads the engine's dynamic AO, as RTAO finds it): the scene's
//     target and depth target, the depth range and the camera's projection are kept;
//   - at the first draw after the opaque surfaces that is not the sky (the sky is drawn after them and is to be in
//     the fog too), or at the first see-through draw before that: the scene's depth is copied and one full screen
//     triangle blends the fog over the picture, sample by sample on a multisampled target. The fog at a pixel is
//         maxdensity * saturate((distance - start) / (end - start)) ^ exponent
//     with the true distance from the eye (the sky at an infinite one), in the camera's colour.
// It comes after RTAO's AO (registered before this), so the AO never darkens the fog.
//
// The engine's own fog from the camera (broken: it greys the sky and fogs next to nothing else) is turned off while
// ours is on, where sfm.dll hands the camera's controls to the engine (InstallFogSettingsHook).
//
// Test words, on the first line of s2fm_fog_debug.txt beside the DLL (read four times a second):
//   off          no fog of ours (the engine's is left as it is);
//   keepengine   the engine's fog from the camera is left on beside ours;
//   probe        once a second the settings S2FM hands the engine for the camera's fog, into the log;
//   dump         the viewport's picture as presented, a second and a half after the line changes, into
//                s2fm_fog_picture.ppm (once a line).
#include "../core/core.h"
#include "../common/sfm_doc.h"
#include "../common/shader_table.h"

#include <d3dcompiler.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

std::mutex g_m;

template <typename T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }

// --- Settings: the camera's controls and the test words.
SfmFog g_fog = {};
bool g_fogRead = false;
char g_camName[64] = "";
bool g_off = false, g_keepEngine = false, g_probe = false, g_dumpWanted = false;
ULONGLONG g_dumpAt = 0;

// --- The frame: kept at its first opaque draw.
bool g_firstDone = false;   // this frame's first opaque draw has been seen
bool g_pending = false;     // the fog is still to go on this frame
ID3D11RenderTargetView* g_sceneRtv = nullptr;
ID3D11DepthStencilView* g_sceneDsv = nullptr;
UINT g_sceneW = 0, g_sceneH = 0;
float g_minDepth = 0.0f, g_maxDepth = 1.0f;
float g_projToRel[16] = {};      // clip space to the point less the eye (RTAO's matrix, rtao_engine.cpp MultiplyNow)
float g_toneMapScale = 1.0f;     // the view's g_flToneMapScalarLinear
// S2FM's tone map, from the last post_process draw's b3 (PostProcessingConstantBuffer_t): register 0 the exposure and
// the filmic curve's A, B, C, register 1 its D, E, F and the white's scale (post_process_pc_50_ps disassembled):
//   x = scene * tone map scale * exposure * 2.8
//   shown = sRGB(((x (A x + C B) + D E) / (x (A x + B) + D F) - E / F) * white)
float g_post[8] = {};
bool g_postRead = false;

int TextureSlot(int layout, const char* name) {
    uint8_t reg = 0;
    uint16_t offset = 0;
    return layout >= 0 && ShaderDbFind(layout, name, &reg, &offset) && reg == SDB_REG_TEXTURE ? (int)(offset & 0xFF) : -1;
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

bool FogOn() { return !g_off && g_fogRead && g_fog.enabled && g_fog.maxDensity > 0.0f; }

// The passes that come after the opaque surfaces (as RTAO has them), the sky among them.
bool IsAfterOpaque(const char* name) {
    static const char* const kAfter[] = { "sky", "sfm_volumetrics_frustum", "downsample_depth", "gaussian_bloom_blur",
                                          "post_process", "sfm_accumulation", "copytexture" };
    if (!name || !*name) return false;
    for (const char* a : kAfter) if (!strcmp(name, a)) return true;
    return false;
}

// --- The scene's depth, copied into a texture the pass can read (the engine's has no shader view).
ID3D11Texture2D* g_depthTex = nullptr;
ID3D11ShaderResourceView* g_depthSrv = nullptr;
D3D11_TEXTURE2D_DESC g_depthDesc = {};
bool g_depthMs = false;

bool CopySceneDepth(ID3D11DeviceContext* ctx) {
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
    bool ok = family != DXGI_FORMAT_UNKNOWN && d.ArraySize == 1;
    if (ok && (!g_depthTex || g_depthDesc.Width != d.Width || g_depthDesc.Height != d.Height
               || g_depthDesc.Format != d.Format || g_depthDesc.SampleDesc.Count != d.SampleDesc.Count
               || g_depthDesc.SampleDesc.Quality != d.SampleDesc.Quality || g_depthDesc.MipLevels != d.MipLevels)) {
        Drop(g_depthSrv);
        Drop(g_depthTex);
        D3D11_TEXTURE2D_DESC c = d;
        c.Format = family;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        c.CPUAccessFlags = 0;
        c.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = view;
        g_depthMs = d.SampleDesc.Count > 1;
        if (g_depthMs) sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
        }
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        ok = dev && SUCCEEDED(dev->CreateTexture2D(&c, nullptr, &g_depthTex))
          && SUCCEEDED(dev->CreateShaderResourceView(g_depthTex, &sd, &g_depthSrv));
        if (dev) dev->Release();
        g_depthDesc = d;
        Log("[FOG] the scene's depth: %ux%u, format %u, %u samples; %s", d.Width, d.Height, (UINT)d.Format,
            d.SampleDesc.Count, ok ? "copied for the fog" : "COULD NOT be copied, no fog");
        if (!ok) { Drop(g_depthSrv); Drop(g_depthTex); }
    }
    ok = ok && g_depthTex && g_depthSrv;
    if (ok) ctx->CopyResource(g_depthTex, tex);
    tex->Release();
    return ok;
}

// --- The pass.
const char kVs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
    "}\n";
const char kPs[] =
    "#ifdef MS\n"
    "Texture2DMS<float> g_Depth : register(t0);\n"
    "#else\n"
    "Texture2D<float> g_Depth : register(t0);\n"
    "#endif\n"
    "cbuffer Fog : register(b0) {\n"
    "    row_major float4x4 g_ProjToRel;\n"
    "    float4 g_Range;     // the depth range the view is drawn with, and the target's size\n"
    "    float4 g_Color;     // the colour in the scene's terms, max density\n"
    "    float4 g_Curve;     // start, 1 / (end - start), exponent\n"
    "};\n"
    "#ifdef MS\n"
    "float4 main(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Target {\n"
    "    const float d = g_Depth.Load(int2(pos.xy), s);\n"
    "#else\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "    const float d = g_Depth.Load(int3(pos.xy, 0));\n"
    "#endif\n"
    "    const float z = (d - g_Range.x) / max(g_Range.y - g_Range.x, 1e-6f);\n"
    "    float dist = 1e30f;   // nothing drawn there, or the map's 3D sky past the range: the sky, at no end\n"
    "    if (z > 1e-6f && z < 1.0f - 1e-6f) {\n"
    "        float2 ndc = (pos.xy / g_Range.zw) * 2.0f - 1.0f;\n"
    "        ndc.y = -ndc.y;\n"
    "        const float4 v = mul(float4(ndc, z, 1.0f), g_ProjToRel);\n"
    "        dist = length(v.xyz / v.w);\n"
    "    }\n"
    "    const float f = g_Color.w * pow(saturate((dist - g_Curve.x) * g_Curve.y), g_Curve.z);\n"
    "    return float4(g_Color.rgb, saturate(f));\n"
    "}\n";

ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps[2] = {};   // single sample, multisampled (per sample)
ID3D11Buffer* g_cb = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11DepthStencilState* g_depthOff = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
bool g_broken = false;

bool EnsurePass(ID3D11DeviceContext* ctx) {
    if (g_broken) return false;
    if (g_vs && g_ps[0] && g_ps[1] && g_cb && g_blend && g_depthOff && g_raster) return true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    bool ok = true;
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    if (SUCCEEDED(D3DCompile(kVs, sizeof(kVs) - 1, "fog_vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &err)) && code)
        ok = SUCCEEDED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_vs));
    else ok = false;
    Drop(code);
    if (err) { Log("[FOG] the vertex shader: %s", (const char*)err->GetBufferPointer()); Drop(err); }
    for (int ms = 0; ms < 2; ++ms) {
        const D3D_SHADER_MACRO defs[] = { { "MS", "1" }, { nullptr, nullptr } };
        if (SUCCEEDED(D3DCompile(kPs, sizeof(kPs) - 1, "fog_ps", ms ? defs : nullptr, nullptr, "main", "ps_5_0", 0, 0,
                                 &code, &err)) && code)
            ok = ok && SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps[ms]));
        else ok = false;
        Drop(code);
        if (err) { Log("[FOG] the pixel shader: %s", (const char*)err->GetBufferPointer()); Drop(err); }
    }
    D3D11_BUFFER_DESC cd = {};
    cd.ByteWidth = 112;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &g_cb));
    // The fog over the picture: picture * (1 - f) + colour * f, the target's alpha kept.
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN
                                             | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &g_blend));
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_depthOff));
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_raster));
    dev->Release();
    if (!ok) {
        g_broken = true;
        Log("[FOG] the fog's pass could not be made; no fog of ours");
    }
    return ok;
}

float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// The scene's value that S2FM's tone map shows as the colour channel c (0..1, sRGB, as picked): the filmic curve
// solved for x (a quadratic, A (1 - q) x^2 + B (C - q) x + D (E - q F) = 0 with q = shown / white + E / F), divided
// by what multiplies the scene before it. Without the curve's values, x / (1 + x) undone.
float SceneValueShownAs(float c) {
    const float shown = SrgbToLinear(c < 0.999f ? c : 0.999f);
    if (g_postRead) {
        const float exposure = g_post[0], A = g_post[1], B = g_post[2], C = g_post[3];
        const float D = g_post[4], E = g_post[5], F = g_post[6], white = g_post[7];
        const float scale = g_toneMapScale * exposure * 2.8f;
        if (white > 1e-6f && F != 0.0f && scale > 1e-9f) {
            const double q = shown / white + E / F;
            const double a = A * (1.0 - q), b = B * (C - q), k = D * (E - q * F);
            double x = -1.0;
            if (std::fabs(a) < 1e-12) x = b != 0.0 ? -k / b : -1.0;
            else {
                const double disc = b * b - 4.0 * a * k;
                if (disc >= 0.0) {
                    const double r1 = (-b + std::sqrt(disc)) / (2.0 * a), r2 = (-b - std::sqrt(disc)) / (2.0 * a);
                    // The curve rises from 0: the smallest root not below 0.
                    x = r1 >= 0.0 && (r2 < 0.0 || r1 < r2) ? r1 : r2;
                }
            }
            if (x >= 0.0 && std::isfinite(x)) return (float)(x / scale);
        }
    }
    const float s = shown < 0.98f ? shown : 0.98f;
    return s / (1.0f - s) / g_toneMapScale;
}

std::atomic<long long> g_drawn{0};

void FogNow(ID3D11DeviceContext* ctx) {
    if (!g_sceneRtv || !EnsurePass(ctx) || !CopySceneDepth(ctx)) return;
    // What the engine has bound, put back after.
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
    ID3D11ShaderResourceView* srv0 = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv0);
    ID3D11Buffer* cb0 = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &cb0);
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

    float consts[28] = {};
    memcpy(consts, g_projToRel, sizeof(g_projToRel));
    consts[16] = g_minDepth;
    consts[17] = g_maxDepth;
    consts[18] = (float)g_sceneW;
    consts[19] = (float)g_sceneH;
    // The colour as picked is what a fully fogged pixel shows: S2FM's tone map undone (SceneValueShownAs).
    for (int c = 0; c < 3; ++c) consts[20 + c] = SceneValueShownAs(g_fog.color[c]);
    consts[23] = g_fog.maxDensity;
    const float span = g_fog.end - g_fog.start;
    consts[24] = g_fog.start;
    consts[25] = std::fabs(span) > 1e-3f ? 1.0f / span : 1e3f;
    consts[26] = g_fog.exponent > 1e-3f ? g_fog.exponent : 1e-3f;

    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    ctx->UpdateSubresource(g_cb, 0, nullptr, consts, 0, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_ps[g_depthMs ? 1 : 0], nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &g_depthSrv);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->RSSetState(g_raster);
    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)g_sceneW;
    vp.Height = (FLOAT)g_sceneH;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    const FLOAT one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ctx->OMSetBlendState(g_blend, one, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(g_depthOff, 0);
    ctx->OMSetRenderTargets(1, &g_sceneRtv, nullptr);
    CoreRawDraw(ctx, 3);

    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    ctx->OMSetDepthStencilState(dss, sref);
    ctx->OMSetBlendState(bs, bf, sm);
    ctx->RSSetViewports(nvp, vps);
    ctx->RSSetState(rs);
    ctx->PSSetShaderResources(0, 1, &srv0);
    ctx->PSSetConstantBuffers(0, 1, &cb0);
    ctx->PSSetShader(ps, psCi, psCiN);
    ctx->DSSetShader(ds, nullptr, 0);
    ctx->HSSetShader(hs, nullptr, 0);
    ctx->GSSetShader(gs, nullptr, 0);
    ctx->VSSetShader(vs, vsCi, vsCiN);
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(topo);
    t_CorePassThrough = pass;
    for (ID3D11RenderTargetView* v : rtvs) if (v) v->Release();
    IUnknown* rel[] = { il, vs, gs, hs, ds, ps, rs, bs, dss, dsv, srv0, cb0 };
    for (IUnknown* x : rel) if (x) x->Release();
    for (UINT i = 0; i < vsCiN; ++i) if (vsCi[i]) vsCi[i]->Release();
    for (UINT i = 0; i < psCiN; ++i) if (psCi[i]) psCi[i]->Release();
    g_drawn.fetch_add(1, std::memory_order_relaxed);
}

// --- The engine's fog from the camera. The 6 May 2022 sfm.dll hands the camera's six fog controls to the engine
// through fn 0x1F73A0 (camera, settings): it reads fogenable, fogcolor, fogstart, fogend, fogmaxdensity and fogexponent
// into the settings at +0x58 (a byte), +0x0C, +0x1C, +0x20, +0x28 and +0x2C. The engine's fog from them is broken: it
// greys the sky and fogs next to nothing else (the opaque surfaces' and the sky's view fog buffers have no trace of
// it; it goes through the volumetric fog the sky reads). While ours is on the settings' switch is cleared after the
// call, so the engine gets none of it; the camera's controls themselves are left as they are.
const uint32_t kStamp2022 = 0x6275AD57, kFogSettingsFn = 0x1F73A0;
const uint8_t kFogSettingsStart[9] = { 0x48, 0x85, 0xC9, 0x0F, 0x84, 0x1A, 0x01, 0x00, 0x00 };
const size_t kSettingsEnable = 0x58, kSettingsStart = 0x1C, kSettingsEnd = 0x20, kSettingsDensity = 0x28,
             kSettingsExponent = 0x2C;
typedef void(__fastcall* FogSettingsFn)(void* camera, void* settings, void* a3, void* a4);
FogSettingsFn g_origFogSettings = nullptr;
std::atomic<bool> g_quietEngine{false};   // ours is on and the engine's is to be off (set at each present)
std::atomic<long long> g_quieted{0};

// Four arguments forwarded, whatever the function names (it reads two).
void __fastcall FogSettingsHook(void* camera, void* settings, void* a3, void* a4) {
    g_origFogSettings(camera, settings, a3, a4);
    if (!settings) return;
    uint8_t* s = (uint8_t*)settings;
    if (g_probe) {
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - last >= 1000) {
            last = now;
            Log("[FOG] probe, S2FM's fog settings for the engine: switch %u, start %g, end %g, density %g, exponent %g%s",
                s[kSettingsEnable], *(float*)(s + kSettingsStart), *(float*)(s + kSettingsEnd),
                *(float*)(s + kSettingsDensity), *(float*)(s + kSettingsExponent),
                g_quietEngine.load() ? " (switched off for ours)" : "");
        }
    }
    if (g_quietEngine.load(std::memory_order_relaxed) && s[kSettingsEnable]) {
        s[kSettingsEnable] = 0;
        g_quieted.fetch_add(1, std::memory_order_relaxed);
    }
}

void InstallFogSettingsHook() {
    static bool done = false;
    if (done) return;
    HMODULE sfm = GetModuleHandleA("sfm.dll");
    if (!sfm) return;
    done = true;
    uint8_t* base = (uint8_t*)sfm;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kStamp2022 || memcmp(base + kFogSettingsFn, kFogSettingsStart, sizeof(kFogSettingsStart))) {
        Log("[FOG] sfm.dll is not the 6 May 2022 build: the engine's fog from the camera stays on beside ours");
        return;
    }
    const bool ok = CoreHook(base + kFogSettingsFn, (void*)&FogSettingsHook, (void**)&g_origFogSettings);
    Log("[FOG] the engine's fog from the camera %s", ok ? "is switched off while ours is on (sfm.dll hooked)"
                                                         : "COULD NOT be hooked: it stays on beside ours");
}

// --- The frame.
void BeforeDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    if (st->deferred || st->psLayout < 0 || CoreFrameForeign(st)) return;
    const bool reader = TextureSlot(st->psLayout, "g_tDynamicAmbientOcclusion") >= 0;
    std::lock_guard<std::mutex> lk(g_m);
    const char* name = ShaderDbName(st->psLayout);
    if (reader) {
        // Only an opaque draw starts a frame: a see-through one that reads the AO (glass) comes after the fog.
        if (g_firstDone || st->blend) return;
        // A map's 3D sky is drawn in the last part of the depth range: not the frame's start.
        D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&nvp, vp);
        if (nvp && vp[0].MinDepth >= 0.9f) return;
        g_firstDone = true;
        g_pending = false;
        if (!FogOn() || !st->rtW || !st->rtH) return;
        // From the view's world to projection matrix, as RTAO has it: the inverse gives a point in the camera's
        // offset space; less the eye there, it is the point less the camera, whose length is the distance.
        float pv[204];
        if (!CorePerView(st, pv) || !Inverse(&pv[PV_MAT_WORLD_TO_PROJ_MV], g_projToRel)) return;
        float eye[3];
        for (int c = 0; c < 3; ++c) eye[c] = pv[PV_CAMERA_POSITION_WS + c] + pv[PV_WORLD_TO_CAM_OFF_MV + c];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) g_projToRel[r * 4 + c] -= g_projToRel[r * 4 + 3] * eye[c];
        g_toneMapScale = std::isfinite(pv[PV_TONEMAP_LINEAR]) && pv[PV_TONEMAP_LINEAR] > 1e-6f ? pv[PV_TONEMAP_LINEAR] : 1.0f;
        g_minDepth = nvp ? vp[0].MinDepth : 0.0f;
        g_maxDepth = nvp ? vp[0].MaxDepth : 1.0f;
        Drop(g_sceneRtv);
        Drop(g_sceneDsv);
        ctx->OMGetRenderTargets(1, &g_sceneRtv, &g_sceneDsv);
        g_sceneW = st->rtW;
        g_sceneH = st->rtH;
        g_pending = g_sceneRtv && g_sceneDsv;
        return;
    }
    if (name && !strcmp(name, "post_process")) {
        // The tone map's values, for the next frame's fog colour.
        float post[8];
        const bool ok = st->psCb[3] && CoreCbRead(st->psCb[3], 0, post, sizeof(post));
        if (ok && (!g_postRead || memcmp(post, g_post, sizeof(post)))) {
            memcpy(g_post, post, sizeof(post));
            Log("[FOG] S2FM's tone map: exposure %g, curve A %g B %g C %g D %g E %g F %g, white %g", post[0], post[1],
                post[2], post[3], post[4], post[5], post[6], post[7]);
        }
        g_postRead = g_postRead || ok;
    }
    if (!g_pending) return;
    // After the opaque surfaces: the sky is let draw first (it is to be in the fog), but not a see-through draw.
    const bool sky = name && !strcmp(name, "sky");
    if ((IsAfterOpaque(name) && !sky) || (st->blend && !sky)) {
        g_pending = false;
        FogNow(ctx);
        // The next opaque draw starts another frame: S2FM's refinement may draw the scene again before it presents.
        g_firstDone = false;
    }
}

void ReadWords(ULONGLONG now) {
    static ULONGLONG lastLook = 0;
    static std::string last = "\x01";
    if (now - lastLook < 250) return;
    lastLook = now;
    std::string line;
    FILE* f = nullptr;
    if (_wfopen_s(&f, CorePath(L"s2fm_fog_debug.txt").c_str(), L"r") == 0 && f) {
        char b[200] = {};
        if (fgets(b, sizeof(b), f)) line = b;
        fclose(f);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    }
    if (line == last) return;
    last = line;
    g_off = line.compare(0, 3, "off") == 0;
    g_keepEngine = line.find("keepengine") != std::string::npos;
    g_probe = line.find("probe") != std::string::npos;
    g_dumpWanted = line.find("dump") != std::string::npos;
    g_dumpAt = now + 1500;
    if (!line.empty()) Log("[FOG] test words: \"%s\"", line.c_str());
}

// The presented picture into s2fm_fog_picture.ppm (8 bit back buffers, RGBA or BGRA).
void DumpPicture(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
    D3D11_TEXTURE2D_DESC d = {};
    back->GetDesc(&d);
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                   || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                   || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    ID3D11Device* dev = nullptr;
    back->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    if (dev) dev->GetImmediateContext(&ctx);
    ID3D11Texture2D* staging = nullptr;
    if (ctx && (bgra || rgba) && d.SampleDesc.Count == 1) {
        D3D11_TEXTURE2D_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.MiscFlags = 0;
        s.MipLevels = 1;
        s.ArraySize = 1;
        if (SUCCEEDED(dev->CreateTexture2D(&s, nullptr, &staging))) {
            const bool pass = t_CorePassThrough;
            t_CorePassThrough = true;
            ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, back, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                FILE* f = nullptr;
                if (_wfopen_s(&f, CorePath(L"s2fm_fog_picture.ppm").c_str(), L"wb") == 0 && f) {
                    fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
                    std::string row(d.Width * 3, '\0');
                    for (UINT y = 0; y < d.Height; ++y) {
                        const uint8_t* p = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                        for (UINT x = 0; x < d.Width; ++x) {
                            row[x * 3 + 0] = (char)p[x * 4 + (bgra ? 2 : 0)];
                            row[x * 3 + 1] = (char)p[x * 4 + 1];
                            row[x * 3 + 2] = (char)p[x * 4 + (bgra ? 0 : 2)];
                        }
                        fwrite(row.data(), 1, row.size(), f);
                    }
                    fclose(f);
                    Log("[FOG] the picture dumped (%ux%u)", d.Width, d.Height);
                }
                ctx->Unmap(staging, 0);
            }
            t_CorePassThrough = pass;
        }
    } else {
        Log("[FOG] the picture is not dumped: back buffer format %u, %u samples", (UINT)d.Format, d.SampleDesc.Count);
    }
    Drop(staging);
    Drop(ctx);
    Drop(dev);
    back->Release();
}

void ReadCamera(ULONGLONG now) {
    static ULONGLONG lastRead = 0;
    if (now - lastRead < 250) return;
    lastRead = now;
    SfmFog fog = {};
    char name[64] = "";
    const bool ok = SfmCameraFog(&fog, name, sizeof(name));
    bool finite = ok;
    for (float x : { fog.color[0], fog.color[1], fog.color[2], fog.start, fog.end, fog.maxDensity, fog.exponent })
        finite = finite && std::isfinite(x);
    if (ok && finite && (!g_fogRead || memcmp(&fog, &g_fog, sizeof(fog)) || strcmp(name, g_camName)))
        Log("[FOG] camera \"%s\": fog %s, colour %.3f %.3f %.3f, start %g, end %g, max density %g, exponent %g", name,
            fog.enabled ? "on" : "off", fog.color[0], fog.color[1], fog.color[2], fog.start, fog.end, fog.maxDensity,
            fog.exponent);
    g_fogRead = ok && finite;
    if (g_fogRead) {
        g_fog = fog;
        strcpy_s(g_camName, name);
    }
}

void Present(IDXGISwapChain* sc) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lk(g_m);
    ReadWords(now);
    ReadCamera(now);
    g_quietEngine.store(FogOn() && !g_keepEngine, std::memory_order_relaxed);
    if (sc && CoreSwapChainIsS2fm(sc)) {
        // A frame that ended without a pass after the opaque surfaces has no fog.
        g_pending = false;
        g_firstDone = false;
        if (g_dumpWanted && now >= g_dumpAt) {
            g_dumpWanted = false;
            DumpPicture(sc);
        }
    }
    static ULONGLONG lastSaid = 0;
    if (now - lastSaid > 10000) {
        lastSaid = now;
        const long long drawn = g_drawn.exchange(0), quieted = g_quieted.exchange(0);
        if (drawn || quieted)
            Log("[FOG] in 10 s: the fog drawn %lld times, the engine's fog from the camera switched off %lld times",
                drawn, quieted);
    }
}

const CoreFeature kFeature = { "Fog", BeforeDraw, nullptr, nullptr, Present, nullptr, nullptr };

}  // namespace

void FogRegister() {
    CoreRegister(&kFeature);
    CoreOnModuleLoad(InstallFogSettingsHook);
}
