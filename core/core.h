#pragma once
// The proxy core: S2FM's dxgi.dll stand in. It forwards the three factory functions to the system's dxgi.dll and
// hooks what the features need to see of the engine's D3D11 work: the device's creations, every context's draws,
// dispatches and bindings, the constant buffers' contents and the swapchains' presents. The features (the ReShade
// stages, particle refraction, the viewport AO, the colour editor) register with it and are called at those points.
//
// Every path is beside the DLL; nothing names a machine's folders.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <string>

void Log(const char* fmt, ...);

// This DLL's folder (the game's bin\win64), and a file in it or under it.
const std::wstring& CoreDir();
std::wstring CorePath(const wchar_t* name);
// The game folder (...\Half-Life Alyx\game).
std::wstring CoreGameDir();

// What a context has bound, as the hooks saw it bound. Read on the thread that draws on the context.
struct ContextState {
    ID3D11DeviceContext* ctx = nullptr;
    bool deferred = false;

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    // The bound shaders' layouts in the shader table (shader_table.h), -1 when not known, their checkbox sets, and
    // the pixel shader's render state as the table has it.
    int vsLayout = -1, psLayout = -1, csLayout = -1;
    int vsCombo = -1, psCombo = -1, csCombo = -1;
    uint16_t psFlags = 0;

    ID3D11Buffer* vsCb[14] = {};
    ID3D11Buffer* psCb[14] = {};
    ID3D11Buffer* csCb[14] = {};
    ID3D11ShaderResourceView* psSrv[32] = {};
    ID3D11ShaderResourceView* csSrv[32] = {};
    ID3D11UnorderedAccessView* csUav[8] = {};

    // The first render target's size and format, and the depth target's size, as bound.
    UINT rtW = 0, rtH = 0;
    DXGI_FORMAT rtFormat = DXGI_FORMAT_UNKNOWN;
    UINT dsW = 0, dsH = 0;
    // Counted up on every bind of targets; whose window the draws after a bind are for is decided once a bind
    // (CoreFrameForeign).
    uint32_t rtBindGen = 0, foreignGen = ~0u;
    bool foreign = false;

    // The blend of the first target and whether depth is written, as set.
    bool blend = false;
    uint8_t blendSrc = 0, blendDst = 0;
    bool depthWrites = true;

    // The draw about to be made, set before the features' beforeDraw: indexed or not, from a buffer (indirect: the
    // counts are not known here), vertices or indices, instances, the first index or vertex, the vertex base and the
    // first instance.
    struct Draw {
        bool indexed = false, indirect = false;
        UINT count = 0, instances = 1, start = 0, startInstance = 0;
        INT base = 0;
    } draw;
};
ContextState* CoreState(ID3D11DeviceContext* ctx);

// A constant buffer of 4 KB or less: the CPU's copy of what the engine last wrote into it, read into out from
// offset. False when the buffer has no copy or is shorter. *total, when asked, gets its size.
bool CoreCbRead(ID3D11Buffer* cb, UINT offset, void* out, UINT bytes, UINT* total = nullptr);

// The engine's PerView buffer (816 bytes, vertex stage b1 or b0) of the draw about to be made: its floats.
// False when neither slot holds one.
bool CorePerView(const ContextState* st, float out[204]);
// Float offsets into PerView (the engine's own vertex shader reflection).
enum : int {
    PV_MAT_WORLD_TO_VIEW = 32,
    PV_MAT_VIEW_TO_PROJ = 48,
    PV_TONEMAP_LINEAR = 72,
    PV_TONEMAP_GAMMA = 75,
    PV_CAMERA_POSITION_WS = 76,
    PV_TIME = 87,
    PV_VIEWPORT_SIZE = 106,
    PV_MAT_WORLD_TO_PROJ_MV = 128,
    PV_WORLD_TO_CAM_OFFSET = 176,
    PV_WORLD_TO_CAM_OFF_MV = 180,
};

// A shader object's bytecode as the engine created it, into out (any size; *bytes in, the size out).
bool CoreShaderBytecode(ID3D11DeviceChild* shader, void* out, UINT* bytes);

// Whose window the draws on a context are for: true when another tool's (the Asset Browser's, the Particle
// Editor's preview), whose frames the features leave alone. And whether a swapchain presents into a window of
// S2FM's (its viewports, the engine window a movie render presents into).
bool CoreFrameForeign(ContextState* st);
bool CoreSwapChainIsS2fm(IDXGISwapChain* sc);
// Whether S2FM is rendering a movie to disk now (its Movie Layoff Progress dialog is up).
bool CoreExportActive();

// Set while a feature draws or binds on a context itself: the hooks pass the calls straight through, record the
// bindings and call no feature.
extern thread_local bool t_CorePassThrough;

// A feature: called at these points on the thread the engine drew on. Any member may be null.
struct CoreFeature {
    const char* name;
    // Before and after a draw or a dispatch of the engine's, on any context.
    void (*beforeDraw)(ID3D11DeviceContext* ctx, ContextState* st);
    void (*afterDraw)(ID3D11DeviceContext* ctx, ContextState* st);
    void (*beforeDispatch)(ID3D11DeviceContext* ctx, ContextState* st, UINT x, UINT y, UINT z);
    // A swapchain's Present, before it goes to the system's.
    void (*present)(IDXGISwapChain* sc);
    // A buffer opened for writing (data is where the engine writes) and closed again, before its contents go on.
    void (*mapped)(ID3D11DeviceContext* ctx, ID3D11Resource* res, void* data);
    void (*unmapping)(ID3D11DeviceContext* ctx, ID3D11Resource* res);
    // A resource written through UpdateSubresource (box null: the whole of it), before it goes on.
    void (*updating)(ID3D11DeviceContext* ctx, ID3D11Resource* res, const D3D11_BOX* box, const void* data);
};
// Up to 16, kept for the process's life (the struct must stay alive).
void CoreRegister(const CoreFeature* feature);

// The original, unhooked call of a draw, for a feature's own full screen passes.
void CoreRawDraw(ID3D11DeviceContext* ctx, UINT vertexCount);
// The engine's draw about to be made or just made (st->draw) once more, unhooked, with whatever is bound now. False
// for a draw from a buffer (its counts are not known here).
bool CoreRedraw(ID3D11DeviceContext* ctx, const ContextState* st);

// A hook on any function (MinHook), for a feature's own hooks into the game's modules. False when it could not go on.
bool CoreHook(void* target, void* detour, void** original);
// Called after every library load of the process (the loader's own LdrLoadDll, on the loading thread, the loader
// lock already let go), and once when registered: for a feature waiting for a module of the game to come in. Up to 8.
void CoreOnModuleLoad(void (*fn)());

// Crash reports into s2fm_mods_crash\ beside the DLL (core_crash.cpp); called once at the start.
void CoreCrashInstall();

// The shader table (made from the game's shaders on first start, beside the DLL): whether it is loaded yet.
// Shaders made before it was are named when next bound.
bool CoreTableReady();
