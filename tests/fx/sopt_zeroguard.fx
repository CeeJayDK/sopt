// Test effect for test_fx: the uniform zero guard (fx/guard.hpp). Line numbers are checked.
uniform float Strength < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; > = 0.5;
uniform float Gain < ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; > = 1.0;
texture ZTex { Width = 64; Height = 64; Format = RGBA8; };
sampler ZSamp { Texture = ZTex; };
float4 ZVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 ZPS(float4 p : SV_Position) : SV_Target
{
	float4 c = tex2D(ZSamp, p.xy);
	float3 x = c.rgb * c.rgb * 3.0 + c.gbr;
	x = x * x + c.brg * 0.5;
	return lerp(c, float4(x, 1.0), Strength);
}
float4 ZPS2(float4 p : SV_Position) : SV_Target
{
	float4 c = tex2D(ZSamp, p.xy);
	float3 x = c.rgb * c.rgb * 3.0 + c.gbr;
	return float4(x * Gain, c.a);
}
technique Z { pass { VertexShader = ZVS; PixelShader = ZPS; } pass { VertexShader = ZVS; PixelShader = ZPS2; } }
