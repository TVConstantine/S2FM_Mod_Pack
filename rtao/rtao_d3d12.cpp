// RTAO's rays: a D3D12 device beside the engine's D3D11 one, on the same adapter. Each frame the captured vertices
// (rtao_capture.cpp) become one bottom level structure of all the frame's triangles, built fresh (almost all of them
// are posed again every frame anyway, docs/rtao.md), a top level one of that single instance, and the AO is
// traced with inline ray queries (rtao_cs.hlsl). The engine's context and our queue meet only through two shared
// fences, waited on by the GPU: nothing on the CPU waits for the card, except before reusing a command allocator.
#include "rtao.h"
#include "../core/core.h"

#include <dxgi1_4.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "rtao_toworld_cs.h"
#include "rtao_ao_cs.h"

namespace {

std::mutex g_m;
bool g_tried = false, g_ready = false;
ID3D12Device5* g_dev = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
constexpr int kLists = 2;
ID3D12CommandAllocator* g_alloc[kLists] = {};
UINT64 g_allocDone[kLists] = {};   // the AO fence value the allocator's last list signalled
ID3D12GraphicsCommandList4* g_list = nullptr;
int g_listIndex = 0;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_toWorld = nullptr;
ID3D12PipelineState* g_ao = nullptr;
// Fences: captured (the engine signals, we wait) and AO (we signal, the engine waits).
ID3D12Fence* g_capturedFence12 = nullptr;
ID3D11Fence* g_capturedFence11 = nullptr;
ID3D12Fence* g_aoFence12 = nullptr;
ID3D11Fence* g_aoFence11 = nullptr;
// The capture buffer, both sides.
ID3D11Buffer* g_capture11 = nullptr;
ID3D12Resource* g_capture12 = nullptr;
UINT g_captureBytes = 0;
// The cut outs' uv and alpha (12 bytes a vertex, at the same vertex as its position), both sides.
ID3D11Buffer* g_uv11 = nullptr;
ID3D12Resource* g_uv12 = nullptr;
UINT g_uvBytes = 0;
// The smooth normals (12 bytes a vertex), both sides.
ID3D11Buffer* g_norm11 = nullptr;
ID3D12Resource* g_norm12 = nullptr;
UINT g_normBytes = 0;
// The bottom structure's geometries: the captured vertices cut into runs, opaque or a cut out, and a record each the
// AO shader reads by the geometry a ray struck (first vertex, mask, reference, flags), per list. At most kGeometries:
// past that the rest of the frame's cut outs are traced as solid.
constexpr UINT kGeometries = 512;
ID3D12Resource* g_geoRecords[kLists] = {};
// The frame's views (RtaoView: 16 floats, 3, a flag: 80 bytes each) and then its runs (12 bytes each), per list.
constexpr UINT kViewBytes = 80 * RTAO_VIEWS;
ID3D12Resource* g_viewRecords[kLists] = {};
// The masks' views (heap entries 4 on), and what they were made for.
ID3D12Resource* g_maskViews[RTAO_MASKS] = {};
// What the frame is made of: the positions in the rays' space, the two structures and their scratch, the one
// instance, and the constants (one per list).
ID3D12Resource* g_positions = nullptr;
UINT64 g_positionsBytes = 0;
ID3D12Resource* g_blas = nullptr;
UINT64 g_blasBytes = 0;
ID3D12Resource* g_tlas = nullptr;
UINT64 g_tlasBytes = 0;
ID3D12Resource* g_scratch = nullptr;
UINT64 g_scratchBytes = 0;
// Upload heap, one D3D12_RAYTRACING_INSTANCE_DESC per list: a list still waiting on the card builds its top level
// from its own, so a bottom level that moves (grown) under the next list is never what an older list points at.
ID3D12Resource* g_instance[kLists] = {};
ID3D12Resource* g_consts[kLists] = {};  // upload heap, 256 bytes each
// The card's time for each part of a frame: timestamps before the conversion, after it, after the bottom and the top
// structures and after the AO, per list, read back once the list has run; summed for the 10 s line.
constexpr int kStamps = 5;
ID3D12QueryHeap* g_stamps = nullptr;
ID3D12Resource* g_stampRead = nullptr;
UINT64 g_stampFreq = 0;
bool g_stampPending[kLists] = {};
double g_partMs[kStamps - 1] = {};
long long g_partFrames = 0;
double g_lineMs[kStamps - 1] = {};
long long g_lineFrames = 0, g_lineBuilds = 0, g_lineRefits = 0;
// The views of the two depth and AO pairs: depth SRV then AO UAV, per slot.
ID3D12DescriptorHeap* g_heap = nullptr;
UINT g_heapStep = 0;
ID3D12Resource* g_slotRes[2][2] = {};
std::atomic<long long> g_traced{0}, g_lastTriangles{0};
// The bottom structure is refitted (moved, not made again) while the frame's draws are the frame before's, and made
// afresh when they change and every kRebuildEvery frames, so that a refit stretched by motion does not last.
constexpr int kRebuildEvery = 8;
uint64_t g_builtList = 0;
UINT64 g_builtVertices = 0;
int g_sinceBuild = 0;
std::atomic<long long> g_builds{0}, g_refits{0};
// Why RTAO cannot run here, for the panel ("" while it can).
char g_why[256] = "";

template <typename T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }

// A resource the card may still be using is not released when it is replaced: it waits here, with the AO fence value
// of the last list sent, until the card has passed that value. Releasing it at once let a list still running read
// freed memory when the frame grew while the rays were behind (the driver reset the card and RTAO stopped).
struct Retired {
    IUnknown* p;
    UINT64 after;
};
std::vector<Retired> g_retired;
UINT64 g_lastSent = 0;     // the AO fence value the last list sent signals
UINT g_regrowFrames = 0;   // "regrow": the frames traced with it on

template <typename T> void Retire(T*& p) {
    if (!p) return;
    g_retired.push_back({ p, g_lastSent });
    p = nullptr;
}

void ReleaseDone() {
    if (g_retired.empty() || !g_aoFence12) return;
    const UINT64 done = g_aoFence12->GetCompletedValue();
    size_t kept = 0;
    for (Retired& r : g_retired) {
        if (r.after <= done) r.p->Release();
        else g_retired[kept++] = r;
    }
    g_retired.resize(kept);
}

// Once: whether the card took our device away, and why, so that a stop is never silent.
bool g_removedLogged = false;
bool DeviceLost() {
    const HRESULT why = g_dev ? g_dev->GetDeviceRemovedReason() : S_OK;
    if (why == S_OK) return false;
    if (!g_removedLogged) {
        g_removedLogged = true;
        Log("[RTAO] the card removed our D3D12 device (0x%08X); RTAO stops for this session", (unsigned)why);
        sprintf_s(g_why, "the card removed the D3D12 device (0x%08X)", (unsigned)why);
    }
    g_ready = false;
    return true;
}

void Fail(const char* what, HRESULT hr) {
    Log("[RTAO] %s failed (0x%08X); RTAO stays off for this session", what, (unsigned)hr);
    sprintf_s(g_why, "D3D12 could not be started (%s, 0x%08X)", what, (unsigned)hr);
}

ID3D12Resource* Buffer(UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// A default heap buffer of at least bytes, grown by half again when it is made anew.
bool Grow(ID3D12Resource*& r, UINT64& have, UINT64 need, D3D12_RESOURCE_STATES state) {
    if (r && have >= need) return true;
    Retire(r);
    const UINT64 bytes = (need + need / 2 + 65535) & ~65535ull;
    r = Buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, state);
    have = r ? bytes : 0;
    return r != nullptr;
}

bool MakeFence(ID3D11Device* dev, ID3D12Fence** f12, ID3D11Fence** f11) {
    HANDLE h = nullptr;
    ID3D11Device5* dev5 = nullptr;
    const bool ok = SUCCEEDED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(f12)))
                 && SUCCEEDED(g_dev->CreateSharedHandle(*f12, nullptr, GENERIC_ALL, nullptr, &h)) && h
                 && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device5), (void**)&dev5)) && dev5
                 && SUCCEEDED(dev5->OpenSharedFence(h, IID_PPV_ARGS(f11)));
    if (dev5) dev5->Release();
    if (h) CloseHandle(h);
    return ok;
}

bool Start(ID3D11Device* dev) {
    // The engine's adapter.
    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) || !dxgiDev
        || FAILED(dxgiDev->GetAdapter(&adapter)) || !adapter) {
        if (dxgiDev) dxgiDev->Release();
        Log("[RTAO] the engine's adapter was not found; RTAO stays off");
        strcpy_s(g_why, "the engine's graphics card was not found");
        return false;
    }
    dxgiDev->Release();
    DXGI_ADAPTER_DESC ad = {};
    adapter->GetDesc(&ad);
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev));
    adapter->Release();
    if (FAILED(hr) || !g_dev) { Fail("D3D12 on the engine's adapter", hr); return false; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    g_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        Log("[RTAO] %ls has no DXR tier 1.1 (it has %d); RTAO needs an RTX 20 series, RX 6000 or Arc card or later, and "
            "stays off", ad.Description, (int)o5.RaytracingTier);
        sprintf_s(g_why, "%ls cannot trace rays (DXR 1.1 is needed: RTX 20 series, RX 6000, Arc or later)", ad.Description);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(hr = g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)))) { Fail("the queue", hr); return false; }
    for (int i = 0; i < kLists; ++i) {
        if (FAILED(hr = g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i])))) {
            Fail("a command allocator", hr);
            return false;
        }
        g_consts[i] = Buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!g_consts[i]) { Fail("the constants", E_OUTOFMEMORY); return false; }
    }
    if (FAILED(hr = g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr,
                                             IID_PPV_ARGS(&g_list)))) {
        Fail("the command list", hr);
        return false;
    }
    g_list->Close();

    // b0 the frame's constants, t0 the captured vertices, u0 the positions, t1 the structure; a table of t2 (the
    // depth) and u1 (the AO).
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 2;
    ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 1;
    ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_DESCRIPTOR_RANGE1 maskRange = {};
    maskRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    maskRange.NumDescriptors = RTAO_MASKS;
    maskRange.BaseShaderRegister = 0;
    maskRange.RegisterSpace = 1;
    maskRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    D3D12_ROOT_PARAMETER1 params[11] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    params[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 1;
    params[3].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 2;
    params[4].DescriptorTable.pDescriptorRanges = ranges;
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor.ShaderRegister = 3;
    params[5].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[6].Descriptor.ShaderRegister = 4;
    params[6].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[7].DescriptorTable.NumDescriptorRanges = 1;
    params[7].DescriptorTable.pDescriptorRanges = &maskRange;
    // t5 the views, t6 the runs (ToWorld).
    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[8].Descriptor.ShaderRegister = 5;
    params[8].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[9].Descriptor.ShaderRegister = 6;
    params[9].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    // t7 the smooth normals (Ao).
    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[10].Descriptor.ShaderRegister = 7;
    params[10].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs = {};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = 11;
    rs.Desc_1_1.pParameters = params;
    rs.Desc_1_1.NumStaticSamplers = 1;
    rs.Desc_1_1.pStaticSamplers = &sampler;
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    hr = D3D12SerializeVersionedRootSignature(&rs, &blob, &err);
    if (SUCCEEDED(hr)) hr = g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                       IID_PPV_ARGS(&g_root));
    if (err) { Log("[RTAO] root signature: %s", (const char*)err->GetBufferPointer()); err->Release(); }
    if (blob) blob->Release();
    if (FAILED(hr)) { Fail("the root signature", hr); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_root;
    pd.CS = { kRtaoToWorld, sizeof(kRtaoToWorld) };
    if (FAILED(hr = g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_toWorld)))) { Fail("ToWorld", hr); return false; }
    pd.CS = { kRtaoAo, sizeof(kRtaoAo) };
    if (FAILED(hr = g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_ao)))) { Fail("the AO shader", hr); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4 + RTAO_MASKS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(hr = g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) { Fail("the descriptor heap", hr); return false; }
    g_heapStep = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    {
        // The masks' entries empty until a mask is there.
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
        nd.Format = DXGI_FORMAT_R8_UNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)g_heapStep * 4;
        for (int i = 0; i < RTAO_MASKS; ++i, h.ptr += g_heapStep) g_dev->CreateShaderResourceView(nullptr, &nd, h);
    }
    for (int i = 0; i < kLists; ++i) {
        g_geoRecords[i] = Buffer(16 * kGeometries, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!g_geoRecords[i]) { Fail("the geometries' records", E_OUTOFMEMORY); return false; }
        g_viewRecords[i] = Buffer(kViewBytes + 12 * RTAO_RUNS, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!g_viewRecords[i]) { Fail("the views' records", E_OUTOFMEMORY); return false; }
    }

    {
        D3D12_QUERY_HEAP_DESC qh = {};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = kStamps * kLists;
        if (SUCCEEDED(g_dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&g_stamps))))
            g_stampRead = Buffer(8 * kStamps * kLists, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
        if (!g_stampRead || FAILED(g_queue->GetTimestampFrequency(&g_stampFreq))) g_stampFreq = 0;
    }
    for (int i = 0; i < kLists; ++i) {
        g_instance[i] = Buffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!g_instance[i]) { Fail("the instance", E_OUTOFMEMORY); return false; }
    }
    if (!MakeFence(dev, &g_capturedFence12, &g_capturedFence11) || !MakeFence(dev, &g_aoFence12, &g_aoFence11)) {
        Fail("the shared fences", E_FAIL);
        return false;
    }
    Log("[RTAO] D3D12 on %ls, DXR tier %d.%d", ad.Description, (int)o5.RaytracingTier / 10, (int)o5.RaytracingTier % 10);
    return true;
}

// The views of a slot's depth and AO, rewritten when either texture is new.
void SlotViews(int slot, ID3D12Resource* depth, ID3D12Resource* out) {
    if (g_slotRes[slot][0] == depth && g_slotRes[slot][1] == out) return;
    g_slotRes[slot][0] = depth;
    g_slotRes[slot][1] = out;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)g_heapStep * (slot * 2);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(depth, &sd, h);
    h.ptr += g_heapStep;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_dev->CreateUnorderedAccessView(out, nullptr, &ud, h);
}

void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    g_list->ResourceBarrier(1, &b);
}

void UavBarrier(ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    g_list->ResourceBarrier(1, &b);
}

// The constants rtao_cs.hlsl's Frame holds.
struct Consts {
    float projToRays[16];
    float eye[4];
    float ao[4];
    float view[4];
    float range[4];
    float planes[6][4];   // the camera's frustum in the rays' space, each plane's normal inward and unit long
};

// The six planes of a world to projection matrix (row vectors: clip = p M) whose inside is 0 <= z <= w, |x|, |y| <= w,
// unit normals; a plane at infinity (an endless far plane) as one nothing is behind.
void FrustumPlanes(const float* m, float out[6][4]) {
    auto col = [&](int j, int k) { return m[k * 4 + j]; };
    const float sign[6][2] = { { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { 2, 1 }, { 2, -1 } };
    for (int i = 0; i < 6; ++i) {
        const int j = (int)sign[i][0];
        const float s = sign[i][1];
        float p[4];
        for (int k = 0; k < 4; ++k)
            p[k] = j == 2 ? (s > 0 ? col(2, k) : col(3, k) - col(2, k)) : col(3, k) + s * col(j, k);
        const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (len < 1e-12f || !std::isfinite(len)) {
            out[i][0] = out[i][1] = out[i][2] = 0.0f;
            out[i][3] = 1e30f;
            continue;
        }
        for (int k = 0; k < 4; ++k) out[i][k] = p[k] / len;
    }
}

}  // namespace

bool RtaoD3D12Start(ID3D11Device* dev) {
    std::lock_guard<std::mutex> lk(g_m);
    if (g_tried) return g_ready;
    g_tried = true;
    g_ready = dev && Start(dev);
    return g_ready;
}

bool RtaoD3D12Ready() { return g_ready; }

void RtaoSlotsChanged() {
    std::lock_guard<std::mutex> lk(g_m);
    for (auto& s : g_slotRes) s[0] = s[1] = nullptr;
}

ID3D12Resource* RtaoOpenTexture(ID3D11Texture2D* tex11) {
    if (!g_ready || !tex11) return nullptr;
    ID3D12Resource* out = nullptr;
    IDXGIResource1* r1 = nullptr;
    if (SUCCEEDED(tex11->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1)) && r1) {
        HANDLE h = nullptr;
        if (SUCCEEDED(r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h))
            && h) {
            if (FAILED(g_dev->OpenSharedHandle(h, IID_PPV_ARGS(&out)))) out = nullptr;
            CloseHandle(h);
        }
        r1->Release();
    }
    return out;
}

bool RtaoCaptureBuffer(ID3D11Device* dev, UINT bytes, ID3D11Buffer** out11, UINT* madeBytes) {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_ready) return false;
    if (g_capture11 && g_captureBytes >= bytes) {
        *out11 = g_capture11;
        *madeBytes = g_captureBytes;
        return true;
    }
    // Our lists may still be reading the old one (the engine shows the last AO without waiting when the rays are
    // behind), so it is retired, not released.
    ReleaseDone();
    Retire(g_capture12);
    Retire(g_capture11);
    g_captureBytes = 0;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(dev->CreateBuffer(&d, nullptr, &g_capture11)) || !g_capture11) {
        Log("[RTAO] the %u MB capture buffer could not be made", bytes >> 20);
        return false;
    }
    IDXGIResource* r0 = nullptr;
    HANDLE h = nullptr;
    if (SUCCEEDED(g_capture11->QueryInterface(__uuidof(IDXGIResource), (void**)&r0)) && r0) {
        r0->GetSharedHandle(&h);
        r0->Release();
    }
    if (!h || FAILED(g_dev->OpenSharedHandle(h, IID_PPV_ARGS(&g_capture12))) || !g_capture12) {
        Log("[RTAO] the capture buffer could not be opened on D3D12");
        Drop(g_capture11);
        return false;
    }
    g_captureBytes = bytes;
    Log("[RTAO] capture buffer: %u MB", bytes >> 20);
    *out11 = g_capture11;
    *madeBytes = bytes;
    return true;
}

// A stream output buffer shared with our device, made or made again at bytes or more.
bool SharedSoBuffer(ID3D11Device* dev, UINT bytes, ID3D11Buffer*& b11, ID3D12Resource*& b12, UINT& have,
                    const char* what, ID3D11Buffer** out11, UINT* madeBytes) {
    if (b11 && have >= bytes) {
        *out11 = b11;
        *madeBytes = have;
        return true;
    }
    ReleaseDone();
    Retire(b12);
    Retire(b11);
    have = 0;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(dev->CreateBuffer(&d, nullptr, &b11)) || !b11) {
        Log("[RTAO] the %u MB %s buffer could not be made", bytes >> 20, what);
        return false;
    }
    IDXGIResource* r0 = nullptr;
    HANDLE h = nullptr;
    if (SUCCEEDED(b11->QueryInterface(__uuidof(IDXGIResource), (void**)&r0)) && r0) {
        r0->GetSharedHandle(&h);
        r0->Release();
    }
    if (!h || FAILED(g_dev->OpenSharedHandle(h, IID_PPV_ARGS(&b12))) || !b12) {
        Log("[RTAO] the %s buffer could not be opened on D3D12", what);
        Drop(b11);
        return false;
    }
    have = bytes;
    *out11 = b11;
    *madeBytes = bytes;
    return true;
}

bool RtaoNormalBuffer(ID3D11Device* dev, UINT bytes, ID3D11Buffer** out11, UINT* madeBytes) {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_ready) return false;
    return SharedSoBuffer(dev, bytes, g_norm11, g_norm12, g_normBytes, "normal", out11, madeBytes);
}

bool RtaoUvBuffer(ID3D11Device* dev, UINT bytes, ID3D11Buffer** out11, UINT* madeBytes) {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_ready) return false;
    if (g_uv11 && g_uvBytes >= bytes) {
        *out11 = g_uv11;
        *madeBytes = g_uvBytes;
        return true;
    }
    ReleaseDone();
    Retire(g_uv12);
    Retire(g_uv11);
    g_uvBytes = 0;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(dev->CreateBuffer(&d, nullptr, &g_uv11)) || !g_uv11) {
        Log("[RTAO] the %u MB uv buffer could not be made", bytes >> 20);
        return false;
    }
    IDXGIResource* r0 = nullptr;
    HANDLE h = nullptr;
    if (SUCCEEDED(g_uv11->QueryInterface(__uuidof(IDXGIResource), (void**)&r0)) && r0) {
        r0->GetSharedHandle(&h);
        r0->Release();
    }
    if (!h || FAILED(g_dev->OpenSharedHandle(h, IID_PPV_ARGS(&g_uv12))) || !g_uv12) {
        Log("[RTAO] the uv buffer could not be opened on D3D12");
        Drop(g_uv11);
        return false;
    }
    g_uvBytes = bytes;
    *out11 = g_uv11;
    *madeBytes = bytes;
    return true;
}

ID3D11Fence* RtaoCapturedFence11() { return g_capturedFence11; }
ID3D11Fence* RtaoAoFence11() { return g_aoFence11; }
bool RtaoAoDone(UINT64 value) { return g_aoFence12 && g_aoFence12->GetCompletedValue() >= value; }

bool RtaoTrace(const RtaoFrame& f, UINT64 capturedValue, UINT64 aoValue) {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_ready || !g_capture12 || !f.depth || !f.out || f.slot < 0 || f.slot > 1 || !f.width || !f.height
        || f.vertices < 3 || (UINT64)f.vertices * 16 > g_captureBytes)
        return false;
    if (DeviceLost()) return false;
    ReleaseDone();
    const UINT64 vertices = f.vertices - f.vertices % 3;

    // The allocator this list reuses has finished its last list (two frames back; a wait only when the card is that
    // far behind).
    const int li = g_listIndex;
    g_listIndex = (g_listIndex + 1) % kLists;
    if (g_allocDone[li] && g_aoFence12->GetCompletedValue() < g_allocDone[li]) {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (ev && SUCCEEDED(g_aoFence12->SetEventOnCompletion(g_allocDone[li], ev))) WaitForSingleObject(ev, 2000);
        if (ev) CloseHandle(ev);
        if (g_aoFence12->GetCompletedValue() < g_allocDone[li]) {
            Log("[RTAO] the card has not finished a frame of rays in 2 s; this frame goes without");
            return false;
        }
    }
    // That list's times, now that it has run.
    if (g_stampPending[li] && g_stampFreq) {
        g_stampPending[li] = false;
        const D3D12_RANGE range = { (SIZE_T)8 * kStamps * li, (SIZE_T)8 * kStamps * (li + 1) };
        void* p = nullptr;
        if (SUCCEEDED(g_stampRead->Map(0, &range, &p))) {
            const UINT64* t = (const UINT64*)p + kStamps * li;
            for (int i = 0; i + 1 < kStamps; ++i) {
                const double ms = t[i + 1] > t[i] ? (double)(t[i + 1] - t[i]) * 1000.0 / (double)g_stampFreq : 0.0;
                g_partMs[i] += ms;
                g_lineMs[i] += ms;
            }
            ++g_partFrames;
            ++g_lineFrames;
            const D3D12_RANGE none = { 0, 0 };
            g_stampRead->Unmap(0, &none);
        }
    }

    // The geometries: runs of the captured vertices, opaque between the cut outs, each cut out its own, with the record
    // the AO shader reads by the geometry index (x first vertex, y mask, z reference, w 1 cut out | 2 vertex alpha).
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoms;
    std::vector<uint32_t> records;
    auto run = [&](UINT64 first, UINT64 count, const RtaoCutout* c) {
        if (!count) return;
        D3D12_RAYTRACING_GEOMETRY_DESC g = {};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = c ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = (UINT)count;
        g.Triangles.VertexBuffer.StrideInBytes = 12;
        g.Triangles.VertexBuffer.StartAddress = first * 12;   // an offset until the positions' address is known
        geoms.push_back(g);
        float ref = c ? c->reference : 0.0f;
        uint32_t refBits;
        memcpy(&refBits, &ref, 4);
        records.push_back((uint32_t)first);
        records.push_back(c ? c->mask : 0u);
        records.push_back(refBits);
        records.push_back(c ? (1u | (c->vertexAlpha ? 2u : 0u)) : 0u);
    };
    const bool cutsUsable = g_uv12 && f.cutouts && f.cutoutCount && (UINT64)vertices * 12 <= g_uvBytes;
    UINT64 at = 0;
    for (UINT i = 0; cutsUsable && i < f.cutoutCount && geoms.size() + 2 < kGeometries; ++i) {
        const RtaoCutout& c = f.cutouts[i];
        if (c.firstVertex < at || c.firstVertex + (UINT64)c.vertexCount > vertices || c.mask >= RTAO_MASKS
            || !f.masks[c.mask])
            continue;
        run(at, c.firstVertex - at, nullptr);
        run(c.firstVertex, c.vertexCount, &c);
        at = c.firstVertex + (UINT64)c.vertexCount;
    }
    run(at, vertices - at, nullptr);
    const bool anyCut = geoms.size() > 1 || (geoms.size() == 1 && geoms[0].Flags != D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom = {};
    bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
                 | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    bottom.NumDescs = (UINT)geoms.size();
    bottom.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottom.pGeometryDescs = geoms.data();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bInfo = {};
    g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&bottom, &bInfo);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top = {};
    top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    top.NumDescs = 1;
    top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tInfo = {};
    g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&top, &tInfo);
    UINT64 scratchNeed = bInfo.ScratchDataSizeInBytes > tInfo.ScratchDataSizeInBytes ? bInfo.ScratchDataSizeInBytes
                                                                                    : tInfo.ScratchDataSizeInBytes;
    if (bInfo.UpdateScratchDataSizeInBytes > scratchNeed) scratchNeed = bInfo.UpdateScratchDataSizeInBytes;
    if (f.regrow && ++g_regrowFrames % 30 == 0) {
        // The test: every buffer the structure is made in let go (retired) and made anew, as when a frame grows.
        Retire(g_positions);
        Retire(g_blas);
        Retire(g_tlas);
        Retire(g_scratch);
        g_positionsBytes = g_blasBytes = g_tlasBytes = g_scratchBytes = 0;
    }
    const UINT64 blasBefore = g_blasBytes;
    if (!Grow(g_positions, g_positionsBytes, vertices * 12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        || !Grow(g_blas, g_blasBytes, bInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
        || !Grow(g_tlas, g_tlasBytes, tInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
        || !Grow(g_scratch, g_scratchBytes, scratchNeed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
        Log("[RTAO] the structure for %llu triangles could not be made (out of video memory?)",
            (unsigned long long)(vertices / 3));
        g_builtVertices = 0;
        return false;
    }
    // A refit: the same triangles as the structure was made with, only moved, and it is still there.
    const bool refit = g_builtVertices == vertices && g_builtList == f.drawList && g_sinceBuild < kRebuildEvery
                       && g_blasBytes == blasBefore && !f.noRefit;
    {
        D3D12_RAYTRACING_INSTANCE_DESC inst = {};
        inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.0f;
        inst.InstanceMask = 0xFF;
        inst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        inst.AccelerationStructure = g_blas->GetGPUVirtualAddress();
        void* p = nullptr;
        if (FAILED(g_instance[li]->Map(0, nullptr, &p))) return false;
        memcpy(p, &inst, sizeof(inst));
        g_instance[li]->Unmap(0, nullptr);
    }
    {
        Consts c = {};
        memcpy(c.projToRays, f.projToRays, sizeof(c.projToRays));
        c.eye[0] = f.eye[0];
        c.eye[1] = f.eye[1];
        c.eye[2] = f.eye[2];
        c.ao[0] = f.reach;
        c.ao[1] = f.bias;
        c.ao[2] = f.strength;
        c.ao[3] = (float)f.rays;
        c.view[0] = (float)f.width;
        c.view[1] = (float)f.height;
        c.view[2] = (float)f.seed;
        c.view[3] = f.debug ? 1.0f : 0.0f;
        c.range[0] = f.minDepth;
        c.range[1] = f.maxDepth;
        c.range[2] = (float)vertices;
        c.range[3] = (float)(f.runs && f.runCount ? (f.runCount < RTAO_RUNS ? f.runCount : RTAO_RUNS) : 1);
        FrustumPlanes(f.worldToClip, c.planes);
        void* p = nullptr;
        if (FAILED(g_consts[li]->Map(0, nullptr, &p))) return false;
        memcpy(p, &c, sizeof(c));
        g_consts[li]->Unmap(0, nullptr);
    }
    SlotViews(f.slot, f.depth, f.out);
    {
        void* p = nullptr;
        if (FAILED(g_geoRecords[li]->Map(0, nullptr, &p))) return false;
        memcpy(p, records.data(), records.size() * 4);
        g_geoRecords[li]->Unmap(0, nullptr);
    }
    {
        // The views (the camera's alone when the capture had no others) and the runs (all the camera's then).
        uint8_t* p = nullptr;
        if (FAILED(g_viewRecords[li]->Map(0, nullptr, (void**)&p))) return false;
        UINT views = 0;
        for (UINT i = 0; i < f.viewCount && i < RTAO_VIEWS && f.views; ++i, ++views) memcpy(p + 80 * i, &f.views[i], 80);
        if (!views) {
            RtaoView v = {};
            memcpy(v.clipToRays, f.projToRays, sizeof(v.clipToRays));
            memcpy(p, &v, 80);
            views = 1;
        }
        RtaoRun* r = (RtaoRun*)(p + kViewBytes);
        if (f.runs && f.runCount) {
            for (UINT i = 0; i < f.runCount && i < RTAO_RUNS; ++i) {
                r[i] = f.runs[i];
                if (r[i].view >= views) r[i].view = 0;
                // Normals only where the buffer holds this frame's.
                if (!g_norm12 || (UINT64)vertices * 12 > g_normBytes) r[i].flags = 0;
            }
        } else {
            r[0] = RtaoRun{ 0, 0, 0 };
        }
        g_viewRecords[li]->Unmap(0, nullptr);
    }
    // The masks' views, for those that are new.
    for (int i = 0; i < RTAO_MASKS; ++i) {
        if (!f.masks[i] || f.masks[i] == g_maskViews[i]) continue;
        g_maskViews[i] = f.masks[i];
        D3D12_SHADER_RESOURCE_VIEW_DESC md = {};
        md.Format = DXGI_FORMAT_R8_UNORM;
        md.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        md.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        md.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)g_heapStep * (4 + i);
        g_dev->CreateShaderResourceView(f.masks[i], &md, h);
    }

    if (FAILED(g_alloc[li]->Reset()) || FAILED(g_list->Reset(g_alloc[li], nullptr))) return false;
    ID3D12GraphicsCommandList4* l = g_list;
    l->SetDescriptorHeaps(1, &g_heap);
    l->SetComputeRootSignature(g_root);
    l->SetComputeRootConstantBufferView(0, g_consts[li]->GetGPUVirtualAddress());
    l->SetComputeRootShaderResourceView(1, g_capture12->GetGPUVirtualAddress());
    l->SetComputeRootUnorderedAccessView(2, g_positions->GetGPUVirtualAddress());
    l->SetComputeRootShaderResourceView(3, g_tlas->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE table = g_heap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += (UINT64)g_heapStep * (f.slot * 2);
    l->SetComputeRootDescriptorTable(4, table);
    l->SetComputeRootShaderResourceView(5, g_uv12 ? g_uv12->GetGPUVirtualAddress() : g_capture12->GetGPUVirtualAddress());
    l->SetComputeRootShaderResourceView(6, g_geoRecords[li]->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE masks = g_heap->GetGPUDescriptorHandleForHeapStart();
    masks.ptr += (UINT64)g_heapStep * 4;
    l->SetComputeRootDescriptorTable(7, masks);
    l->SetComputeRootShaderResourceView(8, g_viewRecords[li]->GetGPUVirtualAddress());
    l->SetComputeRootShaderResourceView(9, g_viewRecords[li]->GetGPUVirtualAddress() + kViewBytes);
    // The normals where they were captured; otherwise any buffer (no run says it has them).
    const bool normalsUsable = g_norm12 && (UINT64)vertices * 12 <= g_normBytes;
    l->SetComputeRootShaderResourceView(10, normalsUsable ? g_norm12->GetGPUVirtualAddress()
                                                          : g_capture12->GetGPUVirtualAddress());

    const bool stamps = g_stamps && g_stampFreq;
    const UINT q0 = (UINT)(kStamps * li);
    if (stamps) l->EndQuery(g_stamps, D3D12_QUERY_TYPE_TIMESTAMP, q0);
    // 1. The captured vertices to positions, a triangle a thread.
    l->SetPipelineState(g_toWorld);
    l->Dispatch((UINT)((vertices / 3 + 255) / 256), 1, 1);
    Barrier(g_positions, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (stamps) l->EndQuery(g_stamps, D3D12_QUERY_TYPE_TIMESTAMP, q0 + 1);

    // 2. The structures.
    for (D3D12_RAYTRACING_GEOMETRY_DESC& g : geoms) g.Triangles.VertexBuffer.StartAddress += g_positions->GetGPUVirtualAddress();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bb = {};
    bb.Inputs = bottom;
    bb.Inputs.pGeometryDescs = geoms.data();
    bb.DestAccelerationStructureData = g_blas->GetGPUVirtualAddress();
    if (refit) {
        bb.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        bb.SourceAccelerationStructureData = g_blas->GetGPUVirtualAddress();
    }
    bb.ScratchAccelerationStructureData = g_scratch->GetGPUVirtualAddress();
    l->BuildRaytracingAccelerationStructure(&bb, 0, nullptr);
    UavBarrier(g_blas);
    UavBarrier(g_scratch);
    if (stamps) l->EndQuery(g_stamps, D3D12_QUERY_TYPE_TIMESTAMP, q0 + 2);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tb = {};
    tb.Inputs = top;
    tb.Inputs.InstanceDescs = g_instance[li]->GetGPUVirtualAddress();
    tb.DestAccelerationStructureData = g_tlas->GetGPUVirtualAddress();
    tb.ScratchAccelerationStructureData = g_scratch->GetGPUVirtualAddress();
    l->BuildRaytracingAccelerationStructure(&tb, 0, nullptr);
    UavBarrier(g_tlas);
    Barrier(g_positions, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (stamps) l->EndQuery(g_stamps, D3D12_QUERY_TYPE_TIMESTAMP, q0 + 3);

    // 3. The AO.
    Barrier(f.depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(f.out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The masks the frame's cut outs use, from the engine's side to ours and back.
    bool maskUsed[RTAO_MASKS] = {};
    for (size_t g = 0; anyCut && g < geoms.size(); ++g)
        if (records[g * 4 + 3] & 1u) maskUsed[records[g * 4 + 1]] = true;
    for (int i = 0; i < RTAO_MASKS; ++i)
        if (maskUsed[i]) Barrier(f.masks[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    l->SetPipelineState(g_ao);
    l->Dispatch((f.width + 7) / 8, (f.height + 7) / 8, 1);
    for (int i = 0; i < RTAO_MASKS; ++i)
        if (maskUsed[i]) Barrier(f.masks[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(f.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Barrier(f.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (stamps) {
        l->EndQuery(g_stamps, D3D12_QUERY_TYPE_TIMESTAMP, q0 + 4);
        l->ResolveQueryData(g_stamps, D3D12_QUERY_TYPE_TIMESTAMP, q0, kStamps, g_stampRead, (UINT64)8 * q0);
        g_stampPending[li] = true;
    }
    if (FAILED(l->Close())) return false;

    if (FAILED(g_queue->Wait(g_capturedFence12, capturedValue))) return false;
    ID3D12CommandList* lists[] = { l };
    g_queue->ExecuteCommandLists(1, lists);
    if (FAILED(g_queue->Signal(g_aoFence12, aoValue))) {
        Log("[RTAO] the AO fence could not be signalled; RTAO stops");
        g_ready = false;
        return false;
    }
    g_allocDone[li] = aoValue;
    g_lastSent = aoValue;
    if (refit) {
        ++g_sinceBuild;
        g_refits.fetch_add(1, std::memory_order_relaxed);
        ++g_lineRefits;
    } else {
        g_sinceBuild = 0;
        g_builtList = f.drawList;
        g_builtVertices = vertices;
        g_builds.fetch_add(1, std::memory_order_relaxed);
        ++g_lineBuilds;
    }
    g_traced.fetch_add(1, std::memory_order_relaxed);
    g_lastTriangles.store((long long)(vertices / 3), std::memory_order_relaxed);
    return true;
}

void RtaoD3D12Info(long long* triangles, float* structureMb, const char** why) {
    *triangles = g_lastTriangles.load(std::memory_order_relaxed);
    *structureMb = (float)(g_positionsBytes + g_blasBytes + g_tlasBytes + g_scratchBytes + g_captureBytes + g_uvBytes
                           + g_normBytes)
                   / 1048576.0f;
    *why = g_why;
}

void RtaoD3D12Builds(long long* builds, long long* refits) {
    *builds = g_builds.exchange(0, std::memory_order_relaxed);
    *refits = g_refits.exchange(0, std::memory_order_relaxed);
}

void RtaoD3D12LineTimes(double ms[4], long long* builds, long long* refits) {
    std::lock_guard<std::mutex> lk(g_m);
    for (int i = 0; i < 4; ++i) ms[i] = g_lineFrames ? g_lineMs[i] / (double)g_lineFrames : 0.0;
    *builds = g_lineBuilds;
    *refits = g_lineRefits;
    for (double& v : g_lineMs) v = 0.0;
    g_lineFrames = g_lineBuilds = g_lineRefits = 0;
}

void RtaoD3D12Times(double ms[4]) {
    std::lock_guard<std::mutex> lk(g_m);
    for (int i = 0; i < 4; ++i) ms[i] = g_partFrames ? g_partMs[i] / (double)g_partFrames : 0.0;
    for (double& v : g_partMs) v = 0.0;
    g_partFrames = 0;
}

void RtaoD3D12Stats(long long* traced, long long* triangles, float* structureMb) {
    *traced = g_traced.exchange(0, std::memory_order_relaxed);
    *triangles = g_lastTriangles.load(std::memory_order_relaxed);
    *structureMb = (float)(g_positionsBytes + g_blasBytes + g_tlasBytes + g_scratchBytes + g_captureBytes) / 1048576.0f;
}
