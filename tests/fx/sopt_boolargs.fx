// Test effect for test_fx: bools in float math (lerp by a comparison) and a bool local as a condition.
texture BTex { Width = 64; Height = 64; Format = RGBA8; };
sampler BSamp { Texture = BTex; };
float4 BVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 BPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(BSamp, p.xy).rgb;
	float3 m = lerp(c, float3(1.0, 0.0, 0.0), c.r * c.g > 0.5) * 2.0;
	bool k = c.b > 0.5;
	float o = k ? c.r * c.g + c.b : c.b * c.g;
	return float4(m, o);
}
technique B { pass { VertexShader = BVS; PixelShader = BPS; } }
