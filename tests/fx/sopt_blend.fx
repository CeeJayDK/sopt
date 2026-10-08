// Test effect for test_fx: final blends with the back buffer moved to the blend stage
// (fx/blend.hpp). Line numbers are checked in test_fx.
uniform float Strength < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; > = 0.5;
uniform float3 Tint < ui_type = "color"; > = float3(1.0, 0.8, 0.6);

texture BackBufferTex : COLOR;
sampler BackBuffer { Texture = BackBufferTex; };
texture OverlayTex { Width = 256; Height = 256; Format = RGBA8; };
sampler Overlay { Texture = OverlayTex; };

void PostProcessVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 texcoord : TEXCOORD)
{
	texcoord = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	pos = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 LerpPS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float3 color = tex2D(BackBuffer, texcoord).rgb;
	float3 layer = tex2D(Overlay, texcoord).rgb;
	return lerp(color, layer, Strength);
}

float4 VignettePS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float4 color = tex2D(BackBuffer, texcoord);
	float2 d = texcoord - 0.5;
	float v = saturate(1.0 - dot(d, d) * Strength * 2.0);
	return float4(color.rgb * (v * Tint), color.a);
}

float3 DarkenPS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float3 color = tex2D(BackBuffer, texcoord).rgb;
	return min(color, tex2D(Overlay, texcoord).rgb);
}

float3 SharpenPS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float3 color = tex2D(BackBuffer, texcoord).rgb;
	float3 blur = tex2D(Overlay, texcoord).rgb;
	return color + (color - blur) * Strength; // B = 1 + Strength > 1: not a blend
}

float4 ScreenPS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float4 color = tex2D(BackBuffer, texcoord);
	float3 layer = tex2D(Overlay, texcoord).rgb * Strength;
	color.rgb = color.rgb + layer - color.rgb * layer;
	return color;
}

void AddPS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD, out float4 result : SV_Target)
{
	float3 color = tex2D(BackBuffer, texcoord).rgb;
	result = float4(color + tex2D(Overlay, texcoord).rgb * Strength, 1.0);
	result.rgb = color + tex2D(Overlay, texcoord).rgb * Strength;
}

technique TestBlend
{
	pass { VertexShader = PostProcessVS; PixelShader = LerpPS; }
	pass { VertexShader = PostProcessVS; PixelShader = VignettePS; }
	pass { VertexShader = PostProcessVS; PixelShader = DarkenPS; }
	pass { VertexShader = PostProcessVS; PixelShader = SharpenPS; }
	pass { VertexShader = PostProcessVS; PixelShader = ScreenPS; }
	pass { VertexShader = PostProcessVS; PixelShader = AddPS; }
}
