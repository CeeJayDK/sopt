// Test effect for test_fx (front end, regions, facts, budgets, variants).
#include "sopt_test.fxh"

#define SCALE 2.0

uniform float Strength < ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; > = 1.0;
uniform float Plain = 0.5;
static const float PX = 1.0 / BUFFER_WIDTH;

texture BackBufferTex : COLOR;
sampler BackBuffer { Texture = BackBufferTex; };

void PostProcessVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 SoptPS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float3 color = tex2D(BackBuffer, texcoord).rgb;
	float luma = color.r * 0.25 + color.g * 0.5 + color.b * 0.25;
	float2 uv = texcoord * 0.5 + 0.25;
	float3 other = tex2D(BackBuffer, uv).rgb;
	float gate = luma * Strength + Plain;
	if (gate > 0.5)
		color = SoptHelper(color);
	float scaled = luma * SCALE + 1.0;
	float edge = texcoord.x * PX + luma;
	float3 mixed = lerp(color, other, Strength * 0.5);
	mixed = mixed * 0.5 + tex2D(BackBuffer, texcoord + BUFFER_RCP_WIDTH).rgb * 0.5;
	return float4(mixed * scaled, 1.0);
}

technique SoptTest
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = SoptPS;
	}
}
