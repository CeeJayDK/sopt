// Test effect for test_fx: a vertex shader output picked from a constant array by
// SV_VertexID covers every element's range.
void ArrayVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
	static const float2 corners[4] = { float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0), float2(1.0, 1.0) };
	uv = corners[id];
	pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

float4 ArrayPS(float4 vpos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	float edge = (0.5 - abs(uv.x - 0.5)) * 64.0;
	return float4(edge, uv.y, 0.0, 1.0);
}

technique TestArray { pass { VertexShader = ArrayVS; PixelShader = ArrayPS; } }
