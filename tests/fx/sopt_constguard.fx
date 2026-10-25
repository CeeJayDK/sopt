// Test effect for test_fx: a constant that is a static const in one build and a slider in another (SuperDepth3D's
// Luma_Dark). Line numbers are checked in test_fx.
#ifndef SLIDERS
	#define SLIDERS 0
#endif
#if SLIDERS
	uniform float Dark < ui_type = "slider"; ui_min = 0.0; ui_max = 0.5; > = 0.15;
#else
	static const float Dark = 0.15, Bright = 0.7;
#endif
#define LONG_MACRO(x) \
	((x) * 2.0)
uniform float Amount < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; > = 0.5;
texture GuardTex { Width = 64; Height = 64; Format = RGBA8; };
sampler GuardSamp { Texture = GuardTex; };
float4 GuardVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 GuardPS(float4 p : SV_Position) : SV_Target
{
	float l = tex2D(GuardSamp, p.xy).r;
	float t = smoothstep(Dark, max(Bright, Dark + 0.001), l);
	float u = l * Amount + Amount * 0.5;
	return float4(t, u, LONG_MACRO(l), 1.0);
}
technique TestGuard { pass { VertexShader = GuardVS; PixelShader = GuardPS; } }
