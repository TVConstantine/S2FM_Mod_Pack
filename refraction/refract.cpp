// Particle refraction in S2FM's own viewports: the frame copy the engine never makes.
//
// A shader that reads g_tFrameBufferCopyTexture (the spritecard's refracting variant, S_REFRACT: a shock wave, a
// speaker's ripple) bends a copy of the picture drawn so far. Alyx left that copy out for VR, so the sprite comes out
// grey. Before such a draw the target it draws into is copied (resolved when it is multisampled) and the copy is bound
// where the shader reads it; Valve's own shader then bends it by the particle editor's settings. The copy is laid out
// as the engine's own would have been, which PerView says (cb1[42], g_vFrameBufferCopyInvSizeAndUvScale): a texture
// of its size with the picture in its top left corner. In S2FM's viewport that is a 1920x1080 copy of a 1280x720
// picture, uv scale two thirds.
//
// The draw also gets its blur's constants (cb5, FilterTaps_t) with the uv clamp set: each tap's uv is
// min(uv, cb5[1].zw), the last texel of the picture in the copy, and that is set by whatever makes the copy, so with
// no copy it was 0 and every tap read the copy's top left corner, a flat colour over the whole sprite. One copy
// serves a run of refracting draws and is made again once anything else has drawn, the way the engine would copy
// once before its refracting sprites; S2FM's refinement draws the scene again for every sample without a present
// between, so a copy kept past the next draw of anything would be a sample old.
//
// Test words, on the first line of s2fm_refract_debug.txt beside the DLL (read twice a second):
//   norefract       no copy, as the engine has it;
//   refractblurN    every refracting draw blurred N pixels (of a 1080 line picture), refractbox a box blur;
//   refractprobe    once a line, the first refracting draw's constant buffers as the card has them against the
//                   core's copies, and the picture before and after it (s2fm_refract_probe.txt, _before/_after.ppm);
//   blurdump        once a line, the blur's pictures (refract_blur.h).
#include "../core/core.h"
#include "../common/shader_table.h"
#include "refract_blur.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// --- The test words.
std::atomic<bool> g_off{false};
std::atomic<float> g_blurOverride{-1.0f};
std::atomic<int> g_blurOverrideType{0};
std::atomic<bool> g_probeArmed{false};
std::string g_line;

void ReadSwitch() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 500) return;
    last = now;
    std::string line;
    FILE* f = nullptr;
    if (_wfopen_s(&f, CorePath(L"s2fm_refract_debug.txt").c_str(), L"r") == 0 && f) {
        char b[200] = {};
        if (fgets(b, sizeof(b), f)) line = b;
        fclose(f);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    }
    if (line == g_line) return;
    g_line = line;
    g_off.store(line.find("norefract") != std::string::npos, std::memory_order_relaxed);
    const size_t rb = line.find("refractblur");
    g_blurOverride.store(rb != std::string::npos ? (float)atof(line.c_str() + rb + 11) : -1.0f,
                         std::memory_order_relaxed);
    g_blurOverrideType.store(line.find("refractbox") != std::string::npos ? 1 : 0, std::memory_order_relaxed);
    if (line.find("refractprobe") != std::string::npos) g_probeArmed.store(true);
    if (line.find("blurdump") != std::string::npos) RefractBlurDumpArm();
    Log("[REFRACT] test words: \"%s\"\n", line.c_str());
}

bool BlurOverride(float* radius, int* type) {
    const float r = g_blurOverride.load(std::memory_order_relaxed);
    if (r < 0.0f) return false;
    *radius = r;
    *type = g_blurOverrideType.load(std::memory_order_relaxed);
    return true;
}

// The slot the bound pixel shader reads a texture parameter at, by its name, or -1.
int TextureSlot(const ContextState* st, const char* name) {
    uint8_t reg = 0;
    uint16_t offset = 0;
    return st->psLayout >= 0 && ShaderDbFind(st->psLayout, name, &reg, &offset) && reg == SDB_REG_TEXTURE
               ? (int)(offset & 0xFF) : -1;
}

struct FrameCopySwap {
    int slot = -1;
    ID3D11ShaderResourceView* engine = nullptr;
    ID3D11Buffer* engineTaps = nullptr;
    bool tapsSwapped = false;
    bool probe = false;
};
thread_local FrameCopySwap t_swap;
thread_local bool t_stale = true;
thread_local ID3D11PixelShader* t_ps = nullptr;
thread_local int t_layout = -2;
thread_local int t_slot = -1;
// The copy, the immediate context's alone (a deferred one is left as the engine has it).
ID3D11Texture2D* g_copyTex = nullptr;
ID3D11ShaderResourceView* g_copySrv = nullptr;
UINT g_copyW = 0, g_copyH = 0;
DXGI_FORMAT g_copyFormat = DXGI_FORMAT_UNKNOWN;
// A multisampled target is resolved here first, at its own size: a resolve has no region.
ID3D11Texture2D* g_resolveTex = nullptr;
UINT g_resolveW = 0, g_resolveH = 0;
DXGI_FORMAT g_resolveFormat = DXGI_FORMAT_UNKNOWN;
std::atomic<int> g_draws{0}, g_copiesMade{0}, g_refused{0};
// Counted up each time the copy is made again: the blurred copies (refract_blur.h) are made once per copy.
uint64_t g_copyGen = 0;
// The draw's cb5 with the uv clamp (register 1, zw) at the picture's last texel in the copy.
ID3D11Buffer* g_tapsCb5 = nullptr;
UINT g_tapsCb5Bytes = 0;
std::vector<uint8_t> g_tapsCb5Data;

ID3D11Buffer* ClampedTapsCb5(ID3D11DeviceContext* ctx, ID3D11Buffer* engine, float maxU, float maxV, float was[4],
                             bool oneTap) {
    if (!engine) return nullptr;
    D3D11_BUFFER_DESC bd = {};
    engine->GetDesc(&bd);
    if (bd.ByteWidth < 32 || bd.ByteWidth > 65536) return nullptr;
    g_tapsCb5Data.assign(bd.ByteWidth, 0);
    if (!CoreCbRead(engine, 0, g_tapsCb5Data.data(), bd.ByteWidth)) return nullptr;
    float* reg1 = (float*)(g_tapsCb5Data.data() + 16);
    memcpy(was, reg1, 16);
    reg1[2] = maxU;
    reg1[3] = maxV;
    if (oneTap && bd.ByteWidth >= 7 * 16) {
        // The copy is blurred already: one tap, where the bend puts it, weight one.
        float* reg0 = (float*)g_tapsCb5Data.data();
        float* tap0 = (float*)(g_tapsCb5Data.data() + 6 * 16);
        reg0[0] = 1.0f;
        tap0[0] = 0.0f;
        tap0[1] = 0.0f;
        tap0[2] = 1.0f;
        tap0[3] = 0.0f;
    }
    if (!g_tapsCb5 || g_tapsCb5Bytes != bd.ByteWidth) {
        if (g_tapsCb5) { g_tapsCb5->Release(); g_tapsCb5 = nullptr; }
        g_tapsCb5Bytes = 0;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        D3D11_BUFFER_DESC nd = {};
        nd.ByteWidth = bd.ByteWidth;
        nd.Usage = D3D11_USAGE_DEFAULT;
        nd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (dev && SUCCEEDED(dev->CreateBuffer(&nd, nullptr, &g_tapsCb5))) g_tapsCb5Bytes = bd.ByteWidth;
        else g_tapsCb5 = nullptr;
        if (dev) dev->Release();
        if (!g_tapsCb5) return nullptr;
    }
    ctx->UpdateSubresource(g_tapsCb5, 0, nullptr, g_tapsCb5Data.data(), 0, 0);
    return g_tapsCb5;
}

// --- The probe.
// What a buffer holds on the card, through a staging copy (a stall, once a line).
std::vector<uint8_t> ProbeBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* b) {
    std::vector<uint8_t> out;
    if (!b) return out;
    D3D11_BUFFER_DESC bd = {};
    b->GetDesc(&bd);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    D3D11_BUFFER_DESC sd = {};
    sd.ByteWidth = bd.ByteWidth;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* st = nullptr;
    if (dev && SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &st))) {
        ctx->CopyResource(st, b);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            out.assign((const uint8_t*)m.pData, (const uint8_t*)m.pData + bd.ByteWidth);
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    if (dev) dev->Release();
    return out;
}

// A half float texture's top left w x h, tone mapped (s2fm_refract_<name>.ppm), and its mean.
bool ProbeTexture(ID3D11DeviceContext* ctx, ID3D11Texture2D* t, UINT w, UINT h, const wchar_t* name, double mean[3]) {
    D3D11_TEXTURE2D_DESC td = {};
    t->GetDesc(&td);
    if (td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || td.SampleDesc.Count != 1) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    ID3D11Texture2D* st = nullptr;
    bool ok = false;
    if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
        ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE* fp = nullptr;
            _wfopen_s(&fp, CorePath(name).c_str(), L"wb");
            if (fp) fprintf(fp, "P6\n%u %u\n255\n", w, h);
            mean[0] = mean[1] = mean[2] = 0.0;
            for (UINT y = 0; y < h; ++y) {
                const uint16_t* row = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
                for (UINT x = 0; x < w; ++x) {
                    uint8_t b3[3];
                    for (int c = 0; c < 3; ++c) {
                        const float v = RefractHalfToFloat(row[x * 4 + c]);
                        mean[c] += v;
                        const float g = v > 0.0f ? v / (1.0f + v) : 0.0f;
                        b3[c] = (uint8_t)(powf(g, 1.0f / 2.2f) * 255.0f + 0.5f);
                    }
                    if (fp) fwrite(b3, 1, 3, fp);
                }
            }
            for (int c = 0; c < 3; ++c) mean[c] /= (double)w * h;
            if (fp) fclose(fp);
            ctx->Unmap(st, 0);
            ok = true;
        }
        st->Release();
    }
    if (dev) dev->Release();
    return ok;
}

ID3D11Texture2D* CopyTexture(ID3D11DeviceContext* ctx, UINT w, UINT h, DXGI_FORMAT f, ID3D11ShaderResourceView** srv) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC cd = {};
    cd.Width = w;
    cd.Height = h;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = f;
    cd.SampleDesc.Count = 1;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* t = nullptr;
    if (!dev || FAILED(dev->CreateTexture2D(&cd, nullptr, &t))) t = nullptr;
    if (t && srv && FAILED(dev->CreateShaderResourceView(t, nullptr, srv))) { t->Release(); t = nullptr; }
    if (dev) dev->Release();
    return t;
}

// The copy of what the bound target holds now, at copyW x copyH with the picture in its top left corner, made only
// when asked to (fresh) or when it is of another target; its view, or null when the target is not a plain 2D one
// this can copy.
ID3D11ShaderResourceView* CopyOf(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, bool fresh, UINT copyW,
                                 UINT copyH, UINT* samples, UINT* pictureW, UINT* pictureH) {
    static ID3D11Resource* s_of = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rtv->GetDesc(&rd);
    UINT mip = 0;
    if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) mip = rd.Texture2D.MipSlice;
    else if (rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMS) return nullptr;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (!res) return nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    ((ID3D11Texture2D*)res)->GetDesc(&td);
    const UINT w = td.Width >> mip ? td.Width >> mip : 1;
    const UINT h = td.Height >> mip ? td.Height >> mip : 1;
    *samples = td.SampleDesc.Count;
    *pictureW = w;
    *pictureH = h;
    if (copyW < w || copyH < h || copyW > 16384 || copyH > 16384) {
        // No copy size to go by, or one the picture does not fit in: the picture's own.
        copyW = w;
        copyH = h;
    }
    if (!g_copyTex || copyW != g_copyW || copyH != g_copyH || rd.Format != g_copyFormat) {
        if (g_copySrv) { g_copySrv->Release(); g_copySrv = nullptr; }
        if (g_copyTex) { g_copyTex->Release(); g_copyTex = nullptr; }
        g_copyW = g_copyH = 0;
        g_copyFormat = DXGI_FORMAT_UNKNOWN;
        s_of = nullptr;
        g_copyTex = CopyTexture(ctx, copyW, copyH, rd.Format, &g_copySrv);
        if (g_copyTex) {
            g_copyW = copyW;
            g_copyH = copyH;
            g_copyFormat = rd.Format;
            Log("[REFRACT] the frame copy for the refracting sprites is made, %ux%u, format %d\n", copyW, copyH,
                (int)rd.Format);
        } else {
            Log("[REFRACT] the frame copy could not be made at %ux%u, format %d\n", copyW, copyH, (int)rd.Format);
        }
        fresh = true;
    }
    ID3D11Resource* from = res;
    UINT fromSub = D3D11CalcSubresource(mip, 0, td.MipLevels);
    if (td.SampleDesc.Count > 1) {
        if (!g_resolveTex || w != g_resolveW || h != g_resolveH || rd.Format != g_resolveFormat) {
            if (g_resolveTex) { g_resolveTex->Release(); g_resolveTex = nullptr; }
            g_resolveTex = CopyTexture(ctx, w, h, rd.Format, nullptr);
            g_resolveW = g_resolveTex ? w : 0;
            g_resolveH = g_resolveTex ? h : 0;
            g_resolveFormat = g_resolveTex ? rd.Format : DXGI_FORMAT_UNKNOWN;
            fresh = true;
        }
        from = g_resolveTex;
        fromSub = 0;
    }
    if (g_copySrv && from && (fresh || res != s_of)) {
        if (td.SampleDesc.Count > 1) ctx->ResolveSubresource(g_resolveTex, 0, res, 0, rd.Format);
        const D3D11_BOX box = { 0, 0, 0, w, h, 1 };
        ctx->CopySubresourceRegion(g_copyTex, 0, 0, 0, 0, from, fromSub, &box);
        s_of = res;
        ++g_copyGen;
        g_copiesMade.fetch_add(1, std::memory_order_relaxed);
    }
    res->Release();
    return from ? g_copySrv : nullptr;
}

void BeforeDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    if (st->ps != t_ps || st->psLayout != t_layout) {
        t_ps = st->ps;
        t_layout = st->psLayout;
        t_slot = TextureSlot(st, "g_tFrameBufferCopyTexture");
    }
    const int slot = t_slot;
    if (slot < 0 || slot >= 32) { t_stale = true; return; }
    if (g_off.load(std::memory_order_relaxed)) return;
    if (st->deferred) { g_refused.fetch_add(1, std::memory_order_relaxed); return; }
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) { g_refused.fetch_add(1, std::memory_order_relaxed); return; }
    // The copy's size as the shader is told it (cb1[42].xy, its inverse).
    float fc[4] = {};
    CoreCbRead(st->psCb[1], 672, fc, sizeof(fc));
    const UINT copyW = fc[0] > 0.0f ? (UINT)lroundf(1.0f / fc[0]) : 0;
    const UINT copyH = fc[1] > 0.0f ? (UINT)lroundf(1.0f / fc[1]) : 0;
    UINT samples = 0, pictureW = 0, pictureH = 0;
    ID3D11ShaderResourceView* copy = CopyOf(ctx, rtv, t_stale, copyW, copyH, &samples, &pictureW, &pictureH);
    rtv->Release();
    if (!copy) { g_refused.fetch_add(1, std::memory_order_relaxed); return; }
    t_stale = false;
    // The blur the particle asked for, when particles.dll's taps were made through the hook: the copy blurred here,
    // and one tap. Without it, Valve's taps over the plain copy.
    float asked[4] = {};
    float blurRadius = 0.0f;
    int blurType = 0;
    bool askedBlur = st->psCb[5] && CoreCbRead(st->psCb[5], 0, asked, sizeof(asked))
                     && RefractBlurAsked(asked, &blurRadius, &blurType);
    if (BlurOverride(&blurRadius, &blurType)) askedBlur = true;
    ID3D11ShaderResourceView* shown = copy;
    if (askedBlur) {
        if (ID3D11ShaderResourceView* b = RefractBlurApply(ctx, copy, g_copyW, g_copyH, pictureW, pictureH, g_copyGen,
                                                           blurRadius, blurType)) shown = b;
    }
    ID3D11ShaderResourceView* engine = st->psSrv[slot];
    if (engine) engine->AddRef();
    t_swap.slot = slot;
    t_swap.engine = engine;
    ctx->PSSetShaderResources((UINT)slot, 1, &shown);
    g_draws.fetch_add(1, std::memory_order_relaxed);
    // The blur's uv clamp at the picture's last texel in the copy (a texel centre, so the bilinear read takes nothing
    // past the picture).
    float clampWas[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    const float maxU = ((float)pictureW - 0.5f) / (float)g_copyW;
    const float maxV = ((float)pictureH - 0.5f) / (float)g_copyH;
    ID3D11Buffer* engineTaps = st->psCb[5];
    // One tap when the copy is blurred here, or when the radius is under half a pixel at this size; Valve's taps when
    // the blur could not be made.
    const bool oneTap = askedBlur && (shown != copy || blurRadius * (float)pictureH / 1080.0f < 0.5f);
    if (ID3D11Buffer* taps = ClampedTapsCb5(ctx, engineTaps, maxU, maxV, clampWas, oneTap)) {
        engineTaps->AddRef();
        t_swap.engineTaps = engineTaps;
        t_swap.tapsSwapped = true;
        ctx->PSSetConstantBuffers(5, 1, &taps);
    }
    if (g_probeArmed.exchange(false)) {
        // The constant buffers as the card has them, against the core's copies of them.
        FILE* f = nullptr;
        if (_wfopen_s(&f, CorePath(L"s2fm_refract_probe.txt").c_str(), L"w") == 0 && f) {
            fprintf(f, "line %s\ncopy %ux%u, picture %ux%u, %u samples, blend %d src %d dst %d\n", g_line.c_str(),
                    g_copyW, g_copyH, pictureW, pictureH, samples, st->blend ? 1 : 0, (int)st->blendSrc,
                    (int)st->blendDst);
            const int slots[] = { 1, 3, 4, 5 };
            for (int cbSlot : slots) {
                // cb5 is ours now; the engine's is the one to compare.
                ID3D11Buffer* b = cbSlot == 5 && t_swap.tapsSwapped ? engineTaps : st->psCb[cbSlot];
                const std::vector<uint8_t> real = ProbeBuffer(ctx, b);
                std::vector<uint8_t> shadow(real.size(), 0);
                if (!real.empty()) CoreCbRead(b, 0, shadow.data(), (UINT)shadow.size());
                const size_t regs = real.size() / 16;
                int differ = 0;
                for (size_t r = 0; r < regs; ++r)
                    if (memcmp(real.data() + r * 16, shadow.data() + r * 16, 16) != 0) ++differ;
                fprintf(f, "cb%d: %zu registers, %d differ from the core's copy\n", cbSlot, regs, differ);
                const size_t show = cbSlot == 1 ? 0 : (regs < 40 ? regs : 40);
                for (size_t r = 0; r < show; ++r) {
                    const float* a = (const float*)(real.data() + r * 16);
                    const float* c = (const float*)(shadow.data() + r * 16);
                    fprintf(f, "  [%2zu] card %g %g %g %g | copy %g %g %g %g\n", r, a[0], a[1], a[2], a[3], c[0], c[1],
                            c[2], c[3]);
                }
                if (cbSlot == 1 && regs > 42) {
                    for (size_t r : { (size_t)25, (size_t)42 }) {
                        const float* a = (const float*)(real.data() + r * 16);
                        fprintf(f, "  [%2zu] card %g %g %g %g\n", r, a[0], a[1], a[2], a[3]);
                    }
                }
            }
            double mean[3] = {};
            if (ProbeTexture(ctx, g_copyTex, pictureW, pictureH, L"s2fm_refract_before.ppm", mean))
                fprintf(f, "before (the copy): mean %.5f %.5f %.5f\n", mean[0], mean[1], mean[2]);
            fclose(f);
        }
        t_swap.probe = true;
    }

    // What the engine had there and what the shader reads the copy's uv by (PerView's inverse gbuffer size, cb1[25],
    // and the copy's inverse size and uv scale, cb1[42]), every ten seconds.
    static ULONGLONG lastSaid = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastSaid > 10000) {
        lastSaid = now;
        char had[96] = "nothing";
        if (engine) {
            ID3D11Resource* er = nullptr;
            engine->GetResource(&er);
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            if (er) er->GetType(&dim);
            if (er && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC ed = {};
                ((ID3D11Texture2D*)er)->GetDesc(&ed);
                sprintf_s(had, "%ux%u, format %d", ed.Width, ed.Height, (int)ed.Format);
            } else sprintf_s(had, "a resource of kind %d", (int)dim);
            if (er) er->Release();
        }
        float gb[4] = {}, pc[12] = {};
        int blurMade = 0, blurAsked = 0, blurLastType = 0, blurReduce = 0;
        float blurPixels = 0.0f;
        RefractBlurStats(&blurMade, &blurAsked, &blurPixels, &blurLastType, &blurReduce);
        CoreCbRead(st->psCb[1], 400, gb, sizeof(gb));
        CoreCbRead(st->psCb[4], 0, pc, sizeof(pc));
        Log("[REFRACT] t%d gets a %ux%u copy of a %ux%u %u sample target (the engine had %s) | cb1[25] %g %g, "
            "cb1[42] %g %g %g %g | blur clamp (cb5[1].zw) %g %g, the engine's %g %g%s | blend %d src %d dst %d | "
            "self light %g, lighting %g, alpha out %g, strength %g | in 10 s: %d draws, %d copies, %d left as they "
            "were | blur: %d draws asked for one, %d made, the last %.1f px %s at 1/%d size\n",
            slot, g_copyW, g_copyH, pictureW, pictureH, samples, had, gb[0], gb[1], fc[0], fc[1], fc[2], fc[3], maxU,
            maxV, clampWas[2], clampWas[3], t_swap.tapsSwapped ? "" : " (NOT set: no cb5 copy)", st->blend ? 1 : 0,
            (int)st->blendSrc, (int)st->blendDst, pc[0], pc[1], pc[7], pc[8], g_draws.exchange(0),
            g_copiesMade.exchange(0), g_refused.exchange(0), blurAsked, blurMade, blurPixels,
            blurLastType == 1 ? "box" : "Gaussian", blurReduce);
    }
}

void AfterDraw(ID3D11DeviceContext* ctx, ContextState*) {
    if (t_swap.slot < 0) return;
    ctx->PSSetShaderResources((UINT)t_swap.slot, 1, &t_swap.engine);
    if (t_swap.engine) t_swap.engine->Release();
    t_swap.engine = nullptr;
    t_swap.slot = -1;
    if (t_swap.tapsSwapped) {
        ctx->PSSetConstantBuffers(5, 1, &t_swap.engineTaps);
        t_swap.engineTaps->Release();
        t_swap.engineTaps = nullptr;
        t_swap.tapsSwapped = false;
    }
    if (t_swap.probe) {
        // The target after the one draw, resolved as the copy was.
        t_swap.probe = false;
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) {
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            D3D11_RENDER_TARGET_VIEW_DESC rd = {};
            rtv->GetDesc(&rd);
            D3D11_TEXTURE2D_DESC td = {};
            if (res) ((ID3D11Texture2D*)res)->GetDesc(&td);
            double mean[3] = {};
            bool ok = false;
            if (res && td.SampleDesc.Count > 1 && g_resolveTex) {
                ctx->ResolveSubresource(g_resolveTex, 0, res, 0, rd.Format);
                ok = ProbeTexture(ctx, g_resolveTex, g_resolveW, g_resolveH, L"s2fm_refract_after.ppm", mean);
            } else if (res) {
                ok = ProbeTexture(ctx, (ID3D11Texture2D*)res, td.Width, td.Height, L"s2fm_refract_after.ppm", mean);
            }
            FILE* f = nullptr;
            if (ok && _wfopen_s(&f, CorePath(L"s2fm_refract_probe.txt").c_str(), L"a") == 0 && f) {
                fprintf(f, "after the draw: mean %.5f %.5f %.5f\n", mean[0], mean[1], mean[2]);
                fclose(f);
            }
            if (res) res->Release();
            rtv->Release();
        }
    }
}

void Present(IDXGISwapChain*) { ReadSwitch(); }

const CoreFeature kFeature = { "particle refraction", BeforeDraw, AfterDraw, nullptr, Present, nullptr, nullptr };

}  // namespace

void RefractionRegister() {
    CoreRegister(&kFeature);
    CoreOnModuleLoad(RefractBlurHookIfLoaded);
}
