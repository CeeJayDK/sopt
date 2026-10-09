// sopt_MipTestInt.fx: what ReShade does with the mip levels of integer textures (R32U, RGBA32I).
//
// Pass 1 fills both with values whose 2x2 sums are multiples of 4 (texel = 4 * k), so the exact
// average of every block is an integer; ReShade then generates their mip levels. Pass 2 shows one
// level of one of them:
//   Mode 0: the mip level as ReShade generated it (value / 16384 as gray; RGBA32I: R, G, B + 0.5)
//   Mode 1: the reference: the exact integer average of the 2x2 texels (2t, 2t + 1) of level L - 1
//   Mode 2: classification of every texel:
//           green  = the exact average (what the float formats get)
//           blue   = equal to one of the four texels of the block (nearest / point filtering)
//           gray   = 0 (nothing written, or an unfilterable read returned 0)
//           red    = anything else (e.g. the integer bits averaged as floats)
// Levels 2+ build on the level above as generated, so judge level 1 first.
// OpenGL sends these formats to ReShade's own compute shader, which declares float sampler2D /
// image2D bindings: undefined in GL for integer textures. D3D11's GenerateMips and Vulkan's
// linear blit do not support integer formats; D3D12's compute shader writes them through
// RWTexture2DArray<float4>. If the effect fails to load somewhere, the log says why.

#include "ReShade.fxh"

uniform int Format < ui_type = "combo"; ui_items = "R32U\0RGBA32I\0"; > = 0;
uniform int Level < ui_type = "slider"; ui_min = 1; ui_max = 5; > = 1;
uniform int Mode < ui_type = "combo"; ui_items = "Mip level\0Reference\0Classification\0"; > = 2;

texture texMipU { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; MipLevels = 6; Format = R32U; };
texture texMipI { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; MipLevels = 6; Format = RGBA32I; };
sampler<uint> sMipU { Texture = texMipU; };
sampler<int4> sMipI { Texture = texMipI; };

uint hash(uint x)
{
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

void PS_Fill(float4 pos : SV_Position, float2 uv : TEXCOORD, out uint u : SV_Target0, out int4 i : SV_Target1)
{
	const uint h = hash(uint(pos.x) + hash(uint(pos.y)));
	u = (h & 0xFFFu) * 4u;                                  // 0 .. 16380
	i = int4(int((h >> 12) & 0xFFFu) * 4 - 8192,            // -8192 .. 8188
	         int((h >> 4) & 0xFFFu) * 4 - 8192,
	         int((h >> 20) & 0xFFFu) * 4 - 8192, 1);
}

int4 fetch(int2 t, int lod)
{
	if (Format == 0) return int4(int(tex2Dfetch(sMipU, t, lod)), 0, 0, 0);
	return tex2Dfetch(sMipI, t, lod);
}

float3 PS_Show(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const int2 size = max(int2(BUFFER_WIDTH, BUFFER_HEIGHT) >> Level, 1);
	const int2 t = min(int2(uv * float2(size)), size - 1);

	const int4 mip = fetch(t, Level);
	const int4 v0 = fetch(t * 2, Level - 1), v1 = fetch(t * 2 + int2(1, 0), Level - 1);
	const int4 v2 = fetch(t * 2 + int2(0, 1), Level - 1), v3 = fetch(t * 2 + int2(1, 1), Level - 1);
	const int4 sum = v0 + v1 + v2 + v3;
	const int4 ref = sum / 4; // exact on level 1 (sums are multiples of 4)

	const int n = Format == 0 ? 1 : 3;
	bool same = true, zero = true, texel = true;
	for (int c = 0; c < n; ++c)
	{
		same = same && mip[c] == ref[c];
		zero = zero && mip[c] == 0;
		texel = texel && (mip[c] == v0[c] || mip[c] == v1[c] || mip[c] == v2[c] || mip[c] == v3[c]);
	}

	if (Mode == 0) return Format == 0 ? float(mip.x).xxx / 16384.0 : float3(mip.xyz) / 16384.0 + 0.5;
	if (Mode == 1) return Format == 0 ? float(ref.x).xxx / 16384.0 : float3(ref.xyz) / 16384.0 + 0.5;
	if (same) return float3(0.0, 0.8, 0.0);
	if (zero) return float3(0.3, 0.3, 0.3);
	if (texel) return float3(0.0, 0.3, 1.0);
	return float3(1.0, 0.0, 0.0);
}

technique sopt_MipTestInt
{
	pass Fill
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Fill;
		RenderTarget0 = texMipU;
		RenderTarget1 = texMipI;
	}
	pass Show
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Show;
	}
}
