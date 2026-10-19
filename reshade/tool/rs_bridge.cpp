#include "rs_bridge.h"
#include "rs_log.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace {

template <typename T> bool Get(HMODULE m, const char* name, T& out)
{
    out = (T)GetProcAddress(m, name);
    return out != 0;
}

RsCore Connect()
{
    RsCore c;
    memset(&c, 0, sizeof(c));
    // This DLL's folder (bin\win64\tools) holds the core; the game's bin\win64 is its parent.
    HMODULE self = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&Connect, &self);
    char dir[MAX_PATH] = {};
    GetModuleFileNameA(self, dir, MAX_PATH);
    if (char* slash = strrchr(dir, '\\')) *slash = 0;
    char core[MAX_PATH + 32];
    sprintf_s(core, "%s\\s2fm_reshade_core.dll", dir);
    HMODULE m = GetModuleHandleA("s2fm_reshade_core.dll");
    if (!m) m = LoadLibraryA(core);
    if (!m) {
        sprintf_s(c.why, "the core (%s) could not be loaded: error %lu", core, GetLastError());
        return c;
    }
    RsCore_ApiVersion_Fn version = 0;
    RsCore_Start_Fn start = 0;
    if (!Get(m, "RsCore_ApiVersion", version) || version() != RS_CORE_API_VERSION) {
        sprintf_s(c.why, "the core speaks interface %d, this panel %d", version ? version() : -1, RS_CORE_API_VERSION);
        return c;
    }
    char bin[MAX_PATH] = {};
    strcpy_s(bin, dir);
    if (char* slash = strrchr(bin, '\\')) *slash = 0;   // ...\bin\win64
    if (!Get(m, "RsCore_Start", start) || !start(bin)) {
        sprintf_s(c.why, "the core did not start: dxgi.dll is not the proxy, or speaks another interface");
        return c;
    }
    bool ok = Get(m, "RsCore_Status", c.status) && Get(m, "RsCore_EffectsOn", c.effectsOn)
        && Get(m, "RsCore_LoadPreset", c.loadPreset) && Get(m, "RsCore_SavePreset", c.savePreset)
        && Get(m, "RsCore_ListPresets", c.listPresets) && Get(m, "RsCore_Reload", c.reload)
        && Get(m, "RsCore_TechniqueCount", c.techniqueCount) && Get(m, "RsCore_TechniqueInfo", c.techniqueInfo)
        && Get(m, "RsCore_SetTechniqueEnabled", c.setTechniqueEnabled)
        && Get(m, "RsCore_SetTechniqueStage", c.setTechniqueStage) && Get(m, "RsCore_MoveTechnique", c.moveTechnique)
        && Get(m, "RsCore_EffectErrors", c.effectErrors) && Get(m, "RsCore_Errors", c.errors)
        && Get(m, "RsCore_UniformCount", c.uniformCount) && Get(m, "RsCore_UniformInfo", c.uniformInfo)
        && Get(m, "RsCore_GetUniform", c.getUniform) && Get(m, "RsCore_SetUniform", c.setUniform)
        && Get(m, "RsCore_ResetUniform", c.resetUniform) && Get(m, "RsCore_DefinitionCount", c.definitionCount)
        && Get(m, "RsCore_Definition", c.definition) && Get(m, "RsCore_SetDefinition", c.setDefinition)
        && Get(m, "RsCore_ListEffects", c.listEffects) && Get(m, "RsCore_AddEffect", c.addEffect)
        && Get(m, "RsCore_Tick", c.tick);
    if (!ok) {
        sprintf_s(c.why, "the core lacks a function this panel needs");
        return c;
    }
    c.ok = true;
    RsToolLog("connected to the core %s, game bin %s", core, bin);
    return c;
}

}   // namespace

const RsCore& RsCoreApi()
{
    static const RsCore c = Connect();
    if (!c.ok) {
        static bool said = false;
        if (!said) { said = true; RsToolLog("core: %s", c.why); }
    }
    return c;
}

void RsCoreFrame()
{
    // Only a core already in the process (the proxy loads it for a session's preset, the panel when it opens):
    // the tick never brings it in by itself.
    static bool s_there = false;
    if (!s_there && !(s_there = GetModuleHandleA("s2fm_reshade_core.dll") != 0)) return;
    const RsCore& c = RsCoreApi();
    if (c.ok) c.tick();
}
