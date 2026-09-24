// Test effect for test_fx: deprecated fetch syntax becomes the current one in variants.
texture BackBufferTex : COLOR;
sampler BackBuffer { Texture = BackBufferTex; };

void PostProcessVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 FetchPS(float4 vpos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	float a = tex2Doffset(BackBuffer, uv, int2(1, 0)).r * 0.5 + tex2Dlodoffset(BackBuffer, float4(uv, 0, 0), int2(0, 1)).r * 0.5;
	float b = tex2Dgather(BackBuffer, uv, 1).x * 0.25 + tex2Dgather(BackBuffer, uv, 1).y * 0.75;
	return float4(a, b, 0.0, 1.0);
}

technique TestFetch { pass { VertexShader = PostProcessVS; PixelShader = FetchPS; } }
