// Test effects for rs_harness (reshade/harness). Invert: 1 - colour. Tint: colour times (RsTestGain, 1, 1).
// TimeOut: the timer, framecount and random uniforms as the colour (r = frac(timer ms / 997), g = frac(framecount
// / 251), b = (random % 256) / 255), read back from an 8-bit back buffer.
#include "ReShade.fxh"

uniform float RsTestGain < ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_label = "Gain"; > = 0.5;
uniform float Timer < source = "timer"; >;
uniform int FrameCount < source = "framecount"; >;
uniform int Random < source = "random"; min = 0; max = 1000000; >;

float4 PS_Invert(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(1.0 - tex2D(ReShade::BackBuffer, uv).rgb, 1.0); }
float4 PS_Tint(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(tex2D(ReShade::BackBuffer, uv).rgb * float3(RsTestGain, 1.0, 1.0), 1.0); }
float4 PS_TimeOut(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(frac(Timer / 997.0), frac(FrameCount / 251.0), (Random % 256) / 255.0, 1.0); }

technique RsTestInvert { pass { VertexShader = PostProcessVS; PixelShader = PS_Invert; } }
technique RsTestTint { pass { VertexShader = PostProcessVS; PixelShader = PS_Tint; } }
technique RsTestTimeOut { pass { VertexShader = PostProcessVS; PixelShader = PS_TimeOut; } }
