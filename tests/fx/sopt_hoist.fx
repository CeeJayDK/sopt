// Test effect for test_fx: pixel shader math moved to the vertex shader (fx/hoist.hpp). The
// rotation of the texture coordinate is affine in uv (interpolated), its sin / cos of a uniform are
// constant per draw (flat).
// Line numbers are checked in test_fx.
uniform float Angle < ui_type = "slider"; ui_min = 0.0; ui_max = 6.28; > = 0.5;
uniform float Zoom < ui_type = "slider"; ui_min = 0.5; ui_max = 2.0; > = 1.0;
uniform float Strength < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; > = 0.5;

texture HoistTex { Width = 256; Height = 256; Format = RGBA8; };
sampler HoistSamp { Texture = HoistTex; };

void HoistVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
	uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 HoistPS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD0) : SV_Target
{
	float2 c = texcoord - 0.5;
	float2 r = float2(c.x * cos(Angle) - c.y * sin(Angle), c.x * sin(Angle) + c.y * cos(Angle)) / Zoom + 0.5;
	float3 color = tex2D(HoistSamp, r).rgb;
	float2 o = texcoord + Strength * 0.01; // one add per component: cheaper than an interpolation
	color += tex2D(HoistSamp, o).rgb;
	float k = Strength * exp2(Strength * 3.0) * sin(Angle * 2.0);
	return float4(color * k, 1.0);
}

technique TestHoist { pass { VertexShader = HoistVS; PixelShader = HoistPS; } }
