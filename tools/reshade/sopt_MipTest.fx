// sopt_MipTest.fx: checks the mipmaps ReShade generates (GenerateMipMaps) against a reference.
//
// Pass 1 fills four render targets (RGBA8, RGBA16F, R32F, RGB10A2) with per-pixel noise; ReShade
// then generates their mip levels. Pass 2 shows one level of one of them:
//   Mode 0: the mip level as ReShade generated it
//   Mode 1: the reference: the average of the 2x2 texels (2t, 2t + 1) of level L - 1, which is what
//           ReShade's own mipmap shaders compute (OpenGL, D3D12)
//   Mode 2: |mip - reference| amplified (black = equal; RGBA8 / RGB10A2 show their rounding as grey
//           noise here, which is expected)
//   Mode 3: red where |mip - reference| exceeds Tolerance (in 8-bit steps), else dark gray
// Only RGB is compared (RGB10A2's alpha has two bits). The level is rounded to the format once, so
// half a step is the most it can differ: Tolerance 1 step.
// Only meaningful where ReShade generates the mips itself (OpenGL, D3D12). D3D11 (GenerateMips) and
// Vulkan (vkCmdBlitImage) use the driver's filter: the same for even sizes, but once a level above
// has an odd size (1080 lines: 1080, 540, 270, 135 -> from level 4) it filters differently.

#include "ReShade.fxh"

uniform int Format < ui_type = "combo"; ui_items = "RGBA8\0RGBA16F\0R32F\0RGB10A2\0"; > = 0;
uniform int Level < ui_type = "slider"; ui_min = 1; ui_max = 5; > = 1;
uniform int Mode < ui_type = "combo"; ui_items = "Mip level\0Reference\0Difference (amplified)\0Over tolerance (red)\0"; > = 3;
uniform float Amplify < ui_type = "slider"; ui_min = 1.0; ui_max = 256.0; > = 32.0;
uniform float Tolerance < ui_type = "slider"; ui_min = 0.0; ui_max = 8.0; ui_tooltip = "In 8-bit steps"; > = 1.0;

texture texMipA { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; MipLevels = 6; Format = RGBA8; };
texture texMipB { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; MipLevels = 6; Format = RGBA16F; };
texture texMipC { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; MipLevels = 6; Format = R32F; };
texture texMipD { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; MipLevels = 6; Format = RGB10A2; };
sampler sMipA { Texture = texMipA; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sMipB { Texture = texMipB; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sMipC { Texture = texMipC; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sMipD { Texture = texMipD; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

uint hash(uint x)
{
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

float3 noise(uint2 p)
{
	const uint h = hash(p.x + hash(p.y));
	return float3(h & 0x3FFu, (h >> 10) & 0x3FFu, (h >> 20) & 0x3FFu) / 1023.0;
}

void PS_Fill(float4 pos : SV_Position, float2 uv : TEXCOORD,
	out float4 a : SV_Target0, out float4 b : SV_Target1, out float4 c : SV_Target2, out float4 d : SV_Target3)
{
	const float3 n = noise(uint2(pos.xy));
	a = float4(n, 1.0);
	b = float4(n, 1.0);
	c = n.rrrr;
	d = float4(n, 1.0);
}

float3 fetch(int2 t, int lod)
{
	if (Format == 0) return tex2Dfetch(sMipA, t, lod).rgb;
	if (Format == 1) return tex2Dfetch(sMipB, t, lod).rgb;
	if (Format == 2) return tex2Dfetch(sMipC, t, lod).rrr;
	return tex2Dfetch(sMipD, t, lod).rgb;
}

float3 PS_Show(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const int2 size = max(int2(BUFFER_WIDTH, BUFFER_HEIGHT) >> Level, 1);
	const int2 t = min(int2(uv * float2(size)), size - 1);

	const float3 mip = fetch(t, Level);
	const float3 ref = 0.25 * (fetch(t * 2, Level - 1) + fetch(t * 2 + int2(1, 0), Level - 1) +
	                           fetch(t * 2 + int2(0, 1), Level - 1) + fetch(t * 2 + int2(1, 1), Level - 1));

	const float3 diff = abs(mip - ref);
	if (Mode == 0) return mip;
	if (Mode == 1) return ref;
	if (Mode == 2) return saturate(diff * Amplify);
	const float tol = Tolerance / 255.0;
	return any(diff > tol) ? float3(1.0, 0.0, 0.0) : float3(0.1, 0.1, 0.1);
}

technique sopt_MipTest
{
	pass Fill
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Fill;
		RenderTarget0 = texMipA;
		RenderTarget1 = texMipB;
		RenderTarget2 = texMipC;
		RenderTarget3 = texMipD;
	}
	pass Show
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Show;
	}
}
