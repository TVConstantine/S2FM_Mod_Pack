#pragma once
// The ReShade core's C API (tools/s2fm_reshade_core.dll). Plain C: UTF-8 strings, no wchar_t, no Qt, so the
// Qt panel (built with /Zc:wchar_t-) and the harness can both call it.
#include <stdint.h>

#define RS_CORE_API_VERSION 3

#ifdef RS_CORE_EXPORTS
#define RS_API extern "C" __declspec(dllexport)
#else
#define RS_API extern "C" __declspec(dllimport)
#endif

struct ID3D11Device;
struct ID3D11DeviceContext;

typedef int (*RsCore_ApiVersion_Fn)();
RS_API int RsCore_ApiVersion();

// In the game: the game's bin\win64 folder (UTF-8). Registers the stage callback with dxgi.dll; 1 when it did.
typedef int (*RsCore_Start_Fn)(const char* gameBinWin64);
RS_API int RsCore_Start(const char* gameBinWin64);

// --- The harness's way in (reshade/harness/rs_harness.cpp): a runtime on its own device, a picture put
// into the back buffer and read out, a stage run with the techniques named. Not used in the game.
// configPath: a ReShade.ini-format file. definitions: "NAME=value,NAME=value" for every effect, or null.
typedef int (*RsCore_HarnessInit_Fn)(ID3D11Device* dev, ID3D11DeviceContext* ctx, unsigned width, unsigned height,
                                     const char* configPath, const char* definitions);
typedef int (*RsCore_HarnessWaitLoaded_Fn)(unsigned timeoutMs);
typedef int (*RsCore_HarnessUpload_Fn)(const float* rgba, unsigned width, unsigned height);
typedef int (*RsCore_HarnessDownload_Fn)(float* rgba, unsigned width, unsigned height);
// techniques: comma list of "Name@file.fx" or "Name"; "" every technique; "-" none.
typedef int (*RsCore_HarnessRun_Fn)(double seconds, unsigned long long frameCount, unsigned seed, const char* techniques);
typedef int (*RsCore_HarnessReadTexture_Fn)(const char* name, float* rgba, unsigned width, unsigned height);
typedef void (*RsCore_HarnessShutdown_Fn)();
typedef int (*RsCore_TechniqueCount_Fn)();
typedef int (*RsCore_TechniqueName_Fn)(int i, char* out, int size, int* enabled);
typedef int (*RsCore_Errors_Fn)(char* out, int size);
RS_API int RsCore_HarnessInit(ID3D11Device* dev, ID3D11DeviceContext* ctx, unsigned width, unsigned height,
                              const char* configPath, const char* definitions);
RS_API int RsCore_HarnessWaitLoaded(unsigned timeoutMs);
RS_API int RsCore_HarnessUpload(const float* rgba, unsigned width, unsigned height);
RS_API int RsCore_HarnessDownload(float* rgba, unsigned width, unsigned height);
RS_API int RsCore_HarnessRun(double seconds, unsigned long long frameCount, unsigned seed, const char* techniques);
RS_API int RsCore_HarnessReadTexture(const char* name, float* rgba, unsigned width, unsigned height);
RS_API void RsCore_HarnessShutdown();
// View distance (R32F, width x height, 0 = sky) handed to every effect as its depth, as the game's stages do.
typedef int (*RsCore_HarnessDepth_Fn)(const float* viewZ, unsigned width, unsigned height, double nearPlane, double farPlane);
RS_API int RsCore_HarnessDepth(const float* viewZ, unsigned width, unsigned height, double nearPlane, double farPlane);
// A whole stage on an engine-like texture the harness made (frame: a DxrStageFrame, rs_stage_api.h).
typedef int (*RsCore_HarnessStage_Fn)(const void* frame, const char* techniques);
RS_API int RsCore_HarnessStage(const void* frame, const char* techniques);
// The techniques in the preset's order ("Name@file.fx"); the text the compiler gave for failed effects.
RS_API int RsCore_TechniqueCount();
RS_API int RsCore_TechniqueName(int i, char* out, int size, int* enabled);
RS_API int RsCore_Errors(char* out, int size);

// --- The panel's way in (reshade/tool, Qt's thread). Every call holds the core's lock, so it never meets
// the runtime half way through a frame; a change has S2FM draw the view again. Techniques are numbered in the
// preset's order (0 .. RsCore_TechniqueCount() - 1); their settings are their effect file's.
struct RsStatus {
    int running;          // a runtime is made
    int loading;          // effects are compiling
    int techniques;
    int failed;           // effects that did not compile
    int effectsOn;
    unsigned width, height;
    float stageMs[3];     // what A, B and C cost on the card, ms
    char preset[520];     // the preset in use (its source), UTF-8, "" for none
    char message[256];
};
struct RsTechniqueInfo {
    char name[128];
    char effect[128];     // file name
    int enabled;
    int stage;            // 0 A, 1 B, 2 C: in use
    int autoStage;        // what the rules give
    int overridden;       // set by hand
    int readsDepth;
};
struct RsUniformInfo {
    char name[64];
    char label[128];
    char category[128];
    char tooltip[1024];
    char uiType[16];      // slider, drag, combo, radio, list, input, color, ""
    int baseType;         // 0 bool, 1 int, 2 uint, 3 float
    int components;       // 1 .. 16
    float uiMin, uiMax, uiStep;
    char items[2048];     // combo/radio/list items, each ended by a 0
    int itemCount;
    int categoryClosed;
    char units[32];
};
typedef int (*RsCore_Status_Fn)(RsStatus* out);
typedef int (*RsCore_EffectsOn_Fn)(int on);
typedef int (*RsCore_LoadPreset_Fn)(const char* path);
typedef int (*RsCore_SavePreset_Fn)(const char* path);
typedef int (*RsCore_ListPresets_Fn)(char* out, int size);
typedef int (*RsCore_Reload_Fn)();
typedef int (*RsCore_TechniqueInfo_Fn)(int i, RsTechniqueInfo* out);
typedef int (*RsCore_SetTechniqueEnabled_Fn)(int i, int on);
typedef int (*RsCore_SetTechniqueStage_Fn)(int i, int stage);
typedef int (*RsCore_MoveTechnique_Fn)(int i, int to);
typedef int (*RsCore_EffectErrors_Fn)(int i, char* out, int size);
typedef int (*RsCore_UniformCount_Fn)(int i);
typedef int (*RsCore_UniformInfo_Fn)(int i, int u, RsUniformInfo* out);
typedef int (*RsCore_GetUniform_Fn)(int i, int u, float* values, int count);
typedef int (*RsCore_SetUniform_Fn)(int i, int u, const float* values, int count);
typedef int (*RsCore_ResetUniform_Fn)(int i, int u);
typedef int (*RsCore_DefinitionCount_Fn)(int i);
typedef int (*RsCore_Definition_Fn)(int i, int d, char* name, int nameSize, char* value, int valueSize);
typedef int (*RsCore_SetDefinition_Fn)(int i, const char* name, const char* value);
typedef int (*RsCore_ListEffects_Fn)(char* out, int size);
typedef int (*RsCore_AddEffect_Fn)(const char* file);
typedef int (*RsCore_Tick_Fn)();
RS_API int RsCore_Status(RsStatus* out);
RS_API int RsCore_EffectsOn(int on);                    // -1 asks
RS_API int RsCore_LoadPreset(const char* path);         // "" for none
RS_API int RsCore_SavePreset(const char* path);         // "" or null: back where it came from
RS_API int RsCore_ListPresets(char* out, int size);     // the presets beside ReShade.ini, one full path a line
RS_API int RsCore_Reload();
RS_API int RsCore_Tick();                              // each frame, on S2FM's main thread (the tool's RunFrame)
RS_API int RsCore_TechniqueInfo(int i, RsTechniqueInfo* out);
RS_API int RsCore_SetTechniqueEnabled(int i, int on);
RS_API int RsCore_SetTechniqueStage(int i, int stage);  // -1: back to the rules
RS_API int RsCore_MoveTechnique(int i, int to);
RS_API int RsCore_EffectErrors(int i, char* out, int size);
RS_API int RsCore_UniformCount(int i);
RS_API int RsCore_UniformInfo(int i, int u, RsUniformInfo* out);
RS_API int RsCore_GetUniform(int i, int u, float* values, int count);
RS_API int RsCore_SetUniform(int i, int u, const float* values, int count);
RS_API int RsCore_ResetUniform(int i, int u);
RS_API int RsCore_DefinitionCount(int i);
RS_API int RsCore_Definition(int i, int d, char* name, int nameSize, char* value, int valueSize);
RS_API int RsCore_SetDefinition(int i, const char* name, const char* value);
RS_API int RsCore_ListEffects(char* out, int size);     // the .fx files in the search paths: "file<TAB>collection" a line
RS_API int RsCore_AddEffect(const char* file);          // its techniques added to the preset, enabled
