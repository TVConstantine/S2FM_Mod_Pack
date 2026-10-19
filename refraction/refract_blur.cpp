// Particle refraction's blur. See refract_blur.h.
#include "refract_blur.h"
#include "../core/core.h"

#include <windows.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>


namespace {

// particles.dll, 6 May 2022: void MakeRefractTaps(int radius, BlurFilterType_t type, FilterTaps_t* out), called from the
// sprite renderer's start (rva 0x11c135) with m_nRefractBlurRadius (+0x1E0) and m_nRefractBlurType (+0x1E4); the 0x260
// bytes it fills become the renderer's FilterTaps_t buffer. Hooked only where its first bytes are these.
constexpr uint32_t kTapsRva = 0x11d100;
const uint8_t kTapsPrologue[] = { 0x48, 0x8b, 0xc4, 0x4c, 0x89, 0x40, 0x18, 0x55, 0x48, 0x8d, 0xa8, 0x08, 0xfe, 0xff,
                                  0xff, 0x48, 0x81, 0xec, 0xf0, 0x02, 0x00, 0x00 };
// In FilterTaps_t's register 0: x is the tap count the shader reads; y the radius asked for, z the type, w this.
constexpr float kAskedMark = 271828.1875f;

// Four arguments forwarded, whatever the function names: a detour that declares fewer destroys the rest.
typedef void (*MakeTaps_Fn)(int radius, int type, float* out, void* fourth);
MakeTaps_Fn g_originalTaps = nullptr;
std::atomic<int> g_hooked{0};
std::atomic<int> g_tapsMade{0};

void MakeTaps(int radius, int type, float* out, void* fourth) {
    g_originalTaps(radius, type, out, fourth);
    if (!out) return;
    out[1] = (float)radius;
    out[2] = (float)type;
    out[3] = kAskedMark;
    const int n = g_tapsMade.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) Log("[REFRACT] a refracting sprite renderer starts: blur radius %d, %s\n", radius,
                   type == 1 ? "box" : "Gaussian");
}

// --- The blur: the copy brought down to 1/f of its size (f so that the radius there is at most 12 texels), then
// blurred along x and along y. The shader reads the result through its own uv, which is the copy's.
const char kBlurCs[] = R"(
Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);
cbuffer C : register(b0) {
    uint2 dstSize; uint2 srcSize;     // this pass's output; the copy's size (Down only)
    uint2 picSize; uint2 last;        // the picture in the copy (Down); the last texel of the picture here
    uint f; float radius; uint type; uint axis;   // radius in texels here; type 1 box; axis 0 x, 1 y
};
[numthreads(8, 8, 1)]
void Down(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= dstSize)) return;
    float4 s = 0;
    for (uint j = 0; j < f; ++j)
        for (uint i = 0; i < f; ++i) {
            const float2 p = (float2(id.xy) + (float2(i, j) + 0.5) / f) * float2(srcSize) / float2(dstSize);
            const int2 q = min(int2(p), int2(picSize) - 1);
            s += Src.Load(int3(q, 0));
        }
    Dst[id.xy] = s / float(f * f);
}
[numthreads(8, 8, 1)]
void Blur(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= dstSize)) return;
    const float sigma = max(radius * 0.5, 1e-3);
    const int reach = type == 1 ? (int)floor(radius) : (int)ceil(radius * 1.5);
    const int2 step = axis == 0 ? int2(1, 0) : int2(0, 1);
    float4 s = 0;
    float sum = 0;
    for (int k = -reach; k <= reach; ++k) {
        const float w = type == 1 ? 1.0 : exp(-(float)(k * k) / (2.0 * sigma * sigma));
        const int2 q = clamp(int2(id.xy) + step * k, int2(0, 0), int2(last));
        s += w * Src.Load(int3(q, 0));
        sum += w;
    }
    Dst[id.xy] = s / sum;
}
)";

struct Consts {
    UINT dstW, dstH, srcW, srcH;
    UINT picW, picH, lastX, lastY;
    UINT f;
    float radius;
    UINT type, axis;
};

struct Target {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    UINT w = 0, h = 0;
};

// One blurred copy per radius and type in the same copy; four is more than a frame has had.
struct Entry {
    Target t;
    uint64_t gen = ~0ull;
    float pixels = 0.0f;
    int type = -1;
    UINT copyW = 0, copyH = 0;
    ULONGLONG used = 0;
};

std::mutex g_m;
ID3D11ComputeShader* g_down = nullptr;
ID3D11ComputeShader* g_blur = nullptr;
ID3D11Buffer* g_cb = nullptr;
bool g_broken = false;
Target g_tmp[2];
Entry g_entries[4];
std::atomic<int> g_made{0}, g_asked{0}, g_lastType{0}, g_lastReduce{0};
std::atomic<float> g_lastPixels{0.0f};

template <typename T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }

void DropTarget(Target& t) {
    Drop(t.uav);
    Drop(t.srv);
    Drop(t.tex);
    t.w = t.h = 0;
}

bool EnsureTarget(ID3D11Device* dev, Target& t, UINT w, UINT h) {
    if (t.tex && t.w == w && t.h == h) return true;
    DropTarget(t);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t.tex)) || FAILED(dev->CreateShaderResourceView(t.tex, nullptr, &t.srv))
        || FAILED(dev->CreateUnorderedAccessView(t.tex, nullptr, &t.uav))) {
        DropTarget(t);
        return false;
    }
    t.w = w;
    t.h = h;
    return true;
}

bool EnsureShaders(ID3D11Device* dev) {
    if (g_broken) return false;
    if (g_down && g_blur && g_cb) return true;
    const char* entries[2] = { "Down", "Blur" };
    ID3D11ComputeShader** outs[2] = { &g_down, &g_blur };
    for (int i = 0; i < 2; ++i) {
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(kBlurCs, sizeof(kBlurCs) - 1, "refract_blur", nullptr, nullptr, entries[i], "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)) || !code
            || FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, outs[i]))) {
            Log("[REFRACT] the blur's %s shader could not be made: %s\n", entries[i],
                err ? (const char*)err->GetBufferPointer() : "");
            g_broken = true;
        }
        if (code) code->Release();
        if (err) err->Release();
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (!g_broken && FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb))) g_broken = true;
    return !g_broken;
}

void Pass(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* src, Target& dst,
          const Consts& c, UINT groupsX, UINT groupsY) {
    ctx->UpdateSubresource(g_cb, 0, nullptr, &c, 0, 0);
    ctx->CSSetShader(cs, nullptr, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->CSSetShaderResources(0, 1, &none);   // what the last pass wrote is read next
    ctx->CSSetUnorderedAccessViews(0, 1, &dst.uav, nullptr);
    ctx->CSSetShaderResources(0, 1, &src);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ctx->Dispatch(groupsX, groupsY, 1);
    ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
}

}  // namespace

void RefractBlurHookIfLoaded() {
    if (g_hooked.load(std::memory_order_acquire)) return;
    HMODULE m = GetModuleHandleW(L"particles.dll");
    if (!m) return;
    static std::mutex once;
    std::lock_guard<std::mutex> lk(once);
    if (g_hooked.load(std::memory_order_relaxed)) return;
    g_hooked.store(1, std::memory_order_release);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)m;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)((const uint8_t*)m + dos->e_lfanew);
    uint8_t* fn = (uint8_t*)m + kTapsRva;
    if (nt->OptionalHeader.SizeOfImage < kTapsRva + sizeof(kTapsPrologue)
        || memcmp(fn, kTapsPrologue, sizeof(kTapsPrologue)) != 0) {
        Log("[REFRACT] particles.dll is not the 6 May 2022 build's: the refract blur stays Valve's (4 texels at most)\n");
        return;
    }
    if (CoreHook(fn, (void*)&MakeTaps, (void**)&g_originalTaps))
        Log("[REFRACT] particles.dll's refract taps hooked: the blur radius and type reach the refracting draws\n");
    else
        Log("[REFRACT] particles.dll's refract taps could not be hooked: the blur stays Valve's\n");
}

bool RefractBlurAsked(const float reg0[4], float* radius, int* type) {
    if (reg0[3] != kAskedMark) return false;
    *radius = reg0[1];
    *type = (int)reg0[2];
    g_asked.fetch_add(1, std::memory_order_relaxed);
    return true;
}

namespace {

// The first few reasons a blur could not be made, said once each.
std::atomic<int> g_failSaid{0};
void Failed(const char* why, float pixels) {
    if (g_failSaid.fetch_add(1, std::memory_order_relaxed) < 4)
        Log("[REFRACT] a %.1f px blur could not be made: %s\n", pixels, why);
}

// The copy blurred into out (made or remade at the reduced size). *f is the size it was brought down by, *lastX and
// *lastY the picture's last texel at that size. g_tmp[0] holds the copy brought down until the next blur.
bool RunBlur(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* copy, UINT copyW, UINT copyH, UINT pictureW,
             UINT pictureH, float pixels, int type, Target& out, UINT* f, UINT* lastX, UINT* lastY) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) { Failed("no device", pixels); return false; }
    *f = pixels > 12.0f ? (UINT)std::ceil(pixels / 12.0f) : 1u;
    const UINT w = (copyW + *f - 1) / *f, h = (copyH + *f - 1) / *f;
    const bool shaders = EnsureShaders(dev);
    const bool targets = shaders && EnsureTarget(dev, g_tmp[0], w, h) && EnsureTarget(dev, g_tmp[1], w, h)
                         && EnsureTarget(dev, out, w, h);
    dev->Release();
    if (!shaders) { Failed("its shaders were not made", pixels); return false; }
    if (!targets) { Failed("its textures were not made", pixels); return false; }

    // The picture's last texel at this size; one column and row past it are made too, since a bilinear read at the
    // picture's last texel centre (the taps' clamp) reaches half a texel further here.
    *lastX = (std::min)(w - 1, (UINT)(((double)pictureW - 0.5) * w / copyW));
    *lastY = (std::min)(h - 1, (UINT)(((double)pictureH - 0.5) * h / copyH));
    const UINT doW = (std::min)(w, *lastX + 2), doH = (std::min)(h, *lastY + 2);
    Consts c = { w, h, copyW, copyH, pictureW, pictureH, *lastX, *lastY, *f, pixels / (float)*f, (UINT)type, 0 };

    // What the compute stage had bound, put back after.
    ID3D11ComputeShader* oldCs = nullptr;
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv = nullptr;
    ctx->CSGetShaderResources(0, 1, &oldSrv);
    ID3D11UnorderedAccessView* oldUav = nullptr;
    ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
    ID3D11Buffer* oldCb = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &oldCb);

    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    const UINT gx = (doW + 7) / 8, gy = (doH + 7) / 8;
    Pass(ctx, g_down, copy, g_tmp[0], c, gx, gy);
    c.axis = 0;
    Pass(ctx, g_blur, g_tmp[0].srv, g_tmp[1], c, gx, gy);
    c.axis = 1;
    Pass(ctx, g_blur, g_tmp[1].srv, out, c, gx, gy);
    t_CorePassThrough = pass;

    ID3D11ShaderResourceView* none = nullptr;
    ctx->CSSetShaderResources(0, 1, &none);
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

// "blurdump" on s2fm_refract_debug.txt: at the next blur, the copy brought down, and the same copy blurred both
// ways, written as floating point pictures (the picture's part) for the test to measure on one and the same frame.
std::atomic<bool> g_dumpArmed{false};
Target g_dumpOther;

}  // namespace

float RefractHalfToFloat(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t e = (h >> 10) & 0x1f, m = h & 0x3ff, bits;
    if (e == 0) {
        if (m == 0) bits = sign;
        else {   // subnormal
            e = 1;
            while (!(m & 0x400)) { m <<= 1; --e; }
            m &= 0x3ff;
            bits = sign | ((e + 112) << 23) | (m << 13);
        }
    } else if (e == 31) bits = sign | 0x7f800000 | (m << 13);
    else bits = sign | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

namespace {

bool DumpPfm(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT w, UINT h, const wchar_t* name) {
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11Texture2D* st = nullptr;
    bool ok = false;
    if (dev && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &st)) && st) {
        ctx->CopyResource(st, tex);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE* f = nullptr;
            if (_wfopen_s(&f, CorePath(name).c_str(), L"wb") == 0 && f) {
                fprintf(f, "PF\n%u %u\n-1.0\n", w, h);
                std::vector<float> row(w * 3);
                for (UINT y = h; y-- > 0;) {   // PFM rows run bottom to top
                    const uint16_t* src = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
                    for (UINT x = 0; x < w; ++x)
                        for (int k = 0; k < 3; ++k) row[x * 3 + k] = RefractHalfToFloat(src[x * 4 + k]);
                    fwrite(row.data(), 4, row.size(), f);
                }
                fclose(f);
                ok = true;
            }
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    if (dev) dev->Release();
    return ok;
}

}  // namespace

void RefractBlurDumpArm() { g_dumpArmed.store(true, std::memory_order_relaxed); }

ID3D11ShaderResourceView* RefractBlurApply(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* copy, UINT copyW,
                                           UINT copyH, UINT pictureW, UINT pictureH, uint64_t copyGen, float radius,
                                           int type) {
    if (!ctx || !copy || !copyW || !copyH || !pictureW || !pictureH) return nullptr;
    // Pixels of a 1080 line picture.
    const float pixels = radius * (float)pictureH / 1080.0f;
    if (!(pixels >= 0.5f)) return nullptr;
    type = type == 1 ? 1 : 0;
    std::lock_guard<std::mutex> lk(g_m);
    const ULONGLONG now = GetTickCount64();
    for (Entry& e : g_entries) {
        if (e.t.srv && e.gen == copyGen && e.pixels == pixels && e.type == type && e.copyW == copyW && e.copyH == copyH) {
            e.used = now;
            return e.t.srv;
        }
    }
    Entry* e = &g_entries[0];
    for (Entry& c : g_entries) if (c.used < e->used) e = &c;   // the one used longest ago
    UINT f = 1, lastX = 0, lastY = 0;
    e->gen = ~0ull;
    if (!RunBlur(ctx, copy, copyW, copyH, pictureW, pictureH, pixels, type, e->t, &f, &lastX, &lastY)) return nullptr;
    e->gen = copyGen;
    e->pixels = pixels;
    e->type = type;
    e->copyW = copyW;
    e->copyH = copyH;
    e->used = now;
    g_made.fetch_add(1, std::memory_order_relaxed);
    g_lastPixels.store(pixels, std::memory_order_relaxed);
    g_lastType.store(type, std::memory_order_relaxed);
    g_lastReduce.store((int)f, std::memory_order_relaxed);

    if (g_dumpArmed.exchange(false, std::memory_order_relaxed)) {
        const UINT dw = lastX + 1, dh = lastY + 1;
        bool ok = DumpPfm(ctx, g_tmp[0].tex, dw, dh, L"s2fm_refract_blur_down.pfm");
        ok = ok && DumpPfm(ctx, e->t.tex, dw, dh, type == 1 ? L"s2fm_refract_blur_box.pfm" : L"s2fm_refract_blur_gauss.pfm");
        UINT f2 = 1, lx2 = 0, ly2 = 0;
        if (ok && RunBlur(ctx, copy, copyW, copyH, pictureW, pictureH, pixels, 1 - type, g_dumpOther, &f2, &lx2, &ly2)) {
            ok = DumpPfm(ctx, g_dumpOther.tex, dw, dh,
                         type == 1 ? L"s2fm_refract_blur_gauss.pfm" : L"s2fm_refract_blur_box.pfm");
        } else ok = false;
        FILE* fi = nullptr;
        if (_wfopen_s(&fi, CorePath(L"s2fm_refract_blur_dump.txt").c_str(), L"w") == 0 && fi) {
            fprintf(fi, "pixels %.3f reduce %u picture %ux%u copy %ux%u dumped %ux%u ok %d\n", pixels, f, pictureW, pictureH,
                    copyW, copyH, dw, dh, ok ? 1 : 0);
            fclose(fi);
        }
        Log("[REFRACT] blur dump: %.1f px at 1/%u size, %ux%u, %s\n", pixels, f, dw, dh, ok ? "written" : "FAILED");
    }
    return e->t.srv;
}

void RefractBlurStats(int* made, int* asked, float* lastPixels, int* lastType, int* lastReduce) {
    *made = g_made.exchange(0, std::memory_order_relaxed);
    *asked = g_asked.exchange(0, std::memory_order_relaxed);
    *lastPixels = g_lastPixels.load(std::memory_order_relaxed);
    *lastType = g_lastType.load(std::memory_order_relaxed);
    *lastReduce = g_lastReduce.load(std::memory_order_relaxed);
}
