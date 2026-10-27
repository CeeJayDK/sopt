// Test effect for test_fx: integer variables read as float are inputs written float(x), loop counters with the
// range of their for header, int sliders on whole numbers, array elements at a loop index (2026-10-26). Lines checked.
uniform int Steps < ui_type = "slider"; ui_min = 1; ui_max = 8; > = 4;
static const float W[4] = { 0.1, 0.2, 0.3, 0.4 };
texture ITex { Width = 64; Height = 64; Format = RGBA8; };
sampler ISamp { Texture = ITex; };
float4 IVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 IPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(ISamp, p.xy).rgb;
	float sum = 0.0;
	for (int i = 0; i < 4; i++)
		sum += c.r * float(i) + c.g * 0.5;
	float s = c.b * Steps + c.r * Steps;
	float w = c.g * float(Steps * 2 + 1) + c.b * float(Steps * 2 + 1);
	float ws = 0.0;
	for (int k = 0; k < 4; k++)
		ws += c.r * W[k] + c.b * W[k];
	float2 q = c.xy * int2(Steps, 3) + c.yx;
	float3 m = mul(float3x3(0.5, 0.25, 0.25, 0.1, 0.8, 0.1, 0.2, 0.2, 0.6), c) * c.r;
	return float4(sum + ws + q.x + m.x, s, w, q.y);
}
technique I { pass { VertexShader = IVS; PixelShader = IPS; } }
