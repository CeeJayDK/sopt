uniform float Strength < ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; > = 1.0;
uniform int Pattern < ui_type = "slider"; ui_min = 0; ui_max = 2; > = 0;
texture PDTex { Width = 64; Height = 64; Format = RGBA8; };
sampler PDSamp { Texture = PDTex; };
float4 PDVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 PDPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(PDSamp, p.xy).rgb;
	float3 w = float3(0.2126, 0.7152, 0.0722) * Strength;
	if (Pattern == 1)
		w *= 0.666;
	float k = Strength * 2.0;
	if (c.r > 0.5)
		k = 1.0;
	float a = dot(c, w) * c.g + c.b;
	float b = k * c.g * c.b + c.r;
	return float4(a, b, 0, 1);
}
technique PD { pass { VertexShader = PDVS; PixelShader = PDPS; } }
