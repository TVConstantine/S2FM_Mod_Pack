// The procedural sky (a shader compiler's s2vfx_sky) baked into an HDRI, and handed back as light to the shaders that
// ask for it. When the sky is drawn with settings not yet baked (its material's constants, its seed among them), its
// own draw is made six times more into a cube of float faces of our own, each face a 90 degree view from the camera:
// the sky's vertex stage pins the sphere to the far plane by the view it is given, so only the PerView buffer's
// projection changes. Six more go into a second cube with the sun's disk left out (g_flSunDisk 0): the light, since
// the sun lights a scene as a light of its own. Both are read back a few frames later (no wait on the GPU) and, on a
// thread of their own:
//   the first is turned into an equirectangular picture, written as a Radiance .hdr beside the DLL:
//     s2fm_sky\sky_seed<seed>_<settings>.hdr, a file of its own for each look (the same settings write the same file);
//   the second into the sky light (see lighting.hlsl, g_tSkyLight), an equirectangular 512 x 256 float texture whose
//     mips 0 to 5 are the sky prefiltered with GGX for perceptual roughness 0, 0.2, ... 1 (filtered importance
//     sampling, Krivanek and Colbert 2007) and whose mips 6 on are its diffuse light (cosine weighted, over pi).
// The texture is made at a present (the engine's device may be single threaded) and bound at every draw of a shader
// that declares g_tSkyLight, on every context, the engine's texture put back after it.
//
// The directions are the world's (Z up), laid out as Blender's world texture lays an equirectangular image out, so
// the file is the same sky there. The values are the sky's own output (its exposure in), before the engine's tone map.
//
// Dev word (s2fm_refract_debug.txt, read twice a second): skybake   bake again at the next sky draw.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/shader_table.h"
#include "../core/core.h"

namespace {

template <typename T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }

const UINT kFace = 512;                  // each cube face, in texels
const int kOutW = 2048, kOutH = 1024;    // the equirectangular picture
const int kLightW = 512, kLightH = 256;  // the sky light, and its mips
const int kLightMips = 10;
const int kPrefiltered = 6;              // mips 0 to 5 prefiltered, the rest the diffuse light
const float kPi = 3.14159265358979f;

// Cube 0 the sky as it is (the file), cube 1 without its sun's disk (the light).
ID3D11Texture2D* g_cube[2] = {};
ID3D11RenderTargetView* g_faceRtv[2][6] = {};
ID3D11Texture2D* g_staging[2] = {};
ID3D11RasterizerState* g_raster = nullptr;
ID3D11Buffer* g_perView = nullptr;
ID3D11Buffer* g_sunless = nullptr;      // the sky's material constants with its sun's disk at 0
UINT g_sunlessBytes = 0;

uint64_t g_bakedHash = 0;               // the material constants last baked
bool g_readPending = false;             // the staging copies are made, not read yet
bool g_sunlessDrawn = false;            // and the second, the sunless one, among them
int g_readTries = 0;
float g_pendingSeed = 0.0f;
std::atomic<bool> g_working{false};     // the file and the light are being made on their thread
std::atomic<bool> g_forced{false};
std::atomic<bool> g_dump{false};        // dev word skylightdump: the sky light's mips written as .hdr files too
std::atomic<bool> g_mips{false};        // dev word skylightmips: the sky light's mips each a colour

// The sky light: its mips made on the thread, the texture made from them at a present, then bound.
std::mutex g_lightMutex;
std::vector<std::vector<float>> g_lightMips;   // waiting to be made into the texture
std::atomic<bool> g_lightReady{false};
ID3D11ShaderResourceView* g_lightSrv = nullptr;

uint64_t Fnv(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

void DropAll() {
    for (int k = 0; k < 2; ++k) {
        for (auto& v : g_faceRtv[k]) Drop(v);
        Drop(g_cube[k]);
        Drop(g_staging[k]);
    }
    Drop(g_raster);
    Drop(g_perView);
}

bool Make(ID3D11Device* dev) {
    if (g_cube[0]) return true;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = kFace;
    d.MipLevels = 1;
    d.ArraySize = 6;
    d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bool ok = true;
    for (int k = 0; ok && k < 2; ++k) {
        ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_cube[k])) &&
             SUCCEEDED(dev->CreateTexture2D(&s, nullptr, &g_staging[k]));
        for (UINT f = 0; ok && f < 6; ++f) {
            D3D11_RENDER_TARGET_VIEW_DESC r = {};
            r.Format = d.Format;
            r.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            r.Texture2DArray.FirstArraySlice = f;
            r.Texture2DArray.ArraySize = 1;
            ok = SUCCEEDED(dev->CreateRenderTargetView(g_cube[k], &r, &g_faceRtv[k][f]));
        }
    }
    // Both sides (a face's view may mirror the winding the sky was made with), no scissor.
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_raster));
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 816;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_perView));
    if (!ok) {
        DropAll();
        Log("[SKY] the bake's cubes COULD NOT be made\n");
    }
    return ok;
}

// The six faces as a cube texture has them: forward, up, right.
const float kFaces[6][3][3] = {
    {{ 1, 0, 0}, {0, 1, 0}, { 0, 0, -1}},
    {{-1, 0, 0}, {0, 1, 0}, { 0, 0,  1}},
    {{ 0, 1, 0}, {0, 0, -1}, {1, 0,  0}},
    {{ 0, -1, 0}, {0, 0, 1}, {1, 0,  0}},
    {{ 0, 0, 1}, {0, 1, 0}, { 1, 0,  0}},
    {{ 0, 0, -1}, {0, 1, 0}, {-1, 0, 0}},
};

float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Normalize(float v[3]) {
    const float l = sqrtf(Dot3(v, v));
    for (int c = 0; c < 3; ++c) v[c] /= l > 0.0f ? l : 1.0f;
}

// PerView's world to projection for face f, seen from c (in the space the engine projects from: world plus its
// world to camera offset). The engine's matrix is applied as a row vector times it (clip = p M, register j its row j):
// x along the face's right, y along its up, w along its forward, z a far plane past the sky's pin (1e7 down the view).
void FaceMatrix(int f, const float c[3], float m[16]) {
    const float* fw = kFaces[f][0];
    const float* up = kFaces[f][1];
    const float* rt = kFaces[f][2];
    const float n = 1.0f, back = 1.0e9f;
    const float a = back / (back - n), b = -n * back / (back - n);
    for (int j = 0; j < 3; ++j) {
        m[j * 4 + 0] = rt[j];
        m[j * 4 + 1] = up[j];
        m[j * 4 + 2] = a * fw[j];
        m[j * 4 + 3] = fw[j];
    }
    m[12] = -Dot3(rt, c);
    m[13] = -Dot3(up, c);
    m[14] = -a * Dot3(fw, c) + b;
    m[15] = -Dot3(fw, c);
}

// A cube's levels on the CPU: level 0 the faces as read back, each next one half the size (a 2 x 2 average).
struct Cube {
    std::vector<std::vector<float>> levels;   // 6 faces of size x size RGBA each
    std::vector<int> sizes;
};

Cube Pyramid(std::vector<float> faces, int smallest) {
    Cube c;
    c.levels.push_back(std::move(faces));
    c.sizes.push_back((int)kFace);
    while (c.sizes.back() / 2 >= smallest) {
        const int n = c.sizes.back() / 2, m = c.sizes.back();
        const std::vector<float>& a = c.levels.back();
        std::vector<float> b((size_t)6 * n * n * 4);
        for (int f = 0; f < 6; ++f)
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                    for (int k = 0; k < 4; ++k) {
                        const float* p = a.data() + (size_t)f * m * m * 4;
                        b[(((size_t)f * n + y) * n + x) * 4 + k] =
                            0.25f * (p[((size_t)(2 * y) * m + 2 * x) * 4 + k] + p[((size_t)(2 * y) * m + 2 * x + 1) * 4 + k] +
                                     p[((size_t)(2 * y + 1) * m + 2 * x) * 4 + k] +
                                     p[((size_t)(2 * y + 1) * m + 2 * x + 1) * 4 + k]);
                    }
        c.levels.push_back(std::move(b));
        c.sizes.push_back(n);
    }
    return c;
}

// A level's texel for direction d, bilinear, as a cube sampler picks the face.
void SampleLevel(const Cube& cube, int level, const float d[3], float out[3]) {
    int best = 0;
    float most = -1.0f;
    for (int f = 0; f < 6; ++f) {
        const float k = Dot3(d, kFaces[f][0]);
        if (k > most) { most = k; best = f; }
    }
    const int n = cube.sizes[level];
    const float s = Dot3(d, kFaces[best][2]) / most, t = Dot3(d, kFaces[best][1]) / most;
    float x = (s * 0.5f + 0.5f) * n - 0.5f, y = (0.5f - t * 0.5f) * n - 0.5f;
    x = x < 0.0f ? 0.0f : x > n - 1.0f ? n - 1.0f : x;
    y = y < 0.0f ? 0.0f : y > n - 1.0f ? n - 1.0f : y;
    const int x0 = (int)x, y0 = (int)y;
    const int x1 = x0 + 1 < n ? x0 + 1 : x0, y1 = y0 + 1 < n ? y0 + 1 : y0;
    const float fx = x - x0, fy = y - y0;
    const float* face = cube.levels[level].data() + (size_t)best * n * n * 4;
    for (int c = 0; c < 3; ++c) {
        const float a = face[((size_t)y0 * n + x0) * 4 + c], b = face[((size_t)y0 * n + x1) * 4 + c];
        const float e = face[((size_t)y1 * n + x0) * 4 + c], g = face[((size_t)y1 * n + x1) * 4 + c];
        const float top = a + (b - a) * fx;
        out[c] = top + ((e + (g - e) * fx) - top) * fy;
    }
}

// Between two levels, by a fractional level.
void SampleLod(const Cube& cube, float lod, const float d[3], float out[3]) {
    const float top = (float)(cube.levels.size() - 1);
    lod = lod < 0.0f ? 0.0f : lod > top ? top : lod;
    const int l0 = (int)lod;
    const int l1 = l0 + 1 < (int)cube.levels.size() ? l0 + 1 : l0;
    const float t = lod - l0;
    float a[3], b[3];
    SampleLevel(cube, l0, d, a);
    if (t <= 0.0f || l1 == l0) { for (int c = 0; c < 3; ++c) out[c] = a[c]; return; }
    SampleLevel(cube, l1, d, b);
    for (int c = 0; c < 3; ++c) out[c] = a[c] + (b[c] - a[c]) * t;
}

// The direction of an equirectangular texel (u, v in 0..1, v 0 the zenith), Blender's layout: u 0.5 looks down -x,
// u 0.75 down +y.
void EquirectDir(float u, float v, float d[3]) {
    const float lon = (u - 0.5f) * 2.0f * kPi, lat = (0.5f - v) * kPi;
    d[0] = -cosf(lon) * cosf(lat);
    d[1] = sinf(lon) * cosf(lat);
    d[2] = sinf(lat);
}

// Radiance's shared exponent form of a colour.
void Rgbe(const float c[3], uint8_t out[4]) {
    float v = c[0] > c[1] ? c[0] : c[1];
    v = v > c[2] ? v : c[2];
    if (!(v > 1e-32f)) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    int e = 0;
    const float scale = frexpf(v, &e) * 256.0f / v;
    for (int k = 0; k < 3; ++k) out[k] = (uint8_t)(c[k] > 0.0f ? c[k] * scale : 0.0f);
    out[3] = (uint8_t)(e + 128);
}

void WriteHdr(const Cube& cube, float seed, uint64_t hash) {
    std::vector<uint8_t> pixels((size_t)kOutW * kOutH * 4);
    for (int y = 0; y < kOutH; ++y)
        for (int x = 0; x < kOutW; ++x) {
            float d[3], c[3];
            EquirectDir((x + 0.5f) / kOutW, (y + 0.5f) / kOutH, d);
            SampleLevel(cube, 0, d, c);
            Rgbe(c, &pixels[((size_t)y * kOutW + x) * 4]);
        }
    CreateDirectoryW(CorePath(L"s2fm_sky").c_str(), nullptr);
    wchar_t name[96];
    swprintf(name, 96, L"s2fm_sky\\sky_seed%d_%06x.hdr", (int)floorf(seed + 0.5f), (unsigned)(hash & 0xFFFFFF));
    const std::wstring path = CorePath(name);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        // The top row first, the zenith.
        fprintf(f, "#?RADIANCE\n# s2vfx_sky, seed %g\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n", seed, kOutH, kOutW);
        fwrite(pixels.data(), 1, pixels.size(), f);
        fclose(f);
        Log("[SKY] baked into %ls (%dx%d)\n", name, kOutW, kOutH);
    } else {
        Log("[SKY] %ls COULD NOT be written\n", name);
    }
}

// A frame round n: two directions across it.
void Frame(const float n[3], float t[3], float b[3]) {
    const float up[3] = { fabsf(n[2]) < 0.999f ? 0.0f : 1.0f, 0.0f, fabsf(n[2]) < 0.999f ? 1.0f : 0.0f };
    t[0] = up[1] * n[2] - up[2] * n[1];
    t[1] = up[2] * n[0] - up[0] * n[2];
    t[2] = up[0] * n[1] - up[1] * n[0];
    Normalize(t);
    b[0] = n[1] * t[2] - n[2] * t[1];
    b[1] = n[2] * t[0] - n[0] * t[2];
    b[2] = n[0] * t[1] - n[1] * t[0];
}

// The sky's light toward n for perceptual roughness r: GGX samples round the mirror direction (taken as the normal and
// the view, as a prefiltered map is), each read from the level whose texel covers the sample's share of the lobe.
void Prefilter(const Cube& cube, const float n[3], float r, float out[3]) {
    const int kSamples = 96;
    const float a = r * r, a2 = a * a;
    const float texel = 4.0f * kPi / (6.0f * kFace * kFace);
    float t[3], b[3];
    Frame(n, t, b);
    float sum[3] = {}, weight = 0.0f;
    for (int i = 0; i < kSamples; ++i) {
        // Hammersley: i / N and i's bits reversed.
        uint32_t bits = (uint32_t)i;
        bits = (bits << 16) | (bits >> 16);
        bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
        bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
        bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
        bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
        const float e1 = (i + 0.5f) / kSamples, e2 = bits * 2.3283064365386963e-10f;
        const float cosH = sqrtf((1.0f - e1) / (1.0f + (a2 - 1.0f) * e1));
        const float sinH = sqrtf(1.0f - cosH * cosH);
        const float phi = 2.0f * kPi * e2;
        float h[3];
        for (int c = 0; c < 3; ++c) h[c] = t[c] * sinH * cosf(phi) + b[c] * sinH * sinf(phi) + n[c] * cosH;
        const float nDotH = cosH;
        float l[3];
        for (int c = 0; c < 3; ++c) l[c] = 2.0f * nDotH * h[c] - n[c];
        const float nDotL = Dot3(n, l);
        if (nDotL <= 0.0f) continue;
        // pdf of l: D (n.h) / (4 (v.h)), v = n.
        const float q = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
        const float pdf = a2 / (kPi * q * q) * 0.25f;
        const float share = 1.0f / (kSamples * pdf + 1e-6f);
        const float lod = 0.5f * log2f(share / texel) + 1.0f;
        float c[3];
        SampleLod(cube, lod, l, c);
        for (int k = 0; k < 3; ++k) sum[k] += c[k] * nDotL;
        weight += nDotL;
    }
    for (int k = 0; k < 3; ++k) out[k] = weight > 0.0f ? sum[k] / weight : 0.0f;
}

// The sky light's mips from the sunless cube.
std::vector<std::vector<float>> SkyLight(const Cube& cube) {
    std::vector<std::vector<float>> mips;
    // The cube's 32 level as small directions with their light times their solid angle: the diffuse light's sum.
    int level32 = 0;
    while (level32 + 1 < (int)cube.sizes.size() && cube.sizes[level32] > 32) ++level32;
    const int sn = cube.sizes[level32];
    std::vector<float> dirs, lights;
    for (int f = 0; f < 6; ++f)
        for (int y = 0; y < sn; ++y)
            for (int x = 0; x < sn; ++x) {
                const float s = (x + 0.5f) / sn * 2.0f - 1.0f, t = 1.0f - (y + 0.5f) / sn * 2.0f;
                float d[3];
                for (int c = 0; c < 3; ++c) d[c] = kFaces[f][0][c] + kFaces[f][2][c] * s + kFaces[f][1][c] * t;
                const float r2 = 1.0f + s * s + t * t;
                const float omega = (4.0f / (sn * sn)) / (r2 * sqrtf(r2));
                Normalize(d);
                const float* p = cube.levels[level32].data() + (((size_t)f * sn + y) * sn + x) * 4;
                dirs.insert(dirs.end(), d, d + 3);
                for (int c = 0; c < 3; ++c) lights.push_back(p[c] * omega);
            }
    for (int m = 0; m < kLightMips; ++m) {
        const int w = kLightW >> m > 0 ? kLightW >> m : 1, h = kLightH >> m > 0 ? kLightH >> m : 1;
        std::vector<float> img((size_t)w * h * 4, 1.0f);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float d[3], c[3] = {};
                EquirectDir((x + 0.5f) / w, (y + 0.5f) / h, d);
                if (m == 0) {
                    // A mirror: the level whose texel is this texel's size (2 pi / w across; a face's texel at level
                    // L, pi / 2 / (512 >> L)).
                    SampleLod(cube, log2f(4.0f * kFace / w), d, c);
                } else if (m < kPrefiltered) {
                    Prefilter(cube, d, (float)m / (kPrefiltered - 1), c);
                } else {
                    for (size_t k = 0; k < lights.size() / 3; ++k) {
                        const float cosine = Dot3(d, &dirs[k * 3]);
                        if (cosine <= 0.0f) continue;
                        for (int j = 0; j < 3; ++j) c[j] += lights[k * 3 + j] * cosine;
                    }
                    for (int j = 0; j < 3; ++j) c[j] /= kPi;
                }
                for (int j = 0; j < 3; ++j) img[((size_t)y * w + x) * 4 + j] = c[j];
            }
        mips.push_back(std::move(img));
    }
    return mips;
}

void Work(std::vector<float> withSun, std::vector<float> sunless, bool hasSunless, float seed, uint64_t hash) {
    const ULONGLONG began = GetTickCount64();
    {
        const Cube c = Pyramid(std::move(withSun), kFace);
        WriteHdr(c, seed, hash);
        if (!hasSunless) sunless = c.levels[0];
    }
    const Cube light = Pyramid(std::move(sunless), 8);
    std::vector<std::vector<float>> mips = SkyLight(light);
    if (g_dump.load()) {
        for (int m = 0; m < kLightMips; ++m) {
            const int w = kLightW >> m > 0 ? kLightW >> m : 1, h = kLightH >> m > 0 ? kLightH >> m : 1;
            std::vector<uint8_t> px((size_t)w * h * 4);
            for (size_t i = 0; i < (size_t)w * h; ++i) Rgbe(&mips[m][i * 4], &px[i * 4]);
            wchar_t name[64];
            swprintf(name, 64, L"s2fm_sky\\light_mip%d.hdr", m);
            FILE* f = nullptr;
            if (_wfopen_s(&f, CorePath(name).c_str(), L"wb") == 0 && f) {
                fprintf(f, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n", h, w);
                fwrite(px.data(), 1, px.size(), f);
                fclose(f);
            }
        }
        Log("[SKY] the sky light's mips written into s2fm_sky\\light_mip*.hdr\n");
    }
    {
        std::lock_guard<std::mutex> lk(g_lightMutex);
        g_lightMips = std::move(mips);
        g_lightReady = true;
    }
    Log("[SKY] the sky light made%s, %llu ms\n", hasSunless ? " (its sun's disk left out)" : " (with its sun's disk)",
        (unsigned long long)(GetTickCount64() - began));
    g_working.store(false);
}

void Bake(ID3D11DeviceContext* ctx, ContextState* st, const std::vector<uint8_t>& cb) {
    float pv[204];
    UINT bytes = 0;
    int slotsVs[14], slotsPs[14], nVs = 0, nPs = 0;
    for (int k = 0; k < 14; ++k) {
        D3D11_BUFFER_DESC d = {};
        if (st->vsCb[k]) { st->vsCb[k]->GetDesc(&d); if (d.ByteWidth == 816) slotsVs[nVs++] = k; }
        if (st->psCb[k]) { st->psCb[k]->GetDesc(&d); if (d.ByteWidth == 816) slotsPs[nPs++] = k; }
    }
    if (!nVs || !CoreCbRead(st->vsCb[slotsVs[0]], 0, pv, sizeof(pv), &bytes) || bytes != 816) {
        Log("[SKY] no PerView buffer readable at the sky's draw: not baked\n");
        return;
    }
    // The camera in the projection's space, and a check of the matrix's form: the camera's own clip w is 0 there.
    float c[3];
    for (int k = 0; k < 3; ++k) c[k] = pv[PV_CAMERA_POSITION_WS + k] + pv[PV_WORLD_TO_CAM_OFF_MV + k];
    const float* m = pv + PV_MAT_WORLD_TO_PROJ_MV;
    const float w = c[0] * m[3] + c[1] * m[7] + c[2] * m[11] + m[15];
    if (fabsf(w) > 50.0f) {
        Log("[SKY] the engine's projection is not of the form expected (the camera's w %g): not baked\n", w);
        return;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    bool made = Make(dev);
    // The material's constants with the sun's disk at 0, for the light's cube.
    uint8_t reg = 0;
    uint16_t off = 0;
    const bool sunless = made && ShaderDbFind(st->psLayout, "g_flSunDisk", &reg, &off) && reg == SDB_REG_FLOAT4 &&
                         (size_t)off * 4 + 4 <= cb.size() && cb.size() % 16 == 0;
    if (sunless && (!g_sunless || g_sunlessBytes != cb.size())) {
        Drop(g_sunless);
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (UINT)cb.size();
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_sunlessBytes = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_sunless)) ? (UINT)cb.size() : 0;
    }
    dev->Release();
    if (!made) return;

    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&nVp, vps);
    ID3D11RasterizerState* oldRaster = nullptr;
    ctx->RSGetState(&oldRaster);
    ID3D11Buffer* oldVs[14] = {}, *oldPs[14] = {}, *oldMaterial = nullptr;
    for (int i = 0; i < nVs; ++i) ctx->VSGetConstantBuffers(slotsVs[i], 1, &oldVs[i]);
    for (int i = 0; i < nPs; ++i) ctx->PSGetConstantBuffers(slotsPs[i], 1, &oldPs[i]);
    ctx->PSGetConstantBuffers(0, 1, &oldMaterial);

    const D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)kFace, (float)kFace, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_raster);
    for (int i = 0; i < nVs; ++i) ctx->VSSetConstantBuffers(slotsVs[i], 1, &g_perView);
    for (int i = 0; i < nPs; ++i) ctx->PSSetConstantBuffers(slotsPs[i], 1, &g_perView);
    const bool second = sunless && g_sunless;
    if (second) {
        std::vector<uint8_t> copy = cb;
        memset(copy.data() + (size_t)off * 4, 0, 4);
        ctx->UpdateSubresource(g_sunless, 0, nullptr, copy.data(), 0, 0);
    }
    bool drawn = true;
    for (int k = 0; k < (second ? 2 : 1) && drawn; ++k) {
        if (k == 1) ctx->PSSetConstantBuffers(0, 1, &g_sunless);
        for (int f = 0; f < 6 && drawn; ++f) {
            float face[204];
            memcpy(face, pv, sizeof(face));
            FaceMatrix(f, c, face + PV_MAT_WORLD_TO_PROJ_MV);
            face[PV_VIEWPORT_SIZE] = face[PV_VIEWPORT_SIZE + 1] = (float)kFace;
            ctx->UpdateSubresource(g_perView, 0, nullptr, face, 0, 0);
            const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            ctx->ClearRenderTargetView(g_faceRtv[k][f], black);
            ctx->OMSetRenderTargets(1, &g_faceRtv[k][f], nullptr);
            drawn = CoreRedraw(ctx, st);
        }
    }

    ctx->PSSetConstantBuffers(0, 1, &oldMaterial);
    Drop(oldMaterial);
    for (int i = 0; i < nVs; ++i) { ctx->VSSetConstantBuffers(slotsVs[i], 1, &oldVs[i]); Drop(oldVs[i]); }
    for (int i = 0; i < nPs; ++i) { ctx->PSSetConstantBuffers(slotsPs[i], 1, &oldPs[i]); Drop(oldPs[i]); }
    ctx->RSSetState(oldRaster);
    Drop(oldRaster);
    ctx->RSSetViewports(nVp, vps);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    for (auto& r : rtvs) Drop(r);
    Drop(dsv);
    if (drawn) {
        for (int k = 0; k < (second ? 2 : 1); ++k) ctx->CopyResource(g_staging[k], g_cube[k]);
        g_readPending = true;
        g_readTries = 0;
        g_sunlessDrawn = second;
    } else {
        Log("[SKY] the sky's draw is from a buffer (its counts unknown): not baked\n");
    }
    t_CorePassThrough = pass;
}

// The sky light bound to a shader of ours, and what the engine had there, put back after the draw.
struct LightSwap {
    int slot = -1;
    ID3D11ShaderResourceView* engine = nullptr;
};
thread_local LightSwap t_swap;

void BindLight(ID3D11DeviceContext* ctx, ContextState* st) {
    static thread_local int t_layout = -2;
    static thread_local int t_slot = -1;
    if (st->psLayout != t_layout) {
        t_layout = st->psLayout;
        uint8_t reg = 0;
        uint16_t off = 0;
        t_slot = st->psLayout >= 0 && ShaderDbFind(st->psLayout, "g_tSkyLight", &reg, &off) && reg == SDB_REG_TEXTURE
                     ? (int)(off & 0xFF) : -1;
        // Once a shader: whether it takes the sky light, and where.
        const char* name = st->psLayout >= 0 ? ShaderDbName(st->psLayout) : "";
        if (!strncmp(name, "s2vfx_", 6)) {
            static std::mutex m;
            static std::vector<std::string> said;
            std::lock_guard<std::mutex> lk(m);
            if (std::find(said.begin(), said.end(), name) == said.end()) {
                said.push_back(name);
                Log("[SKY] %s: the sky light %s (slot %d)\n", name, t_slot >= 0 ? "taken" : "not declared", t_slot);
            }
        }
    }
    if (t_slot < 0 || t_slot >= 32) return;
    ID3D11ShaderResourceView* light = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lightMutex);
        light = g_lightSrv;
        if (light) light->AddRef();
    }
    if (!light) return;
    static std::atomic<bool> s_first{true};
    if (s_first.exchange(false)) Log("[SKY] the sky light bound, first at slot %d\n", t_slot);
    t_swap.slot = t_slot;
    t_swap.engine = st->psSrv[t_slot];
    if (t_swap.engine) t_swap.engine->AddRef();
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    ctx->PSSetShaderResources((UINT)t_slot, 1, &light);
    t_CorePassThrough = pass;
    light->Release();
}

void MakeLight(ID3D11Device* dev);

void BeforeDraw(ID3D11DeviceContext* ctx, ContextState* st) {
    // The sky light's texture as soon as its mips are there, on the engine's own thread: a viewport left alone
    // presents nothing, so a present may be long in coming.
    if (!st->deferred && g_lightReady.load()) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            MakeLight(dev);
            dev->Release();
        }
    }
    BindLight(ctx, st);
    if (st->deferred || st->psLayout < 0 || st->rtFormat == DXGI_FORMAT_UNKNOWN || !st->psCb[0]) return;
    static thread_local int t_layout = -2;
    static thread_local bool t_sky = false;
    if (st->psLayout != t_layout) {
        t_layout = st->psLayout;
        t_sky = !strcmp(ShaderDbName(t_layout), "s2vfx_sky");
    }
    if (!t_sky || g_readPending || g_working.load() || CoreFrameForeign(st)) return;
    // What the sky looks like is its material's constants (and its variant): baked again when they change.
    UINT bytes = 0;
    if (!CoreCbRead(st->psCb[0], 0, nullptr, 0, &bytes) || !bytes) return;
    std::vector<uint8_t> cb(bytes);
    if (!CoreCbRead(st->psCb[0], 0, cb.data(), bytes)) return;
    const uint64_t hash = Fnv(&st->psLayout, sizeof(st->psLayout), Fnv(cb.data(), cb.size()));
    const bool forced = g_forced.exchange(false);
    if (hash == g_bakedHash && !forced) return;
    uint8_t reg = 0;
    uint16_t off = 0;
    float seed = 0.0f;
    if (ShaderDbFind(st->psLayout, "g_flSeed", &reg, &off) && reg == SDB_REG_FLOAT4 && (size_t)off * 4 + 4 <= cb.size())
        memcpy(&seed, cb.data() + (size_t)off * 4, 4);
    g_bakedHash = hash;
    g_pendingSeed = seed;
    Log("[SKY] the sky's settings %s (seed %g): baking\n", forced ? "asked for again" : "are new", seed);
    Bake(ctx, st, cb);
}

void AfterDraw(ID3D11DeviceContext* ctx, ContextState*) {
    if (t_swap.slot < 0) return;
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    ctx->PSSetShaderResources((UINT)t_swap.slot, 1, &t_swap.engine);
    t_CorePassThrough = pass;
    Drop(t_swap.engine);
    t_swap.slot = -1;
}

// The sky light's texture, from the mips the thread made.
void MakeLight(ID3D11Device* dev) {
    std::vector<std::vector<float>> mips;
    {
        std::lock_guard<std::mutex> lk(g_lightMutex);
        if (!g_lightReady) return;
        g_lightReady = false;
        mips = std::move(g_lightMips);
    }
    // Dev word skylightmips: each mip a colour of its own (mip 0 a checker, 1 red, 2 green, 3 blue, 4 yellow, 5
    // magenta, 6 cyan, 7 white, 8 orange, 9 grey), so a surface shows which mip it reads.
    if (g_mips.load()) {
        static const float kColours[9][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, 0, 1},
                                             {0, 1, 1}, {1, 1, 1}, {1, 0.5f, 0}, {0.3f, 0.3f, 0.3f}};
        for (int m = 0; m < kLightMips; ++m) {
            const int w = kLightW >> m > 0 ? kLightW >> m : 1;
            for (size_t i = 0; i < mips[m].size() / 4; ++i) {
                const int x = (int)(i % w), y = (int)(i / w);
                for (int c = 0; c < 3; ++c)
                    mips[m][i * 4 + c] = m == 0 ? (((x / 8) + (y / 8)) % 2 ? 1.0f : 0.05f)
                                                : kColours[m - 1][c];
            }
        }
        Log("[SKY] the sky light's mips coloured (skylightmips)\n");
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = kLightW;
    d.Height = kLightH;
    d.MipLevels = kLightMips;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init[kLightMips] = {};
    for (int m = 0; m < kLightMips; ++m) {
        init[m].pSysMem = mips[m].data();
        init[m].SysMemPitch = (UINT)((kLightW >> m > 0 ? kLightW >> m : 1) * 16);
    }
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, init, &tex)) || FAILED(dev->CreateShaderResourceView(tex, nullptr, &srv))) {
        Drop(tex);
        Log("[SKY] the sky light's texture COULD NOT be made\n");
        return;
    }
    tex->Release();
    std::lock_guard<std::mutex> lk(g_lightMutex);
    Drop(g_lightSrv);
    g_lightSrv = srv;
    Log("[SKY] the sky light's texture made (%dx%d, %d mips)\n", kLightW, kLightH, kLightMips);
}

// At each present: the staging copies once the GPU has made them, and the sky light once the thread has made it.
void Present(IDXGISwapChain* sc) {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 500) {
        last = now;
        FILE* f = nullptr;
        static bool was = false;
        bool is = false;
        bool dump = false, mips = false;
        if (_wfopen_s(&f, CorePath(L"s2fm_refract_debug.txt").c_str(), L"r") == 0 && f) {
            char b[200] = {};
            if (fgets(b, sizeof(b), f)) {
                is = strstr(b, "skybake") != nullptr;
                dump = strstr(b, "skylightdump") != nullptr;
                mips = strstr(b, "skylightmips") != nullptr;
            }
            fclose(f);
        }
        g_dump.store(dump);
        g_mips.store(mips);
        if (is && !was) g_forced.store(true);
        was = is;
    }
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
    MakeLight(dev);
    if (!g_readPending || !g_staging[0]) { dev->Release(); return; }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    dev->Release();
    if (!ctx) return;
    const int count = g_sunlessDrawn ? 2 : 1;
    std::vector<float> faces[2];
    bool all = true;
    const bool pass = t_CorePassThrough;
    t_CorePassThrough = true;
    // Waited for only after a few presents: the GPU is a frame or two behind.
    const UINT flags = ++g_readTries < 8 ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0;
    for (int k = 0; k < count && all; ++k) {
        faces[k].resize((size_t)6 * kFace * kFace * 4);
        for (UINT f = 0; f < 6 && all; ++f) {
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (FAILED(ctx->Map(g_staging[k], f, D3D11_MAP_READ, flags, &m))) { all = false; break; }
            for (UINT y = 0; y < kFace; ++y)
                memcpy(&faces[k][((size_t)f * kFace + y) * kFace * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch,
                       kFace * 16);
            ctx->Unmap(g_staging[k], f);
        }
    }
    t_CorePassThrough = pass;
    ctx->Release();
    if (!all) {
        if (g_readTries >= 8) { g_readPending = false; Log("[SKY] the bake COULD NOT be read back\n"); }
        return;
    }
    g_readPending = false;
    g_working.store(true);
    std::thread(Work, std::move(faces[0]), std::move(faces[1]), count == 2, g_pendingSeed, g_bakedHash).detach();
}

const CoreFeature kSkyBake = {
    "sky bake",
    BeforeDraw,
    AfterDraw,
    nullptr,
    Present,
    nullptr,
    nullptr,
    nullptr,
};

}  // namespace

void SkyBakeRegister() { CoreRegister(&kSkyBake); }
