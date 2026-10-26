// Test effect for test_fx: integer variables read as float are inputs written float(x), loop counters with the
// range of their for header, int sliders on whole numbers (2026-10-26). Line numbers are checked.
uniform int Steps < ui_type = "slider"; ui_min = 1; ui_max = 8; > = 4;
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
	return float4(sum, s, 0.0, 1.0);
}
technique I { pass { VertexShader = IVS; PixelShader = IPS; } }
