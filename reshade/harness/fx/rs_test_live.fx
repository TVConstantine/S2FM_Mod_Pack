// Live test effects, read back from the shown 8-bit picture.
// RsTestDepthView: the view distance as grey, z / 10000 units (ReShade::GetLinearizedDepth * N * F, with
// N = SOURCE2_NEAR_PLANE and F = RESHADE_DEPTH_LINEARIZATION_FAR_PLANE).
// RsTestClockView: r = frac(timer / 997 ms), the playhead as ReShade's timer uniform sees it.
#include "ReShade.fxh"

#ifndef SOURCE2_NEAR_PLANE
    #define SOURCE2_NEAR_PLANE 12.0
#endif

uniform float RsTimer < source = "timer"; >;

float4 PS_DepthView(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float z = ReShade::GetLinearizedDepth(uv) * SOURCE2_NEAR_PLANE * RESHADE_DEPTH_LINEARIZATION_FAR_PLANE;
    return float4(saturate(z / 10000.0).xxx, 1.0);
}
float4 PS_ClockView(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return float4(frac(RsTimer / 997.0), 0.0, 0.0, 1.0);
}

technique RsTestDepthView { pass { VertexShader = PostProcessVS; PixelShader = PS_DepthView; } }
technique RsTestClockView { pass { VertexShader = PostProcessVS; PixelShader = PS_ClockView; } }
