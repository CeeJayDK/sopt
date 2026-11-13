void UiVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) { uv.x = (id == 2) ? 2.0 : 0.0; uv.y = (id == 1) ? 2.0 : 0.0; pos = float4(uv, 0.0, 1.0); }
uniform float OffA < ui_type = "drag"; ui_min = -1; ui_max = 1; > = 0;
uniform float OffB < ui_type = "drag"; ui_min = -2; ui_max = 3; > = 0;
float4 UiPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float y = uv.y * OffA + OffB * 0.25;
	return float4(y, y, y, 1.0);
}

technique UiRange { pass { VertexShader = UiVS; PixelShader = UiPS; } }
