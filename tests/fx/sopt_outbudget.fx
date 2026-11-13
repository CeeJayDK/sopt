void OutVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) { uv.x = (id == 2) ? 2.0 : 0.0; uv.y = (id == 1) ? 2.0 : 0.0; pos = float4(uv, 0.0, 1.0); }
float4 PlainPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float v = uv.x * 0.7 + uv.y * 0.2 + 0.05;
	float w = v * 0.5 + uv.x * 0.25;
	return float4(w, w, w, 1.0);
}
float4 WavePS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float v = uv.x * 0.7 + uv.y * 0.2 + 0.05;
	float w = abs(frac(v * 64.0) - 0.5) * 2.0;
	return float4(w, w, w, 1.0);
}
float4 DiscardPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float v = uv.x * 0.7 + uv.y * 0.2 + 0.05;
	float m = v * 2.0 - 1.0;
	if (m < 0.0) discard;
	return float4(v * 0.001, uv.x, uv.y, 1.0);
}
float lum(float3 c)
{
	float l = c.x * 0.3 + c.y * 0.59 + c.z * 0.11;
	return l;
}
float4 EdgePS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float a = lum(float3(uv, 0.3));
	float b = lum(float3(uv.yx, 0.7));
	float d = (a - b) * 4.0 + 0.5;
	return float4(d, d, d, 1.0);
}

technique OutBudget
{
	pass { VertexShader = OutVS; PixelShader = PlainPS; }
	pass { VertexShader = OutVS; PixelShader = WavePS; }
	pass { VertexShader = OutVS; PixelShader = DiscardPS; }
	pass { VertexShader = OutVS; PixelShader = EdgePS; }
}
