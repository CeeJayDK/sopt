uniform int FC < source = "framecount"; >; void SplitVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) { uv.x = (id == 2) ? 2.0 : 0.0; uv.y = (id == 1) ? 2.0 : 0.0; pos = float4(uv, 0.0, 1.0); }
float4 SplitPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float x = saturate(uv.x);
	float y = saturate(uv.y);
	float q = x * (1.0 - x);
	float l = lerp(x, y, y);
	float2 c = saturate(uv);
	float e = c.x - c.y;
	float v = x;
	float w = v;
	v = y;
	float d = w - v;
	float r1 = q * (1.0 - l) + l * uv.y;
	float r2 = d * (1.0 - e) + e * uv.y;
	float f = float(FC) - 7.0 * floor(float(FC) / 7.0);
	float r3 = f * (1.0 - l) + l * uv.y;
	return r1 + r2 + r3;
}

technique RangeSplit { pass { VertexShader = SplitVS; PixelShader = SplitPS; } }
