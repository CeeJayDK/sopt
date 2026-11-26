void StepVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) { uv.x = (id == 2) ? 2.0 : 0.0; uv.y = (id == 1) ? 2.0 : 0.0; pos = float4(uv, 0.0, 1.0); }
float4 StepPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float s = step(0.5, uv.y);
	float m = smoothstep(0.0, 1.0, uv.y);
	float r = uv.x * (1.0 - s) + s * uv.y;
	float q = uv.x * (1.0 - m) + m * uv.y;
	return r + q;
}

technique StepGrid { pass { VertexShader = StepVS; PixelShader = StepPS; } }
