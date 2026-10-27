#ifndef SOPT_FIXEDMACRO_FX
#define SOPT_FIXEDMACRO_FX
// Test effect for test_fx: macros are expanded (one defined inside #if under its condition, not the include guard's),
// function-like ones too; one that is a texture fetch is an input. Line numbers are checked.
#define LIT 0.75
#ifndef SETTING
	#define SETTING 1
#endif
#if SETTING
	#define COND 0.5
#else
	#define COND 0.25
#endif
texture FmTex { Width = 64; Height = 64; Format = RGBA8; };
sampler FmSamp { Texture = FmTex; };
float4 FmVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 FmPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(FmSamp, p.xy).rgb;
	float a = c.r * LIT + c.g;
	float b = c.r * COND + c.g;
	return float4(a, b, 0.0, 1.0);
}
#define maxOf3(a) max(max(a.x, a.y), a.z)
#define getColor(c) tex2Dlod(FmSamp, float4((c).xy, 0, 0))
#if SETTING
	#define BLEND(x, y) ((x) * 0.5 + (y))
#else
	#define BLEND(x, y) ((x) * 0.25 + (y))
#endif
float4 FmPS2(float4 p : SV_Position) : SV_Target
{
	float3 c = getColor(p.xy).rgb;
	float m = maxOf3(c) * 2.0 + c.r;
	float b = BLEND(c.g, c.b) * c.r;
	float d = getColor(p.yx).g * 3.0 + c.b;
	return float4(m, b, d, 1.0);
}
technique Fm { pass { VertexShader = FmVS; PixelShader = FmPS; } pass { VertexShader = FmVS; PixelShader = FmPS2; } }
#endif
