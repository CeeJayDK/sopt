#ifndef SOPT_FIXEDMACRO_FX
#define SOPT_FIXEDMACRO_FX
// Test effect for test_fx: a macro with one value for every user is expanded, one defined inside #if is not (its
// value depends on a setting). The include guard around the file does not count. Line numbers are checked.
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
technique Fm { pass { VertexShader = FmVS; PixelShader = FmPS; } }
#endif
