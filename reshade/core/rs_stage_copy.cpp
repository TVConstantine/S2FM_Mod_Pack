// The engine's picture into the core's back buffer and back out (rs_stage_io.h, plan task 5).
#include "rs_stage_io.h"

#include <d3dcompiler.h>

namespace {

const char kCopyHlsl[] = R"(
Texture2D<float4> Src : register(t0);
Texture2D<float4> Resolved : register(t1);
Texture2D<float4> Before : register(t2);   // the back buffer as the copy in left it, before the effects
cbuffer C : register(b0) { float exposure; float rcpExposure; float2 pad; };

float4 VS(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 Encode(float3 c)
{
    c = max(c, 0.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float3 Decode(float3 v)
{
    v = max(v, 0.0);
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

// The scene (A): exposed, then x / (1 + x), then encoded. Its inverse back to the scene.
float3 SceneIn(float3 c) { const float3 x = max(c, 0.0) * exposure; return Encode(x / (1.0 + x)); }
float3 SceneOut(float3 v) { const float3 y = min(Decode(v), 0.999); return y / (1.0 - y) * rcpExposure; }

float4 PS_InScene(float4 pos : SV_Position) : SV_Target { return float4(SceneIn(Src.Load(int3(pos.xy, 0)).rgb), 1.0); }
// B: what the effects see is the picture as it will be shown, 0 to 1 (post_process's output goes over 1 in the
// sky, which the 8-bit picture clips; presets are tuned on that picture, and a bloom fed what is over
// white blooms far too much). What is over 1 and left untouched keeps its value: only the change goes back.
float4 PS_InLinear(float4 pos : SV_Position) : SV_Target { return float4(Encode(saturate(Src.Load(int3(pos.xy, 0)).rgb)), 1.0); }
float4 PS_InRaw(float4 pos : SV_Position) : SV_Target { return float4(Src.Load(int3(pos.xy, 0)).rgb, 1.0); }

// Out, A and B: where an effect changed the pixel, the samples move by what takes their resolve to the
// effect's value (each sample keeps its own offset, so an edge keeps its coverage); where none did, nothing
// moves. Values past what the 8 bits carry (the scene past x/(1+x) = 0.999, the post output past white) keep
// their excess and move by the effect's change instead.
float4 PS_OutScene(float4 pos : SV_Position) : SV_Target
{
    const float3 after = Src.Load(int3(pos.xy, 0)).rgb;
    const float3 before = Before.Load(int3(pos.xy, 0)).rgb;
    if (all(after == before)) return 0.0;
    const float3 c = Resolved.Load(int3(pos.xy, 0)).rgb;
    const float3 limit = 999.0 * rcpExposure;
    return float4(c > limit ? SceneOut(after) - SceneOut(before) : SceneOut(after) - c, 0.0);
}
float4 PS_OutLinear(float4 pos : SV_Position) : SV_Target
{
    const float3 after = Src.Load(int3(pos.xy, 0)).rgb;
    const float3 before = Before.Load(int3(pos.xy, 0)).rgb;
    if (all(after == before)) return 0.0;
    const float3 c = Resolved.Load(int3(pos.xy, 0)).rgb;
    return float4(Decode(after) - min(c, 1.0), 0.0);
}
float4 PS_OutRaw(float4 pos : SV_Position) : SV_Target { return float4(Src.Load(int3(pos.xy, 0)).rgb, 1.0); }
)";

enum { PS_IN_SCENE, PS_IN_LINEAR, PS_IN_RAW, PS_OUT_SCENE, PS_OUT_LINEAR, PS_OUT_RAW, PS_COUNT };
const char* const kPsNames[PS_COUNT] = { "PS_InScene", "PS_InLinear", "PS_InRaw", "PS_OutScene", "PS_OutLinear", "PS_OutRaw" };

ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps[PS_COUNT] = {};
ID3D11Buffer* g_cb = nullptr;
ID3D11BlendState* g_blendAdd = nullptr;      // A and B out: dest + difference, colour only
ID3D11BlendState* g_blendColour = nullptr;   // in, and C out: colour written, alpha left
ID3D11DepthStencilState* g_noDepth = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
bool g_broken = false;
// The back buffer as the copy in left it (A and B): what the effects changed is told from it, byte for byte (a
// float written to an 8-bit target may round either way within 0.6 of a step, so it cannot be worked out again).
ID3D11Texture2D* g_before = nullptr;
ID3D11ShaderResourceView* g_beforeSrv = nullptr;
// The resolved picture (A and B), kept from in to out.
ID3D11Texture2D* g_resolved = nullptr;
ID3D11ShaderResourceView* g_resolvedSrv = nullptr;
uint32_t g_resolvedW = 0, g_resolvedH = 0;

template <typename T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }

ID3DBlob* Compile(const char* entry, const char* profile) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    D3DCompile(kCopyHlsl, sizeof(kCopyHlsl) - 1, "rs_stage_copy", nullptr, nullptr, entry, profile,
               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err) err->Release();
    return code;
}

bool EnsurePipeline(ID3D11Device* dev) {
    if (g_broken) return false;
    if (g_vs) return true;
    bool ok = true;
    if (ID3DBlob* b = Compile("VS", "vs_5_0")) {
        ok = SUCCEEDED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs)) && ok;
        b->Release();
    } else ok = false;
    for (int i = 0; i < PS_COUNT; ++i) {
        if (ID3DBlob* b = Compile(kPsNames[i], "ps_5_0")) {
            ok = SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_ps[i])) && ok;
            b->Release();
        } else ok = false;
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb)) && ok;
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ok = SUCCEEDED(dev->CreateBlendState(&blend, &g_blendColour)) && ok;
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    ok = SUCCEEDED(dev->CreateBlendState(&blend, &g_blendAdd)) && ok;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ok = SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_noDepth)) && ok;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = SUCCEEDED(dev->CreateRasterizerState(&rd, &g_raster)) && ok;
    if (!ok) g_broken = true;
    return ok;
}

bool EnsureResolved(ID3D11Device* dev, uint32_t w, uint32_t h) {
    if (g_resolved && g_before && g_resolvedW == w && g_resolvedH == h) return true;
    Drop(g_resolvedSrv);
    Drop(g_resolved);
    Drop(g_beforeSrv);
    Drop(g_before);
    {
        D3D11_TEXTURE2D_DESC b = {};
        b.Width = w;
        b.Height = h;
        b.MipLevels = 1;
        b.ArraySize = 1;
        b.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        b.SampleDesc.Count = 1;
        b.Usage = D3D11_USAGE_DEFAULT;
        b.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&b, nullptr, &g_before))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(g_before, &sd, &g_beforeSrv);
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_resolved))) return false;
    dev->CreateShaderResourceView(g_resolved, nullptr, &g_resolvedSrv);
    g_resolvedW = w;
    g_resolvedH = h;
    return g_resolvedSrv != nullptr;
}

// One full screen draw of ours into rtv at the picture's size; the caller saves and restores the state.
void Draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, ID3D11PixelShader* ps, ID3D11BlendState* blend,
          ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1, uint32_t w, uint32_t h, float exposure,
          ID3D11ShaderResourceView* t2 = nullptr) {
    const float c[4] = { exposure, exposure > 0.0f ? 1.0f / exposure : 1.0f, 0.0f, 0.0f };
    ctx->UpdateSubresource(g_cb, 0, nullptr, c, 0, 0);
    ID3D11ShaderResourceView* none[3] = {};
    ctx->PSSetShaderResources(0, 3, none);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const float factor[4] = {};
    ctx->OMSetBlendState(blend, factor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_noDepth, 0);
    ctx->RSSetState(g_raster);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ID3D11ShaderResourceView* srvs[3] = { t0, t1, t2 };
    ctx->PSSetShaderResources(0, 3, srvs);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 3, none);
    ID3D11RenderTargetView* noRtv = nullptr;
    ctx->OMSetRenderTargets(1, &noRtv, nullptr);
}

ID3D11RenderTargetView* TargetRtv(ID3D11Device* dev, const DxrStageFrame& f) {
    D3D11_RENDER_TARGET_VIEW_DESC d = {};
    d.Format = f.viewFormat;
    d.ViewDimension = f.samples > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* v = nullptr;
    dev->CreateRenderTargetView(f.target, &d, &v);
    return v;
}

ID3D11ShaderResourceView* TargetSrv(ID3D11Device* dev, const DxrStageFrame& f) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = f.viewFormat;
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* v = nullptr;
    dev->CreateShaderResourceView(f.target, &d, &v);
    return v;
}

}  // namespace

bool rs::stage_copy_in(ID3D11DeviceContext* ctx, const DxrStageFrame& f, ID3D11RenderTargetView* back) {
    if (!ctx || !f.target || !back) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    bool ok = dev && EnsurePipeline(dev);
    if (ok && f.encoding != DXR_ENC_SRGB8) {
        ok = EnsureResolved(dev, f.width, f.height);
        if (ok) {
            if (f.samples > 1) {
                ctx->ResolveSubresource(g_resolved, 0, f.target, 0, f.viewFormat);
            } else {
                const D3D11_BOX box = { 0, 0, 0, f.width, f.height, 1 };
                ctx->CopySubresourceRegion(g_resolved, 0, 0, 0, 0, f.target, 0, &box);
            }
            Draw(ctx, back, g_ps[f.encoding == DXR_ENC_SCENE ? PS_IN_SCENE : PS_IN_LINEAR], g_blendColour,
                 g_resolvedSrv, nullptr, f.width, f.height, f.exposure);
            ID3D11Resource* backTex = nullptr;
            back->GetResource(&backTex);
            if (backTex) { ctx->CopyResource(g_before, backTex); backTex->Release(); }
        }
    } else if (ok) {
        ID3D11ShaderResourceView* src = TargetSrv(dev, f);
        ok = src != nullptr;
        if (ok) Draw(ctx, back, g_ps[PS_IN_RAW], g_blendColour, src, nullptr, f.width, f.height, 1.0f);
        if (src) src->Release();
    }
    if (dev) dev->Release();
    return ok;
}

bool rs::stage_copy_out(ID3D11DeviceContext* ctx, const DxrStageFrame& f, ID3D11ShaderResourceView* back) {
    if (!ctx || !f.target || !back) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    bool ok = dev && EnsurePipeline(dev);
    ID3D11RenderTargetView* rtv = ok ? TargetRtv(dev, f) : nullptr;
    ok = ok && rtv;
    if (ok) {
        if (f.encoding == DXR_ENC_SRGB8)
            Draw(ctx, rtv, g_ps[PS_OUT_RAW], g_blendColour, back, nullptr, f.width, f.height, 1.0f);
        else
            Draw(ctx, rtv, g_ps[f.encoding == DXR_ENC_SCENE ? PS_OUT_SCENE : PS_OUT_LINEAR], g_blendAdd, back,
                 g_resolvedSrv, f.width, f.height, f.exposure, g_beforeSrv);
    }
    if (rtv) rtv->Release();
    if (dev) dev->Release();
    return ok;
}

void rs::stage_io_release() {
    Drop(g_beforeSrv);
    Drop(g_before);
    Drop(g_resolvedSrv);
    Drop(g_resolved);
    g_resolvedW = g_resolvedH = 0;
    Drop(g_vs);
    for (auto& p : g_ps) Drop(p);
    Drop(g_cb);
    Drop(g_blendAdd);
    Drop(g_blendColour);
    Drop(g_noDepth);
    Drop(g_raster);
    g_broken = false;
}
