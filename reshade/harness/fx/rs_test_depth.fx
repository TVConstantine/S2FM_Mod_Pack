// What ReShade.fxh and MartysMods' depth header make of the depth the core hands over (rs_harness "depth"):
// colour r = ReShade::GetLinearizedDepth, g = MartysMods' Depth::get_linear_depth; and the same in full float in
// RsDepthOut, which the harness reads back.
#include "ReShade.fxh"

texture DepthInputTex : DEPTH;
sampler DepthInput { Texture = DepthInputTex; };
#include "iMMERSE/MartysMods/mmx_depth.fxh"

texture RsDepthOut { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA32F; };

float4 PS_Depth(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return float4(ReShade::GetLinearizedDepth(uv), Depth::get_linear_depth(uv), 0.0, 1.0);
}

technique RsTestDepth
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_Depth; }
    pass { VertexShader = PostProcessVS; PixelShader = PS_Depth; RenderTarget = RsDepthOut; }
}
