#!/usr/bin/env python3
"""Writes tools/reshade/sopt_IEEE754.fx: a ReShade effect that checks IEEE 754 / HLSL float rules on the GPU, the way
ReShade compiles them for the current API (owner, 2026-10-06; grew out of the owner's NaNtest.fx from before ReShade's
first release).

    python3.12 tools/reshade/gen_ieee754.py      (needs Pillow and DejaVu Sans Mono for the built-in font)

Every test is a bool expression with the placeholders N (NaN), PINF, NINF, Z (+0), NZ (-0) and ONE, evaluated three ways:
  Folded   - from literals only: ReShade's parser (or the backend compiler) folds the whole test to a constant.
  Literal  - the special value is folded to a constant first and written into the shader code as a literal, then used at
             run time (times a run-time 1): checks how ReShade writes NaN / inf / -0 for each backend.
  Run time - the special value is made on the GPU from a run-time zero (timer based), so nothing folds: checks the code
             ReShade generates for the operator and what the GPU does with it.
"""
import os
import sys

sys.dont_write_bytecode = True
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
GW, GH = 7, 13  # glyph cell (pixels at scale 1)

# kind: ieee = IEEE 754 / HLSL language rule (red FAIL), d3d = Direct3D's rule where Vulkan / OpenGL leave it to the
# driver (orange "D3D"), info = either result is allowed (blue yes / no).
GROUPS = [
    ("NaN comparisons (IEEE 754)", [
        ("isnan(NaN)", "isnan(N)", "ieee", ""),
        ("isnan(NaN), NaN precise", "isnanPrecise(N)", "ieee", ""),
        ("NaN bit test", "isnanBits(N)", "ieee", "sm4"),
        ("x != x (x = NaN)", "(N != N)", "ieee", ""),
        ("NaN != another NaN", "(N != N2)", "ieee", ""),
        ("NaN != 1", "(N != ONE)", "ieee", ""),
        ("!(x == x) (x = NaN)", "!(N == N)", "ieee", ""),
        ("!(NaN < 1)", "!(N < ONE)", "ieee", ""),
        ("!(NaN > 1)", "!(N > ONE)", "ieee", ""),
        ("!(NaN <= 1)", "!(N <= ONE)", "ieee", ""),
        ("!(NaN >= 1)", "!(N >= ONE)", "ieee", ""),
        ("!isinf(NaN)", "!isinf(N)", "ieee", ""),
        ("NaN + 1 is NaN", "isnan(N + ONE)", "ieee", ""),
        ("NaN * 0 is NaN", "isnan(N * Z)", "ieee", ""),
    ]),
    ("NaN to bool / int", [
        ("(bool)NaN is true", "(bool)(N)", "ieee", ""),
        ("(bool)-NaN is true", "(bool)(-N)", "ieee", ""),
        ("NaN ? 1 : 0 is 1", "((N) ? 1.0 : 0.0) == 1.0", "ieee", ""),
        ("int(NaN) == 0", "(int)(N) == 0", "d3d", "sm4"),
    ]),
    ("NaN in min / max / saturate", [
        ("min(NaN, 1) == 1", "min(N, ONE) == 1.0", "d3d", ""),
        ("max(NaN, 1) == 1", "max(N, ONE) == 1.0", "d3d", ""),
        ("min(1, NaN) == 1", "min(ONE, N) == 1.0", "d3d", ""),
        ("saturate(NaN) == 0", "saturate(N) == 0.0", "d3d", ""),
        ("clamp(NaN, 0, 1) == 0", "clamp(N, 0.0, ONE) == 0.0", "d3d", ""),
    ]),
    ("Signed zero", [
        ("-0 == +0", "(NZ == Z)", "ieee", ""),
        ("!(-0 < +0)", "!(NZ < Z)", "ieee", ""),
        ("1 / -0 < 0", "(ONE / NZ) < 0.0", "ieee", ""),
        ("1 / +0 > 0", "(ONE / Z) > 0.0", "ieee", ""),
        ("rcp(-0) < 0", "rcp(NZ) < 0.0", "ieee", ""),
        ("rcp(+0) > 0", "rcp(Z) > 0.0", "ieee", ""),
        ("1 / (-0 + +0) > 0", "(ONE / (NZ + Z)) > 0.0", "ieee", ""),
        ("1 / abs(-0) > 0", "(ONE / abs(NZ)) > 0.0", "ieee", ""),
        ("-0 has the sign bit", "asuint(NZ) == 0x80000000u", "ieee", "sm4"),
        ("sign(-0) == 0", "sign(NZ) == 0.0", "ieee", ""),
    ]),
    ("Infinity", [
        ("1 / 0 > FLT_MAX", "PINF > 3.402823466e38", "ieee", ""),
        ("-1 / 0 < -FLT_MAX", "NINF < -3.402823466e38", "ieee", ""),
        ("+inf > -inf", "(PINF > NINF)", "ieee", ""),
        ("isinf(+inf) && isinf(-inf)", "(isinf(PINF) && isinf(NINF))", "ieee", ""),
        ("inf - inf is NaN", "isnan(PINF - PINF2)", "ieee", ""),
        ("inf * 0 is NaN", "isnan(PINF * Z)", "ieee", ""),
        ("1 / inf == 0", "(ONE / PINF) == 0.0", "ieee", ""),
        ("1 / (1 / -inf) < 0", "(ONE / (ONE / NINF)) < 0.0", "ieee", ""),
        ("sqrt(inf) == inf", "sqrt(PINF) == PINF", "ieee", ""),
        ("exp2(-inf) == 0", "exp2(NINF) == 0.0", "ieee", ""),
        ("exp2(inf) == inf", "isinf(exp2(PINF))", "ieee", ""),
    ]),
    ("Math at the edges", [
        ("rsqrt(+0) == +inf", "rsqrt(Z) > 3.402823466e38", "ieee", ""),
        ("log2(+0) == -inf", "log2(Z) < -3.402823466e38", "ieee", ""),
        ("sqrt(-1) is NaN", "isnan(sqrt(-ONE))", "ieee", ""),
        ("log2(-1) is NaN", "isnan(log2(-ONE))", "ieee", "sm4"),
    ]),
    ("Rounding and conversion", [
        ("1 + 2^-23 != 1 (fp32)", "(ONE + 1.1920929e-7) != ONE", "ieee", ""),
        ("0.062499996 != 0.06249999", "(Z + 0.062499996) != 0.06249999", "ieee", ""),
        ("2^24 + 1 == 2^24 (even)", "(16777216.0 * ONE + 1.0) == 16777216.0", "ieee", ""),
        ("round(0.5) == 0 (even)", "round(0.5 * ONE) == 0.0", "d3d", ""),
        ("round(1.5) == 2", "round(1.5 * ONE) == 2.0", "ieee", ""),
        ("round(2.5) == 2 (even)", "round(2.5 * ONE) == 2.0", "d3d", ""),
        ("frac(-0.25) == 0.75", "frac(-0.25 * ONE) == 0.75", "ieee", ""),
        ("-1.5 % 1 == -0.5", "((-1.5 * ONE) % ONE) == -0.5", "ieee", ""),
        ("trunc(-1.5) == -1", "trunc(-1.5 * ONE) == -1.0", "ieee", ""),
        ("int(-1.5) == -1", "(int)(-1.5 * ONE) == -1", "ieee", "sm4"),
        ("uint(-1.5) == 0", "(uint)(-1.5 * ONE) == 0u", "d3d", "sm4"),
        ("int(3e9) == 2147483647", "(int)(3e9 * ONE) == 2147483647", "d3d", "sm4"),
    ]),
    ("Denormals (either is allowed)", [
        ("FLT_MIN / 2 kept", "(1.17549435e-38 * ONE * 0.5) > 0.0", "info", ""),
        ("1e-40 kept", "(1e-40 * ONE) > 0.0", "info", ""),
    ]),
]

SUBST = {
    # Folded: literals only.
    "A": {"N": "(0.0 / 0.0)", "N2": "(0.0 / 0.0)", "PINF": "(1.0 / 0.0)", "PINF2": "(1.0 / 0.0)", "NINF": "(-1.0 / 0.0)", "Z": "0.0", "NZ": "(-0.0)", "ONE": "1.0"},
    # Literal: the value folded into a literal, used at run time.
    "B": {"N": "((0.0 / 0.0) * one)", "N2": "((0.0 / 0.0) * one)", "PINF": "((1.0 / 0.0) * one)",
          "PINF2": "((1.0 / 0.0) * one)", "NINF": "((-1.0 / 0.0) * one)",
          "Z": "(0.0 * one)", "NZ": "(-0.0 * one)", "ONE": "one"},
    # Run time: made on the GPU.
    # Four different run-time zeros, so the compiler cannot use x / x = 1 or x - x = 0 (fxc does).
    "C": {"N": "(zz.x / zz.y)", "N2": "(zz.z / zz.w)", "PINF": "(one / zz.x)", "PINF2": "(one / zz.w)",
          "NINF": "(-one / zz.z)", "Z": "zz.w", "NZ": "(-zz.z)", "ONE": "one"},
}


def subst(expr, col):
    import re
    return re.sub(r"\b(NINF|PINF2|PINF|NZ|N2|N|Z|ONE)\b", lambda m: SUBST[col][m.group(1)], expr)


def font_table():
    f = ImageFont.truetype(FONT, 12)
    words = []
    for code in range(32, 127):
        im = Image.new("L", (GW, GH + 2), 0)
        ImageDraw.Draw(im).text((0, 0), chr(code), font=f, fill=255)
        bits = [0, 0, 0, 0]
        for y in range(GH):
            for x in range(GW):
                if im.getpixel((x, y + 2)) > 100:
                    bits[y // 4] |= 1 << ((y % 4) * GW + x)
        words += bits
    return words


class Text:
    def __init__(self):
        self.chars = []

    def add(self, s):
        off = len(self.chars)
        self.chars += [ord(c) for c in s]
        return off, len(s)

    def words(self):
        c = self.chars + [32] * (-len(self.chars) % 4)
        return [c[i] | c[i + 1] << 8 | c[i + 2] << 16 | c[i + 3] << 24 for i in range(0, len(c), 4)]


def main():
    text = Text()
    tests = []
    lines = []  # (kind 1 = heading, 2 = test; text offset, length, test index)
    for title, items in GROUPS:
        off, n = text.add(title)
        lines.append((1, off, n, 0))
        for label, expr, kind, req in items:
            off, n = text.add(label)
            lines.append((2, off, n, len(tests)))
            tests.append((label, expr, kind, req))
    # Two columns: split at a group boundary near the middle.
    starts = [i for i, l in enumerate(lines) if l[0] == 1]
    split = min(starts, key=lambda i: abs(i - len(lines) / 2))
    left, right = lines[:split], lines[split:]
    rows = max(len(left), len(right))

    def enc(l):
        return l[0] | l[1] << 2 | l[2] << 16 | l[3] << 24 if l else 0

    line_words = [enc(left[i] if i < len(left) else None) for i in range(rows)] + \
                 [enc(right[i] if i < len(right) else None) for i in range(rows)]
    kinds = {"ieee": 0, "d3d": 1, "info": 2}
    kind_bits = 0
    kind_words = [0] * ((len(tests) * 2 + 31) // 32)
    for i, t in enumerate(tests):
        kind_words[i * 2 // 32] |= kinds[t[2]] << (i * 2 % 32)

    fixed = {}
    for name, s in [("title", "ReShade IEEE 754 test"),
                    ("cols", " Folded Literal Run time"),
                    ("legend1", "ok = right    FAIL = wrong (IEEE 754 / HLSL)    D3D = differs from Direct3D's rule"),
                    ("legend2", "Folded: literals only   Literal: NaN / inf / -0 written as a literal   "
                                "Run time: made on the GPU"),
                    ("wrong", "wrong (IEEE 754 / HLSL)"), ("d3d", "not as Direct3D"),
                    ("api_d3d9", "Direct3D 9"), ("api_d3d10", "Direct3D 10"), ("api_d3d11", "Direct3D 11"),
                    ("api_d3d12", "Direct3D 12"), ("api_gl", "OpenGL"), ("api_vk", "Vulkan"), ("api_other", "API ?"),
                    ("reshade", "ReShade"),
                    ("ok", "  ok"), ("fail", " FAIL"), ("d3dw", "  D3D"), ("yes", " yes"), ("no", "  no"),
                    ("na", "  -")]:
        fixed[name] = text.add(s)

    out = []
    w = out.append
    w("// Generated by tools/reshade/gen_ieee754.py: edit the generator, not this file.")
    w("//")
    w("// sopt_IEEE754.fx: checks IEEE 754 / HLSL float rules the way ReShade compiles them for the current API.")
    w("// Each test runs three ways:")
    w("//   Folded   - literals only: the parser or the backend compiler folds the whole test.")
    w("//   Literal  - NaN / inf / -0 are folded to a constant, written into the generated code as a literal and used at")
    w("//              run time: shows how ReShade writes special values for the backend.")
    w("//   Run time - the values are made on the GPU from a run-time zero, so nothing folds: shows the code ReShade")
    w("//              generates for the operator and what the GPU does with it.")
    w("// ok = as IEEE 754 / HLSL require, FAIL = wrong, D3D = differs from Direct3D's rule (Vulkan / OpenGL leave the")
    w("// result to the driver there), yes / no = informative (either is allowed). Grew out of CeeJay's NaNtest.fx.")
    w("//")
    w("// Tests (left to right in the effect):")
    for i, (label, expr, kind, req) in enumerate(tests):
        w(f"//   {i:2d} [{kind:4s}] {label:28s} {expr}")
    w("")
    w('uniform float sopt_timer < source = "timer"; >;')
    w("")
    w("#define SOPT_SM4 (__RENDERER__ >= 0xa000)")
    w("")
    w(f"static const int kTests = {len(tests)};")
    w(f"static const int kRows = {rows};")
    w(f"static const int kGlyphW = {GW};")
    w(f"static const int kGlyphH = {GH};")
    for name, (off, n) in fixed.items():
        w(f"static const int2 kT_{name} = int2({off}, {n});")
    w("")
    w("// Workarounds where the compiler assumes no NaN (fxc without D3DCOMPILE_IEEE_STRICTNESS removes isnan(x) and")
    w("// x != x): a precise value, or the bit test.")
    w("bool isnanPrecise(float x) { precise float v = x; return isnan(v); }")
    w("#if SOPT_SM4")
    w("bool isnanBits(float x) { return (asuint(x) & 0x7FFFFFFFu) > 0x7F800000u; }")
    w("#endif")
    w("")
    w("// Results: bit 0 Folded, bit 1 Literal, bit 2 Run time (1 = test true); 8 = not available here.")
    w("uint runTest(int id, float4 zz, float one)")
    w("{")
    w("\tuint r = 0;")
    w("\t[forcecase] switch (id)")
    w("\t{")
    for i, (label, expr, kind, req) in enumerate(tests):
        w(f"\tcase {i}: // {label}")
        w("#if SOPT_SM4")
        w(f"\t\tr = (({subst(expr, 'A')}) ? 1u : 0u)")
        w(f"\t\t  + (({subst(expr, 'B')}) ? 2u : 0u)")
        w(f"\t\t  + (({subst(expr, 'C')}) ? 4u : 0u);")
        w("#else")
        # Shader model 3 allows no NaN / inf literals: run time only.
        w("\t\tr = 8u;" if req == "sm4" else f"\t\tr = (({subst(expr, 'C')}) ? 4u : 0u);")
        w("#endif")
        w("\t\tbreak;")
    w("\t}")
    w("\treturn r;")
    w("}")
    w("")
    w("// Pass 1 runs every test once, one pixel each, into this texture; the screen and the totals read it, so both show")
    w("// the same results (fxc compiles a test inside the totals loop differently than in a cell: different folds).")
    w(f"texture2D sopt_IEEE754Results {{ Width = {len(tests)}; Height = 1; Format = RGBA8; }};")
    w("sampler2D sopt_IEEE754ResultsS { Texture = sopt_IEEE754Results; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };")
    w("")
    w("float4 IEEE754RunPS(float4 vpos : SV_Position) : SV_Target")
    w("{")
    w("\tfloat4 zz = floor(sopt_timer * float4(1e-12, 1.1e-12, 1.2e-12, 1.3e-12)); // run-time +0s nothing can fold")
    w("\tfloat one = zz.x + 1.0;")
    w("\treturn float(runTest(int(vpos.x), zz, one)) / 255.0;")
    w("}")
    w("")
    w("uint result(int id) { return uint(tex2Dfetch(sopt_IEEE754ResultsS, int2(id, 0)).x * 255.0 + 0.5); }")
    w("")
    w("#if SOPT_SM4")
    w("")

    def table(name, words, per=8):
        w(f"static const uint {name}[{len(words)}] = {{")
        for i in range(0, len(words), per):
            w("\t" + ", ".join(f"0x{x:08X}u" for x in words[i:i + per]) + ",")
        w("};")

    table("kFont", font_table())
    table("kText", text.words())
    table("kLines", line_words)
    table("kKinds", kind_words)
    w(BODY)
    w("#else")
    w(BODY_SM3)
    w("#endif")
    w(TAIL)
    dst = os.path.join(HERE, "sopt_IEEE754.fx")
    with open(dst, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print(dst, len(tests), "tests,", rows, "rows")


BODY = r"""
int kindOf(int id) { return int((kKinds[id / 16] >> uint((id % 16) * 2)) & 3u); }

// 1 where the glyph of character code c covers cell pixel p (0..kGlyphW-1, 0..kGlyphH-1).
float glyph(uint c, int2 p)
{
	if (c < 32u || c > 126u || p.x < 0 || p.y < 0 || p.x >= kGlyphW || p.y >= kGlyphH) return 0.0;
	uint w = kFont[(c - 32u) * 4u + uint(p.y / 4)];
	return float((w >> uint((p.y % 4) * kGlyphW + p.x)) & 1u);
}

uint textChar(int2 t, int i) { return i < 0 || i >= t.y ? 32u : (kText[(t.x + i) / 4] >> uint(((t.x + i) % 4) * 8)) & 0xFFu; }

// Text t drawn from character cell 'start' on; cell = character cell of this pixel, p = pixel inside it.
float drawText(int2 t, int start, int cell, int2 p) { return glyph(textChar(t, cell - start), p); }

// Digit d (0..9) of number n drawn right-aligned so that its last digit is at cell 'last'.
float drawNumber(uint n, int last, int cell, int2 p)
{
	int k = last - cell;
	if (k < 0 || k > 9) return 0.0;
	uint m = n;
	for (int i = 0; i < k; ++i) m /= 10u;
	if (m == 0u && k > 0) return 0.0;
	return glyph(48u + m % 10u, p);
}

float3 IEEE754PS(float4 vpos : SV_Position) : SV_Target
{
	int s = max(1, min(BUFFER_HEIGHT / 540, BUFFER_WIDTH / 800)); // pixel scale
	int cw = kGlyphW * s;
	int lh = kGlyphH * s;
	int2 px = int2(vpos.xy) - int2(2 * cw, lh / 2);
	int cell = px.x >= 0 ? px.x / cw : -1;
	int line = px.y >= 0 ? px.y / lh : -1;
	int2 p = int2(px.x - cell * cw, px.y - line * lh) / s;
	int side = cell >= 52 ? 1 : 0;
	int c0 = cell - side * 52;

	float3 col = float3(0.08, 0.09, 0.11);
	float3 ink = float3(0.92, 0.93, 0.95);

	if (line == 0)
	{
		// Title, API and ReShade version.
		float t = drawText(kT_title, 0, cell, p);
		int2 api = kT_api_other;
		if (__RENDERER__ >= 0x20000) api = kT_api_vk;
		else if (__RENDERER__ >= 0x10000) api = kT_api_gl;
		else if (__RENDERER__ >= 0xc000) api = kT_api_d3d12;
		else if (__RENDERER__ >= 0xb000) api = kT_api_d3d11;
		else if (__RENDERER__ >= 0xa000) api = kT_api_d3d10;
		t += drawText(api, 25, cell, p);
		t += drawText(kT_reshade, 40, cell, p);
		t += drawNumber(uint(__RESHADE__ / 10000), 48, cell, p);
		t += cell == 49 ? glyph(46u, p) : 0.0;
		t += drawNumber(uint((__RESHADE__ / 100) % 100), 50, cell, p);
		t += cell == 51 ? glyph(46u, p) : 0.0;
		t += drawNumber(uint(__RESHADE__ % 100), 52, cell, p);
		return lerp(col, float3(1.0, 0.85, 0.3), saturate(t));
	}
	if (line == 1)
		return lerp(col, ink * 0.75, saturate(drawText(kT_legend1, 0, cell, p)));
	if (line == 2)
	{
		// Totals per column: left half wrong (IEEE 754 / HLSL), right half not as Direct3D.
		if (cell < 0 || (c0 % 8 == 7 && c0 < 24)) return col;
		uint3 wrong = uint3(0u, 0u, 0u);
		uint3 notd3d = uint3(0u, 0u, 0u);
		[loop] for (int i = 0; i < kTests; ++i)
		{
			uint r = result(i);
			if (r == 8u) continue;
			uint3 fails = uint3((r & 1u) ^ 1u, ((r >> 1) & 1u) ^ 1u, ((r >> 2) & 1u) ^ 1u);
			int k = kindOf(i);
			if (k == 0) wrong += fails;
			else if (k == 1) notd3d += fails;
		}
		uint3 n = side == 0 ? wrong : notd3d;
		float t = drawNumber(n.x, 4, c0, p) + drawNumber(n.y, 12, c0, p) + drawNumber(n.z, 20, c0, p);
		t += drawText(side == 0 ? kT_wrong : kT_d3d, 25, c0, p);
		bool bad = side == 0 ? any(n != 0u) : false;
		return lerp(col, bad ? float3(1.0, 0.45, 0.4) : float3(0.5, 0.95, 0.55), saturate(t));
	}
	if (line == 3)
		return lerp(col, ink * 0.6, saturate(drawText(kT_cols, 0, c0, p)));

	int row = line - 4;
	if (row < 0 || row >= kRows || cell < 0) return col;
	uint d = kLines[side * kRows + row];
	int type = int(d & 3u);
	int2 t = int2(int((d >> 2) & 0x3FFFu), int((d >> 16) & 0xFFu));
	if (type == 0) return col;
	if (type == 1)
		return lerp(col, float3(0.45, 0.8, 1.0), saturate(drawText(t, 0, c0, p)));

	int id = int(d >> 24);
	if (c0 >= 25)
		return lerp(col, ink, saturate(drawText(t, 25, c0, p)));
	// Result boxes: 7 cells + 1 gap each.
	int box = c0 / 8;
	if (c0 % 8 == 7 || box > 2) return col;
	int py = px.y - line * lh;
	if (py < s / 2 || py >= lh - s / 2) return col;
	uint r = result(id);
	int k = kindOf(id);
	bool good = (r & (1u << uint(box))) != 0u;
	float3 bg;
	int2 word;
	if (r == 8u) { bg = float3(0.25, 0.25, 0.27); word = kT_na; }
	else if (k == 2) { bg = float3(0.2, 0.35, 0.6); word = good ? kT_yes : kT_no; }
	else if (good) { bg = float3(0.12, 0.45, 0.18); word = kT_ok; }
	else if (k == 1) { bg = float3(0.75, 0.42, 0.05); word = kT_d3dw; }
	else { bg = float3(0.75, 0.12, 0.1); word = kT_fail; }
	return lerp(bg, float3(1.0, 1.0, 1.0), saturate(drawText(word, box * 8, c0, p)));
}
"""

BODY_SM3 = r"""
// Direct3D 9 (shader model 3) has no integer bit operations for the font and allows no NaN / inf literals: the run-time
// results only, one box per test, top to bottom in the order listed at the top of this file (green true, red false; grey
// = needs shader model 4).
float3 IEEE754PS(float4 vpos : SV_Position) : SV_Target
{
	float2 c = floor(vpos.xy / float2(48.0, 14.0));
	int id = int(c.y) - 1;
	if (id < 0 || id >= kTests || c.x != 1.0) return float3(0.08, 0.09, 0.11);
	float r = float(result(id));
	if (r > 7.5) return float3(0.25, 0.25, 0.27);
	return r > 3.5 ? float3(0.12, 0.6, 0.18) : float3(0.8, 0.12, 0.1);
}
"""

TAIL = r"""
void IEEE754VS(in uint id : SV_VertexID, out float4 position : SV_Position)
{
	const float2 uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

technique sopt_IEEE754 < ui_tooltip = "IEEE 754 / HLSL float rules as ReShade compiles them for this API.\n"
	"Folded = literals only, Literal = NaN / inf / -0 written as literals, Run time = made on the GPU."; >
{
	pass Run
	{
		VertexShader = IEEE754VS;
		PixelShader = IEEE754RunPS;
		RenderTarget = sopt_IEEE754Results;
	}
	pass Show
	{
		VertexShader = IEEE754VS;
		PixelShader = IEEE754PS;
	}
}
"""

if __name__ == "__main__":
    main()
