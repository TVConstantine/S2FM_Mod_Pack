#pragma once
// The contract between the proxy (dxgi.dll, reshade/stages.cpp) and the ReShade core
// (tools/s2fm_reshade_core.dll): what a stage is given. The proxy finds the stage points in S2FM's frame
// and calls the callback the core registered with DxrStages_Register.
#include <d3d11.h>
#include <stdint.h>

#define DXR_STAGES_API_VERSION 1

enum DxrStageId { DXR_STAGE_A = 0, DXR_STAGE_B = 1, DXR_STAGE_C = 2 };

// How the engine's texture holds its colour.
enum DxrStageEncoding {
    DXR_ENC_SCENE = 0,    // linear HDR scene before the camera's exposure, MSAA: stage A
    DXR_ENC_LINEAR = 1,   // linear display-referred (post_process's output), every sample of a pixel the same: B
    DXR_ENC_SRGB8 = 2,    // 8-bit, typeless, holding sRGB-encoded bytes, read and written raw: C
};

enum DxrStageFlags { DXR_STAGE_RENDERING = 1, DXR_STAGE_REFINING = 2 };

struct DxrStageFrame {
    uint32_t size;                     // sizeof(DxrStageFrame)
    int stage;                         // DxrStageId
    ID3D11DeviceContext* ctx;          // the engine's immediate context
    ID3D11Texture2D* target;           // read and written in place
    DXGI_FORMAT viewFormat;            // the typed format to view it with (R16G16B16A16_FLOAT, R8G8B8A8_UNORM)
    int encoding;                      // DxrStageEncoding
    uint32_t width, height;            // the picture, at the target's top left
    uint32_t samples;                  // the target's sample count
    ID3D11ShaderResourceView* viewZ;   // R32F view distance, the picture's size, 0 = sky; null when unknown
    float exposure;                    // what post_process multiplies the scene by (g_flToneMapScalarLinear); A only
    double seconds;                    // the playhead
    uint32_t filmFrame;                // the playhead in film frames
    uint32_t sampleIndex;              // refinement sample, 0 = first (or not refining)
    uint32_t flags;                    // DxrStageFlags
};

// Returns 1 when it drew. Called on the render thread inside the engine's frame; its draws pass the proxy's
// hooks untouched (t_DxrStagePassThrough).
typedef int(__cdecl* DxrStageCallback)(const DxrStageFrame* frame, void* user);
typedef int(__cdecl* DxrStages_ApiVersion_Fn)();
typedef int(__cdecl* DxrStages_Register_Fn)(DxrStageCallback cb, void* user);
