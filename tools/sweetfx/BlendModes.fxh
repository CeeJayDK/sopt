/*------------------.
| :: Description :: |
'-------------------/

	BlendModes.fxh (version 1.0)

	License: MIT

	About:
	The blend modes known from image editors, for any effect to include.
	b = the base (backdrop, usually the back buffer), s = the blend layer, both in [0, 1].
	Formulas: W3C Compositing and Blending Level 1 (normal ... luminosity) plus the usual
	image editor extras (linear burn, vivid light, hard mix, divide ...).

	Every mode is a function BlendModes::<Mode>(b, s), and BlendModes::Blend(mode, b, s, opacity)
	picks one by number (the BLENDMODES_LIST order, for a combo uniform).

	Eight modes can also run in the blend stage instead of the pixel shader, so the shader does not
	read the back buffer at all (one texture fetch and the mixing math less):
	the pixel shader returns BlendModes::Source_<Mode>(s, opacity) and the pass sets
	BLENDMODES_STATE_<MODE>. The back buffer's alpha is kept.

		float4 MyPS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
		{
			return BlendModes::Source_Screen(tex2D(MyLayer, texcoord).rgb, Opacity);
		}
		technique My { pass { VertexShader = PostProcessVS; PixelShader = MyPS; BLENDMODES_STATE_SCREEN } }

	The blend stage clamps the source to [0, 1] on 8 / 10-bit back buffers and rounds it once more
	(at most one 8-bit step off the shader version). On an scRGB (FP16) back buffer nothing is clamped,
	like the shader versions.
	Add, linear burn and subtract with an opacity below 1 apply it before the clamp, like an image editor's
	Fill (Blend() applies it after the clamp, like Opacity); normal, multiply, screen and exclusion match.

	History:
	(*) Feature (+) Improvement	(x) Bugfix (-) Information (!) Compatibility

	Version 1.0
	* 27 blend modes, 8 of them also as blend states
*/

#pragma once

// Names for a combo uniform: ui_items = BLENDMODES_LIST;
#define BLENDMODES_LIST "Normal\0Darken\0Multiply\0Color Burn\0Linear Burn\0Darker Color\0" \
	"Lighten\0Screen\0Color Dodge\0Linear Dodge (Add)\0Lighter Color\0" \
	"Overlay\0Soft Light\0Hard Light\0Vivid Light\0Linear Light\0Pin Light\0Hard Mix\0" \
	"Difference\0Exclusion\0Subtract\0Divide\0" \
	"Hue\0Saturation\0Color\0Luminosity\0Grain Merge\0"

// Blend states for the modes the blend stage can do (put them in the pass after PixelShader).
// Source alpha = what Source_* returns; the destination alpha is kept.
#define BLENDMODES_KEEP_ALPHA SrcBlendAlpha = ZERO; DestBlendAlpha = ONE; BlendOpAlpha = ADD;
#define BLENDMODES_STATE_NORMAL      BlendEnable = true; BlendOp = ADD; SrcBlend = ONE; DestBlend = SRCALPHA; BLENDMODES_KEEP_ALPHA
#define BLENDMODES_STATE_DARKEN      BlendEnable = true; BlendOp = MIN; BLENDMODES_KEEP_ALPHA
#define BLENDMODES_STATE_MULTIPLY    BlendEnable = true; BlendOp = ADD; SrcBlend = DESTCOLOR; DestBlend = ZERO; BLENDMODES_KEEP_ALPHA
#define BLENDMODES_STATE_LINEARBURN  BlendEnable = true; BlendOp = REVSUBTRACT; SrcBlend = ONE; DestBlend = ONE; BLENDMODES_KEEP_ALPHA
#define BLENDMODES_STATE_LIGHTEN     BlendEnable = true; BlendOp = MAX; BLENDMODES_KEEP_ALPHA
#define BLENDMODES_STATE_SCREEN      BlendEnable = true; BlendOp = ADD; SrcBlend = ONE; DestBlend = INVSRCCOLOR; BLENDMODES_KEEP_ALPHA
#define BLENDMODES_STATE_ADD         BlendEnable = true; BlendOp = ADD; SrcBlend = ONE; DestBlend = ONE; BLENDMODES_KEEP_ALPHA
#define BLENDMODES_STATE_EXCLUSION   BlendEnable = true; BlendOp = ADD; SrcBlend = INVDESTCOLOR; DestBlend = INVSRCCOLOR; BLENDMODES_KEEP_ALPHA
#define BLENDMODES_STATE_SUBTRACT    BLENDMODES_STATE_LINEARBURN

namespace BlendModes
{
	// Rec. 601 weights, as in the W3C spec (the non-separable modes and darker / lighter color)
	float Lum(float3 c) { return dot(c, float3(0.3, 0.59, 0.11)); }

	float3 ClipColor(float3 c)
	{
		float l = Lum(c);
		float n = min(min(c.r, c.g), c.b);
		float x = max(max(c.r, c.g), c.b);
		if (n < 0.0) c = l + (c - l) * l / (l - n);
		if (x > 1.0) c = l + (c - l) * (1.0 - l) / (x - l);
		return c;
	}

	float3 SetLum(float3 c, float l) { return ClipColor(c + (l - Lum(c))); }

	float Sat(float3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }

	// The spec's channel sorting, written per channel: min -> 0, max -> s, the middle scaled between.
	float3 SetSat(float3 c, float s)
	{
		float n = min(min(c.r, c.g), c.b);
		float range = max(max(c.r, c.g), c.b) - n;
		return range > 0.0 ? (c - n) * (s / range) : 0.0;
	}

	// Separable modes
	float3 Normal(float3 b, float3 s)      { return s; }
	float3 Darken(float3 b, float3 s)      { return min(b, s); }
	float3 Multiply(float3 b, float3 s)    { return b * s; }
	float3 ColorBurn(float3 b, float3 s)   { return b >= 1.0 ? 1.0 : 1.0 - min(1.0, (1.0 - b) / s); }   // s = 0: x / 0 = inf
	float3 LinearBurn(float3 b, float3 s)  { return max(b + s - 1.0, 0.0); }
	float3 Lighten(float3 b, float3 s)     { return max(b, s); }
	float3 Screen(float3 b, float3 s)      { return b + s - b * s; }
	float3 ColorDodge(float3 b, float3 s)  { return b <= 0.0 ? 0.0 : min(1.0, b / (1.0 - s)); }         // s = 1: x / 0 = inf
	float3 LinearDodge(float3 b, float3 s) { return min(b + s, 1.0); }
	// Overlay without a select (found by SweetOpt): below b = 0.5 the max picks s and saturate gives 2b;
	// above it the mad (the screen half) is >= s and saturate gives 1.
	float3 Overlay(float3 b, float3 s)     { return max(s, mad(2.0 - 2.0 * b, s - 1.0, 1.0)) * saturate(2.0 * b); }
	float3 HardLight(float3 b, float3 s)   { return Overlay(s, b); }
	float3 SoftLight(float3 b, float3 s)
	{
		float3 d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
		return s <= 0.5 ? b * mad(2.0 * s, 1.0 - b, b) : mad(2.0 * s - 1.0, d - b, b);
	}
	float3 VividLight(float3 b, float3 s)  { return s <= 0.5 ? ColorBurn(b, 2.0 * s) : ColorDodge(b, 2.0 * s - 1.0); }
	float3 LinearLight(float3 b, float3 s) { return saturate(mad(s, 2.0, b) - 1.0); }
	float3 PinLight(float3 b, float3 s)    { return clamp(b, 2.0 * s - 1.0, 2.0 * s); }
	float3 HardMix(float3 b, float3 s)     { return step(1.0, b + s); }
	float3 Difference(float3 b, float3 s)  { return abs(b - s); }
	float3 Exclusion(float3 b, float3 s)   { return mad(2.0 * b, 0.5 - s, s); }
	float3 Subtract(float3 b, float3 s)    { return max(b - s, 0.0); }
	float3 Divide(float3 b, float3 s)      { return s <= 0.0 ? (b > 0.0 ? 1.0 : 0.0) : min(b / s, 1.0); }
	float3 GrainMerge(float3 b, float3 s)  { return saturate(b + s - 0.5); }

	// Non-separable modes
	float3 Hue(float3 b, float3 s)          { return SetLum(SetSat(s, Sat(b)), Lum(b)); }
	float3 Saturation(float3 b, float3 s)   { return SetLum(SetSat(b, Sat(s)), Lum(b)); }
	float3 Color(float3 b, float3 s)        { return SetLum(s, Lum(b)); }
	float3 Luminosity(float3 b, float3 s)   { return SetLum(b, Lum(s)); }
	float3 DarkerColor(float3 b, float3 s)  { return Lum(s) < Lum(b) ? s : b; }
	float3 LighterColor(float3 b, float3 s) { return Lum(s) > Lum(b) ? s : b; }

	// Mode by number (BLENDMODES_LIST order), mixed with the base by opacity.
	float3 Blend(int mode, float3 b, float3 s, float opacity)
	{
		float3 r;
		switch (mode)
		{
			default: r = Normal(b, s); break;
			case 1: r = Darken(b, s); break;
			case 2: r = Multiply(b, s); break;
			case 3: r = ColorBurn(b, s); break;
			case 4: r = LinearBurn(b, s); break;
			case 5: r = DarkerColor(b, s); break;
			case 6: r = Lighten(b, s); break;
			case 7: r = Screen(b, s); break;
			case 8: r = ColorDodge(b, s); break;
			case 9: r = LinearDodge(b, s); break;
			case 10: r = LighterColor(b, s); break;
			case 11: r = Overlay(b, s); break;
			case 12: r = SoftLight(b, s); break;
			case 13: r = HardLight(b, s); break;
			case 14: r = VividLight(b, s); break;
			case 15: r = LinearLight(b, s); break;
			case 16: r = PinLight(b, s); break;
			case 17: r = HardMix(b, s); break;
			case 18: r = Difference(b, s); break;
			case 19: r = Exclusion(b, s); break;
			case 20: r = Subtract(b, s); break;
			case 21: r = Divide(b, s); break;
			case 22: r = Hue(b, s); break;
			case 23: r = Saturation(b, s); break;
			case 24: r = Color(b, s); break;
			case 25: r = Luminosity(b, s); break;
			case 26: r = GrainMerge(b, s); break;
		}
		return lerp(b, r, opacity);
	}

	// Sources for the blend states: lerp(b, Mode(b, s), opacity) computed by the blend stage.
	float4 Source_Normal(float3 s, float opacity)     { return float4(s * opacity, 1.0 - opacity); } // ONE, SRCALPHA
	float4 Source_Darken(float3 s)                    { return float4(s, 0.0); }                     // MIN (no opacity)
	float4 Source_Multiply(float3 s, float opacity)   { return float4(s * opacity + (1.0 - opacity), 0.0); }
	float4 Source_LinearBurn(float3 s, float opacity) { return float4((1.0 - s) * opacity, 0.0); }   // b - o (1 - s)
	float4 Source_Lighten(float3 s)                   { return float4(s, 0.0); }                     // MAX (no opacity)
	float4 Source_Screen(float3 s, float opacity)     { return float4(s * opacity, 0.0); }           // so + b (1 - so)
	float4 Source_Add(float3 s, float opacity)        { return float4(s * opacity, 0.0); }
	float4 Source_Exclusion(float3 s, float opacity)  { return float4(s * opacity, 0.0); }           // so (1 - b) + b (1 - so)
	float4 Source_Subtract(float3 s, float opacity)   { return float4(s * opacity, 0.0); }           // b - so
}
