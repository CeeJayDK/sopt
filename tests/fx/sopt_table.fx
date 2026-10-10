// Test effect for test_fx: local arrays of constants indexed at run time become static const
// tables (fx/classic.hpp); Weights is written after its initializer and stays, Gains / Tints have
// an entry that is not stable at the use (a changed local, a texture read) and stay.
uniform int Preset < ui_type = "combo"; ui_items = "Custom\0A\0B\0C\0"; > = 1;
uniform float3 Custom < ui_type = "color"; > = float3(0.2, 0.7, 0.1);

texture TableTex { Width = 256; Height = 256; Format = RGBA8; };
sampler TableSamp { Texture = TableTex; };

void TableVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
	uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 TablePS(float4 vpos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	float3 c = tex2D(TableSamp, uv).rgb;
	float3 Coefficients[4] =
	{
		Custom, // custom
		float3(0.21, 0.72, 0.07), // A
		float3(0.33, 0.34, 0.33), // B
		float3(0.18, 0.41, 0.41)  // C
	};
	float Weights[3] = { 0.25, 0.5, 0.25 };
	Weights[1] = c.r;
	float k = c.g;
	float Gains[3] = { 0.5, 1.0, k };  // k changes before the use: stays
	k = c.b;
	float Tints[3] = { 0.5, 1.0, tex2D(TableSamp, uv).a };  // a texture read: stays
	float g = dot(Coefficients[Preset], c);
	return float4(g * Weights[Preset % 3], g * Gains[Preset % 3] + k, g * Tints[Preset % 3], 1.0);
}

technique TestTable { pass { VertexShader = TableVS; PixelShader = TablePS; } }
