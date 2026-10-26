// Test effect for test_fx: code that only other values of a setting compile (owner, 2026-10-26). Line numbers are
// checked.
#ifndef SETTING_MODE
	#define SETTING_MODE 0
#endif
texture StTex { Width = 64; Height = 64; Format = RGBA8; };
sampler StSamp { Texture = StTex; };
float4 StVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 StPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(StSamp, p.xy).rgb;
#if SETTING_MODE == 2
	float a = c.r * c.g + c.b * c.g;
#elif SETTING_MODE > 3
	float a = c.r * 0.5 + c.r * 0.25;
#else
	float a = c.r * c.r + c.g * c.r;
#endif
	return float4(a, 0.0, 0.0, 1.0);
}
technique St { pass { VertexShader = StVS; PixelShader = StPS; } }
