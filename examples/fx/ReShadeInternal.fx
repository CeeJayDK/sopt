// ReShade's own shaders (6.8.0 res/shaders), ported to ReShade FX so sopt-fx can search them
// (docs/reshade-internal-shaders.md). The API-specific originals: copy_ps.hlsl (back buffer
// copy), mipmap_cs_5_0.hlsl / mipmap_cs_430.glsl (the 2x2 reduce of mipmap generation). The
// vertex shader fullscreen_vs.hlsl is integer code on SV_VertexID (3 vertices per pass).
#include "ReShade.fxh"

texture SrcTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler SrcPoint { Texture = SrcTex; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// copy_ps.hlsl: col = t0.Sample(s0, uv); col.a = 1.0;
float4 CopyPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float4 col = tex2D(ReShade::BackBuffer, uv);
	col.a = 1.0;
	return col;
}

// mipmap_cs: reduce(v0, v1, v2, v3) = (v0 + v1 + v2 + v3) * 0.25 over a 2x2 block of the larger
// level (load_and_reduce reads (0,0) (1,0) (0,1) (1,1); the GL version the same set).
float4 ReducePS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	int2 p = int2(vpos.xy) * 2;
	float4 v0 = tex2Dfetch(SrcPoint, p + int2(0, 0));
	float4 v1 = tex2Dfetch(SrcPoint, p + int2(1, 0));
	float4 v2 = tex2Dfetch(SrcPoint, p + int2(0, 1));
	float4 v3 = tex2Dfetch(SrcPoint, p + int2(1, 1));
	return (v0 + v1 + v2 + v3) * 0.25;
}

technique ReShadeInternalCopy { pass { VertexShader = PostProcessVS; PixelShader = CopyPS; } }
technique ReShadeInternalReduce { pass { VertexShader = PostProcessVS; PixelShader = ReducePS; RenderTarget = SrcTex; } }
