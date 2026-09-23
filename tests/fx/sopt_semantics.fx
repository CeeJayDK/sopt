// Test effect for test_fx: ranges from semantics (struct inputs, unknown vertex shaders).
uniform float Scale < ui_min = 0.0; ui_max = 4.0; > = 1.0;
uniform float2 Offset = float2(0.0, 0.0);

struct VSOUT
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	float4 tint : COLOR0;
};

VSOUT StructVS(uint id : SV_VertexID)
{
	VSOUT o;
	o.uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	o.tint = float4(1.0, 1.0, 1.0, 1.0);
	return o;
}

float4 StructPS(VSOUT i) : SV_Target
{
	float2 a = i.uv * Scale + 0.25;
	float3 t = i.tint.rgb * Scale - 1.0;
	return float4(a, t.x, 1.0);
}

void OffsetVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD)
{
	uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0) + Offset;
	pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 OffsetPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float2 b = uv * Scale + 0.5;
	return float4(b, vpos.x * 0.5 + 1.0, 1.0);
}

technique SoptSemantics
{
	pass { VertexShader = StructVS; PixelShader = StructPS; }
	pass { VertexShader = OffsetVS; PixelShader = OffsetPS; }
}
