uniform bool Flag < ui_type = "radio"; > = false;
uniform int Mode < ui_type = "slider"; ui_min = 0; ui_max = 2; > = 0;
texture VTex { Width = 64; Height = 64; Format = RGBA8; };
sampler VSamp { Texture = VTex; };
float4 VVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 VPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(VSamp, p.xy).rgb;
	float2 a = (c.r < 0.5) ? c.xy * 2.0 + 1.0 : c.yz * 0.5;
	float2 b = Flag ? c.xy * 2.0 + 1.0 : c.yz * 0.5;
	float2 d = c.b ? c.xy * 2.0 + 1.0 : c.yz * 0.5;
	float2 e = Mode ? c.xy * 2.0 + 1.0 : c.yz * 0.5;
	float f = (c.r < 0.5) ? c.x * 2.0 + 1.0 : c.y * 0.5;
	float g = Flag ? c.x * 2.0 + 1.0 : c.y * 0.5;
	float h = c.b ? c.x * 2.0 + 1.0 : c.y * 0.5;
	return float4(a + b + d + e, f + g, h);
}
technique V { pass { VertexShader = VVS; PixelShader = VPS; } }
