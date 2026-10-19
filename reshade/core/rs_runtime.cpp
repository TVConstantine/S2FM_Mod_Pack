// The core's ReShade runtime (rs_runtime.h).
#include "rs_runtime.h"
#include "rs_vswap.h"
#include "rs_stage_io.h"

#include "runtime.hpp"
#include "dll_log.hpp"
#include "d3d11/d3d11_impl_device.hpp"
#include "d3d11/d3d11_impl_device_context.hpp"
#include "d3d11/d3d11_impl_state_block.hpp"

#include <windows.h>
#include <chrono>
#include <thread>

extern std::filesystem::path g_reshade_base_path;

namespace {

struct core_state {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    reshade::d3d11::device_impl* device = nullptr;
    reshade::d3d11::device_context_impl* queue = nullptr;
    rs::virtual_swapchain* swap = nullptr;
    reshade::runtime* runtime = nullptr;
    bool initialised = false;
    rs::depth_scale depthScale;
};
core_state g;
void TimersRelease();   // the stage timers (below)

reshade::api::resource_view View(ID3D11View* v) { return { reinterpret_cast<uintptr_t>(v) }; }

void OpenLogOnce() {
    static bool opened = false;
    if (opened) return;
    opened = true;
    std::error_code ec;
    reshade::log::open_log_file(g_reshade_base_path / L"s2fm_reshade.log", ec);
}

}  // namespace

void rs::core_open_log() { OpenLogOnce(); }

bool rs::core_init(ID3D11Device* dev, ID3D11DeviceContext* immediate, uint32_t width, uint32_t height,
                   const std::string& configPath, const std::vector<std::pair<std::string, std::string>>* definitions) {
    core_shutdown();
    OpenLogOnce();
    g.dev = dev;
    g.ctx = immediate;
    dev->AddRef();
    immediate->AddRef();
    g.device = new reshade::d3d11::device_impl(dev);
    g.queue = new reshade::d3d11::device_context_impl(g.device, immediate);
    g.swap = new rs::virtual_swapchain(g.device, dev, width, height);
    if (!g.swap->ok()) {
        reshade::log::message(reshade::log::level::error, "S2FM: the virtual back buffer (%ux%u) could not be made.", width, height);
        core_shutdown();
        return false;
    }
    g.runtime = new reshade::runtime(g.swap, g.queue, std::filesystem::u8path(configPath), false);
    // The constructor read the config's own definitions; ours replace them before on_init loads the effects.
    if (definitions) g.runtime->s2fm_set_global_definitions(*definitions);
    g.depthScale = rs::depth_scale_from(g.runtime->s2fm_global_definitions());
    g.initialised = g.runtime->on_init();
    if (!g.initialised) {
        reshade::log::message(reshade::log::level::error, "S2FM: the runtime did not initialise (%ux%u).", width, height);
        core_shutdown();
        return false;
    }
    // The first frame begun starts loading the effects (on worker threads; what they compile is made on this
    // thread by the frames after).
    core_pump();
    return true;
}

void rs::core_shutdown() {
    TimersRelease();
    rs::stage_io_release();
    rs::depth_release();
    if (g.runtime) {
        if (g.initialised) g.runtime->on_reset();
        delete g.runtime;
    }
    delete g.swap;
    delete g.queue;
    delete g.device;
    if (g.ctx) g.ctx->Release();
    if (g.dev) g.dev->Release();
    g = core_state();
}

bool rs::core_ready() { return g.runtime && g.initialised; }
uint32_t rs::core_width() { return g.swap ? g.swap->width() : 0; }
uint32_t rs::core_height() { return g.swap ? g.swap->height() : 0; }
bool rs::core_loading() { return g.runtime && g.runtime->is_loading(); }
ID3D11Texture2D* rs::core_back_buffer() { return g.swap ? g.swap->texture() : nullptr; }
ID3D11RenderTargetView* rs::core_back_rtv() { return g.swap ? g.swap->rtv() : nullptr; }
ID3D11ShaderResourceView* rs::core_back_srv() { return g.swap ? g.swap->srv() : nullptr; }

bool rs::core_wait_loaded(uint32_t timeoutMs) {
    if (!core_ready()) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    reshade::runtime::s2fm_clock c = { 0.0, 0, 0, 1000.0f / 24.0f };
    g.runtime->s2fm_begin_frame(c);       // the first frame begun starts the loading
    while (g.runtime->is_loading()) {
        g.runtime->s2fm_begin_frame(c);   // what is compiled is made on this thread
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    g.runtime->s2fm_begin_frame(c);
    return true;
}

void rs::core_pump() {
    if (!core_ready()) return;
    reshade::runtime::s2fm_clock c = { 0.0, 0, 0, 1000.0f / 24.0f };
    g.runtime->s2fm_begin_frame(c);
}

bool rs::core_run_stage(const clock& c, const std::vector<bool>& techniques) {
    if (!core_ready()) return false;
    const reshade::runtime::s2fm_clock rc = { c.seconds, c.frameCount, c.randomSeed, c.frameTimeMs };
    g.runtime->s2fm_begin_frame(rc);
    g.runtime->s2fm_render_stage(g.queue, View(g.swap->rtv()), View(g.swap->rtv_srgb()), techniques);
    return true;
}

// --- What each stage costs on the card: a timestamp pair inside a disjoint query, read back a few calls later
// without waiting, smoothed. Logged every 10 s and given to the panel (core_stage_ms).
namespace {
struct StageTimer {
    static const int kRing = 8;
    ID3D11Query* disjoint[kRing] = {};
    ID3D11Query* t0[kRing] = {};
    ID3D11Query* t1[kRing] = {};
    bool pending[kRing] = {};
    int next = 0;
    double ms = 0.0;
    int samples = 0;
};
StageTimer g_timers[3];
ULONGLONG g_timerSaid = 0;

bool TimerMake(StageTimer& t, int k) {
    if (t.disjoint[k]) return true;
    D3D11_QUERY_DESC d = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC ts = { D3D11_QUERY_TIMESTAMP, 0 };
    return SUCCEEDED(g.dev->CreateQuery(&d, &t.disjoint[k])) && SUCCEEDED(g.dev->CreateQuery(&ts, &t.t0[k]))
        && SUCCEEDED(g.dev->CreateQuery(&ts, &t.t1[k]));
}

void TimerCollect(ID3D11DeviceContext* ctx, StageTimer& t) {
    for (int k = 0; k < StageTimer::kRing; ++k) {
        if (!t.pending[k]) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        UINT64 a = 0, b = 0;
        if (ctx->GetData(t.disjoint[k], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx->GetData(t.t0[k], &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx->GetData(t.t1[k], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        t.pending[k] = false;
        if (dj.Disjoint || !dj.Frequency || b < a) continue;
        const double ms = (double)(b - a) * 1000.0 / (double)dj.Frequency;
        t.ms = t.samples ? t.ms * 0.95 + ms * 0.05 : ms;
        ++t.samples;
    }
}

void TimersRelease() {
    for (StageTimer& t : g_timers) {
        for (int k = 0; k < StageTimer::kRing; ++k) {
            if (t.disjoint[k]) t.disjoint[k]->Release();
            if (t.t0[k]) t.t0[k]->Release();
            if (t.t1[k]) t.t1[k]->Release();
        }
        t = StageTimer();
    }
}
}  // namespace

double rs::core_stage_ms(int stage) { return stage >= 0 && stage < 3 ? g_timers[stage].ms : 0.0; }

bool rs::core_stage(const DxrStageFrame& f, const clock& c, const std::vector<bool>& techniques) {
    if (!core_ready() || !f.ctx || f.width != core_width() || f.height != core_height()) return false;
    StageTimer* timer = f.stage >= 0 && f.stage < 3 ? &g_timers[f.stage] : nullptr;
    int slot = -1;
    if (timer) {
        TimerCollect(f.ctx, *timer);
        slot = timer->next;
        if (timer->pending[slot] || !TimerMake(*timer, slot)) slot = -1;
        else timer->next = (timer->next + 1) % StageTimer::kRing;
    }
    if (slot >= 0) {
        f.ctx->Begin(timer->disjoint[slot]);
        f.ctx->End(timer->t0[slot]);
    }
    reshade::d3d11::state_block saved(g.dev);
    saved.capture(f.ctx);
    f.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    bool ok = rs::stage_copy_in(f.ctx, f, g.swap->rtv());
    if (ok) {
        if (f.viewZ && rs::depth_encode(f.ctx, f.viewZ, f.width, f.height, g.depthScale))
            core_bind_semantic("DEPTH", rs::depth_srv());
        ok = core_run_stage(c, techniques);
        ok = rs::stage_copy_out(f.ctx, f, g.swap->srv()) && ok;
    }
    saved.apply_and_release();
    if (slot >= 0) {
        f.ctx->End(timer->t1[slot]);
        f.ctx->End(timer->disjoint[slot]);
        timer->pending[slot] = true;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_timerSaid >= 10000) {
        g_timerSaid = now;
        reshade::log::message(reshade::log::level::info, "S2FM: stage cost on the card, ms: A %.3f, B %.3f, C %.3f",
                              g_timers[0].ms, g_timers[1].ms, g_timers[2].ms);
    }
    return ok;
}

size_t rs::core_technique_count() { return core_ready() ? g.runtime->s2fm_technique_count() : 0; }

std::vector<std::string> rs::core_technique_names(std::vector<bool>* enabled, std::vector<size_t>* indices) {
    struct Ctx { std::vector<std::string> names; std::vector<bool>* enabled; std::vector<size_t>* indices; };
    Ctx x = { {}, enabled, indices };
    if (enabled) enabled->clear();
    if (indices) indices->clear();
    if (!core_ready()) return x.names;
    g.runtime->enumerate_techniques(nullptr, [](reshade::api::effect_runtime* rt, reshade::api::effect_technique t, void* user) {
        Ctx& x = *static_cast<Ctx*>(user);
        char name[256] = "", effect[256] = "";
        size_t n = sizeof(name), e = sizeof(effect);
        rt->get_technique_name(t, name, &n);
        rt->get_technique_effect_name(t, effect, &e);
        x.names.push_back(std::string(name) + "@" + effect);
        if (x.enabled) x.enabled->push_back(rt->get_technique_state(t));
        if (x.indices) x.indices->push_back(static_cast<reshade::runtime*>(rt)->s2fm_technique_index(t));
    }, &x);
    return x.names;
}

std::vector<bool> rs::core_filter_from_names(const std::string& list) {
    const size_t n = core_technique_count();
    std::vector<bool> f(n, list.empty());
    if (list.empty() || list == "-") return f;
    std::vector<size_t> indices;
    const std::vector<std::string> names = core_technique_names(nullptr, &indices);
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(',', start);
        if (end == std::string::npos) end = list.size();
        const std::string want = list.substr(start, end - start);
        for (size_t i = 0; i < names.size(); ++i) {
            const std::string& full = names[i];
            if (full == want || full.substr(0, full.find('@')) == want) {
                if (indices[i] < n) f[indices[i]] = true;
            }
        }
        start = end + 1;
    }
    return f;
}

reshade::runtime* rs::core_runtime() { return core_ready() ? g.runtime : nullptr; }

std::vector<rs::technique_facts> rs::core_technique_facts(std::vector<size_t>* indices) {
    std::vector<technique_facts> out;
    if (indices) indices->clear();
    if (!core_ready()) return out;
    struct Ctx { std::vector<technique_facts>* out; std::vector<size_t>* indices; };
    Ctx x = { &out, indices };
    g.runtime->enumerate_techniques(nullptr, [](reshade::api::effect_runtime* rt, reshade::api::effect_technique t, void* user) {
        Ctx& x = *static_cast<Ctx*>(user);
        const reshade::runtime* r = static_cast<reshade::runtime*>(rt);
        const reshade::runtime::s2fm_technique_facts f = r->s2fm_technique_info(t);
        technique_facts tf;
        tf.name = f.name;
        tf.file = f.file;
        tf.readsDepth = f.reads_depth;
        tf.reads = f.reads;
        tf.writes = f.writes;
        x.out->push_back(tf);
        if (x.indices) x.indices->push_back(r->s2fm_technique_index(t));
    }, &x);
    return out;
}

void rs::core_set_effects_on(bool on) { if (core_ready()) g.runtime->set_effects_state(on); }

std::string rs::core_errors() { return core_ready() ? g.runtime->s2fm_errors() : std::string("the runtime is not running"); }

void rs::core_bind_semantic(const char* semantic, ID3D11ShaderResourceView* srv) {
    if (core_ready()) g.runtime->update_texture_bindings(semantic, View(srv), View(srv));
}

bool rs::core_read_effect_texture(const char* name, std::vector<float>& out, uint32_t* w, uint32_t* h) {
    if (!core_ready()) return false;
    const reshade::api::effect_texture_variable v = g.runtime->find_texture_variable(nullptr, name);
    if (v.handle == 0) return false;
    reshade::api::resource_view srvHandle = {}, srgb = {};
    g.runtime->get_texture_binding(v, &srvHandle, &srgb);
    ID3D11ShaderResourceView* srv = reinterpret_cast<ID3D11ShaderResourceView*>(srvHandle.handle);
    if (!srv) return false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    bool ok = false;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
        D3D11_TEXTURE2D_DESC d = {};
        tex->GetDesc(&d);
        D3D11_TEXTURE2D_DESC sd = d;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        if (SUCCEEDED(g.dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
            g.ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                *w = d.Width;
                *h = d.Height;
                out.assign((size_t)d.Width * d.Height * 4, 0.0f);
                for (UINT y = 0; y < d.Height; ++y) {
                    const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                    for (UINT x = 0; x < d.Width; ++x) {
                        float* o = &out[((size_t)y * d.Width + x) * 4];
                        if (d.Format == DXGI_FORMAT_R32_FLOAT || d.Format == DXGI_FORMAT_R32_TYPELESS) {
                            memcpy(o, row + (size_t)x * 4, 4);
                        } else if (d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || d.Format == DXGI_FORMAT_R32G32B32A32_TYPELESS) {
                            memcpy(o, row + (size_t)x * 16, 16);
                        }
                    }
                }
                g.ctx->Unmap(st, 0);
                ok = d.Format == DXGI_FORMAT_R32_FLOAT || d.Format == DXGI_FORMAT_R32_TYPELESS
                  || d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || d.Format == DXGI_FORMAT_R32G32B32A32_TYPELESS;
            }
            st->Release();
        }
        tex->Release();
    }
    if (res) res->Release();
    return ok;
}
