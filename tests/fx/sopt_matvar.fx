// Test effect for test_fx: mul() of a matrix variable (a static const, a local from a slider, a helper's matrix,
// a uniform).
uniform float Angle < ui_type = "slider"; ui_min = 0.0; ui_max = 6.0; > = 1.0;
uniform float3x3 Mix < ui_min = 0.0; ui_max = 1.0; > = float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
texture MTex { Width = 64; Height = 64; Format = RGBA8; };
sampler MSamp { Texture = MTex; };
static const float3x3 ToLms = float3x3(0.5, 0.25, 0.25, 0.1, 0.8, 0.1, 0.2, 0.2, 0.6);
float2x2 Rot(float a) { return float2x2(cos(a), -sin(a), sin(a), cos(a)); }
float4 MVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 MPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(MSamp, p.xy).rgb;
	float3 l = mul(ToLms, c) * c.r;
	const float2x2 r = float2x2(cos(Angle), sin(Angle), -sin(Angle), cos(Angle));
	float2 q = mul(r, c.xy) * 2.0 + c.yz;
	float2 h = mul(Rot(Angle), c.xy) * 2.0 + c.zx;
	float3 u = mul(Mix, c) * c.g;
	float3 w = mul(c, Mix) + c.b;
	return float4(l + float3(q, h.x) + u * w, h.y);
}
technique M { pass { VertexShader = MVS; PixelShader = MPS; } }
