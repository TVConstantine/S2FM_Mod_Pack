#include "rs_stage_io.h"

#include <d3dcompiler.h>
#include <cstdlib>
#include <cstring>

namespace {

// The encode, one thread a texel. rcpNF in double on the CPU: N*F is 2.4e7.
const char kDepthCs[] = R"(
Texture2D<float> ViewZ : register(t0);
RWTexture2D<float> Out : register(u0);
cbuffer C : register(b0) { float rcpNF; float F; uint width; uint height; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    const float z = ViewZ[id.xy];
    const float L = (z <= 0.0 || z >= 1e30) ? 1.0 : saturate(z * rcpNF);
    Out[id.xy] = L * F / (1.0 + L * (F - 1.0));
}
)";

ID3D11ComputeShader* g_depthCs = nullptr;
ID3D11Buffer* g_depthCb = nullptr;
ID3D11Texture2D* g_depthTex = nullptr;
ID3D11UnorderedAccessView* g_depthUav = nullptr;
ID3D11ShaderResourceView* g_depthSrv = nullptr;
uint32_t g_depthW = 0, g_depthH = 0;
bool g_depthBroken = false;

template <typename T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool EnsureDepth(ID3D11Device* dev, uint32_t w, uint32_t h) {
    if (g_depthBroken) return false;
    if (!g_depthCs) {
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(kDepthCs, sizeof(kDepthCs) - 1, "rs_depth_encode", nullptr, nullptr, "main", "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)) || !code) {
            if (err) err->Release();
            g_depthBroken = true;
            return false;
        }
        dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_depthCs);
        code->Release();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, &g_depthCb);
        if (!g_depthCs || !g_depthCb) { g_depthBroken = true; return false; }
    }
    if (g_depthTex && g_depthW == w && g_depthH == h) return true;
    Drop(g_depthSrv);
    Drop(g_depthUav);
    Drop(g_depthTex);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_depthTex))) return false;
    dev->CreateUnorderedAccessView(g_depthTex, nullptr, &g_depthUav);
    dev->CreateShaderResourceView(g_depthTex, nullptr, &g_depthSrv);
    g_depthW = w;
    g_depthH = h;
    return g_depthUav && g_depthSrv;
}

double DefinitionValue(const std::vector<std::pair<std::string, std::string>>& defs, const char* name, double fallback) {
    for (const auto& d : defs) {
        if (d.first == name) {
            char* end = nullptr;
            const double v = strtod(d.second.c_str(), &end);
            if (end != d.second.c_str() && v > 0.0) return v;
        }
    }
    return fallback;
}

}  // namespace

rs::depth_scale rs::depth_scale_from(const std::vector<std::pair<std::string, std::string>>& definitions) {
    depth_scale s;
    s.nearPlane = DefinitionValue(definitions, "SOURCE2_NEAR_PLANE", s.nearPlane);
    s.farPlane = DefinitionValue(definitions, "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", s.farPlane);
    return s;
}

bool rs::depth_encode(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* viewZ, uint32_t w, uint32_t h, depth_scale s) {
    if (!ctx || !viewZ || !w || !h) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    const bool ok = dev && EnsureDepth(dev, w, h);
    if (dev) dev->Release();
    if (!ok) return false;
    struct { float rcpNF; float F; uint32_t width, height; } c = {
        (float)(1.0 / (s.nearPlane * s.farPlane)), (float)s.farPlane, w, h };
    ctx->UpdateSubresource(g_depthCb, 0, nullptr, &c, 0, 0);
    // What the compute stage had bound, put back after.
    ID3D11ComputeShader* oldCs = nullptr;
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv = nullptr;
    ctx->CSGetShaderResources(0, 1, &oldSrv);
    ID3D11UnorderedAccessView* oldUav = nullptr;
    ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
    ID3D11Buffer* oldCb = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &oldCb);

    ctx->CSSetShader(g_depthCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &viewZ);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_depthUav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g_depthCb);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    ID3D11UnorderedAccessView* none = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    const UINT keep = (UINT)-1;
    ctx->CSSetShader(oldCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &oldSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
    ctx->CSSetConstantBuffers(0, 1, &oldCb);
    if (oldCs) oldCs->Release();
    if (oldSrv) oldSrv->Release();
    if (oldUav) oldUav->Release();
    if (oldCb) oldCb->Release();
    return true;
}

ID3D11ShaderResourceView* rs::depth_srv() { return g_depthSrv; }

void rs::depth_release() {
    Drop(g_depthSrv);
    Drop(g_depthUav);
    Drop(g_depthTex);
    Drop(g_depthCb);
    Drop(g_depthCs);
    g_depthW = g_depthH = 0;
    g_depthBroken = false;
}
