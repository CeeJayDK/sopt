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
float3 OMul(float3 a, float3 b) { return a * b + a * 0.5; }
float4 OPS2(float4 p : SV_Position) : SV_Target
{
	float3 c = tex2D(OSamp, p.xy).rgb;
	float a = c.x * c.y + c.z; float b = c.y * 3.0 + c.y * 0.5;
	float s = c.x * 2.0 + c.y, t = c.z * c.z + c.y * c.z;
	return float4(OMul(c, c.zyx).x, a, b, s + t);
}
technique O { pass { VertexShader = OVS; PixelShader = OPS; } pass { VertexShader = OVS; PixelShader = OPS2; } }
