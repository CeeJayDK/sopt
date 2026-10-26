uniform bool Flag < ui_type = "radio"; > = false;
texture OTex { Width = 64; Height = 64; Format = RGBA8; };
sampler OSamp { Texture = OTex; };
float4 OVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 OPS(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(OSamp, p.xy).rgb;
	float2 pos = c.xy;
	if (c.z > 0.5) pos.x += c.y * 0.25 + c.x * 0.25;
	else pos.y = c.x * 2.0 + c.x * 0.5;
	if (Flag) c.r = c.r * c.g + c.b * c.g;
	return float4(pos, c.r, 1.0);
}
technique O { pass { VertexShader = OVS; PixelShader = OPS; } }
