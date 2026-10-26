// Test effect for test_fx: a temporary read once with the default settings and once more with another (ArcaneBloom.fx,
// 2026-10-26): no window may remove its declaration. Line numbers are checked.
#ifndef TEMP_FIX
	#define TEMP_FIX 0
#endif
uniform float Exposure < ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; > = 1.0;
texture TtTex { Width = 64; Height = 64; Format = RGBA8; };
sampler TtSamp { Texture = TtTex; };
float4 TtVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 TtPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(TtSamp, p.xy).rgb;
	float e = c.r * Exposure + c.g;
	float a = e * e + c.b;
#if TEMP_FIX
	a += e * 0.5;
#endif
	return float4(a, 0.0, 0.0, 1.0);
}
technique Tt { pass { VertexShader = TtVS; PixelShader = TtPS; } }
