// Test effect for test_fx: overloads share ReShade's unique name; a call reads the overload it calls.
texture OTex { Width = 64; Height = 64; Format = RGBA8; };
sampler OSamp { Texture = OTex; };
float  Gamma(float E)  { return E <= 0.04 ? E / 12.92 : E * E; }
float3 Gamma(float3 E) { return E <= 0.04 ? E / 12.92 : E * E; }
float4 OVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 OPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(OSamp, p.xy).rgb;
	float3 g = Gamma(c) * 2.0 + c;
	return float4(g, 1.0);
}
technique O { pass { VertexShader = OVS; PixelShader = OPS; } }
