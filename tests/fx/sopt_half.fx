// Test effect for test_fx: min16float math is float math verified at half precision too (Program::half).
texture HTex { Width = 64; Height = 64; Format = RGBA8; };
sampler HSamp { Texture = HTex; };
float4 HVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 HPS(float4 p : SV_Position) : SV_Target
{
	min16float3 c = (min16float3)tex2D(HSamp, p.xy).rgb;
	min16float a = c.r * c.g + c.b * 0.5 - c.r;
	float3 d = tex2D(HSamp, p.yx).rgb;
	float b = d.r * d.g + d.b * 0.5 - d.r;
	return float4(a, b, 0.0, 1.0);
}
technique H { pass { VertexShader = HVS; PixelShader = HPS; } }
