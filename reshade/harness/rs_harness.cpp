// The ReShade core's tests, without the game: its own D3D11 device, the core DLL loaded from beside it.
//     rs_harness.exe <test>       exit code 0 = pass
// Tests: version, invert, filter, none, clock, preset (lists which of the user's preset's effects compile).
// Each test runs in a fresh folder bin\run: the harness config (rs_test_reshade.ini) with RUNDIR filled in,
// its own preset, and the test effects, so the runtime never sees a file it may write back that matters.
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <d3dcompiler.h>

#include "core/rs_core_api.h"
#include "rs_stage_api.h"

namespace fs = std::filesystem;

static HMODULE g_core = nullptr;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static fs::path g_here, g_run, g_src;

template <typename T> static T Fn(const char* name) {
    T f = (T)GetProcAddress(g_core, name);
    if (!f) printf("FAIL: %s is not exported\n", name);
    return f;
}

static std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static void WriteText(const fs::path& p, const std::string& t) {
    std::ofstream f(p, std::ios::binary);
    f << t;
}

// A fresh run folder: the config with RUNDIR replaced, the preset given, the test effects.
static bool PrepareRun(const fs::path& preset, const std::string& extraGeneral = "") {
    std::error_code ec;
    fs::remove_all(g_run / "fx", ec);
    fs::create_directories(g_run / "fx", ec);
    fs::create_directories(g_run / "cache", ec);
    for (const auto& e : fs::directory_iterator(g_src / "fx")) fs::copy_file(e.path(), g_run / "fx" / e.path().filename(), fs::copy_options::overwrite_existing, ec);
    std::string ini = ReadText(g_src / "rs_test_reshade.ini");
    const std::string run = g_run.string();
    for (size_t at; (at = ini.find("RUNDIR")) != std::string::npos;) ini.replace(at, 6, run);
    if (!extraGeneral.empty()) {
        const size_t at = ini.find("[GENERAL]");
        ini.insert(at + 9, "\r\n" + extraGeneral);
    }
    WriteText(g_run / "ReShade.ini", ini);
    fs::copy_file(preset, g_run / "preset.ini", fs::copy_options::overwrite_existing, ec);
    return !ec;
}

static bool MakeDevice() {
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &want, 1,
                                 D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx))) {
        printf("FAIL: no D3D11 device\n");
        return false;
    }
    return true;
}

static const unsigned W = 256, H = 128;

static bool Init(const fs::path& preset, const char* definitions = nullptr, unsigned w = W, unsigned h = H,
                 unsigned waitMs = 20000) {
    if (!PrepareRun(preset)) { printf("FAIL: run folder\n"); return false; }
    auto init = Fn<RsCore_HarnessInit_Fn>("RsCore_HarnessInit");
    auto wait = Fn<RsCore_HarnessWaitLoaded_Fn>("RsCore_HarnessWaitLoaded");
    if (!init || !wait) return false;
    const std::string cfg = (g_run / "ReShade.ini").string();
    if (!init(g_dev, g_ctx, w, h, cfg.c_str(), definitions)) { printf("FAIL: the runtime did not start\n"); return false; }
    if (!wait(waitMs)) { printf("FAIL: effects still loading after %u ms\n", waitMs); return false; }
    return true;
}

static void Shutdown() {
    if (auto s = Fn<RsCore_HarnessShutdown_Fn>("RsCore_HarnessShutdown")) s();
}

static void PrintErrors() {
    auto e = Fn<RsCore_Errors_Fn>("RsCore_Errors");
    if (!e) return;
    std::vector<char> b(1 << 20);
    if (e(b.data(), (int)b.size()) > 0) printf("compiler said:\n%s\n", b.data());
}

static std::vector<float> Gradient() {
    std::vector<float> p((size_t)W * H * 4);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            float* o = &p[((size_t)y * W + x) * 4];
            o[0] = x / float(W - 1);
            o[1] = y / float(H - 1);
            o[2] = 0.25f;
            o[3] = 1.0f;
        }
    return p;
}

static bool Run(const std::vector<float>& in, std::vector<float>& out, const char* techniques, double seconds = 1.0,
                unsigned long long frame = 1, unsigned seed = 1) {
    auto up = Fn<RsCore_HarnessUpload_Fn>("RsCore_HarnessUpload");
    auto run = Fn<RsCore_HarnessRun_Fn>("RsCore_HarnessRun");
    auto down = Fn<RsCore_HarnessDownload_Fn>("RsCore_HarnessDownload");
    if (!up || !run || !down) return false;
    out.assign(in.size(), 0.0f);
    if (!up(in.data(), W, H)) { printf("FAIL: upload\n"); return false; }
    if (!run(seconds, frame, seed, techniques)) { printf("FAIL: run\n"); return false; }
    if (!down(out.data(), W, H)) { printf("FAIL: download\n"); return false; }
    return true;
}

// The largest difference between what came out and what was expected, over r, g, b.
template <typename F> static float WorstOff(const std::vector<float>& in, const std::vector<float>& out, F expect) {
    float worst = 0.0f;
    for (size_t i = 0; i < in.size(); i += 4)
        for (int c = 0; c < 3; ++c) {
            const float d = std::fabs(out[i + c] - expect(in[i + c], c));
            if (d > worst) worst = d;
        }
    return worst;
}

static fs::path TestPreset() { return g_src / "presets" / "rs_test.ini"; }

static int TestVersion() {
    auto v = Fn<RsCore_ApiVersion_Fn>("RsCore_ApiVersion");
    if (!v) return 1;
    printf("core API version %d\n", v());
    return v() == RS_CORE_API_VERSION ? 0 : 1;
}

static int TestInvert() {
    if (!Init(TestPreset())) { PrintErrors(); return 1; }
    const auto in = Gradient();
    std::vector<float> out;
    if (!Run(in, out, "RsTestInvert@rs_test.fx")) return 1;
    const float worst = WorstOff(in, out, [](float v, int) { return 1.0f - v; });
    printf("invert: worst %.5f\n", worst);
    Shutdown();
    return worst <= 1.01f / 255.0f ? 0 : 1;
}

static int TestFilter() {
    if (!Init(TestPreset())) { PrintErrors(); return 1; }
    const auto in = Gradient();
    std::vector<float> out;
    if (!Run(in, out, "RsTestTint")) return 1;
    const float worst = WorstOff(in, out, [](float v, int c) { return c == 0 ? v * 0.5f : v; });
    printf("tint alone: worst %.5f\n", worst);
    Shutdown();
    return worst <= 1.01f / 255.0f ? 0 : 1;
}

static int TestNone() {
    if (!Init(TestPreset())) { PrintErrors(); return 1; }
    const auto in = Gradient();
    std::vector<float> out;
    if (!Run(in, out, "-")) return 1;
    const float worst = WorstOff(in, out, [](float v, int) { return v; });
    printf("no technique: worst %.6f\n", worst);
    Shutdown();
    return worst <= 1.01f / 255.0f ? 0 : 1;
}

static int TestClock() {
    if (!Init(TestPreset())) { PrintErrors(); return 1; }
    const auto in = Gradient();
    std::vector<float> a, b, c;
    if (!Run(in, a, "RsTestTimeOut", 2.5, 7, 42) || !Run(in, b, "RsTestTimeOut", 2.5, 7, 42)
        || !Run(in, c, "RsTestTimeOut", 2.5, 7, 43)) return 1;
    Shutdown();
    const bool same = a == b;
    const float timer = a[0], frame = a[1];
    const float wantTimer = std::fmod(2500.0f / 997.0f, 1.0f), wantFrame = 7.0f / 251.0f;
    const bool seedMatters = a[2] != c[2];
    printf("clock: frac(timer/997) %.4f (%.4f), frac(framecount/251) %.4f (%.4f), random %.0f vs %.0f, same twice %d\n",
           timer, wantTimer, frame, wantFrame, a[2] * 255.0f, c[2] * 255.0f, same ? 1 : 0);
    // An 8-bit back buffer: within one level.
    return same && std::fabs(timer - wantTimer) <= 1.01f / 255.0f && std::fabs(frame - wantFrame) <= 1.01f / 255.0f
        && seedMatters ? 0 : 1;
}

// A preset of the user's for the two tests below, named by S2FM_TEST_PRESET; without it they are skipped (3).
static bool UserPreset(fs::path& preset) {
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, "S2FM_TEST_PRESET") == 0 && v) preset = v;
    free(v);
    if (!preset.empty() && fs::exists(preset)) return true;
    printf("SKIP: set S2FM_TEST_PRESET to a preset file\n");
    return false;
}

// The user's preset (a copy) against the user's shaders: which effects compile. Reported, passes if any did.
static int TestUserPreset() {
    fs::path preset;
    if (!UserPreset(preset)) return 3;
    const fs::path copy = g_run / "user_preset_copy.ini";
    std::error_code ec;
    fs::create_directories(g_run, ec);
    fs::copy_file(preset, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) { printf("FAIL: could not copy %s\n", preset.string().c_str()); return 1; }
    const char* defs = "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=2000000.0,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0,"
                       "RESHADE_DEPTH_INPUT_IS_REVERSED=0,RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0,SOURCE2_NEAR_PLANE=12.0";
    const bool loaded = Init(copy, defs, 1280, 720, 240000);
    auto count = Fn<RsCore_TechniqueCount_Fn>("RsCore_TechniqueCount");
    auto name = Fn<RsCore_TechniqueName_Fn>("RsCore_TechniqueName");
    int n = count ? count() : 0, enabled = 0;
    for (int i = 0; i < n; ++i) {
        char b[256] = "";
        int en = 0;
        name(i, b, sizeof(b), &en);
        if (en) { ++enabled; printf("  enabled: %s\n", b); }
    }
    printf("preset: %d techniques, %d enabled, loaded %d\n", n, enabled, loaded ? 1 : 0);
    PrintErrors();
    Shutdown();
    return enabled > 0 ? 0 : 1;
}

// The depth the stages hand over: a view distance per column from 12 to 1.2e6 units (row 0 sky) must come
// back out of ReShade.fxh's GetLinearizedDepth and MartysMods' get_linear_depth as L = z / (N*F), in full float
// (RsDepthOut). The headers' own formula loses precision as L grows (dL/dd = (1 + L(F-1))^2 / F), which is
// the tolerance, plus a little for the encode.
static const char* kDepthDefinitions =
    "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=2000000.0,SOURCE2_NEAR_PLANE=12.0,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0,"
    "RESHADE_DEPTH_INPUT_IS_REVERSED=0,RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0,RESHADE_DEPTH_INPUT_X_SCALE=1,"
    "RESHADE_DEPTH_INPUT_Y_SCALE=1,RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET=0,RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET=0,"
    "RESHADE_DEPTH_MULTIPLIER=1";

static int TestDepth() {
    if (!Init(g_src / "presets" / "rs_test_depth.ini", kDepthDefinitions)) { PrintErrors(); return 1; }
    const double N = 12.0, F = 2000000.0;
    std::vector<float> z((size_t)W * H);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) z[(size_t)y * W + x] = y == 0 ? 0.0f : (float)(12.0 * std::pow(10.0, 5.0 * x / (W - 1)));
    auto depth = Fn<RsCore_HarnessDepth_Fn>("RsCore_HarnessDepth");
    auto read = Fn<RsCore_HarnessReadTexture_Fn>("RsCore_HarnessReadTexture");
    if (!depth || !read) return 1;
    if (!depth(z.data(), W, H, N, F)) { printf("FAIL: depth not handed over\n"); return 1; }
    const auto in = Gradient();
    std::vector<float> out, full((size_t)W * H * 4);
    if (!Run(in, out, "RsTestDepth")) return 1;
    if (!read("RsDepthOut", full.data(), W, H)) { printf("FAIL: RsDepthOut not read\n"); PrintErrors(); return 1; }
    Shutdown();
    int bad = 0;
    double worstRel[2] = { 0, 0 };
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            const size_t i = (size_t)y * W + x;
            const double L = z[i] <= 0.0f ? 1.0 : std::min(1.0, z[i] / (N * F));
            const double slope = (1.0 + L * (F - 1.0)) * (1.0 + L * (F - 1.0)) / F;
            const double tol = 4.0 * 6e-8 * slope + 2e-6 * L + 1e-9;
            for (int c = 0; c < 2; ++c) {
                const double got = full[i * 4 + c];
                const double off = std::fabs(got - L);
                if (off > tol) {
                    if (bad < 5) printf("  (%u,%u) z %.1f: %s %.9g, expected %.9g (tolerance %.3g)\n", x, y, z[i],
                                        c ? "mmx" : "ReShade.fxh", got, L, tol);
                    ++bad;
                }
                if (L < 1.0 && L > 0.0 && off / L > worstRel[c]) worstRel[c] = off / L;
            }
        }
    printf("depth: %d texels off; worst relative error ReShade.fxh %.2e, mmx %.2e (z 12 to 1.2e6)\n", bad, worstRel[0], worstRel[1]);
    return bad == 0 ? 0 : 1;
}

// --- Stages on engine-like textures (task 5).
static const char kFillHlsl[] = R"(
cbuffer C : register(b0) { float scale; float3 pad; };
float4 VS(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
// Every sample its own value when 'scale' is not 0: a ramp across x and y, and the sample index on top.
float4 PS(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Target
{
    const float2 p = floor(pos.xy);
    return float4((p.x / 255.0) * scale + s * 0.01 * scale, (p.y / 127.0) * scale, 0.3 * scale + s * 0.002, 1.0);
}
)";

// An engine-like texture: typeless, render target and shader resource, 'samples' samples.
static ID3D11Texture2D* MakeTarget(DXGI_FORMAT typeless, unsigned samples) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = W;
    d.Height = H;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = typeless;
    d.SampleDesc.Count = samples;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* t = nullptr;
    g_dev->CreateTexture2D(&d, nullptr, &t);
    return t;
}

// Fills a 16F target with the ramp, every sample its own value (scale 0 gives a flat 0.3 blue only).
static bool Fill(ID3D11Texture2D* t, unsigned samples, float scale) {
    static ID3D11VertexShader* vs = nullptr;
    static ID3D11PixelShader* ps = nullptr;
    static ID3D11Buffer* cb = nullptr;
    if (!vs) {
        ID3DBlob* b = nullptr;
        if (FAILED(D3DCompile(kFillHlsl, sizeof(kFillHlsl) - 1, "fill", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &b, nullptr))) return false;
        g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
        b->Release();
        if (FAILED(D3DCompile(kFillHlsl, sizeof(kFillHlsl) - 1, "fill", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &b, nullptr))) return false;
        g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps);
        b->Release();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_dev->CreateBuffer(&bd, nullptr, &cb);
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.ViewDimension = samples > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(g_dev->CreateRenderTargetView(t, &rd, &rtv))) return false;
    const float c[4] = { scale, 0, 0, 0 };
    g_ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0);
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(vs, nullptr, 0);
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &cb);
    g_ctx->Draw(3, 0);
    ID3D11RenderTargetView* none = nullptr;
    g_ctx->OMSetRenderTargets(1, &none, nullptr);
    rtv->Release();
    return true;
}

// A target read back as floats: 16F resolved (or copied), or 8-bit bytes as 0..255.
static bool ReadBack(ID3D11Texture2D* t, DXGI_FORMAT typed, unsigned samples, std::vector<float>& out) {
    D3D11_TEXTURE2D_DESC d = {};
    t->GetDesc(&d);
    d.SampleDesc.Count = 1;
    d.BindFlags = 0;
    d.Format = typed;
    ID3D11Texture2D* single = nullptr;
    ID3D11Texture2D* from = t;
    if (samples > 1) {
        d.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &single))) return false;
        g_ctx->ResolveSubresource(single, 0, t, 0, typed);
        from = single;
    }
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return false;
    g_ctx->CopyResource(st, from);
    D3D11_MAPPED_SUBRESOURCE m = {};
    bool ok = SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        out.assign((size_t)W * H * 4, 0.0f);
        for (unsigned y = 0; y < H; ++y) {
            const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
            for (unsigned x = 0; x < W * 4; ++x) {
                float v;
                if (typed == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    const uint16_t h = ((const uint16_t*)row)[x];
                    const uint32_t sign = (h >> 15) & 1, e = (h >> 10) & 31, mant = h & 1023;
                    v = e == 0 ? std::ldexp((float)mant, -24) : std::ldexp((float)(mant | 1024), (int)e - 25);
                    if (sign) v = -v;
                } else {
                    v = (float)row[x];
                }
                out[(size_t)y * W * 4 + x] = v;
            }
        }
        g_ctx->Unmap(st, 0);
    }
    st->Release();
    if (single) single->Release();
    return ok;
}

static DxrStageFrame Frame(int stage, ID3D11Texture2D* t, DXGI_FORMAT typed, int encoding, unsigned samples, float exposure) {
    DxrStageFrame f = {};
    f.size = sizeof(f);
    f.stage = stage;
    f.ctx = g_ctx;
    f.target = t;
    f.viewFormat = typed;
    f.encoding = encoding;
    f.width = W;
    f.height = H;
    f.samples = samples;
    f.exposure = exposure;
    f.seconds = 1.0;
    f.filmFrame = 24;
    return f;
}

static double Srgb(double c) { c = c < 0 ? 0 : c; return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }
static double Linear(double v) { v = v < 0 ? 0 : v; return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

// A: the scene, 8 samples. No technique leaves every pixel exactly as it was; the tint (red times RsTestGain,
// 0.5, in the curve's space) gives back, through the inverse curve, what the resolve of the result says.
static int TestIoA() {
    if (!Init(TestPreset())) { PrintErrors(); return 1; }
    auto stage = Fn<RsCore_HarnessStage_Fn>("RsCore_HarnessStage");
    ID3D11Texture2D* t = MakeTarget(DXGI_FORMAT_R16G16B16A16_TYPELESS, 8);
    const float exposure = 50.0f;
    // The ramp at 1/50 of display white and above: 0 to about 0.09, so up to 4.5 times white after exposure.
    if (!stage || !t || !Fill(t, 8, 0.02f)) { printf("FAIL: setup\n"); return 1; }
    std::vector<float> before, same, tinted;
    ReadBack(t, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, before);
    DxrStageFrame f = Frame(DXR_STAGE_A, t, DXGI_FORMAT_R16G16B16A16_FLOAT, DXR_ENC_SCENE, 8, exposure);
    if (!stage(&f, "-")) { printf("FAIL: stage A (none)\n"); return 1; }
    ReadBack(t, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, same);
    float worstSame = 0;
    for (size_t i = 0; i < before.size(); ++i) worstSame = std::max(worstSame, std::fabs(same[i] - before[i]));
    if (!stage(&f, "RsTestTint")) { printf("FAIL: stage A (tint)\n"); return 1; }
    ReadBack(t, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, tinted);
    // Expected, per pixel of the resolve: red through the curve, halved, back; green as it was. Judged where it
    // is seen, in the curve's encoded space (0..1), where half floats keep about 1/2048.
    auto seen = [exposure](double c) { const double x = (c < 0 ? 0 : c) * exposure; return Srgb(x / (1.0 + x)); };
    double worstRel = 0;
    for (size_t i = 0; i < before.size(); i += 4) {
        const double x = same[i] * exposure;
        const double y = Linear(0.5 * Srgb(x / (1.0 + x)));
        const double expect = y / (1.0 - y) / exposure;
        const double off = std::fabs(seen(tinted[i]) - seen(expect));
        if (off > worstRel && getenv("RS_VERBOSE"))
            printf("  pixel %zu: before %.6g (seen %.5f) tinted %.6g (seen %.5f) expected %.6g (seen %.5f)\n", i / 4,
                   same[i], seen(same[i]), tinted[i], seen(tinted[i]), expect, seen(expect));
        worstRel = std::max(worstRel, off);
        worstRel = std::max(worstRel, std::fabs(seen(tinted[i + 1]) - seen(same[i + 1])));
    }
    t->Release();
    Shutdown();
    printf("io A: untouched worst %.3g; tinted worst off %.5f where it is seen\n", worstSame, worstRel);
    return worstSame == 0.0f && worstRel < 3.0 / 255.0 ? 0 : 1;   // the back buffer keeps 8 bits
}

// B: post_process's linear output (8 samples, equal in a pixel). None leaves it exactly; the invert gives
// decode(1 - encode(c)).
static int TestIoB() {
    if (!Init(TestPreset())) { PrintErrors(); return 1; }
    auto stage = Fn<RsCore_HarnessStage_Fn>("RsCore_HarnessStage");
    ID3D11Texture2D* t = MakeTarget(DXGI_FORMAT_R16G16B16A16_TYPELESS, 8);
    if (!stage || !t || !Fill(t, 8, 1.0f)) { printf("FAIL: setup\n"); return 1; }
    std::vector<float> before, same, inverted;
    ReadBack(t, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, before);
    DxrStageFrame f = Frame(DXR_STAGE_B, t, DXGI_FORMAT_R16G16B16A16_FLOAT, DXR_ENC_LINEAR, 8, 1.0f);
    if (!stage(&f, "-")) { printf("FAIL: stage B (none)\n"); return 1; }
    ReadBack(t, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, same);
    float worstSame = 0;
    for (size_t i = 0; i < before.size(); ++i) worstSame = std::max(worstSame, std::fabs(same[i] - before[i]));
    if (!stage(&f, "RsTestInvert")) { printf("FAIL: stage B (invert)\n"); return 1; }
    ReadBack(t, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, inverted);
    double worst = 0;
    for (size_t i = 0; i < before.size(); i += 4)
        for (int c = 0; c < 3; ++c) {
            const double v = same[i + c];
            if (v > 1.0) continue;   // the encode of what is over white has no inverse in 0..1
            worst = std::max(worst, std::fabs(inverted[i + c] - Linear(1.0 - Srgb(v))));
        }
    t->Release();
    Shutdown();
    printf("io B: untouched worst %.3g; inverted worst %.5f\n", worstSame, worst);
    return worstSame == 0.0f && worst < 8e-3 ? 0 : 1;
}

// C: the 8-bit picture. None leaves every byte; the invert gives 255 - b.
static int TestIoC() {
    if (!Init(TestPreset())) { PrintErrors(); return 1; }
    auto stage = Fn<RsCore_HarnessStage_Fn>("RsCore_HarnessStage");
    ID3D11Texture2D* t = MakeTarget(DXGI_FORMAT_R8G8B8A8_TYPELESS, 1);
    if (!stage || !t) { printf("FAIL: setup\n"); return 1; }
    std::vector<uint8_t> bytes((size_t)W * H * 4);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = (uint8_t)((i * 37 + i / 1024) & 255);
    g_ctx->UpdateSubresource(t, 0, nullptr, bytes.data(), W * 4, 0);
    std::vector<float> same, inverted;
    DxrStageFrame f = Frame(DXR_STAGE_C, t, DXGI_FORMAT_R8G8B8A8_UNORM, DXR_ENC_SRGB8, 1, 1.0f);
    if (!stage(&f, "-")) { printf("FAIL: stage C (none)\n"); return 1; }
    ReadBack(t, DXGI_FORMAT_R8G8B8A8_UNORM, 1, same);
    if (!stage(&f, "RsTestInvert")) { printf("FAIL: stage C (invert)\n"); return 1; }
    ReadBack(t, DXGI_FORMAT_R8G8B8A8_UNORM, 1, inverted);
    int offSame = 0, offInv = 0;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i % 4 == 3) continue;
        if (same[i] != bytes[i]) ++offSame;
        if (inverted[i] != 255.0f - bytes[i]) ++offInv;
    }
    t->Release();
    Shutdown();
    printf("io C: untouched bytes off %d; inverted bytes off %d\n", offSame, offInv);
    return offSame == 0 && offInv == 0 ? 0 : 1;
}

// --- The panel's API (tasks 9 and 10).
static int FindTechnique(const char* name) {
    auto info = Fn<RsCore_TechniqueInfo_Fn>("RsCore_TechniqueInfo");
    auto count = Fn<RsCore_TechniqueCount_Fn>("RsCore_TechniqueCount");
    for (int i = 0; info && count && i < count(); ++i) {
        RsTechniqueInfo ti = {};
        if (info(i, &ti) && !strcmp(ti.name, name)) return i;
    }
    return -1;
}

// The user's preset: each technique's stage by the rules, against what the design says for the effects it has.
static int TestAssign() {
    fs::path preset;
    if (!UserPreset(preset)) return 3;
    const fs::path copy = g_run / "user_preset_copy.ini";
    std::error_code ec;
    fs::create_directories(g_run, ec);
    fs::copy_file(preset, copy, fs::copy_options::overwrite_existing, ec);
    if (!Init(copy, kDepthDefinitions, 1280, 720, 240000)) { PrintErrors(); return 1; }
    struct Want { const char* name; int stage; };
    const Want want[] = {
        { "MartysMods_MXAO", 0 }, { "MartysMods_SPECGI_Specular", 0 }, { "MartysMods_SOLARIS", 1 },
        { "MartysMods_ConvolutionBloom", 1 }, { "MartysMods_Clarity", 1 }, { "prod80_05_LumaSharpen", 2 },
        { "Xenon", 1 }, { "AdaptiveSharpen", 2 }, { "MagicHDR", 1 }, { "ChromaticitySimple", 2 },
        { "AmbientLight", 1 }, { "MartysMods_AntiAliasing", 1 }, { "MartysMods_AntiAliasing_Prepass", 1 },
        { "Information_SD", 1 },
    };
    auto info = Fn<RsCore_TechniqueInfo_Fn>("RsCore_TechniqueInfo");
    auto count = Fn<RsCore_TechniqueCount_Fn>("RsCore_TechniqueCount");
    int wrong = 0;
    const char* letter = "ABC";
    for (int i = 0; info && count && i < count(); ++i) {
        RsTechniqueInfo ti = {};
        info(i, &ti);
        int expect = -1;
        for (const Want& w : want) if (!strcmp(w.name, ti.name)) expect = w.stage;
        const bool ok = expect < 0 || expect == ti.autoStage;
        if (!ok) ++wrong;
        printf("  %-34s %-34s stage %c%s%s\n", ti.name, ti.effect, letter[ti.autoStage], ti.readsDepth ? ", reads depth" : "",
               ok ? "" : (expect >= 0 ? "   <-- expected another" : ""));
    }
    Shutdown();
    printf("assign: %d placed differently from the design\n", wrong);
    return wrong == 0 ? 0 : 1;
}

// A setting through the API: RsTestGain read (0.5), its annotations, set to 0.25, and the stage follows.
static int TestApiUniform() {
    if (!Init(TestPreset())) { PrintErrors(); return 1; }
    const int t = FindTechnique("RsTestTint");
    auto ucount = Fn<RsCore_UniformCount_Fn>("RsCore_UniformCount");
    auto uinfo = Fn<RsCore_UniformInfo_Fn>("RsCore_UniformInfo");
    auto get = Fn<RsCore_GetUniform_Fn>("RsCore_GetUniform");
    auto set = Fn<RsCore_SetUniform_Fn>("RsCore_SetUniform");
    if (t < 0 || !ucount || !uinfo || !get || !set) { printf("FAIL: RsTestTint or the API not found\n"); return 1; }
    int u = -1;
    RsUniformInfo ui = {};
    for (int k = 0; k < ucount(t); ++k) {
        RsUniformInfo x = {};
        if (uinfo(t, k, &x) && !strcmp(x.name, "RsTestGain")) { u = k; ui = x; }
    }
    if (u < 0) { printf("FAIL: RsTestGain not listed (%d settings)\n", ucount(t)); return 1; }
    float v = -1.0f;
    get(t, u, &v, 1);
    printf("RsTestGain: label \"%s\", type \"%s\", base %d, %d components, %g to %g, value %g\n", ui.label, ui.uiType,
           ui.baseType, ui.components, ui.uiMin, ui.uiMax, v);
    const float quarter = 0.25f;
    set(t, u, &quarter, 1);
    const auto in = Gradient();
    std::vector<float> out;
    if (!Run(in, out, "RsTestTint")) return 1;
    Shutdown();
    const float worst = WorstOff(in, out, [](float x, int c) { return c == 0 ? x * 0.25f : x; });
    printf("api uniform: after setting 0.25, worst %.5f\n", worst);
    return std::fabs(v - 0.5f) < 1e-6f && !strcmp(ui.uiType, "slider") && ui.baseType == 3 && ui.uiMax == 2.0f
        && worst <= 1.01f / 255.0f ? 0 : 1;
}

static int TestList() {
    if (!Init(TestPreset())) { PrintErrors(); return 1; }
    auto count = Fn<RsCore_TechniqueCount_Fn>("RsCore_TechniqueCount");
    auto name = Fn<RsCore_TechniqueName_Fn>("RsCore_TechniqueName");
    const int n = count ? count() : 0;
    for (int i = 0; i < n; ++i) {
        char b[256] = "";
        int en = 0;
        name(i, b, sizeof(b), &en);
        printf("  %d %s enabled %d\n", i, b, en);
    }
    PrintErrors();
    Shutdown();
    return n == 3 ? 0 : 1;
}

// A picture through a preset's techniques, to be looked at: a dark room with three lamps and a
// lit window, 960x540, display values. rs_harness image <preset> <techniques, or all> <out.ppm>
static int TestImage(int argc, char** argv) {
    if (argc < 5) { printf("FAIL: rs_harness image <preset> <techniques|all> <out.ppm>\n"); return 1; }
    const unsigned w = 960, h = 540;
    if (!Init(argv[2], nullptr, w, h, 240000)) { PrintErrors(); return 1; }
    struct Lamp { float x, y, r, c[3]; };
    const Lamp lamps[] = { { 0.30f, 0.35f, 0.030f, { 1.0f, 1.0f, 1.0f } }, { 0.72f, 0.60f, 0.050f, { 1.0f, 0.6f, 0.2f } },
                           { 0.55f, 0.20f, 0.015f, { 1.0f, 1.0f, 1.0f } } };
    std::vector<float> in((size_t)w * h * 4), out(in.size());
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            float c[3] = { 0.05f + 0.03f * v, 0.06f + 0.03f * v, 0.08f + 0.03f * v };
            if (u > 0.08f && u < 0.20f && v > 0.55f && v < 0.90f) c[0] = c[1] = c[2] = 0.7f;
            for (const Lamp& l : lamps) {
                const float dx = (u - l.x) * w / h, dy = v - l.y;
                if (dx * dx + dy * dy < l.r * l.r) for (int k = 0; k < 3; ++k) c[k] = l.c[k];
            }
            float* p = &in[((size_t)y * w + x) * 4];
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = 1.0f;
        }
    auto up = Fn<RsCore_HarnessUpload_Fn>("RsCore_HarnessUpload");
    auto run = Fn<RsCore_HarnessRun_Fn>("RsCore_HarnessRun");
    auto down = Fn<RsCore_HarnessDownload_Fn>("RsCore_HarnessDownload");
    const char* techniques = strcmp(argv[3], "all") ? argv[3] : "";
    const bool ok = up && run && down && up(in.data(), w, h) && run(1.0, 1, 1, techniques) && down(out.data(), w, h);
    PrintErrors();
    Shutdown();
    if (!ok) { printf("FAIL: the run\n"); return 1; }
    FILE* f = nullptr;
    if (fopen_s(&f, argv[4], "wb") != 0 || !f) { printf("FAIL: %s not written\n", argv[4]); return 1; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; ++i)
        for (int k = 0; k < 3; ++k) {
            const float v = out[i * 4 + k] < 0.0f ? 0.0f : out[i * 4 + k] > 1.0f ? 1.0f : out[i * 4 + k];
            fputc((int)(v * 255.0f + 0.5f), f);
        }
    fclose(f);
    printf("image: %s written\n", argv[4]);
    return 0;
}

int main(int argc, char** argv) {
    WCHAR self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_here = fs::path(self).parent_path();                         // reshade\bin
    g_run = g_here / "run";
    g_src = g_here.parent_path() / "harness";                      // reshade\harness
    g_core = LoadLibraryW((g_here / L"s2fm_reshade_core.dll").c_str());
    if (!g_core) { printf("FAIL: s2fm_reshade_core.dll not loaded (%lu)\n", GetLastError()); return 1; }
    const char* t = argc > 1 ? argv[1] : "version";
    int r = 1;
    if (!strcmp(t, "version")) r = TestVersion();
    else if (!MakeDevice()) r = 1;
    else if (!strcmp(t, "invert")) r = TestInvert();
    else if (!strcmp(t, "filter")) r = TestFilter();
    else if (!strcmp(t, "none")) r = TestNone();
    else if (!strcmp(t, "clock")) r = TestClock();
    else if (!strcmp(t, "preset")) r = TestUserPreset();
    else if (!strcmp(t, "list")) r = TestList();
    else if (!strcmp(t, "depth")) r = TestDepth();
    else if (!strcmp(t, "io_a")) r = TestIoA();
    else if (!strcmp(t, "io_b")) r = TestIoB();
    else if (!strcmp(t, "io_c")) r = TestIoC();
    else if (!strcmp(t, "assign")) r = TestAssign();
    else if (!strcmp(t, "api_uniform")) r = TestApiUniform();
    else if (!strcmp(t, "image")) r = TestImage(argc, argv);
    else printf("FAIL: unknown test %s\n", t);
    printf("%s %s\n", r == 0 ? "PASS" : r == 3 ? "SKIP" : "FAIL", t);
    return r == 3 ? 0 : r;
}
