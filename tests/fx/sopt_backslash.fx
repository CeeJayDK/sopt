// Test effect for test_fx: #include with Windows path separators (as in iMMERSE).
#include ".\sub\sopt_sub.fxh"

void PostProcessVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 BackslashPS(float4 vpos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	return float4(Half(uv.x), uv.y, 0.0, 1.0);
}

technique TestBackslash { pass { VertexShader = PostProcessVS; PixelShader = BackslashPS; } }
