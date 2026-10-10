// Test effect for test_fx: float %, all / any and asin / acos are regions (written out as fxc writes them).
uniform float Size < ui_type = "slider"; ui_min = 0.1; ui_max = 1.0; > = 0.5;

texture IntrTex { Width = 256; Height = 256; Format = RGBA8; };
sampler IntrSamp { Texture = IntrTex; };

void IntrVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
	uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 IntrPS(float4 vpos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	float3 c = tex2D(IntrSamp, uv).rgb;
	float m = (uv.x * 1000.0 + c.r) % 1.001;
	float2 d = uv - 0.5;
	float box = all(min(Size - abs(d), Size * 0.5));
	float hit = any(abs(d) > Size) ? c.g * 0.5 : c.g * 2.0 + 0.25;
	float a = asin(c.b * 2.0 - 1.0) * 0.6366 + 0.5;
	float b = acos(c.r * 2.0 - 1.0) * 0.3183;
	m %= 0.25 + c.b;
	return float4(m + box, hit, a, b);
}

technique TestIntrinsics { pass { VertexShader = IntrVS; PixelShader = IntrPS; } }
