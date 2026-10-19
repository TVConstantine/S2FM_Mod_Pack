// The ReShade core: ReShade 6.8.0's runtime and effect compiler (third_party/reshade, fetched by
// tools/reshade_sdk.py) without its hooks, overlay or other graphics APIs, run by S2FM's frame through the
// stage points (reshade/stages.cpp).
#define RS_CORE_EXPORTS
#include "rs_core_api.h"

#include <windows.h>
#include <filesystem>

// What ReShade's dll_main.cpp would define: the module (its embedded shaders are found through it) and the
// paths a relative path in a config resolves against (ReShade's own base: the folder its DLL is in).
HMODULE g_module_handle = nullptr;
std::filesystem::path g_reshade_dll_path;
std::filesystem::path g_reshade_base_path;
std::filesystem::path g_target_executable_path;

// d3d11_impl_device.cpp asks this once, for its driver extensions.
bool is_windows7() { return false; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module_handle = module;
        WCHAR path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        g_reshade_dll_path = path;
        g_reshade_base_path = g_reshade_dll_path.parent_path();
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        g_target_executable_path = path;
    }
    return TRUE;
}

RS_API int RsCore_ApiVersion() { return RS_CORE_API_VERSION; }

// --- The harness's way in.
#include "rs_runtime.h"
#include "rs_stage_io.h"
#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <string>
#include <vector>

namespace {
std::vector<std::pair<std::string, std::string>> ParseDefinitions(const char* text) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!text) return out;
    std::string s = text;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(',', start);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(start, end - start);
        const size_t eq = item.find('=');
        if (!item.empty()) out.emplace_back(item.substr(0, eq), eq == std::string::npos ? "1" : item.substr(eq + 1));
        start = end + 1;
    }
    return out;
}
ID3D11Device* g_harnessDev = nullptr;
ID3D11DeviceContext* g_harnessCtx = nullptr;
}  // namespace

RS_API int RsCore_HarnessInit(ID3D11Device* dev, ID3D11DeviceContext* ctx, unsigned width, unsigned height,
                              const char* configPath, const char* definitions) {
    g_harnessDev = dev;
    g_harnessCtx = ctx;
    const auto defs = ParseDefinitions(definitions);
    return rs::core_init(dev, ctx, width, height, configPath ? configPath : "", definitions ? &defs : nullptr) ? 1 : 0;
}

RS_API int RsCore_HarnessWaitLoaded(unsigned timeoutMs) { return rs::core_wait_loaded(timeoutMs) ? 1 : 0; }

RS_API int RsCore_HarnessUpload(const float* rgba, unsigned width, unsigned height) {
    ID3D11Texture2D* back = rs::core_back_buffer();
    if (!back || width != rs::core_width() || height != rs::core_height()) return 0;
    std::vector<uint8_t> bytes((size_t)width * height * 4);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const float v = rgba[i] < 0.0f ? 0.0f : rgba[i] > 1.0f ? 1.0f : rgba[i];
        bytes[i] = (uint8_t)(v * 255.0f + 0.5f);
    }
    g_harnessCtx->UpdateSubresource(back, 0, nullptr, bytes.data(), width * 4, 0);
    return 1;
}

RS_API int RsCore_HarnessDownload(float* rgba, unsigned width, unsigned height) {
    ID3D11Texture2D* back = rs::core_back_buffer();
    if (!back || width != rs::core_width() || height != rs::core_height()) return 0;
    D3D11_TEXTURE2D_DESC d = {};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_harnessDev->CreateTexture2D(&d, nullptr, &st)) || !st) return 0;
    g_harnessCtx->CopyResource(st, back);
    D3D11_MAPPED_SUBRESOURCE m = {};
    int ok = 0;
    if (SUCCEEDED(g_harnessCtx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        for (unsigned y = 0; y < height; ++y) {
            const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
            for (unsigned x = 0; x < width * 4; ++x) rgba[(size_t)y * width * 4 + x] = row[x] / 255.0f;
        }
        g_harnessCtx->Unmap(st, 0);
        ok = 1;
    }
    st->Release();
    return ok;
}

RS_API int RsCore_HarnessRun(double seconds, unsigned long long frameCount, unsigned seed, const char* techniques) {
    rs::clock c;
    c.seconds = seconds;
    c.frameCount = frameCount;
    c.randomSeed = seed;
    return rs::core_run_stage(c, rs::core_filter_from_names(techniques ? techniques : "")) ? 1 : 0;
}

RS_API int RsCore_HarnessReadTexture(const char* name, float* rgba, unsigned width, unsigned height) {
    std::vector<float> v;
    uint32_t w = 0, h = 0;
    if (!rs::core_read_effect_texture(name, v, &w, &h) || w != width || h != height) return 0;
    memcpy(rgba, v.data(), v.size() * sizeof(float));
    return 1;
}

RS_API void RsCore_HarnessShutdown() { rs::core_shutdown(); }

RS_API int RsCore_HarnessStage(const void* frame, const char* techniques) {
    const DxrStageFrame& f = *static_cast<const DxrStageFrame*>(frame);
    if (f.size != sizeof(DxrStageFrame)) return 0;
    rs::clock c;
    c.seconds = f.seconds;
    c.frameCount = f.filmFrame;
    c.randomSeed = f.sampleIndex;
    return rs::core_stage(f, c, rs::core_filter_from_names(techniques ? techniques : "")) ? 1 : 0;
}

RS_API int RsCore_HarnessDepth(const float* viewZ, unsigned width, unsigned height, double nearPlane, double farPlane) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { viewZ, width * 4, 0 };
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(g_harnessDev->CreateTexture2D(&d, &init, &tex)) || !tex) return 0;
    g_harnessDev->CreateShaderResourceView(tex, nullptr, &srv);
    rs::depth_scale sc;
    sc.nearPlane = nearPlane;
    sc.farPlane = farPlane;
    const bool ok = srv && rs::depth_encode(g_harnessCtx, srv, width, height, sc);
    if (ok) rs::core_bind_semantic("DEPTH", rs::depth_srv());
    if (srv) srv->Release();
    tex->Release();
    return ok ? 1 : 0;
}

RS_API int RsCore_TechniqueCount() { return (int)rs::core_technique_names(nullptr, nullptr).size(); }

RS_API int RsCore_TechniqueName(int i, char* out, int size, int* enabled) {
    std::vector<bool> en;
    const std::vector<std::string> names = rs::core_technique_names(&en, nullptr);
    if (i < 0 || i >= (int)names.size() || !out || size <= 0) return 0;
    strncpy_s(out, size, names[i].c_str(), _TRUNCATE);
    if (enabled) *enabled = en[i] ? 1 : 0;
    return 1;
}

RS_API int RsCore_Errors(char* out, int size) {
    const std::string e = rs::core_errors();
    if (out && size > 0) strncpy_s(out, size, e.c_str(), _TRUNCATE);
    return (int)e.size();
}
