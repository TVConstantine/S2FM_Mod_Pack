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
// Any other shader that reads it (a refracting glass of a shader compiler's own, found in the shader table by its
// loose files) gets the picture alone at its own size with a full mip chain instead, and no blur constants: it works
// out its own uv from its pixel position and the texture's size, and reads a smaller mip where it is rough.
//
// Test words, on the first line of s2fm_refract_debug.txt beside the DLL (read twice a second):
//   norefract       no copy, as the engine has it;
//   refractblurN    every refracting draw blurred N pixels (of a 1080 line picture), refractbox a box blur;
//   refractprobe    once a line, the first refracting draw's constant buffers as the card has them against the
//                   core's copies, and the picture before and after it (s2fm_refract_probe.txt, _before/_after.ppm);
//   blurdump        once a line, the blur's pictures (refract_blur.h);
//   wetdebug        the water's wetness pass shows its maps: wet red, carried green, the water's height known blue
//                   (water_fx.h);
//   nocarry         no wetness carried on the moving models (water_carry.h);
//   noend           no wetness at the frame's end when no water is in the view;
//   wettrace        the next 40 events of the wetness's frame logged one by one (water_fx.h);
//   wetnomirror, wetnoaround, wetnoshine, wetnoadd, wetnomul   parts of the wet look left out (water_fx.h);
//   beaddebug       every bead of water on the moving models shown, blue over the dimmed picture (water_carry.h);
//   modeldebug      the wetness pass shows where it takes a moving model (red), carried wet (green), wet (blue);
//   copydebug       the wetness pass shows the picture it reads as what lies under each pixel;
//   dropdebug       the droplets drawn eight times their size, plain magenta;
//   nocopyeach      a shader compiler's own see-through surfaces share one frame copy (one behind another vanishes);
//   nolightfog      the weather's light table left empty: every light counts whole, whatever its fog setting;
//   nosortbias      the weather volumes left where the engine sorts them, no depth sort bias (weather_sort.cpp);
//   seethrough      every 3 s, one frame's depth targets (each one's size, samples, draws and first pixel shaders)
//                   and the draws of a shader compiler's own shaders (s2vfx_*): their context, the depth and target
//                   bound, the viewport, and what this feature gave them.
#include "../core/core.h"
#include "../common/shader_table.h"
#include "refract_blur.h"
#include "scene_depth.h"
#include "water_fx.h"
#include "water_carry.h"
#include "../common/sfm_doc.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

void WeatherSortOff(bool off);     // weather_sort.cpp

namespace {

// --- The test words.
std::atomic<bool> g_off{false};
std::atomic<float> g_blurOverride{-1.0f};
std::atomic<int> g_blurOverrideType{0};
std::atomic<bool> g_probeArmed{false};
std::atomic<bool> g_seeThrough{false};
std::atomic<bool> g_noLightFog{false};
std::atomic<bool> g_shareCopy{false};    // test word nocopyeach: our own surfaces share one copy, as before   // test word nolightfog: an empty light table, every light whole
// Presents so far: what times the scene depth a shader reading it without the picture gets.
std::atomic<uint64_t> g_presents{0};

// Whether a shader is a shader compiler's own: a loose file in game\core\shaders\vfx (the game's own come from its
// archives). Some of Valve's read a g_tSceneDepth of the engine's too, and keep it.
bool OwnShader(const char* name) {
    static std::vector<std::string> own;
    static std::once_flag once;
    std::call_once(once, [] {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((CoreGameDir() + L"\\core\\shaders\\vfx\\*_ps.vcs").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            std::wstring f = fd.cFileName;
            const size_t cut = f.find(L"_pc_");
            if (cut == std::wstring::npos) continue;
            std::string s;
            for (size_t i = 0; i < cut; ++i) s += (char)f[i];    // shader names are plain ASCII
            own.push_back(s);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    });
    if (!name || !*name) return false;
    for (const std::string& s : own) if (s == name) return true;
    return false;
}
std::string g_line;

// Test word weathercbN: at the next draw of s2vfx_weather after N changed, every pixel shader constant buffer bound to
// it written out as the core has it (s2fm_weathercb_<N>_<slot>.bin beside the DLL), to compare two states of a light.
std::atomic<int> g_weatherCbArmed{-1};
int g_weatherCbDone = -1;

void WeatherCbDump(ContextState* st, const char* name) {
    const int n = g_weatherCbArmed.load(std::memory_order_relaxed);
    if (n < 0 || n == g_weatherCbDone || !name || strncmp(name, "s2vfx_weather", 13)) return;
    g_weatherCbDone = n;
    for (int slot = 0; slot < 14; slot++) {
        ID3D11Buffer* cb = st->psCb[slot];
        if (!cb) continue;
        D3D11_BUFFER_DESC bd = {};
        cb->GetDesc(&bd);
        std::vector<uint8_t> bytes(bd.ByteWidth);
        UINT total = 0;
        const bool ok = CoreCbRead(cb, 0, bytes.data(), bd.ByteWidth, &total);
        wchar_t file[64];
        swprintf(file, 64, L"s2fm_weathercb_%d_%d.bin", n, slot);
        FILE* f = nullptr;
        if (ok && _wfopen_s(&f, CorePath(file).c_str(), L"wb") == 0 && f) {
            fwrite(bytes.data(), 1, bytes.size(), f);
            fclose(f);
        }
        Log("[REFRACT] weathercb %d: slot %d, %u bytes, %s\n", n, slot, bd.ByteWidth, ok ? "written" : "not readable");
    }
}

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
    WaterFxDebug(line.find("wetdebug") != std::string::npos);
    WaterCarryOff(line.find("nocarry") != std::string::npos);
    WaterFxNoEnd(line.find("noend") != std::string::npos);
    const bool beads = line.find("beaddebug") != std::string::npos;
    WaterCarryBeadView(beads);
    WaterFxLookOff((line.find("wetnomirror") != std::string::npos ? 1u : 0u)
                   | (line.find("wetnoaround") != std::string::npos ? 2u : 0u)
                   | (line.find("wetnoshine") != std::string::npos ? 4u : 0u) | (beads ? 8u : 0u)
                   | (line.find("wetnoadd") != std::string::npos ? 16u : 0u)
                   | (line.find("wetnomul") != std::string::npos ? 32u : 0u)
                   | (line.find("modeldebug") != std::string::npos ? 64u : 0u)
                   | (line.find("copydebug") != std::string::npos ? 128u : 0u)
                   | (line.find("dropdebug") != std::string::npos ? 256u : 0u));
    WaterFxTrace(line.find("wettrace") != std::string::npos);
    g_seeThrough.store(line.find("seethrough") != std::string::npos, std::memory_order_relaxed);
    g_noLightFog.store(line.find("nolightfog") != std::string::npos, std::memory_order_relaxed);
    g_shareCopy.store(line.find("nocopyeach") != std::string::npos, std::memory_order_relaxed);
    WeatherSortOff(line.find("nosortbias") != std::string::npos);
    const size_t wc = line.find("weathercb");
    if (wc != std::string::npos) g_weatherCbArmed.store(atoi(line.c_str() + wc + 9), std::memory_order_relaxed);
    Log("[REFRACT] test words: \"%s\"\n", line.c_str());
}

// --- "seethrough": what the see-through draws of a shader compiler's own meet, one frame every 3 s.
struct DepthUse {
    ID3D11Resource* res = nullptr;   // not held: an identity only
    UINT w = 0, h = 0, samples = 0, flags = 0;
    int format = 0, draws = 0, deferredDraws = 0;
    std::string first;               // the first pixel shaders that drew with it, by name
    int ownAt = -1;                  // the draw (of this depth's) our own last drew at, -1 none
    std::string after;               // the pixel shaders that drew with it after ours
    std::string seq;                 // its draws in order, a run of one shader as "name xN", ours marked "*"
    std::string seqLast;             // the run being counted
    int seqRun = 0;
};

void SeqFlush(DepthUse& u) {
    if (u.seqRun > 0 && u.seq.size() < 6000)
        u.seq += (u.seq.empty() ? "" : ", ") + u.seqLast + (u.seqRun > 1 ? " x" + std::to_string(u.seqRun) : "");
    u.seqRun = 0;
}
std::mutex g_seeMutex;
std::string g_seeFrame, g_seeFrameLast;   // the frame's draws with a colour target, in runs (SeeThroughDraw)
int g_seeFrameRun = 0;
std::vector<DepthUse> g_seeDepths;
std::vector<std::string> g_seeDraws;
bool g_seeHadOwn = false;
thread_local int t_seeIndex = -1;    // this draw's line in g_seeDraws, for what the feature then did

std::string ViewText(ID3D11View* v) {
    if (!v) return "none";
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    D3D11_TEXTURE2D_DESC d = {};
    if (r) ((ID3D11Texture2D*)r)->GetDesc(&d);
    char b[160];
    snprintf(b, sizeof(b), "%p %ux%u fmt %d x%u", (void*)r, d.Width, d.Height, (int)d.Format, d.SampleDesc.Count);
    if (r) r->Release();
    return b;
}

void SeeThroughDraw(ID3D11DeviceContext* ctx, ContextState* st, const char* name) {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11Resource* dres = nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
    D3D11_TEXTURE2D_DESC td = {};
    if (dsv) {
        dsv->GetResource(&dres);
        dsv->GetDesc(&dd);
        if (dres) ((ID3D11Texture2D*)dres)->GetDesc(&td);
    }
    const bool own = name && !strncmp(name, "s2vfx", 5);
    // Every draw with a colour target, in order, depth or not (particles may draw with none): "name WxH", "-" without
    // a depth, ours marked "*", a run of one as " xN".
    std::string frameRun;
    if (rtv) {
        ID3D11Resource* rr = nullptr;
        rtv->GetResource(&rr);
        D3D11_TEXTURE2D_DESC rd = {};
        if (rr) ((ID3D11Texture2D*)rr)->GetDesc(&rd);
        if (rr) rr->Release();
        char b[160];
        snprintf(b, sizeof(b), "%s%s %ux%u%s", own ? "*" : "", name ? name : "?", rd.Width, rd.Height, dsv ? "" : " -");
        frameRun = b;
    }
    {
        std::lock_guard<std::mutex> lk(g_seeMutex);
        if (!frameRun.empty()) {
            if (frameRun != g_seeFrameLast) {
                if (g_seeFrameRun > 0 && g_seeFrame.size() < 12000)
                    g_seeFrame += (g_seeFrame.empty() ? "" : ", ") + g_seeFrameLast +
                                  (g_seeFrameRun > 1 ? " x" + std::to_string(g_seeFrameRun) : "");
                g_seeFrameLast = frameRun;
                g_seeFrameRun = 0;
            }
            g_seeFrameRun++;
        }
        if (dres) {
            DepthUse* u = nullptr;
            for (DepthUse& e : g_seeDepths) if (e.res == dres) { u = &e; break; }
            if (!u && g_seeDepths.size() < 64) {
                g_seeDepths.push_back(DepthUse());
                u = &g_seeDepths.back();
                u->res = dres;
                u->w = td.Width;
                u->h = td.Height;
                u->samples = td.SampleDesc.Count;
                u->format = (int)td.Format;
                u->flags = dd.Flags;
            }
            if (u) {
                u->draws++;
                if (st->deferred) u->deferredDraws++;
                const std::string runName = std::string(own ? "*" : "") + (name ? name : "?");
                if (runName != u->seqLast) {
                    SeqFlush(*u);
                    u->seqLast = runName;
                }
                u->seqRun++;
                if (name && u->first.size() < 160 && u->first.find(name) == std::string::npos)
                    u->first += std::string(u->first.empty() ? "" : ", ") + name;
                if (own && dd.Flags) {
                    // Our see-through draw (on the read-only view): what comes after it starts again.
                    u->ownAt = u->draws;
                    u->after.clear();
                } else if (u->ownAt >= 0 && name && u->after.size() < 300 && u->after.find(name) == std::string::npos) {
                    u->after += std::string(u->after.empty() ? "" : ", ") + name;
                }
            }
        }
        t_seeIndex = -1;
        if (own && g_seeDraws.size() < 40) {
            D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
            UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            ctx->RSGetViewports(&nvp, vp);
            char b[512];
            snprintf(b, sizeof(b), "%s on a %s context: depth %s (view flags %u), target %s, viewport %g %g %g x %g, "
                     "depth %g to %g", name, st->deferred ? "DEFERRED" : "immediate", ViewText(dsv).c_str(), dd.Flags,
                     ViewText(rtv).c_str(), nvp ? vp[0].TopLeftX : 0.0f, nvp ? vp[0].TopLeftY : 0.0f,
                     nvp ? vp[0].Width : 0.0f, nvp ? vp[0].Height : 0.0f, nvp ? vp[0].MinDepth : 0.0f,
                     nvp ? vp[0].MaxDepth : 0.0f);
            g_seeDraws.push_back(b);
            t_seeIndex = (int)g_seeDraws.size() - 1;
            g_seeHadOwn = true;
        }
    }
    if (dres) dres->Release();
    if (dsv) dsv->Release();
    if (rtv) rtv->Release();
}

// The scene depth given to our last see-through draw (held), written out every 3 s with the census.
ID3D11ShaderResourceView* g_seeDepthGiven = nullptr;

void SeeThroughKeepDepth(ID3D11ShaderResourceView* depth) {
    std::lock_guard<std::mutex> lk(g_seeMutex);
    if (g_seeDepthGiven == depth) return;
    if (g_seeDepthGiven) g_seeDepthGiven->Release();
    g_seeDepthGiven = depth;
    if (depth) depth->AddRef();
}

// The depth given, as it is on the card, into s2fm_seethrough_depth.pfm (one float a texel, rows bottom up).
void SeeThroughDumpDepth() {
    if (!g_seeDepthGiven) return;
    ID3D11Resource* res = nullptr;
    g_seeDepthGiven->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    if (res) res->Release();
    if (!tex) return;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    tex->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    if (dev) dev->GetImmediateContext(&ctx);
    if (d.Format == DXGI_FORMAT_R32_FLOAT && d.SampleDesc.Count == 1 && ctx) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
            const bool pass = t_CorePassThrough;
            t_CorePassThrough = true;
            ctx->CopyResource(st, tex);
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                FILE* f = nullptr;
                if (_wfopen_s(&f, CorePath(L"s2fm_seethrough_depth.pfm").c_str(), L"wb") == 0 && f) {
                    fprintf(f, "Pf\n%u %u\n-1.0\n", d.Width, d.Height);
                    for (UINT y = d.Height; y-- > 0;)
                        fwrite((const uint8_t*)m.pData + (size_t)y * m.RowPitch, 4, d.Width, f);
                    fclose(f);
                }
                ctx->Unmap(st, 0);
            }
            t_CorePassThrough = pass;
            st->Release();
        }
    } else {
        Log("[SEETHROUGH] the depth given is format %d, %u samples: not written\n", (int)d.Format, d.SampleDesc.Count);
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    tex->Release();
}

// What the feature did with the draw just noted.
void SeeThroughNote(const char* what) {
    if (t_seeIndex < 0) return;
    std::lock_guard<std::mutex> lk(g_seeMutex);
    if (t_seeIndex < (int)g_seeDraws.size()) g_seeDraws[t_seeIndex] += std::string(" | ") + what;
}

// At a present: every 3 s, the frame's census when a draw of our own was in it; the lists start again.
void SeeThroughPresent() {
    std::lock_guard<std::mutex> lk(g_seeMutex);
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (g_seeHadOwn && now - last >= 3000) {
        last = now;
        Log("[SEETHROUGH] a frame: %d depth targets, %d draws of our own shaders\n", (int)g_seeDepths.size(),
            (int)g_seeDraws.size());
        for (DepthUse& u : g_seeDepths) {
            Log("[SEETHROUGH]   depth %p %ux%u fmt %d x%u view flags %u: %d draws (%d deferred), first: %s\n",
                (void*)u.res, u.w, u.h, u.format, u.samples, u.flags, u.draws, u.deferredDraws, u.first.c_str());
            if (u.ownAt >= 0)
                Log("[SEETHROUGH]     our see-through draw was its draw %d of %d; after it: %s\n", u.ownAt, u.draws,
                    u.after.empty() ? "nothing" : u.after.c_str());
            SeqFlush(u);
            if (u.seq.find('*') != std::string::npos)
                Log("[SEETHROUGH]     in order: %s\n", u.seq.c_str());
        }
        for (const std::string& d : g_seeDraws) Log("[SEETHROUGH]   draw %s\n", d.c_str());
        if (g_seeFrameRun > 0)
            g_seeFrame += (g_seeFrame.empty() ? "" : ", ") + g_seeFrameLast +
                          (g_seeFrameRun > 1 ? " x" + std::to_string(g_seeFrameRun) : "");
        Log("[SEETHROUGH]   every draw with a colour target, in order: %s\n", g_seeFrame.c_str());
        SeeThroughDumpDepth();
    }
    g_seeDepths.clear();
    g_seeDraws.clear();
    g_seeFrame.clear();
    g_seeFrameLast.clear();
    g_seeFrameRun = 0;
    g_seeHadOwn = false;
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
    int depthSlot = -1;                         // a surface's g_tSceneDepth, and what the engine had there
    ID3D11ShaderResourceView* depthEngine = nullptr;
    int fxSlot = -1;                            // a water's g_tWaterEffects (water_fx.cpp), and the engine's there
    ID3D11ShaderResourceView* fxEngine = nullptr;
    int fogSlot = -1;                           // g_tLightFog (the lights' fog weights), and the engine's there
    ID3D11ShaderResourceView* fogEngine = nullptr;
    ID3D11ShaderResourceView* sceneDepth = nullptr;   // the depth it was given, for its effects after the draw
    ID3D11ShaderResourceView* copy = nullptr;         // and the frame copy it was given, which its splashes bend
    uint64_t copyGen = 0;
    ID3D11Buffer* engineTaps = nullptr;
    bool tapsSwapped = false;
    bool probe = false;
    bool own = false;                           // a shader compiler's own surface, not a particle: see AfterDraw
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
// A texture of the other kind of target met last, kept aside: S2FM's viewport (16 bit float, 8 samples) and the
// Material Editor's preview (8 bit, 4 samples) draw by turns, and one texture made over at every turn stalled the frame.
// Swapped back in when its kind comes round again (Stash), instead of made over.
struct Kept {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
Kept g_copyKept, g_resolveKept, g_mipKept;

// The texture in use and the one kept aside change places; true when the one now in use is of the size and format
// wanted (else the caller makes it over).
bool Stash(Kept& kept, ID3D11Texture2D*& tex, ID3D11ShaderResourceView** srv, UINT& w, UINT& h, DXGI_FORMAT& format,
           UINT wantW, UINT wantH, DXGI_FORMAT want) {
    std::swap(kept.tex, tex);
    if (srv) std::swap(kept.srv, *srv);
    std::swap(kept.w, w);
    std::swap(kept.h, h);
    std::swap(kept.format, format);
    return tex && w == wantW && h == wantH && format == want;
}
std::atomic<int> g_draws{0}, g_copiesMade{0}, g_refused{0}, g_surfaceDraws{0};
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
        s_of = nullptr;
        fresh = true;
        if (!Stash(g_copyKept, g_copyTex, &g_copySrv, g_copyW, g_copyH, g_copyFormat, copyW, copyH, rd.Format)) {
            if (g_copySrv) { g_copySrv->Release(); g_copySrv = nullptr; }
            if (g_copyTex) { g_copyTex->Release(); g_copyTex = nullptr; }
            g_copyW = g_copyH = 0;
            g_copyFormat = DXGI_FORMAT_UNKNOWN;
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
        }
    }
    ID3D11Resource* from = res;
    UINT fromSub = D3D11CalcSubresource(mip, 0, td.MipLevels);
    if (td.SampleDesc.Count > 1) {
        if (!g_resolveTex || w != g_resolveW || h != g_resolveH || rd.Format != g_resolveFormat) {
            fresh = true;
            if (!Stash(g_resolveKept, g_resolveTex, nullptr, g_resolveW, g_resolveH, g_resolveFormat, w, h, rd.Format)) {
                if (g_resolveTex) { g_resolveTex->Release(); g_resolveTex = nullptr; }
                g_resolveTex = CopyTexture(ctx, w, h, rd.Format, nullptr);
                g_resolveW = g_resolveTex ? w : 0;
                g_resolveH = g_resolveTex ? h : 0;
                g_resolveFormat = g_resolveTex ? rd.Format : DXGI_FORMAT_UNKNOWN;
            }
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

// The lights' fog weights and sizes for a shader of our own that declares g_tLightFog (the weather; and every lit one,
// for its lights' sizes): 128x3 texels of four floats.
//   row 0: texel 0 the count, then each projected light's world position and weight (sfm_doc.h, SfmLightFogs), read
//          from the document a few times a second; the shader finds its projected lights here by position.
//   row 1: the weight of each light of the engine's light list the draw has (x, by its index), and in texel 0's y 1
//          when the row is there for this draw. Made at each weather draw from the list as the engine wrote it (its
//          positions, copied at Map) and the shot's point, spot and orthographic lights where S2FM hands them to the
//          engine. Its z is that light's size (S2FM's radius), for soft highlights.
//   row 2: each projected light's size, in the texel row 0 has its position.
// A light not found, or no table, counts whole and as a point.
// One table for each device context: a deferred context's draws run later, when the engine plays its command list,
// in an order of its own, so the rows a draw was given have to be in a texture no other context writes. Each is
// written whole (no box: a deferred context without the driver's own command lists misplaces a boxed update).
struct LightTable {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    float texels[3][128 * 4];
    bool made = false;
};
// The tables, the document's lights and the log below: our shaders draw on the engine's worker threads too.
std::mutex g_tableMutex;
std::unordered_map<ID3D11DeviceContext*, LightTable> g_tables;
float g_sharedRows[2][128 * 4] = {};    // rows 0 and 2, the same for every draw
std::string g_fogSaid;
SfmLightFog g_fogLights[127];
int g_fogLightCount = 0;

// The engine's light lists (PerViewLightingConstantBufferVr_t, 37616 bytes: the count at 0, each light's position,
// less the high precision offset, at 2144 on), as the engine last wrote each one, by buffer (not held: a name only).
const UINT kLightListBytes = 37616, kLightListPositions = 2144;
struct LightList {
    int count = 0;
    float position[128][4];
};
std::mutex g_listMutex;
std::unordered_map<ID3D11Buffer*, LightList> g_lists;
thread_local ID3D11Buffer* t_mappedList = nullptr;
thread_local const uint8_t* t_mappedData = nullptr;
std::atomic<int> g_listWrites{0};
// Matching, counted for the log every 3 s: weather draws with a list, list lights found among S2FM's, not found.
int g_listDraws = 0, g_listFound = 0, g_listMissed = 0;

void Mapped(ID3D11DeviceContext*, ID3D11Resource* res, void* data) {
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC d = {};
    static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
    if (d.ByteWidth != kLightListBytes) return;
    t_mappedList = static_cast<ID3D11Buffer*>(res);
    t_mappedData = (const uint8_t*)data;
}

void KeepList(ID3D11Buffer* buffer, const uint8_t* data) {
    LightList l;
    int count = 0;
    memcpy(&count, data, 4);
    l.count = count < 0 ? 0 : count > 128 ? 128 : count;
    memcpy(l.position, data + kLightListPositions, (size_t)l.count * 16);
    {
        std::lock_guard<std::mutex> lk(g_listMutex);
        g_lists[buffer] = l;
    }
    g_listWrites.fetch_add(1, std::memory_order_relaxed);
}

// A whole light list written through UpdateSubresource.
void Updating(ID3D11DeviceContext*, ID3D11Resource* res, const D3D11_BOX* box, const void* data) {
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC d = {};
    static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
    if (d.ByteWidth != kLightListBytes) return;
    if (box && (box->left != 0 || box->right < kLightListPositions + 128 * 16)) return;
    KeepList(static_cast<ID3D11Buffer*>(res), (const uint8_t*)data);
}

void Unmapping(ID3D11DeviceContext*, ID3D11Resource* res) {
    if (!t_mappedList || res != t_mappedList) return;
    KeepList(t_mappedList, t_mappedData);
    t_mappedList = nullptr;
}

ID3D11ShaderResourceView* LightFogTable(ID3D11DeviceContext* ctx, const ContextState* st) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    LightTable& table = g_tables[ctx];
    if (!table.tex) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = 128;
        d.Height = 3;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        static const float zero[128 * 3 * 4] = {};
        D3D11_SUBRESOURCE_DATA init = {zero, 128 * 4 * sizeof(float), 0};
        if (!dev || FAILED(dev->CreateTexture2D(&d, &init, &table.tex)) ||
            FAILED(dev->CreateShaderResourceView(table.tex, nullptr, &table.srv))) {
            if (table.tex) { table.tex->Release(); table.tex = nullptr; }
        }
        if (dev) dev->Release();
        if (!table.tex) return nullptr;
    }
    if (last == 0 || now - last >= 250) {
        last = now;
        const int n = g_noLightFog.load(std::memory_order_relaxed) ? 0 : SfmLightFogs(g_fogLights, 127);
        g_fogLightCount = n > 0 ? n : 0;
        float* row = g_sharedRows[0];
        float* sizes = g_sharedRows[1];
        memset(g_sharedRows, 0, sizeof(g_sharedRows));
        int projected = 0;
        std::string said = n < 0 ? "not read" : "";
        for (int k = 0; k < g_fogLightCount; ++k) {
            const SfmLightFog& l = g_fogLights[k];
            if (l.projected) {
                ++projected;
                for (int c = 0; c < 3; ++c) row[projected * 4 + c] = l.position[c];
                row[projected * 4 + 3] = l.weight;
                sizes[projected * 4] = l.radius;
            }
            char b[128];
            snprintf(b, sizeof(b), l.radius > 0.0f ? "%s%s %.2f%s size %.1f" : "%s%s %.2f%s", k ? ", " : "", l.name,
                     l.weight, l.projected || l.fromEngine ? "" : " (document position)", l.radius);
            said += b;
        }
        row[0] = (float)projected;
        if (said != g_fogSaid) {
            g_fogSaid = said;
            Log("[REFRACT] the lights' fog weights for the weather: %s\n", said.empty() ? "no lights" : said.c_str());
        }
    }
    // Row 1, for this draw's light list.
    ID3D11Buffer* list = nullptr;
    float offset[4] = {};
    bool haveOffset = false;
    for (int k = 0; k < 14; ++k) {
        if (!st->psCb[k]) continue;
        D3D11_BUFFER_DESC d = {};
        st->psCb[k]->GetDesc(&d);
        if (d.ByteWidth == kLightListBytes) {
            list = st->psCb[k];
            static bool said = false;
            if (!said) {
                said = true;
                std::string sizes;
                for (int j = 0; j < 14; ++j) {
                    if (!st->psCb[j]) continue;
                    D3D11_BUFFER_DESC e = {};
                    st->psCb[j]->GetDesc(&e);
                    UINT total = 0;
                    const bool copy = CoreCbRead(st->psCb[j], 0, nullptr, 0, &total);
                    sizes += " " + std::to_string(j) + ":" + std::to_string(e.ByteWidth) + (copy ? "c" : "");
                }
                Log("[REFRACT] the weather's light list buffer: slot %d, usage %d, CPU access 0x%x, bind 0x%x; the "
                    "pixel stage's buffers (c: a CPU copy):%s\n", k, (int)d.Usage, d.CPUAccessFlags, d.BindFlags,
                    sizes.c_str());
            }
        }
    }
    // The list's positions are less the high precision lighting offset, which is the camera's position: PerView's
    // world to camera offset turned round (measured 28 Oct 2026, the two the same to the float).
    {
        float pv[204];
        if (CorePerView(st, pv)) {
            for (int c = 0; c < 3; ++c) offset[c] = -pv[PV_WORLD_TO_CAM_OFFSET + c];
            haveOffset = true;
        }
    }
    LightList l;
    bool haveList = false;
    if (list) {
        std::lock_guard<std::mutex> lk(g_listMutex);
        auto it = g_lists.find(list);
        if (it != g_lists.end()) {
            l = it->second;
            haveList = true;
        }
    }
    float texels[3][128 * 4];
    memcpy(texels[0], g_sharedRows[0], sizeof(texels[0]));
    memcpy(texels[2], g_sharedRows[1], sizeof(texels[2]));
    float* row1 = texels[1];
    memset(row1, 0, sizeof(texels[1]));
    std::string matched;
    if (haveList && haveOffset) {
        // Where each light is now (a light moving with an animation is not where the last read of the document left
        // it, and would be matched by none: counted whole now and then).
        for (int k = 0; k < g_fogLightCount; ++k)
            SfmLightNow(g_fogLights[k].engine, g_fogLights[k].position, g_fogLights[k].forward);
        row1[1] = 1.0f;
        ++g_listDraws;
        for (int i = 0; i < l.count; ++i) {
            float weight = 1.0f, nearest = 32.0f * 32.0f;
            const SfmLightFog* found = nullptr;
            for (int k = 0; k < g_fogLightCount; ++k) {
                const SfmLightFog& f = g_fogLights[k];
                if (f.projected) continue;
                float d2 = 0.0f;
                float v[3];
                for (int c = 0; c < 3; ++c) {
                    v[c] = l.position[i][c] + offset[c] - f.position[c];
                    d2 += v[c] * v[c];
                }
                // An orthographic light is in the list far back along its forward (a million units, measured): how
                // far it is from that line, against how far back it is.
                if (f.ortho) {
                    const float back = -(v[0] * f.forward[0] + v[1] * f.forward[1] + v[2] * f.forward[2]);
                    if (back < 1000.0f) continue;
                    float side2 = 0.0f;
                    for (int c = 0; c < 3; ++c) {
                        const float e = v[c] + f.forward[c] * back;
                        side2 += e * e;
                    }
                    if (side2 > (0.001f * back) * (0.001f * back) + 16.0f * 16.0f) continue;
                    d2 = 0.0f;
                }
                if (d2 < nearest) {
                    nearest = d2;
                    weight = f.weight;
                    found = &f;
                }
            }
            row1[i * 4] = weight;
            row1[i * 4 + 2] = found ? found->radius : 0.0f;
            ++(found ? g_listFound : g_listMissed);
            char b[160];
            if (found) {
                snprintf(b, sizeof(b), found->radius > 0.0f ? " %d=%s %.2f size %.1f" : " %d=%s %.2f", i, found->name,
                         weight, found->radius);
            } else {
                snprintf(b, sizeof(b), " %d at %.0f %.0f %.0f matches none", i, l.position[i][0] + offset[0],
                         l.position[i][1] + offset[1], l.position[i][2] + offset[2]);
            }
            matched += b;
        }
    }
    // Every lit surface of ours reads the table now, a list of its own for each view: said at most every 3 s, a
    // change inside those 3 s kept until then (or the last state of the list would never be said).
    static std::string s_said, s_unsaid;
    static ULONGLONG s_saidAt = 0;
    if (!table.made || memcmp(texels, table.texels, sizeof(texels)) != 0) {
        ctx->UpdateSubresource(table.tex, 0, nullptr, texels, sizeof(texels[0]), sizeof(texels));
        memcpy(table.texels, texels, sizeof(texels));
        table.made = true;
        char head[200];
        snprintf(head, sizeof(head), "%s, %d light(s) in the list (%d writes of lists seen; a %s context, %d with a table)",
                 !list ? "no light list bound" : !haveList ? "the list was never seen written"
                 : !haveOffset ? "no lighting offset" : "matched", haveList ? l.count : 0,
                 g_listWrites.load(std::memory_order_relaxed), st->deferred ? "deferred" : "immediate",
                 (int)g_tables.size());
        const std::string said = std::string(head) + matched;
        s_unsaid = said != s_said ? said : std::string();
    }
    if (!s_unsaid.empty() && now - s_saidAt >= 3000) {
        s_said = s_unsaid;
        s_unsaid.clear();
        s_saidAt = now;
        Log("[REFRACT] the weather's light list: %s\n", s_said.c_str());
    }
    return table.srv;
}

// For a reader that is not the spritecard (a refracting glass shader of a shader compiler's own): the picture alone,
// at its own size, with its whole mip chain, so a rough surface can read the scene behind it blurred by taking a
// smaller mip. Made again for each new copy (g_copyGen); null when the format cannot make its own mips.
ID3D11Texture2D* g_mipTex = nullptr;
ID3D11ShaderResourceView* g_mipSrv = nullptr;
UINT g_mipW = 0, g_mipH = 0;
DXGI_FORMAT g_mipFormat = DXGI_FORMAT_UNKNOWN;
uint64_t g_mipGen = 0;

ID3D11ShaderResourceView* MippedCopy(ID3D11DeviceContext* ctx, UINT w, UINT h) {
    if (!g_copyTex || !w || !h) return nullptr;
    if ((!g_mipTex || w != g_mipW || h != g_mipH || g_copyFormat != g_mipFormat)
        && Stash(g_mipKept, g_mipTex, &g_mipSrv, g_mipW, g_mipH, g_mipFormat, w, h, g_copyFormat))
        g_mipGen = 0;    // the kept one, made for an older copy
    if (!g_mipTex || w != g_mipW || h != g_mipH || g_copyFormat != g_mipFormat) {
        if (g_mipSrv) { g_mipSrv->Release(); g_mipSrv = nullptr; }
        if (g_mipTex) { g_mipTex->Release(); g_mipTex = nullptr; }
        g_mipW = g_mipH = 0;
        g_mipFormat = DXGI_FORMAT_UNKNOWN;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        UINT support = 0;
        if (dev && SUCCEEDED(dev->CheckFormatSupport(g_copyFormat, &support))
            && (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) {
            D3D11_TEXTURE2D_DESC cd = {};
            cd.Width = w;
            cd.Height = h;
            cd.MipLevels = 0;
            cd.ArraySize = 1;
            cd.Format = g_copyFormat;
            cd.SampleDesc.Count = 1;
            cd.Usage = D3D11_USAGE_DEFAULT;
            cd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            cd.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (SUCCEEDED(dev->CreateTexture2D(&cd, nullptr, &g_mipTex))
                && FAILED(dev->CreateShaderResourceView(g_mipTex, nullptr, &g_mipSrv))) {
                g_mipTex->Release();
                g_mipTex = nullptr;
            }
        }
        if (dev) dev->Release();
        if (!g_mipTex) {
            Log("[REFRACT] a mipped copy could not be made at %ux%u, format %d: the plain copy is used\n", w, h,
                (int)g_copyFormat);
            return nullptr;
        }
        g_mipW = w;
        g_mipH = h;
        g_mipFormat = g_copyFormat;
        g_mipGen = 0;
        Log("[REFRACT] the mipped frame copy for refracting surfaces is made, %ux%u, format %d\n", w, h,
            (int)g_copyFormat);
    }
    if (g_mipGen != g_copyGen) {
        const D3D11_BOX box = { 0, 0, 0, w, h, 1 };
        ctx->CopySubresourceRegion(g_mipTex, 0, 0, 0, 0, g_copyTex, 0, &box);
        ctx->GenerateMips(g_mipSrv);
        g_mipGen = g_copyGen;
    }
    return g_mipSrv;
}

// The passes after the scene (all of it drawn, the see-through surfaces too): the first draw of one is the frame's end
// for the wetness when no water drew it.
bool AfterScene(const char* name) {
    static const char* const kAfter[] = { "downsample_depth", "gaussian_bloom_blur", "post_process", "sfm_accumulation",
                                          "copytexture" };
    if (!name || !*name) return false;
    for (const char* a : kAfter) if (!strcmp(name, a)) return true;
    return false;
}

// The wetness at the frame's end: the scene's picture copied and its depth, and drawn into the scene.
void WetnessAtFrameEnd(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* copy = nullptr;
    ID3D11ShaderResourceView* depth = nullptr;
    if (WaterFxWetPending()) {
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11Texture2D* depthTex = nullptr;
        WaterFxSceneTargets(&rtv, &depthTex);
        UINT samples = 0, pictureW = 0, pictureH = 0;
        // At the copy's size as the refracting draws have it, so it is not made again for them.
        copy = CopyOf(ctx, rtv, true, g_copyW, g_copyH, &samples, &pictureW, &pictureH);
        if (copy) {
            if (ID3D11ShaderResourceView* mipped = MippedCopy(ctx, pictureW, pictureH)) copy = mipped;
            depth = SceneDepthOf(ctx, depthTex, g_copyGen, 2);
        }
    }
    WaterFxFrameEnd(ctx, copy, g_copyGen, depth);
}

void BeforeDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    // The scene noted at its draws, the passes after it not (one of them is the frame's end), nor the sky.
    const char* name = st->psLayout >= 0 ? ShaderDbName(st->psLayout) : nullptr;
    if (g_seeThrough.load(std::memory_order_relaxed)) SeeThroughDraw(ctx, st, name);
    if (!st->deferred) WeatherCbDump(st, name);
    if (AfterScene(name)) {
        if (WaterFxTracing())
            Log("[WATER] trace: a pass after the scene: %s, the scene noted %d, %ux%u, deferred %d\n", name,
                (int)WaterFxSceneNoted(), st->rtW, st->rtH, (int)st->deferred);
        if (WaterFxSceneNoted() && !st->deferred) WetnessAtFrameEnd(ctx);
    } else if (!name || strcmp(name, "sky") != 0) {
        WaterFxSceneDraw(ctx, st);
    }
    static thread_local bool t_sprite = false;
    static thread_local int t_depthSlot = -1;
    static thread_local int t_depthOnlySlot = -1;
    static thread_local int t_fxSlot = -1;
    static thread_local int t_fogSlot = -1;
    static thread_local int t_tableOnlySlot = -1;
    if (st->ps != t_ps || st->psLayout != t_layout) {
        t_ps = st->ps;
        t_layout = st->psLayout;
        t_slot = TextureSlot(st, "g_tFrameBufferCopyTexture");
        const char* name = t_slot >= 0 ? ShaderDbName(st->psLayout) : nullptr;
        t_sprite = name && !strcmp(name, "spritecard");
        t_depthSlot = t_slot >= 0 && !t_sprite ? TextureSlot(st, "g_tSceneDepth") : -1;
        t_fxSlot = t_depthSlot >= 0 ? TextureSlot(st, "g_tWaterEffects") : -1;
        t_fogSlot = t_slot >= 0 && !t_sprite ? TextureSlot(st, "g_tLightFog") : -1;
        // A shader of a shader compiler's own that reads the scene's depth without the picture (a depth effect, a
        // test view) gets the depth alone.
        t_depthOnlySlot = t_slot < 0 && st->psLayout >= 0 && OwnShader(ShaderDbName(st->psLayout))
                              ? TextureSlot(st, "g_tSceneDepth") : -1;
        // One that lights by the engine's lights without the picture (s2vfx_lit) gets the light table alone: its
        // lights' sizes.
        t_tableOnlySlot = t_slot < 0 && st->psLayout >= 0 && OwnShader(ShaderDbName(st->psLayout))
                              ? TextureSlot(st, "g_tLightFog") : -1;
        {
            // Once a shader: whether one of ours got its light table, and where.
            static std::mutex m;
            static std::unordered_map<std::string, int> said;
            const char* own = st->psLayout >= 0 ? ShaderDbName(st->psLayout) : nullptr;
            if (own && t_slot < 0 && OwnShader(own)) {
                std::lock_guard<std::mutex> lk(m);
                if (said.emplace(own, t_tableOnlySlot).second)
                    Log("[REFRACT] %s: the light table %s (slot %d)\n", own,
                        t_tableOnlySlot >= 0 ? "given" : "not declared", t_tableOnlySlot);
            }
        }
    }
    const int slot = t_slot;
    if (slot < 0 || slot >= 32) {
        t_stale = true;
        // On a deferred context too (the engine's opaque passes are mostly recorded on its worker threads): each
        // context has a table of its own.
        if (t_tableOnlySlot >= 0 && t_tableOnlySlot < 32) {
            if (ID3D11ShaderResourceView* table = LightFogTable(ctx, st)) {
                t_swap.fogSlot = t_tableOnlySlot;
                t_swap.fogEngine = st->psSrv[t_tableOnlySlot];
                if (t_swap.fogEngine) t_swap.fogEngine->AddRef();
                ctx->PSSetShaderResources((UINT)t_tableOnlySlot, 1, &table);
            }
        }
        if (t_depthOnlySlot >= 0 && t_depthOnlySlot < 32 && !st->deferred && !g_off.load(std::memory_order_relaxed)) {
            // Made again at each present: there is no picture copy here to time it by. Its own count, apart from the
            // copies' (the top bit), so the two kinds of readers in one frame each get theirs.
            const uint64_t gen = (1ull << 63) | g_presents.load(std::memory_order_relaxed);
            if (ID3D11ShaderResourceView* depth = SceneDepthFor(ctx, gen)) {
                t_swap.depthSlot = t_depthOnlySlot;
                t_swap.depthEngine = st->psSrv[t_depthOnlySlot];
                if (t_swap.depthEngine) t_swap.depthEngine->AddRef();
                ctx->PSSetShaderResources((UINT)t_depthOnlySlot, 1, &depth);
                SeeThroughNote("the scene depth alone given");
                return;
            }
        }
        SeeThroughNote("no frame copy slot found");
        return;
    }
    if (g_off.load(std::memory_order_relaxed)) { SeeThroughNote("norefract"); return; }
    if (st->deferred) {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        SeeThroughNote("REFUSED: a deferred context");
        return;
    }
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) { g_refused.fetch_add(1, std::memory_order_relaxed); SeeThroughNote("REFUSED: no target"); return; }
    // The copy's size as the shader is told it (cb1[42].xy, its inverse).
    float fc[4] = {};
    CoreCbRead(st->psCb[1], 672, fc, sizeof(fc));
    const UINT copyW = fc[0] > 0.0f ? (UINT)lroundf(1.0f / fc[0]) : 0;
    const UINT copyH = fc[1] > 0.0f ? (UINT)lroundf(1.0f / fc[1]) : 0;
    UINT samples = 0, pictureW = 0, pictureH = 0;
    ID3D11ShaderResourceView* copy = CopyOf(ctx, rtv, t_stale, copyW, copyH, &samples, &pictureW, &pictureH);
    rtv->Release();
    if (!copy) { g_refused.fetch_add(1, std::memory_order_relaxed); SeeThroughNote("REFUSED: no copy"); return; }
    t_stale = false;
    if (!t_sprite) {
        // Not a particle: the picture with its mips, and no blur taps (its cb5 is not the spritecard's FilterTaps_t).
        ID3D11ShaderResourceView* mipped = MippedCopy(ctx, pictureW, pictureH);
        ID3D11ShaderResourceView* engine = st->psSrv[slot];
        if (engine) engine->AddRef();
        t_swap.slot = slot;
        t_swap.engine = engine;
        t_swap.own = true;
        ID3D11ShaderResourceView* bind = mipped ? mipped : copy;
        ctx->PSSetShaderResources((UINT)slot, 1, &bind);
        if (t_fogSlot >= 0 && t_fogSlot < 32) {
            if (ID3D11ShaderResourceView* fog = LightFogTable(ctx, st)) {
                t_swap.fogSlot = t_fogSlot;
                t_swap.fogEngine = st->psSrv[t_fogSlot];
                if (t_swap.fogEngine) t_swap.fogEngine->AddRef();
                ctx->PSSetShaderResources((UINT)t_fogSlot, 1, &fog);
            }
        }
        // A surface that reads the scene's depth (a water) gets it, one sample, made with the copy.
        if (g_seeThrough.load(std::memory_order_relaxed)) {
            char b[96];
            snprintf(b, sizeof(b), "copy given (gen %llu), depth slot %d", (unsigned long long)g_copyGen, t_depthSlot);
            SeeThroughNote(b);
        }
        if (t_depthSlot >= 0 && t_depthSlot < 32) {
            if (ID3D11ShaderResourceView* depth = SceneDepthFor(ctx, g_copyGen)) {
                SeeThroughNote("scene depth given");
                if (g_seeThrough.load(std::memory_order_relaxed) && name && !strncmp(name, "s2vfx_weather", 13))
                    SeeThroughKeepDepth(depth);
                t_swap.depthSlot = t_depthSlot;
                t_swap.depthEngine = st->psSrv[t_depthSlot];
                if (t_swap.depthEngine) t_swap.depthEngine->AddRef();
                ctx->PSSetShaderResources((UINT)t_depthSlot, 1, &depth);
                // A water: the map of what touches it, worked out after the draw (water_fx.cpp).
                if (t_fxSlot >= 0 && t_fxSlot < 32) {
                    if (ID3D11ShaderResourceView* fx = WaterFxBefore(ctx, st)) {
                        t_swap.fxSlot = t_fxSlot;
                        t_swap.fxEngine = st->psSrv[t_fxSlot];
                        if (t_swap.fxEngine) t_swap.fxEngine->AddRef();
                        t_swap.sceneDepth = depth;
                        t_swap.copy = bind;
                        t_swap.copyGen = g_copyGen;
                        ctx->PSSetShaderResources((UINT)t_fxSlot, 1, &fx);
                    }
                }
            }
        }
        g_draws.fetch_add(1, std::memory_order_relaxed);
        g_surfaceDraws.fetch_add(1, std::memory_order_relaxed);
        return;
    }
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

void AfterDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    WaterCarryDraw(ctx, st);
    if (t_swap.slot < 0) {
        // The scene depth alone, or the light table alone (no picture copy): what the engine had there goes back.
        if (t_swap.depthSlot >= 0) {
            ctx->PSSetShaderResources((UINT)t_swap.depthSlot, 1, &t_swap.depthEngine);
            if (t_swap.depthEngine) t_swap.depthEngine->Release();
            t_swap.depthEngine = nullptr;
            t_swap.depthSlot = -1;
        }
        if (t_swap.fogSlot >= 0) {
            ctx->PSSetShaderResources((UINT)t_swap.fogSlot, 1, &t_swap.fogEngine);
            if (t_swap.fogEngine) t_swap.fogEngine->Release();
            t_swap.fogEngine = nullptr;
            t_swap.fogSlot = -1;
        }
        return;
    }
    if (t_swap.fxSlot >= 0) {
        WaterFxAfter(ctx, st, t_swap.sceneDepth, t_swap.copy, t_swap.copyGen);
        t_swap.copy = nullptr;
        ctx->PSSetShaderResources((UINT)t_swap.fxSlot, 1, &t_swap.fxEngine);
        if (t_swap.fxEngine) t_swap.fxEngine->Release();
        t_swap.fxEngine = nullptr;
        t_swap.fxSlot = -1;
        t_swap.sceneDepth = nullptr;
    }
    if (t_swap.depthSlot >= 0) {
        ctx->PSSetShaderResources((UINT)t_swap.depthSlot, 1, &t_swap.depthEngine);
        if (t_swap.depthEngine) t_swap.depthEngine->Release();
        t_swap.depthEngine = nullptr;
        t_swap.depthSlot = -1;
    }
    if (t_swap.fogSlot >= 0) {
        ctx->PSSetShaderResources((UINT)t_swap.fogSlot, 1, &t_swap.fogEngine);
        if (t_swap.fogEngine) t_swap.fogEngine->Release();
        t_swap.fogEngine = nullptr;
        t_swap.fogSlot = -1;
    }
    ctx->PSSetShaderResources((UINT)t_swap.slot, 1, &t_swap.engine);
    if (t_swap.engine) t_swap.engine->Release();
    t_swap.engine = nullptr;
    t_swap.slot = -1;
    // One of our own see-through surfaces (a glass, a water) is in the picture now: the next one to read the copy,
    // which may be in front of it, gets a copy made again with it in, or the one behind would not show through it.
    // The particles keep sharing theirs (their draws come many to a frame, and one does not stand behind another).
    if (t_swap.own && !g_shareCopy.load(std::memory_order_relaxed)) t_stale = true;
    t_swap.own = false;
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

void Present(IDXGISwapChain*) {
    {
        static ULONGLONG lastList = 0;
        const ULONGLONG nowList = GetTickCount64();
        if (nowList - lastList >= 3000) {
            lastList = nowList;
            if (g_listDraws)
                Log("[REFRACT] the weather's light list in 3 s: %d draws, %d lights found, %d not found\n", g_listDraws,
                    g_listFound, g_listMissed);
            g_listDraws = g_listFound = g_listMissed = 0;
        }
    }
    g_presents.fetch_add(1, std::memory_order_relaxed);
    ReadSwitch();
    if (g_seeThrough.load(std::memory_order_relaxed)) SeeThroughPresent();
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 10000) {
        last = now;
        if (const int n = g_surfaceDraws.exchange(0))
            Log("[REFRACT] in 10 s: %d refracting surface draws, the copy %ux%u with mips\n", n, g_mipW, g_mipH);
    }
}

const CoreFeature kFeature = { "particle refraction", BeforeDraw, AfterDraw, nullptr, Present, Mapped, Unmapping, Updating };

}  // namespace

void RefractionRegister() {
    CoreRegister(&kFeature);
    CoreOnModuleLoad(RefractBlurHookIfLoaded);
}
