// Test effect for test_fx: a float read as int is a threshold (exact budget), and int values
// truncate (their range is widened to whole numbers).
uniform float Scale < ui_type = "slider"; ui_min = 1.5; ui_max = 2.5; > = 2.0;

texture IntTex { Width = 256; Height = 256; Format = RGBA8; };
sampler IntSamp { Texture = IntTex; };

void IntVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
	uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 IntPS(float4 vpos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	float2 p = uv * 255.0 + 0.5;
	float n = int(Scale);
	float s = n * 0.5 + 0.25;
	return tex2Dfetch(IntSamp, int2(p)) * s;
}

technique TestInt { pass { VertexShader = IntVS; PixelShader = IntPS; } }
