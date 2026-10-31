// The scene's depth for a see-through surface of a shader compiler's own (a water: how deep it is under each pixel,
// its shore, the floor its caustics fall on). Alyx gives such a draw no depth it can read: the depth buffer is bound
// for the draw itself. Before the draw the bound depth is copied; a multisampled one is brought down to one sample
// (sample 0, as the picture's copy is resolved) by a compute pass into R32_FLOAT. The value is the depth buffer's own,
// 0..1 as the viewport's depth range has it.
#include "../core/core.h"
#include "scene_depth.h"

#include <d3dcompiler.h>

namespace {

const char kDepthCs[] =
    "Texture2DMS<float> g_depth : register(t0);\n"
    "RWTexture2D<float> g_out : register(u0);\n"
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    uint w, h, n;\n"
    "    g_depth.GetDimensions(w, h, n);\n"
    "    if (id.x < w && id.y < h) g_out[id.xy] = g_depth.Load(id.xy, 0);\n"
    "}\n";

ID3D11ComputeShader* g_cs = nullptr;
bool g_broken = false;
// A copy of one depth buffer: of the bound depth (its typeless family, multisampled as it is) and the one sample
// result; used: the generation it was last made for.
struct Copy {
    ID3D11Texture2D* copy = nullptr;
    ID3D11ShaderResourceView* copySrv = nullptr;
    D3D11_TEXTURE2D_DESC copyDesc = {};
    ID3D11Texture2D* one = nullptr;
    ID3D11ShaderResourceView* oneSrv = nullptr;
    ID3D11UnorderedAccessView* oneUav = nullptr;
    UINT oneW = 0, oneH = 0;
    uint64_t used = 0;
};
// One set per use (0 the surface's own, 1 the water's effects after its draw, 2 the wetness at the frame's end), and in
// each a copy for every kind of depth buffer it meets. S2FM's viewport (8 samples) and the Material Editor's preview (4)
// draw by turns: one copy, made over at every turn, was some 40 new 1920x1080 textures a second and stalled the frame.
const int kKinds = 4;
struct Use {
    Copy copies[kKinds];
    uint64_t madeFor = 0;
    ID3D11ShaderResourceView* last = nullptr;
};
Use g_uses[3];

template <class T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool Family(DXGI_FORMAT f, DXGI_FORMAT* family, DXGI_FORMAT* view) {
    switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
        *family = DXGI_FORMAT_R24G8_TYPELESS; *view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return true;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT:
        *family = DXGI_FORMAT_R32_TYPELESS; *view = DXGI_FORMAT_R32_FLOAT; return true;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        *family = DXGI_FORMAT_R32G8X24_TYPELESS; *view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return true;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM:
        *family = DXGI_FORMAT_R16_TYPELESS; *view = DXGI_FORMAT_R16_UNORM; return true;
    default: return false;
    }
}

}  // namespace

ID3D11ShaderResourceView* SceneDepthFor(ID3D11DeviceContext* ctx, uint64_t gen, int which) {
    if (g_broken || which < 0 || which > 2) return nullptr;
    Use& u = g_uses[which];
    if (u.last && gen == u.madeFor) return u.last;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv) return nullptr;
    {
        static int said[3] = {};
        D3D11_DEPTH_STENCIL_VIEW_DESC vd = {};
        dsv->GetDesc(&vd);
        if (!said[which]) {
            said[which] = 1;
            Log("[DEPTH] copy %d: the bound depth view is format %d, dimension %d, flags %u (1 read-only depth, 2 "
                "read-only stencil)\n", which, (int)vd.Format, (int)vd.ViewDimension, vd.Flags);
        }
    }
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    dsv->Release();
    ID3D11Texture2D* tex = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    if (res) res->Release();
    if (!tex) return nullptr;
    ID3D11ShaderResourceView* out = SceneDepthOf(ctx, tex, gen, which);
    tex->Release();
    return out;
}

ID3D11ShaderResourceView* SceneDepthOf(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, uint64_t gen, int which) {
    if (g_broken || which < 0 || which > 2 || !tex) return nullptr;
    Use& u = g_uses[which];
    if (u.last && gen == u.madeFor) return u.last;
    tex->AddRef();   // let go below as the copy is made
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    DXGI_FORMAT family, view;
    if (!Family(d.Format, &family, &view) || d.ArraySize != 1 || d.MipLevels != 1) { tex->Release(); return nullptr; }
    // The copy kept for this kind of buffer; else a free one, or the one used least lately, made over for it.
    Copy* pick = nullptr;
    for (Copy& c : u.copies)
        if (c.copy && c.copyDesc.Width == d.Width && c.copyDesc.Height == d.Height && c.copyDesc.Format == d.Format
            && c.copyDesc.SampleDesc.Count == d.SampleDesc.Count && c.copyDesc.SampleDesc.Quality == d.SampleDesc.Quality) {
            pick = &c;
            break;
        }
    if (!pick) {
        pick = &u.copies[0];
        for (Copy& c : u.copies) {
            if (!c.copy) { pick = &c; break; }
            if (c.used < pick->used) pick = &c;
        }
    }
    Copy& c0 = *pick;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    const bool ms = d.SampleDesc.Count > 1;
    if (!c0.copy || c0.copyDesc.Width != d.Width || c0.copyDesc.Height != d.Height || c0.copyDesc.Format != d.Format
        || c0.copyDesc.SampleDesc.Count != d.SampleDesc.Count || c0.copyDesc.SampleDesc.Quality != d.SampleDesc.Quality) {
        Drop(c0.copySrv);
        Drop(c0.copy);
        D3D11_TEXTURE2D_DESC c = d;
        c.Format = family;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        c.CPUAccessFlags = 0;
        c.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = view;
        if (ms) sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
        }
        if (!dev || FAILED(dev->CreateTexture2D(&c, nullptr, &c0.copy))
            || FAILED(dev->CreateShaderResourceView(c0.copy, &sd, &c0.copySrv))) {
            Drop(c0.copy);
            Drop(c0.copySrv);
            Log("[DEPTH] the scene depth copy could not be made (%ux%u, format %d, %u samples)\n", d.Width, d.Height,
                (int)d.Format, d.SampleDesc.Count);
            g_broken = true;
        } else {
            c0.copyDesc = d;
            Log("[DEPTH] the scene depth for see-through surfaces: %ux%u, format %d, %u samples\n", d.Width, d.Height,
                (int)d.Format, d.SampleDesc.Count);
        }
    }
    if (g_broken) { tex->Release(); if (dev) dev->Release(); return nullptr; }
    ctx->CopyResource(c0.copy, tex);
    tex->Release();
    u.last = c0.copySrv;
    if (ms) {
        if (!g_cs) {
            ID3DBlob* code = nullptr;
            ID3DBlob* err = nullptr;
            if (FAILED(D3DCompile(kDepthCs, sizeof(kDepthCs) - 1, "scene_depth", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                                  &code, &err)) || !code
                || FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs))) {
                Log("[DEPTH] the scene depth's shader could not be made: %s\n",
                    err ? (const char*)err->GetBufferPointer() : "");
                g_broken = true;
            }
            if (code) code->Release();
            if (err) err->Release();
        }
        if (!g_broken && (!c0.one || c0.oneW != d.Width || c0.oneH != d.Height)) {
            Drop(c0.oneUav);
            Drop(c0.oneSrv);
            Drop(c0.one);
            D3D11_TEXTURE2D_DESC o = {};
            o.Width = d.Width;
            o.Height = d.Height;
            o.MipLevels = 1;
            o.ArraySize = 1;
            o.Format = DXGI_FORMAT_R32_FLOAT;
            o.SampleDesc.Count = 1;
            o.Usage = D3D11_USAGE_DEFAULT;
            o.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(dev->CreateTexture2D(&o, nullptr, &c0.one)) || FAILED(dev->CreateShaderResourceView(c0.one, nullptr, &c0.oneSrv))
                || FAILED(dev->CreateUnorderedAccessView(c0.one, nullptr, &c0.oneUav))) {
                Drop(c0.oneUav);
                Drop(c0.oneSrv);
                Drop(c0.one);
                g_broken = true;
            } else {
                c0.oneW = d.Width;
                c0.oneH = d.Height;
            }
        }
        if (g_broken) { if (dev) dev->Release(); u.last = nullptr; return nullptr; }
        const bool pass = t_CorePassThrough;
        t_CorePassThrough = true;
        // What the engine had in the compute stage, put back after.
        ID3D11ComputeShader* oldCs = nullptr;
        ctx->CSGetShader(&oldCs, nullptr, nullptr);
        ID3D11ShaderResourceView* oldSrv = nullptr;
        ctx->CSGetShaderResources(0, 1, &oldSrv);
        ID3D11UnorderedAccessView* oldUav = nullptr;
        ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
        ctx->CSSetShader(g_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &c0.copySrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &c0.oneUav, nullptr);
        ctx->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->CSSetShaderResources(0, 1, &none);
        const UINT keep = (UINT)-1;
        ctx->CSSetShader(oldCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &oldSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
        if (oldCs) oldCs->Release();
        if (oldSrv) oldSrv->Release();
        if (oldUav) oldUav->Release();
        t_CorePassThrough = pass;
        u.last = c0.oneSrv;
    }
    if (dev) dev->Release();
    c0.used = gen;
    u.madeFor = gen;
    return u.last;
}
