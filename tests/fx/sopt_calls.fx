// Test effect for test_fx: a helper sopt cannot inline (a texture read, a loop) is an input named by its call text
// (2026-10-26). Line numbers are checked.
texture CaTex { Width = 64; Height = 64; Format = RGBA8; };
sampler CaSamp { Texture = CaTex; };
float GetLuma(float2 uv) { return dot(tex2D(CaSamp, uv).rgb, float3(0.25, 0.5, 0.25)); }
float Sum(float x) { float s = 0.0; for (int i = 0; i < 4; i++) s += x * 0.25; return s; }
float4 CaVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 CaPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(CaSamp, p.xy).rgb;
	float a = GetLuma(p.xy) * 2.0 + GetLuma(p.xy) * 0.5;
	float b = Sum(c.r) * c.g + Sum(c.r) * c.b;
	return float4(a, b, 0.0, 1.0);
}
technique Ca { pass { VertexShader = CaVS; PixelShader = CaPS; } }
