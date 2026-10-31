// Shared by every stage of a model shader: the engine's per view buffers and the tools visualisation mode.
#include "engine_cbuffers.hlsl"

StaticCombo(S_MODE_TOOLS_VIS, 0..1, Sys(ALL));
DynamicCombo(D_MULTIVIEW_INSTANCING, 0..1, Sys(ALL));

// What the vertex stage hands the pixel stage (S2FM draws models in the VrForward mode).
struct PixelInput
{
    float3 vPositionWs : TEXCOORD0;    // less the lighting offset (g_vHighPrecisionLightingOffsetWs)
    float4 vNormalWs : TEXCOORD1;      // w: the flexes' wrinkle amount (below 0 squish, above 0 stretch)
    float2 vTexCoord : TEXCOORD2;
    float2 vTexCoord2 : TEXCOORD3;     // the mesh's second uv (F_SECONDARY_UV), or the first again
    float4 vTint : TEXCOORD4;          // the object's tint in rgb
    float3 vTangentWs : TEXCOORD6;
    float3 vBitangentWs : TEXCOORD7;
#if S2VFX_DISPLACE
    float4 vExtra : TEXCOORD8;         // a shader's own values from its Displace (vertex.hlsl)
#endif
#if S2VFX_OBJECT_MATRIX
    // The matrix that places the model (vertex.hlsl's m), row by row: a shader that works in the model's own space.
    nointerpolation float4 vObjectRow0 : TEXCOORD9;
    nointerpolation float4 vObjectRow1 : TEXCOORD10;
    nointerpolation float4 vObjectRow2 : TEXCOORD11;
#endif
#if D_MULTIVIEW_INSTANCING
    nointerpolation uint2 nObjectId : TEXCOORD12;
#else
    nointerpolation uint nObjectId : TEXCOORD12;
#endif
    float4 vPositionPs : SV_Position;
#if D_MULTIVIEW_INSTANCING
    float flClip : SV_ClipDistance0;
#endif
};
