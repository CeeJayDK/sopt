// Test effect for test_fx: precise values are judged against float math only.
uniform float Amount < ui_min = 0.0; ui_max = 1000.0; > = 100.0;

void VS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD)
{
	uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	precise float r = (uv.x * Amount + 12582912.0) - 12582912.0;
	float t = uv.y * Amount + 12582912.0;
	precise float s = t - 12582912.0;
	float q = (uv.x * Amount + 12582912.0) - 12582912.0;
	return float4(r, s, q, 1.0);
}

technique Precise { pass { VertexShader = VS; PixelShader = PS; } }
