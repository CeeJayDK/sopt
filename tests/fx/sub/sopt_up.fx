// Test effect for test_fx: an include from a folder above, found without an include path (as ReShade.fxh in
// reshade-shaders\Shaders is found from Shaders\SweetFX).
#include "sopt_test.fxh"

void PostProcessVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 UpPS(float4 vpos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	return float4(SoptHelper(float3(uv, 0.5)), 1.0);
}

technique TestUp { pass { VertexShader = PostProcessVS; PixelShader = UpPS; } }
