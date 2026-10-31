// Test effect for test_fx: a slider whose code handles values beyond its ui_max gets no range from it (2026-10-31).
uniform float Amount < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; > = 0.5;
uniform float2 tuv;
uniform float Plain < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; > = 0.5;
texture STex { Width = 64; Height = 64; Format = RGBA8; };
sampler SSamp { Texture = STex; };
float4 SVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 SPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(SSamp, p.xy).rgb;
	float a = lerp(c.r, c.r * c.r, saturate(Amount - 1.0));
	float b = c.g * saturate(Plain) + c.b;
	float d = tuv.x * 2.0 + c.r;
	float e = saturate(tuv.y - tuv.y * tuv.y) + c.g;
	return float4(a, b, d, e);
}
technique S { pass { VertexShader = SVS; PixelShader = SPS; } }
