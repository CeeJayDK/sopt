uniform float Zoom < ui_type = "drag"; ui_min = 1.0; ui_max = 8.0; > = 2.0;
void Tri(in uint id, out float4 pos, out float2 uv)
{
	uv.x = (id == 2) ? 2.0 : 0.0;
	uv.y = (id == 1) ? 2.0 : 0.0;
	pos = float4(uv, 0.0, 1.0);
}
void IdVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0, out float2 bits : TEXCOORD1)
{
	Tri(id, pos, uv);
	bits = float2((id << 1) & 2, id & 2);
}
void ZoomVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0, out float2 scaled : TEXCOORD1)
{
	uv.x = (id == 2) ? 2.0 : 0.0;
	uv.y = (id == 1) ? 2.0 : 0.0;
	scaled = uv * Zoom;
	pos = float4(uv, 0.0, 1.0);
}
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 k : TEXCOORD1; };
V StructVS(in uint id : SV_VertexID)
{
	V o;
	Tri(id, o.pos, o.uv);
	o.k = o.uv * Zoom;
	return o;
}
float4 IdPS(float4 vpos : SV_Position, float2 uv : TEXCOORD0, float2 bits : TEXCOORD1) : SV_Target
{
	float a = bits.x * 0.5 + bits.y * 0.25 + uv.x * 0.125;
	return float4(a, a, a, 1.0);
}
float4 ZoomPS(float4 vpos : SV_Position, float2 uv : TEXCOORD0, float2 scaled : TEXCOORD1) : SV_Target
{
	float b = scaled.x * 0.5 + scaled.y * 0.25 + uv.x * 0.125;
	return float4(b, b, b, 1.0);
}
float4 StructPS(V i) : SV_Target
{
	float s = i.k.x * 0.5 + i.k.y * 0.25 + i.uv.x * 0.125;
	return float4(s, s, s, 1.0);
}
float4 AcosPS(float4 vpos : SV_Position, float2 uv : TEXCOORD0, float2 bits : TEXCOORD1) : SV_Target
{
	float c = acos(uv.x * 0.5) * 0.5 + uv.y;
	float d = (uv.x * 7.0) % 2.0 + uv.y;
	float e = sin(uv.x * 0.5) * 0.5 + uv.y;
	return float4(c, d, e, 1.0);
}

technique TexCoord { pass { VertexShader = IdVS; PixelShader = IdPS; } pass { VertexShader = ZoomVS; PixelShader = ZoomPS; } pass { VertexShader = StructVS; PixelShader = StructPS; } pass { VertexShader = IdVS; PixelShader = AcosPS; } }
