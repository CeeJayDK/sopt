// Test effect for test_fx: integer comparisons compared as integers (uint inputs, unsigned compares).
uniform int Mode < ui_type = "combo"; ui_items = "a\0b\0c\0"; > = 0;
texture CTex { Width = 64; Height = 64; Format = RGBA8; };
sampler CSamp { Texture = CTex; };
float4 CVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 CPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(CSamp, p.xy).rgb;
	float g = Mode < 1 ? c.r * c.g + c.b : c.b * c.g;
	float h = Mode == 2 ? c.r * 2.0 + c.g : c.g * c.r;
	return float4(c, g + h);
}
technique C { pass { VertexShader = CVS; PixelShader = CPS; } }
