// The panel's way to the ReShade core (tools/s2fm_reshade_core.dll): found loaded (the proxy loads it when
// effects are on) or loaded from beside this DLL, started with the game's bin\win64 folder, and its C API
// (core/rs_core_api.h) looked up. Plain C types only: this DLL is built with /Zc:wchar_t- for Qt 4.
#pragma once

#include "core/rs_core_api.h"

struct RsCore
{
    bool ok;
    char why[256];
    RsCore_Status_Fn status;
    RsCore_EffectsOn_Fn effectsOn;
    RsCore_LoadPreset_Fn loadPreset;
    RsCore_SavePreset_Fn savePreset;
    RsCore_ListPresets_Fn listPresets;
    RsCore_Reload_Fn reload;
    RsCore_TechniqueCount_Fn techniqueCount;
    RsCore_TechniqueInfo_Fn techniqueInfo;
    RsCore_SetTechniqueEnabled_Fn setTechniqueEnabled;
    RsCore_SetTechniqueStage_Fn setTechniqueStage;
    RsCore_MoveTechnique_Fn moveTechnique;
    RsCore_EffectErrors_Fn effectErrors;
    RsCore_Errors_Fn errors;
    RsCore_UniformCount_Fn uniformCount;
    RsCore_UniformInfo_Fn uniformInfo;
    RsCore_GetUniform_Fn getUniform;
    RsCore_SetUniform_Fn setUniform;
    RsCore_ResetUniform_Fn resetUniform;
    RsCore_DefinitionCount_Fn definitionCount;
    RsCore_Definition_Fn definition;
    RsCore_SetDefinition_Fn setDefinition;
    RsCore_ListEffects_Fn listEffects;
    RsCore_AddEffect_Fn addEffect;
    RsCore_Tick_Fn tick;
};

// The core, connected once; ok false (with why) when it cannot be.
const RsCore& RsCoreApi();

// Each frame the tool framework runs, on S2FM's main thread (rs_tool.cpp, RunFrame): the core's tick, which has
// S2FM draw while effects load (they are made on the card only as frames begin).
void RsCoreFrame();
