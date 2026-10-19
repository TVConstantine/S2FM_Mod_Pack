// The hooks: dxgi.dll's exports forwarded to the system's, the factory's swapchain creation, the device's creations,
// every context's draws, dispatches and bindings, the constant buffers' CPU copies, and Present.
//
// Hooks go on the functions a vtable points at (MinHook), so every object of a class shares them; a context's
// originals are kept per vtable, since the immediate and the deferred contexts are different classes.
#include "core.h"
#include "../common/shader_table.h"
#include "../minhook/include/MinHook.h"
#include "../package/version.h"

#include <tlhelp32.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

void CoreViewsPresent(IDXGISwapChain* sc);   // core_views.cpp
void CoreTableStart();                        // core_table.cpp
void CoreDevStart();                          // core_dev.cpp
void CoreFeaturesInit();                      // package/features.cpp
uint32_t CoreTableGen();

thread_local bool t_CorePassThrough = false;

namespace {

// --- What the hooks hang on objects.
// The CPU's copy of a small constant buffer, and while the engine has it open, where the real one is mapped.
const GUID kGuidShadow = { 0x5f1d0a11, 0x6c2e, 0x4b57, { 0x9a, 0x31, 0x2e, 0x44, 0x10, 0x7b, 0xc3, 0x01 } };
// A shader's bytecode as created, and which game shader it is.
const GUID kGuidBytecode = { 0x5f1d0a12, 0x6c2e, 0x4b57, { 0x9a, 0x31, 0x2e, 0x44, 0x10, 0x7b, 0xc3, 0x02 } };
const GUID kGuidTag = { 0x5f1d0a13, 0x6c2e, 0x4b57, { 0x9a, 0x31, 0x2e, 0x44, 0x10, 0x7b, 0xc3, 0x03 } };

constexpr UINT kShadowMax = 4096;

class Shadow : public IUnknown {
public:
    std::vector<uint8_t> data;
    void* real = nullptr;
    explicit Shadow(UINT bytes) : data(bytes, 0) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = (IUnknown*)this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
private:
    std::atomic<ULONG> ref_{1};
};

Shadow* ShadowOf(ID3D11Resource* res) {
    if (!res) return nullptr;
    Shadow* s = nullptr;
    UINT size = sizeof(s);
    if (FAILED(res->GetPrivateData(kGuidShadow, &size, (void**)&s))) return nullptr;
    return s;   // AddRef'd
}

struct ShaderTag {
    int32_t layout = -1;
    int32_t combo = -1;
    uint16_t flags = 0;
    uint16_t pad = 0;
    uint32_t tableGen = 0;   // the table's load this was looked up in, 0 before it was there
};

std::atomic<long long> g_shadersMade{0};
std::atomic<bool> g_deviceHooked{false};

void TagShader(ID3D11DeviceChild* sh, const void* code, SIZE_T size) {
    g_shadersMade.fetch_add(1, std::memory_order_relaxed);
    sh->SetPrivateData(kGuidBytecode, (UINT)size, code);
    ShaderTag tag;
    tag.tableGen = CoreTableGen();
    if (tag.tableGen) tag.layout = ShaderDbLookup(code, size, &tag.flags, &tag.combo);
    sh->SetPrivateData(kGuidTag, sizeof(tag), &tag);
}

// The layout of a shader being bound, looked up again when the table came after it was made.
int LayoutOf(ID3D11DeviceChild* sh, int* combo, uint16_t* flags) {
    if (combo) *combo = -1;
    if (flags) *flags = 0;
    if (!sh) return -1;
    ShaderTag tag;
    UINT size = sizeof(tag);
    if (FAILED(sh->GetPrivateData(kGuidTag, &size, &tag)) || size != sizeof(tag)) return -1;
    const uint32_t gen = CoreTableGen();
    if (gen && tag.tableGen != gen) {
        UINT n = 0;
        if (SUCCEEDED(sh->GetPrivateData(kGuidBytecode, &n, nullptr)) && n) {
            std::vector<uint8_t> code(n);
            if (SUCCEEDED(sh->GetPrivateData(kGuidBytecode, &n, code.data())))
                tag.layout = ShaderDbLookup(code.data(), n, &tag.flags, &tag.combo);
        }
        tag.tableGen = gen;
        sh->SetPrivateData(kGuidTag, sizeof(tag), &tag);
    }
    if (combo) *combo = tag.combo;
    if (flags) *flags = tag.flags;
    return tag.layout;
}

// --- Installing.
std::recursive_mutex g_hookMutex;
std::unordered_map<void*, void*> g_trampolines;   // target -> trampoline: MinHook will not hook an address twice

bool Install(void* target, void* detour, void** original) {
    auto it = g_trampolines.find(target);
    if (it != g_trampolines.end()) { *original = it->second; return true; }
    void* orig = nullptr;
    if (MH_CreateHook(target, detour, &orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        Log("[CORE] a hook could not go on at %p\n", target);
        return false;
    }
    g_trampolines[target] = orig;
    *original = orig;
    return true;
}

bool MinHookUp() {
    static int up = -1;
    if (up < 0) {
        const MH_STATUS s = MH_Initialize();
        up = (s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED) ? 1 : 0;
        if (!up) Log("[CORE] MinHook did not start (%d): nothing is hooked\n", (int)s);
    }
    return up == 1;
}

// A context class's originals, by vtable slot.
constexpr int kSlots = 80;
struct VtOriginals {
    void** vt = nullptr;
    void* orig[kSlots] = {};
};
VtOriginals g_vts[8];
std::atomic<int> g_vtCount{0};

void HookContext(ID3D11DeviceContext* ctx);

VtOriginals* OriginalsOf(ID3D11DeviceContext* ctx) {
    void** vt = *(void***)ctx;
    for (int pass = 0; pass < 2; ++pass) {
        const int n = g_vtCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i) if (g_vts[i].vt == vt) return &g_vts[i];
        if (pass == 0) HookContext(ctx);
    }
    return nullptr;
}

template <typename Fn>
Fn Orig(ID3D11DeviceContext* ctx, int slot) {
    VtOriginals* o = OriginalsOf(ctx);
    return o ? (Fn)o->orig[slot] : nullptr;
}

// --- Contexts' states: appended, never removed; the count is published after the entry is filled in.
struct StateEntry {
    ID3D11DeviceContext* ctx = nullptr;
    ContextState state;
};
StateEntry g_states[64];
std::atomic<int> g_stateCount{0};
std::mutex g_stateMutex;

ContextState* StateOf(ID3D11DeviceContext* ctx) {
    if (!ctx) return nullptr;
    const int n = g_stateCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) if (g_states[i].ctx == ctx) return &g_states[i].state;
    std::lock_guard<std::mutex> lk(g_stateMutex);
    const int m = g_stateCount.load(std::memory_order_relaxed);
    for (int i = 0; i < m; ++i) if (g_states[i].ctx == ctx) return &g_states[i].state;
    if (m >= 64) return nullptr;
    g_states[m].ctx = ctx;
    g_states[m].state = ContextState();
    g_states[m].state.ctx = ctx;
    g_states[m].state.deferred = ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
    g_stateCount.store(m + 1, std::memory_order_release);
    return &g_states[m].state;
}

// --- Features.
const CoreFeature* g_features[16] = {};
std::atomic<int> g_featureCount{0};

#define EACH_FEATURE(member, ...)                                                   \
    do {                                                                            \
        const int fc_ = g_featureCount.load(std::memory_order_acquire);             \
        for (int fi_ = 0; fi_ < fc_; ++fi_)                                         \
            if (g_features[fi_]->member) g_features[fi_]->member(__VA_ARGS__);      \
    } while (0)

// --- Size of a view's resource.
void ViewSize(ID3D11View* v, UINT* w, UINT* h, DXGI_FORMAT* f) {
    *w = *h = 0;
    if (f) *f = DXGI_FORMAT_UNKNOWN;
    if (!v) return;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return;
    ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) {
        D3D11_TEXTURE2D_DESC d = {};
        t->GetDesc(&d);
        *w = d.Width;
        *h = d.Height;
        if (f) *f = d.Format;
        t->Release();
    }
    r->Release();
}

// --- Context detours. Slot numbers are ID3D11DeviceContext's (the first context method is 7).
enum Slot {
    S_VSSetConstantBuffers = 7, S_PSSetShaderResources = 8, S_PSSetShader = 9, S_VSSetShader = 11,
    S_DrawIndexed = 12, S_Draw = 13, S_Map = 14, S_Unmap = 15, S_PSSetConstantBuffers = 16,
    S_DrawIndexedInstanced = 20, S_DrawInstanced = 21, S_OMSetRenderTargets = 33,
    S_OMSetRenderTargetsAndUAVs = 34, S_OMSetBlendState = 35, S_OMSetDepthStencilState = 36,
    S_DrawIndexedInstancedIndirect = 39, S_DrawInstancedIndirect = 40, S_Dispatch = 41, S_DispatchIndirect = 42,
    S_UpdateSubresource = 48, S_CSSetShaderResources = 67, S_CSSetUnorderedAccessViews = 68, S_CSSetShader = 69,
    S_CSSetConstantBuffers = 71,
};

// For the ten second line: the engine's draws and dispatches, pixel shader binds named by the table or not, and
// constant buffers given a CPU copy.
std::atomic<long long> g_draws{0}, g_dispatches{0}, g_psNamed{0}, g_psUnnamed{0}, g_shadows{0};

inline void Before(ID3D11DeviceContext* c, ContextState*& st, bool indexed, bool indirect, UINT count, UINT instances,
                   UINT start, INT base, UINT startInstance) {
    st = t_CorePassThrough ? nullptr : StateOf(c);
    if (!st) return;
    g_draws.fetch_add(1, std::memory_order_relaxed);
    st->draw.indexed = indexed;
    st->draw.indirect = indirect;
    st->draw.count = count;
    st->draw.instances = instances;
    st->draw.start = start;
    st->draw.base = base;
    st->draw.startInstance = startInstance;
    EACH_FEATURE(beforeDraw, c, st);
}
inline void After(ID3D11DeviceContext* c, ContextState* st) {
    if (st) EACH_FEATURE(afterDraw, c, st);
}

typedef void(STDMETHODCALLTYPE* DrawIndexed_Fn)(ID3D11DeviceContext*, UINT, UINT, INT);
void STDMETHODCALLTYPE H_DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    ContextState* st;
    Before(c, st, true, false, n, 1, start, base, 0);
    Orig<DrawIndexed_Fn>(c, S_DrawIndexed)(c, n, start, base);
    After(c, st);
}
typedef void(STDMETHODCALLTYPE* Draw_Fn)(ID3D11DeviceContext*, UINT, UINT);
void STDMETHODCALLTYPE H_Draw(ID3D11DeviceContext* c, UINT n, UINT start) {
    ContextState* st;
    Before(c, st, false, false, n, 1, start, 0, 0);
    Orig<Draw_Fn>(c, S_Draw)(c, n, start);
    After(c, st);
}
typedef void(STDMETHODCALLTYPE* DrawIndexedInstanced_Fn)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
void STDMETHODCALLTYPE H_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, INT base,
                                              UINT startInst) {
    ContextState* st;
    Before(c, st, true, false, n, inst, start, base, startInst);
    Orig<DrawIndexedInstanced_Fn>(c, S_DrawIndexedInstanced)(c, n, inst, start, base, startInst);
    After(c, st);
}
typedef void(STDMETHODCALLTYPE* DrawInstanced_Fn)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
void STDMETHODCALLTYPE H_DrawInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, UINT startInst) {
    ContextState* st;
    Before(c, st, false, false, n, inst, start, 0, startInst);
    Orig<DrawInstanced_Fn>(c, S_DrawInstanced)(c, n, inst, start, startInst);
    After(c, st);
}
typedef void(STDMETHODCALLTYPE* DrawIndirect_Fn)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
void STDMETHODCALLTYPE H_DrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off) {
    ContextState* st;
    Before(c, st, true, true, 0, 0, 0, 0, 0);
    Orig<DrawIndirect_Fn>(c, S_DrawIndexedInstancedIndirect)(c, b, off);
    After(c, st);
}
void STDMETHODCALLTYPE H_DrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off) {
    ContextState* st;
    Before(c, st, false, true, 0, 0, 0, 0, 0);
    Orig<DrawIndirect_Fn>(c, S_DrawInstancedIndirect)(c, b, off);
    After(c, st);
}
typedef void(STDMETHODCALLTYPE* Dispatch_Fn)(ID3D11DeviceContext*, UINT, UINT, UINT);
void STDMETHODCALLTYPE H_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    if (!t_CorePassThrough) g_dispatches.fetch_add(1, std::memory_order_relaxed);
    if (!t_CorePassThrough)
        if (ContextState* st = StateOf(c)) EACH_FEATURE(beforeDispatch, c, st, x, y, z);
    Orig<Dispatch_Fn>(c, S_Dispatch)(c, x, y, z);
}
void STDMETHODCALLTYPE H_DispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off) {
    if (!t_CorePassThrough)
        if (ContextState* st = StateOf(c)) EACH_FEATURE(beforeDispatch, c, st, 0u, 0u, 0u);
    Orig<DrawIndirect_Fn>(c, S_DispatchIndirect)(c, b, off);
}

typedef void(STDMETHODCALLTYPE* SetCb_Fn)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
void KeepCbs(ID3D11Buffer** into, UINT start, UINT n, ID3D11Buffer* const* p) {
    for (UINT i = 0; i < n && start + i < 14; ++i) into[start + i] = p ? p[i] : nullptr;
}
void STDMETHODCALLTYPE H_VSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* p) {
    if (ContextState* st = StateOf(c)) KeepCbs(st->vsCb, s, n, p);
    Orig<SetCb_Fn>(c, S_VSSetConstantBuffers)(c, s, n, p);
}
void STDMETHODCALLTYPE H_PSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* p) {
    if (ContextState* st = StateOf(c)) KeepCbs(st->psCb, s, n, p);
    Orig<SetCb_Fn>(c, S_PSSetConstantBuffers)(c, s, n, p);
}
void STDMETHODCALLTYPE H_CSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* p) {
    if (ContextState* st = StateOf(c)) KeepCbs(st->csCb, s, n, p);
    Orig<SetCb_Fn>(c, S_CSSetConstantBuffers)(c, s, n, p);
}

typedef void(STDMETHODCALLTYPE* SetSrv_Fn)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
void STDMETHODCALLTYPE H_PSSetShaderResources(ID3D11DeviceContext* c, UINT s, UINT n,
                                              ID3D11ShaderResourceView* const* p) {
    if (ContextState* st = StateOf(c))
        for (UINT i = 0; i < n && s + i < 32; ++i) st->psSrv[s + i] = p ? p[i] : nullptr;
    Orig<SetSrv_Fn>(c, S_PSSetShaderResources)(c, s, n, p);
}
void STDMETHODCALLTYPE H_CSSetShaderResources(ID3D11DeviceContext* c, UINT s, UINT n,
                                              ID3D11ShaderResourceView* const* p) {
    if (ContextState* st = StateOf(c))
        for (UINT i = 0; i < n && s + i < 32; ++i) st->csSrv[s + i] = p ? p[i] : nullptr;
    Orig<SetSrv_Fn>(c, S_CSSetShaderResources)(c, s, n, p);
}
typedef void(STDMETHODCALLTYPE* SetUav_Fn)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*,
                                          const UINT*);
void STDMETHODCALLTYPE H_CSSetUnorderedAccessViews(ID3D11DeviceContext* c, UINT s, UINT n,
                                                   ID3D11UnorderedAccessView* const* p, const UINT* counts) {
    if (ContextState* st = StateOf(c))
        for (UINT i = 0; i < n && s + i < 8; ++i) st->csUav[s + i] = p ? p[i] : nullptr;
    Orig<SetUav_Fn>(c, S_CSSetUnorderedAccessViews)(c, s, n, p, counts);
}

typedef void(STDMETHODCALLTYPE* SetShader_Fn)(ID3D11DeviceContext*, ID3D11DeviceChild*, ID3D11ClassInstance* const*,
                                             UINT);
void STDMETHODCALLTYPE H_VSSetShader(ID3D11DeviceContext* c, ID3D11VertexShader* sh, ID3D11ClassInstance* const* ci,
                                     UINT n) {
    if (ContextState* st = StateOf(c)) {
        if (st->vs != sh || (sh && st->vsLayout < 0)) st->vsLayout = LayoutOf(sh, &st->vsCombo, nullptr);
        st->vs = sh;
    }
    Orig<SetShader_Fn>(c, S_VSSetShader)(c, sh, ci, n);
}
void STDMETHODCALLTYPE H_PSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* sh, ID3D11ClassInstance* const* ci,
                                     UINT n) {
    if (ContextState* st = StateOf(c)) {
        if (st->ps != sh || (sh && st->psLayout < 0)) st->psLayout = LayoutOf(sh, &st->psCombo, &st->psFlags);
        st->ps = sh;
        if (sh && !t_CorePassThrough) (st->psLayout >= 0 ? g_psNamed : g_psUnnamed).fetch_add(1, std::memory_order_relaxed);
    }
    Orig<SetShader_Fn>(c, S_PSSetShader)(c, sh, ci, n);
}
void STDMETHODCALLTYPE H_CSSetShader(ID3D11DeviceContext* c, ID3D11ComputeShader* sh, ID3D11ClassInstance* const* ci,
                                     UINT n) {
    if (ContextState* st = StateOf(c)) {
        if (st->cs != sh || (sh && st->csLayout < 0)) st->csLayout = LayoutOf(sh, &st->csCombo, nullptr);
        st->cs = sh;
    }
    Orig<SetShader_Fn>(c, S_CSSetShader)(c, sh, ci, n);
}

void KeepTargets(ContextState* st, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    ++st->rtBindGen;
    ViewSize(n && rtv ? rtv[0] : nullptr, &st->rtW, &st->rtH, &st->rtFormat);
    ViewSize(dsv, &st->dsW, &st->dsH, nullptr);
}
typedef void(STDMETHODCALLTYPE* OMSetRT_Fn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                           ID3D11DepthStencilView*);
void STDMETHODCALLTYPE H_OMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv,
                                            ID3D11DepthStencilView* dsv) {
    if (ContextState* st = StateOf(c)) KeepTargets(st, n, rtv, dsv);
    Orig<OMSetRT_Fn>(c, S_OMSetRenderTargets)(c, n, rtv, dsv);
}
typedef void(STDMETHODCALLTYPE* OMSetRTUav_Fn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                              ID3D11DepthStencilView*, UINT, UINT,
                                              ID3D11UnorderedAccessView* const*, const UINT*);
void STDMETHODCALLTYPE H_OMSetRenderTargetsAndUAVs(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv,
                                                   ID3D11DepthStencilView* dsv, UINT uavStart, UINT uavs,
                                                   ID3D11UnorderedAccessView* const* uav, const UINT* counts) {
    // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL leaves the targets as they are.
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        if (ContextState* st = StateOf(c)) KeepTargets(st, n, rtv, dsv);
    Orig<OMSetRTUav_Fn>(c, S_OMSetRenderTargetsAndUAVs)(c, n, rtv, dsv, uavStart, uavs, uav, counts);
}
typedef void(STDMETHODCALLTYPE* OMSetBlend_Fn)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
void STDMETHODCALLTYPE H_OMSetBlendState(ID3D11DeviceContext* c, ID3D11BlendState* b, const FLOAT f[4], UINT m) {
    if (ContextState* st = StateOf(c)) {
        D3D11_BLEND_DESC d = {};
        if (b) b->GetDesc(&d);
        st->blend = b && d.RenderTarget[0].BlendEnable;
        st->blendSrc = (uint8_t)d.RenderTarget[0].SrcBlend;
        st->blendDst = (uint8_t)d.RenderTarget[0].DestBlend;
    }
    Orig<OMSetBlend_Fn>(c, S_OMSetBlendState)(c, b, f, m);
}
typedef void(STDMETHODCALLTYPE* OMSetDepth_Fn)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
void STDMETHODCALLTYPE H_OMSetDepthStencilState(ID3D11DeviceContext* c, ID3D11DepthStencilState* s, UINT ref) {
    if (ContextState* st = StateOf(c)) {
        D3D11_DEPTH_STENCIL_DESC d = {};
        if (s) s->GetDesc(&d);
        // No state is the default: depth on and written.
        st->depthWrites = !s || (d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL);
    }
    Orig<OMSetDepth_Fn>(c, S_OMSetDepthStencilState)(c, s, ref);
}

// The engine writes a small constant buffer into our copy; at Unmap the copy goes on to the real buffer.
typedef HRESULT(STDMETHODCALLTYPE* Map_Fn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
HRESULT STDMETHODCALLTYPE H_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
                                D3D11_MAPPED_SUBRESOURCE* out) {
    const HRESULT hr = Orig<Map_Fn>(c, S_Map)(c, r, sub, type, flags, out);
    if (FAILED(hr) || !out || !out->pData || t_CorePassThrough) return hr;
    const bool write = type == D3D11_MAP_WRITE || type == D3D11_MAP_READ_WRITE || type == D3D11_MAP_WRITE_DISCARD
                    || type == D3D11_MAP_WRITE_NO_OVERWRITE;
    if (!write) return hr;
    if (Shadow* s = ShadowOf(r)) {
        s->real = out->pData;
        out->pData = s->data.data();
        s->Release();
    }
    EACH_FEATURE(mapped, c, r, out->pData);
    return hr;
}
typedef void(STDMETHODCALLTYPE* Unmap_Fn)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
void STDMETHODCALLTYPE H_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (!t_CorePassThrough) EACH_FEATURE(unmapping, c, r);
    if (Shadow* s = ShadowOf(r)) {
        if (s->real) {
            memcpy(s->real, s->data.data(), s->data.size());
            s->real = nullptr;
        }
        s->Release();
    }
    Orig<Unmap_Fn>(c, S_Unmap)(c, r, sub);
}
typedef void(STDMETHODCALLTYPE* Update_Fn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
                                          UINT, UINT);
void STDMETHODCALLTYPE H_UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                           const void* data, UINT rowPitch, UINT depthPitch) {
    if (data) {
        if (Shadow* s = ShadowOf(r)) {
            const size_t size = s->data.size();
            const size_t from = box ? box->left : 0;
            const size_t to = box ? box->right : size;
            if (from < to && to <= size) memcpy(s->data.data() + from, data, to - from);
            s->Release();
        }
    }
    Orig<Update_Fn>(c, S_UpdateSubresource)(c, r, sub, box, data, rowPitch, depthPitch);
}

void HookContext(ID3D11DeviceContext* ctx) {
    if (!ctx || !MinHookUp()) return;
    std::lock_guard<std::recursive_mutex> lk(g_hookMutex);
    void** vt = *(void***)ctx;
    const int n = g_vtCount.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) if (g_vts[i].vt == vt) return;
    if (n >= 8) return;
    VtOriginals& o = g_vts[n];
    o.vt = vt;
    struct { int slot; void* detour; } const list[] = {
        { S_VSSetConstantBuffers, (void*)&H_VSSetConstantBuffers },
        { S_PSSetShaderResources, (void*)&H_PSSetShaderResources },
        { S_PSSetShader, (void*)&H_PSSetShader },
        { S_VSSetShader, (void*)&H_VSSetShader },
        { S_DrawIndexed, (void*)&H_DrawIndexed },
        { S_Draw, (void*)&H_Draw },
        { S_Map, (void*)&H_Map },
        { S_Unmap, (void*)&H_Unmap },
        { S_PSSetConstantBuffers, (void*)&H_PSSetConstantBuffers },
        { S_DrawIndexedInstanced, (void*)&H_DrawIndexedInstanced },
        { S_DrawInstanced, (void*)&H_DrawInstanced },
        { S_OMSetRenderTargets, (void*)&H_OMSetRenderTargets },
        { S_OMSetRenderTargetsAndUAVs, (void*)&H_OMSetRenderTargetsAndUAVs },
        { S_OMSetBlendState, (void*)&H_OMSetBlendState },
        { S_OMSetDepthStencilState, (void*)&H_OMSetDepthStencilState },
        { S_DrawIndexedInstancedIndirect, (void*)&H_DrawIndexedInstancedIndirect },
        { S_DrawInstancedIndirect, (void*)&H_DrawInstancedIndirect },
        { S_Dispatch, (void*)&H_Dispatch },
        { S_DispatchIndirect, (void*)&H_DispatchIndirect },
        { S_UpdateSubresource, (void*)&H_UpdateSubresource },
        { S_CSSetShaderResources, (void*)&H_CSSetShaderResources },
        { S_CSSetUnorderedAccessViews, (void*)&H_CSSetUnorderedAccessViews },
        { S_CSSetShader, (void*)&H_CSSetShader },
        { S_CSSetConstantBuffers, (void*)&H_CSSetConstantBuffers },
    };
    bool ok = true;
    for (const auto& h : list) ok = Install(vt[h.slot], h.detour, &o.orig[h.slot]) && ok;
    g_vtCount.store(n + 1, std::memory_order_release);
    Log("[CORE] %s context class %p hooked%s\n", ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? "deferred" : "immediate",
        (void*)vt, ok ? "" : " (NOT all of it)");
}

// --- Device detours. Slots are ID3D11Device's.
typedef HRESULT(STDMETHODCALLTYPE* CreateBuffer_Fn)(ID3D11Device*, const D3D11_BUFFER_DESC*,
                                                   const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
CreateBuffer_Fn g_origCreateBuffer = nullptr;
HRESULT STDMETHODCALLTYPE H_CreateBuffer(ID3D11Device* d, const D3D11_BUFFER_DESC* desc,
                                         const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
    const HRESULT hr = g_origCreateBuffer(d, desc, init, out);
    if (SUCCEEDED(hr) && desc && out && *out && (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER)
        && desc->ByteWidth <= kShadowMax) {
        Shadow* s = new Shadow(desc->ByteWidth);
        if (init && init->pSysMem) memcpy(s->data.data(), init->pSysMem, desc->ByteWidth);
        (*out)->SetPrivateDataInterface(kGuidShadow, s);
        s->Release();
        g_shadows.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}
typedef HRESULT(STDMETHODCALLTYPE* CreateShader_Fn)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                                   ID3D11DeviceChild**);
CreateShader_Fn g_origCreateVS = nullptr, g_origCreatePS = nullptr, g_origCreateCS = nullptr;
HRESULT STDMETHODCALLTYPE H_CreateVertexShader(ID3D11Device* d, const void* code, SIZE_T n, ID3D11ClassLinkage* l,
                                               ID3D11DeviceChild** out) {
    const HRESULT hr = g_origCreateVS(d, code, n, l, out);
    if (SUCCEEDED(hr) && out && *out) TagShader(*out, code, n);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreatePixelShader(ID3D11Device* d, const void* code, SIZE_T n, ID3D11ClassLinkage* l,
                                              ID3D11DeviceChild** out) {
    const HRESULT hr = g_origCreatePS(d, code, n, l, out);
    if (SUCCEEDED(hr) && out && *out) TagShader(*out, code, n);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateComputeShader(ID3D11Device* d, const void* code, SIZE_T n, ID3D11ClassLinkage* l,
                                                ID3D11DeviceChild** out) {
    const HRESULT hr = g_origCreateCS(d, code, n, l, out);
    if (SUCCEEDED(hr) && out && *out) TagShader(*out, code, n);
    return hr;
}
typedef HRESULT(STDMETHODCALLTYPE* CreateDeferred_Fn)(ID3D11Device*, UINT, ID3D11DeviceContext**);
CreateDeferred_Fn g_origCreateDeferred = nullptr;
HRESULT STDMETHODCALLTYPE H_CreateDeferredContext(ID3D11Device* d, UINT flags, ID3D11DeviceContext** out) {
    const HRESULT hr = g_origCreateDeferred(d, flags, out);
    if (SUCCEEDED(hr) && out && *out) HookContext(*out);
    return hr;
}

void HookDevice(ID3D11Device* dev) {
    if (!dev || !MinHookUp()) return;
    std::lock_guard<std::recursive_mutex> lk(g_hookMutex);
    if (g_origCreateBuffer) return;
    void** vt = *(void***)dev;
    bool ok = Install(vt[3], (void*)&H_CreateBuffer, (void**)&g_origCreateBuffer);
    ok = Install(vt[12], (void*)&H_CreateVertexShader, (void**)&g_origCreateVS) && ok;
    ok = Install(vt[15], (void*)&H_CreatePixelShader, (void**)&g_origCreatePS) && ok;
    ok = Install(vt[18], (void*)&H_CreateComputeShader, (void**)&g_origCreateCS) && ok;
    ok = Install(vt[27], (void*)&H_CreateDeferredContext, (void**)&g_origCreateDeferred) && ok;
    Log("[CORE] device %p hooked%s\n", (void*)dev, ok ? "" : " (NOT all of it)");
    g_deviceHooked.store(true, std::memory_order_relaxed);
    CoreTableStart();
}

// --- Swapchains.
typedef HRESULT(STDMETHODCALLTYPE* Present_Fn)(IDXGISwapChain*, UINT, UINT);
Present_Fn g_origPresent = nullptr;
std::atomic<long long> g_presents{0};
long long g_presentsAtLine = 0;
HRESULT STDMETHODCALLTYPE H_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    const long long presents = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (!last) last = now;
    if (now - last >= 10000) {
        last = now;
        Log("[CORE] in 10 s: %lld presents (%lld in all), %lld draws, %lld dispatches; pixel shaders bound %lld times "
            "named by the table, %lld not; %lld constant buffers with a CPU copy and %lld shaders made so far; table %s\n",
            presents - g_presentsAtLine, presents, g_draws.exchange(0), g_dispatches.exchange(0), g_psNamed.exchange(0),
            g_psUnnamed.exchange(0), g_shadows.load(), g_shadersMade.load(), CoreTableReady() ? "loaded" : "not yet");
        g_presentsAtLine = presents;
    }
    CoreDevStart();
    CoreViewsPresent(sc);
    EACH_FEATURE(present, sc);
    return g_origPresent(sc, sync, flags);
}

void HookSwapChain(IDXGISwapChain* sc) {
    if (!sc || !MinHookUp()) return;
    std::lock_guard<std::recursive_mutex> lk(g_hookMutex);
    if (g_origPresent) return;
    void** vt = *(void***)sc;
    const bool ok = Install(vt[8], (void*)&H_Present, (void**)&g_origPresent);
    Log("[CORE] swapchain Present hooked%s\n", ok ? "" : " (FAILED)");
}

// The device behind a new swapchain: its creations and its immediate context hooked.
void HookFromDevice(IUnknown* device) {
    if (!device) return;
    ID3D11Device* dev = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
    HookDevice(dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (ctx) {
        HookContext(ctx);
        StateOf(ctx);
        ctx->Release();
    }
    dev->Release();
}

typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChain_Fn)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
CreateSwapChain_Fn g_origCreateSwapChain = nullptr;
HRESULT STDMETHODCALLTYPE H_CreateSwapChain(IDXGIFactory* f, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                            IDXGISwapChain** out) {
    HookFromDevice(device);
    const HRESULT hr = g_origCreateSwapChain(f, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}
typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChainForHwnd_Fn)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
CreateSwapChainForHwnd_Fn g_origCreateSwapChainForHwnd = nullptr;
HRESULT STDMETHODCALLTYPE H_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* device, HWND hwnd,
                                                   const DXGI_SWAP_CHAIN_DESC1* desc,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
                                                   IDXGISwapChain1** out) {
    HookFromDevice(device);
    const HRESULT hr = g_origCreateSwapChainForHwnd(f, device, hwnd, desc, fs, output, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}

void HookFactory(IUnknown* factory) {
    if (!factory || !MinHookUp()) return;
    std::lock_guard<std::recursive_mutex> lk(g_hookMutex);
    if (g_origCreateSwapChain) return;
    void** vt = *(void***)factory;
    bool ok = Install(vt[10], (void*)&H_CreateSwapChain, (void**)&g_origCreateSwapChain);
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)) && f2) {
        void** vt2 = *(void***)f2;
        ok = Install(vt2[15], (void*)&H_CreateSwapChainForHwnd, (void**)&g_origCreateSwapChainForHwnd) && ok;
        f2->Release();
    }
    Log("[CORE] factory hooked%s\n", ok ? "" : " (NOT all of it)");
}

// --- The system's dxgi.dll.
typedef HRESULT(WINAPI* CreateFactory_Fn)(REFIID, void**);
typedef HRESULT(WINAPI* CreateFactory2_Fn)(UINT, REFIID, void**);
HMODULE g_dxgi = nullptr;
CreateFactory_Fn g_realCreateFactory = nullptr, g_realCreateFactory1 = nullptr;
CreateFactory2_Fn g_realCreateFactory2 = nullptr;

void ProbeLoaded(const std::wstring& name, const char* how);   // below, with the library load watch

void LoadRealDxgi() {
    if (g_dxgi) return;
    wchar_t path[MAX_PATH] = {};
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\dxgi.dll");
    g_dxgi = LoadLibraryW(path);
    if (!g_dxgi) {
        Log("[CORE] the system's dxgi.dll could not be loaded (%lu)\n", GetLastError());
        return;
    }
    g_realCreateFactory = (CreateFactory_Fn)GetProcAddress(g_dxgi, "CreateDXGIFactory");
    g_realCreateFactory1 = (CreateFactory_Fn)GetProcAddress(g_dxgi, "CreateDXGIFactory1");
    g_realCreateFactory2 = (CreateFactory2_Fn)GetProcAddress(g_dxgi, "CreateDXGIFactory2");
    Log("[CORE] S2FM Mod Pack %s, beside %ls\n", S2FM_MODS_VERSION, CoreDir().c_str());
    CoreCrashInstall();
    {
        // What is in already when the engine first asks for a factory.
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        MODULEENTRY32W me = { sizeof(me) };
        if (snap != INVALID_HANDLE_VALUE) {
            for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
                ProbeLoaded(me.szExePath, "in before the first factory");
            CloseHandle(snap);
        }
    }
    CoreFeaturesInit();
    CoreOnModuleLoad([] {});   // the library load watch on, for the start order
}

}  // namespace

// --- dxgi.dll's exports.
extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** out) {
    LoadRealDxgi();
    if (!g_realCreateFactory) return E_FAIL;
    const HRESULT hr = g_realCreateFactory(riid, out);
    if (SUCCEEDED(hr) && out && *out) HookFactory((IUnknown*)*out);
    return hr;
}
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** out) {
    LoadRealDxgi();
    if (!g_realCreateFactory1) return E_FAIL;
    const HRESULT hr = g_realCreateFactory1(riid, out);
    if (SUCCEEDED(hr) && out && *out) HookFactory((IUnknown*)*out);
    return hr;
}
extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    LoadRealDxgi();
    if (!g_realCreateFactory2) return E_FAIL;
    const HRESULT hr = g_realCreateFactory2(flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) HookFactory((IUnknown*)*out);
    return hr;
}

// --- The core's own API.
ContextState* CoreState(ID3D11DeviceContext* ctx) { return StateOf(ctx); }

bool CoreCbRead(ID3D11Buffer* cb, UINT offset, void* out, UINT bytes, UINT* total) {
    Shadow* s = ShadowOf(cb);
    if (!s) return false;
    const size_t size = s->data.size();
    if (total) *total = (UINT)size;
    const bool ok = (size_t)offset + bytes <= size;
    if (ok && bytes) memcpy(out, s->data.data() + offset, bytes);
    s->Release();
    return ok;
}

bool CorePerView(const ContextState* st, float out[204]) {
    if (!st) return false;
    for (int slot : { 1, 0 }) {
        UINT total = 0;
        if (st->vsCb[slot] && CoreCbRead(st->vsCb[slot], 0, nullptr, 0, &total) && total == 816
            && CoreCbRead(st->vsCb[slot], 0, out, 816))
            return true;
    }
    return false;
}

bool CoreShaderBytecode(ID3D11DeviceChild* shader, void* out, UINT* bytes) {
    return shader && bytes && SUCCEEDED(shader->GetPrivateData(kGuidBytecode, bytes, out));
}

void CoreRegister(const CoreFeature* f) {
    const int n = g_featureCount.load(std::memory_order_relaxed);
    if (!f || n >= 16) return;
    g_features[n] = f;
    g_featureCount.store(n + 1, std::memory_order_release);
    Log("[CORE] feature registered: %s\n", f->name ? f->name : "?");
}

bool CoreHook(void* target, void* detour, void** original) {
    if (!target || !MinHookUp()) return false;
    std::lock_guard<std::recursive_mutex> lk(g_hookMutex);
    return Install(target, detour, original);
}

// --- Library loads. LdrLoadDll is where every LoadLibrary ends up, and the modules a load brings in as its
// dependencies come in inside the same call, so a hook there sees every module once it is in.
namespace {
struct UnicodeStringLite {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};
typedef LONG(NTAPI* LdrLoadDll_Fn)(PWSTR path, ULONG* characteristics, UnicodeStringLite* name, void** handle);
LdrLoadDll_Fn g_origLdrLoadDll = nullptr;
void (*g_onLoad[8])() = {};
std::atomic<int> g_onLoadCount{0};

// The start's order, for the engine tool route (README step 4): when the game's own modules and every tool come in,
// against the device and the shaders made so far. Each name once.
bool Interesting(const std::wstring& n) {
    std::wstring low = n;
    for (wchar_t& c : low) c = (wchar_t)towlower(c);
    if (low.find(L"tools") != std::wstring::npos) return true;
    static const wchar_t* const kNames[] = { L"toolframework2.dll", L"rendersystemdx11.dll", L"d3d11.dll", L"sfm.dll",
                                             L"engine2.dll", L"materialsystem2.dll", L"scenesystem.dll", L"client.dll",
                                             L"dxgi.dll", L"d3d12.dll" };
    for (const wchar_t* k : kNames) {
        const size_t kl = wcslen(k);
        if (low.size() >= kl && low.compare(low.size() - kl, kl, k) == 0) return true;
    }
    return false;
}

void ProbeLoaded(const std::wstring& name, const char* how) {
    static std::mutex m;
    static std::map<std::wstring, bool> said;
    std::lock_guard<std::mutex> lk(m);
    if (said[name] || !Interesting(name)) return;
    said[name] = true;
    Log("[CORE] start order: %ls %s | device %s, shaders made %lld, presents %lld", name.c_str(), how,
        g_deviceHooked.load() ? "hooked" : "not yet", g_shadersMade.load(), g_presents.load());
}

LONG NTAPI H_LdrLoadDll(PWSTR path, ULONG* characteristics, UnicodeStringLite* name, void** handle) {
    const LONG status = g_origLdrLoadDll(path, characteristics, name, handle);
    if (status >= 0 && name && name->Buffer && name->Length)
        ProbeLoaded(std::wstring(name->Buffer, name->Length / sizeof(wchar_t)), "loaded");
    if (status >= 0) {
        const int n = g_onLoadCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i) g_onLoad[i]();
    }
    return status;
}
}  // namespace

void CoreOnModuleLoad(void (*fn)()) {
    if (!fn) return;
    {
        std::lock_guard<std::recursive_mutex> lk(g_hookMutex);
        const int n = g_onLoadCount.load(std::memory_order_relaxed);
        if (n >= 8) return;
        g_onLoad[n] = fn;
        g_onLoadCount.store(n + 1, std::memory_order_release);
        if (!g_origLdrLoadDll) {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            void* ldr = ntdll ? (void*)GetProcAddress(ntdll, "LdrLoadDll") : nullptr;
            if (ldr && CoreHook(ldr, (void*)&H_LdrLoadDll, (void**)&g_origLdrLoadDll))
                Log("[CORE] library loads are watched");
            else
                Log("[CORE] library loads could not be watched");
        }
    }
    fn();   // for a module that is in already
}

void CoreRawDraw(ID3D11DeviceContext* ctx, UINT vertexCount) {
    Orig<Draw_Fn>(ctx, S_Draw)(ctx, vertexCount, 0);
}

bool CoreRedraw(ID3D11DeviceContext* ctx, const ContextState* st) {
    if (!ctx || !st || st->draw.indirect) return false;
    const ContextState::Draw& d = st->draw;
    if (d.indexed)
        Orig<DrawIndexedInstanced_Fn>(ctx, S_DrawIndexedInstanced)(ctx, d.count, d.instances, d.start, d.base,
                                                                   d.startInstance);
    else
        Orig<DrawInstanced_Fn>(ctx, S_DrawInstanced)(ctx, d.count, d.instances, d.start, d.startInstance);
    return true;
}

long long CorePresents() { return g_presents.load(std::memory_order_relaxed); }

// Nothing at unload: the process is ending, and taking hooks off under threads still in them is the risk.
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
