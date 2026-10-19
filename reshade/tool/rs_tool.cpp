// The ReShade tool's tool system: the object CreateInterface("ToolSystem2_001") hands the engine's
// tool framework: IAppSystem's eleven slots, then
// IToolSystem2's thirteen. The rare slots log their call and where it came from; QueryInterface and
// the two every frame slots do nothing that costs. A slot of unknown meaning takes three integers and
// returns zero.
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rs_bridge.h"
#include "rs_log.h"
#include "rs_window.h"

typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);

namespace {

struct AppSystemInfo
{
    const char* module;
    const char* iface;
};
const AppSystemInfo kNoDependencies[1] = { { nullptr, nullptr } };

// A pointer for the log: as text when it points at up to 200 printable bytes and a zero.
void Describe(const void* p, char* out, size_t outSize)
{
    sprintf_s(out, outSize, "%p", p);
    if (!p) return;
    __try {
        const unsigned char* s = (const unsigned char*)p;
        size_t n = 0;
        while (n < 200 && s[n] >= 32 && s[n] < 127) ++n;
        if (n >= 3 && s[n] == 0) sprintf_s(out, outSize, "%p \"%.*s\"", p, (int)n, (const char*)s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Where a call came from, as module+rva of its return address.
struct Caller
{
    char text[MAX_PATH + 32];
    explicit Caller(const void* ret)
    {
        HMODULE m = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)ret, &m) && m)
            GetModuleFileNameA(m, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        sprintf_s(text, "%s+0x%llX", name ? name + 1 : path,
                  (unsigned long long)((const char*)ret - (const char*)m));
    }
};
#define FROM Caller(_ReturnAddress()).text

class RsToolSystem
{
public:
    // IAppSystem, slots 0 to 10.
    virtual bool Connect(CreateInterfaceFn factory) { m_factory = factory; return true; }
    virtual void Disconnect() { RsToolLog("slot 1 Disconnect from %s", FROM); }
    // Nothing: answering another tool's "ToolSystem2_001" with this object would open ours in its
    // place.
    virtual void* QueryInterface(const char*) { return nullptr; }
    virtual int Init() { return 1; }   // INIT_OK
    virtual void Shutdown() { RsToolLog("slot 4 Shutdown from %s", FROM); RsWindowShutdown(); }
    virtual void PreShutdown() { RsToolLog("slot 5 PreShutdown from %s", FROM); }
    virtual const AppSystemInfo* GetDependencies() { return kNoDependencies; }
    virtual int GetTier() { return 3; }   // APP_SYSTEM_TIER3
    virtual void Reconnect(CreateInterfaceFn factory, const char* name)
    {
        char d[300];
        Describe(name, d, sizeof(d));
        RsToolLog("slot 8 Reconnect(factory %p, %s) from %s", (void*)factory, d, FROM);
    }
    virtual bool IsSingleton() { return false; }
    virtual int GetBuildType() { return 2; }   // kBuildTypeRelease

    // IToolSystem2, slots 11 to 23.
    virtual bool CreateTool(CreateInterfaceFn factory, uintptr_t index)
    {
        m_toolFactory = factory;
        m_mainThread = GetCurrentThreadId();
        RsToolLog("slot 11 CreateTool(factory %p, index %llu) from %s", (void*)factory, (unsigned long long)index, FROM);
        return true;
    }
    virtual uintptr_t Slot12(uintptr_t, uintptr_t, uintptr_t) { return 0; }   // every frame
    // Every frame. On the thread the tool was made on (S2FM's main thread, where the panel's Qt lives too), the
    // core's tick: it has S2FM draw while effects load (core/rs_game.cpp, RsCore_Tick).
    virtual void RunFrame(uintptr_t, uintptr_t, uintptr_t)
    {
        const DWORD thread = GetCurrentThreadId();
        static bool said = false;
        if (!said) {
            said = true;
            RsToolLog("slot 13 RunFrame, first call, on the thread the tool was made on: %s (%lu)",
                      thread == m_mainThread ? "yes" : "no", (unsigned long)m_mainThread);
        }
        if (thread == m_mainThread) RsCoreFrame();
    }
    // Asked as the tools or the game quit; non-zero cancels the quit. There is no document to ask
    // about: a change still waiting for its save is written now, and the quit goes on.
    virtual int CancelQuit(uintptr_t, uintptr_t, uintptr_t)
    {
        RsToolLog("slot 14 CancelQuit -> 0 from %s", FROM);
        RsWindowFlush();
        return 0;
    }
    virtual uintptr_t Slot15(uintptr_t a, uintptr_t b, uintptr_t c) { return Unknown(15, _ReturnAddress(), a, b, c); }
    virtual void OpenAssets(const void*)
    {
        RsToolLog("slot 16 OpenAssets from %s", FROM);
        RsWindowShow();
    }
    virtual void Show(const void* context)
    {
        char d[300];
        Describe(context, d, sizeof(d));
        RsToolLog("slot 17 Show(%s) from %s", d, FROM);
        if (!context) RsWindowShow();
    }
    virtual void Hide(uintptr_t, uintptr_t, uintptr_t)
    {
        RsToolLog("slot 18 Hide from %s", FROM);
        RsWindowHide();
    }
    virtual uintptr_t Slot19(uintptr_t a, uintptr_t b, uintptr_t c) { return Unknown(19, _ReturnAddress(), a, b, c); }
    virtual int SupportsMode(int mode)
    {
        RsToolLog("slot 20 SupportsMode(%d) -> %d from %s", mode, mode == 0 ? 1 : 0, FROM);
        return mode == 0 ? 1 : 0;
    }
    virtual uintptr_t Slot21(uintptr_t a, uintptr_t b, uintptr_t c) { return Unknown(21, _ReturnAddress(), a, b, c); }
    virtual uintptr_t Slot22(uintptr_t a, uintptr_t b, uintptr_t c) { return Unknown(22, _ReturnAddress(), a, b, c); }
    virtual uintptr_t Slot23(uintptr_t a, uintptr_t b, uintptr_t c) { return Unknown(23, _ReturnAddress(), a, b, c); }

private:
    uintptr_t Unknown(int slot, const void* ret, uintptr_t a, uintptr_t b, uintptr_t c)
    {
        char da[300], db[300], dc[300];
        Describe((const void*)a, da, sizeof(da));
        Describe((const void*)b, db, sizeof(db));
        Describe((const void*)c, dc, sizeof(dc));
        RsToolLog("slot %d (%s, %s, %s) -> 0 from %s", slot, da, db, dc, Caller(ret).text);
        return 0;
    }

    CreateInterfaceFn m_factory = nullptr;
    CreateInterfaceFn m_toolFactory = nullptr;
    DWORD m_mainThread = 0;
};

RsToolSystem g_toolSystem;

}

extern "C" __declspec(dllexport) void* CreateInterface(const char* name, int* returnCode)
{
    const bool ours = name && strcmp(name, "ToolSystem2_001") == 0;
    if (returnCode) *returnCode = ours ? 0 : 1;   // IFACE_OK, IFACE_FAILED
    return ours ? &g_toolSystem : nullptr;
}

extern "C" __declspec(dllexport) bool InstallSchemaBindings(const char*, void*)
{
    return true;
}

extern "C" __declspec(dllexport) int GetResourceManifestCount()
{
    return 0;
}

extern "C" __declspec(dllexport) void GetResourceManifests(void*, void*, void*)
{
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        RsToolLog("s2fm_reshade.dll loaded into %s, process %lu", exe, GetCurrentProcessId());
    } else if (reason == DLL_PROCESS_DETACH) {
        RsToolLog("s2fm_reshade.dll unloaded (%s)", reserved ? "process exit" : "FreeLibrary");
    }
    return TRUE;
}
