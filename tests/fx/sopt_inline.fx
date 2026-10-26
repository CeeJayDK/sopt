#define MyFloat float
uniform float Strength < ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; > = 1.0;
texture InTex { Width = 64; Height = 64; Format = RGBA8; };
sampler InSamp { Texture = InTex; };
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float Sq(float x) { const float y = x * x; return y; } float Twice(float x) { x = x * 2.0; float y = x + 1.0; y *= x; return y; }
float Branchy(float x) { if (x > 0.5) return 1.0; return x; } float SideB(float x, float y) { float r = x; if (y > 0.5) r = y * 2.0; else r = x * 3.0; return r; } float3 Tint(float3 v) { v.rg *= 0.5; return v; }
float4 InVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 InPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(InSamp, p.xy).rgb;
	float a = Luma(c) * Strength + Luma(c.bgr) * c.g;
	float b = Sq(Luma(c)) * c.r + Twice(c.b);
	float d = Branchy(c.r) * c.g + SideB(c.b, c.g) + Tint(c).g;
	MyFloat e = smoothstep(0.2, 0.8, c.g);
	float f = c.r * c.g;
	return float4(a + f, b + f, d, e);
}
technique In { pass { VertexShader = InVS; PixelShader = InPS; } }
