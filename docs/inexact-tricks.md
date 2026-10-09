# Not-exact and conditional tricks

Rewrites that look like faster versions of a shader expression but are not exact everywhere.
They are collected here for two reasons: so sopt (and we) can recognise and avoid them when
they turn up again, and so a programmer can still use one where the stated limitation cannot
affect their code.

"Exact" means: the same float32 result as the original for every input in the stated range (or
within sopt's budget for the region). Costs are throughput units where one mad = 4 (sopt-opbench,
docs/opbench/; Turing = GTX 1660 / RTX 2060 S, Ampere = RTX 3050, Blackwell = RTX 5080 / 5090,
Intel = Gen9 / 9.5 iGPUs, AMD = the rdna3 model from RGA).

## Conditional tricks: exact under a condition

### Two-way sign: `x >= 0.0 ? 1.0 : -1.0`
- Replaces `sign(x)`. 2 instructions (ge + movc), no int -> float conversion.
- Cost: Turing 4.3, Ampere 10.6, Blackwell 8, Intel ~7.5 (sign: 8.4, 28, 18, 14.5).
- **What's wrong:** gives 1 at x = 0 (and at -0) where sign gives 0.
- **Safe when** the result is multiplied by something that is 0 at x = 0, e.g. the signed power
  `sign(x) * pow(abs(x), g)` with g > 0. That case is a library rule; sopt finds it on Turing.

### Copying x's sign bit: `asfloat((asuint(x) & 0x80000000u) | 0x3F800000u)` for `x >= 0.0 ? 1.0 : -1.0`
- Bit trick (sopt `--bits`, library): and + or, no compare, no select; also `y` with x's sign
  (`asfloat(asuint(y) ^ (asuint(x) & 0x80000000u))` for `x >= 0.0 ? y : -y`).
- **What's wrong:** differs at x = -0.0 (-0 >= 0 is true, but its sign bit is set): -1 instead of 1.
- **Safe when** x is never -0.0 (-0 comes from negating or multiplying a zero, e.g. `-a` with a = 0,
  `x * -1` at 0). sopt keeps such variants marked "differs at x = -0.0"; never picked by SOPT_AUTO.

### Magic-constant approximations (`0x5F3759DF` rsqrt, `0x7EF311C7` rcp, `0x1FBD1DF5` sqrt)
- Integer subtracts / shifts on the bits give a rough first guess; Newton-Raphson (NR) steps refine it. Michal
  Drobot's ShaderFastMathLib.h (github.com/michaldrobot/ShaderFastLibs, 2014, tuned for AMD GCN) packages them:
  `fastRcpSqrtNR0(x) = asfloat(0x5F3759DF - (asint(x) >> 1))`, `fastSqrtNR0 = asfloat(0x1FBD1DF5 + (asint(x) >> 1))`,
  `fastRcpNR0 = asfloat(0x7EF311C2 - asint(x))`, NR1 / NR2 add one / two steps (`g * (1.5 - 0.5x * g * g)`,
  `g * (2 - x * g)`).
- Max relative error (checked 2026-10-09, float32, x in [1e-6, 1e6]): rsqrt NR0 3.4%, NR1 0.18%, NR2 0.0005% (as the
  library says); sqrt NR0 **4.5%** (the library says < 0.7%), rcp NR0 **5.1%** (says < 0.4%), rcp NR1 **0.26%** (says
  < 0.02%), rcp NR2 0.0007%. The hardware rcp / rsqrt / sqrt are within ~1-2 ulp (~1e-7).
- Cost (units, mad = 4): rsqrt NR0 = a shift + a subtract (bit casts are free) vs the hardware rsqrt: Turing ~5 vs 12,
  Ampere / Ada ~8 vs 24, Blackwell ~7 vs 23, RDNA 2 ~6 vs 8, RDNA 3 / 4 ~14 vs 27, GCN ~4 vs 8.5, Intel Gen9 ~11 vs 12,
  Gen12 ~8 vs 11, Gen7.5 ~13 vs 3.5 (Haswell's math unit is cheap, its integer ops half rate). rcp NR0 is one integer
  subtract (Turing 0.6 vs 12). One NR step costs about four more fma (~16 units): NR1 is about even with the hardware
  op on Ampere / Ada / Blackwell and slower everywhere else; NR2 is always slower.
- **What's wrong:** a few percent off (NR0) is visible in 8-bit color (3.4% of 1.0 is ~9 steps); 0 gives a large
  finite value instead of inf (rsqrt NR0(0) = 1.3e19, rcp NR0(0) = 1.6e38); negative inputs give garbage; denormals
  are handled differently. No integer ops in D3D9 / SM3: ReShade's DX9 path cannot compile them (guard with
  `__RENDERER__ >= 0xA000`).
- **Safe when** a few percent (NR0) or 0.2% (NR1) does not matter for the result, e.g. a weight that is normalized
  again later, an approximate falloff, or where the value only steers a choice; never for colors written as they are.
  The constants are in sopt's `--bits` pool, and with `--loose` such forms are listed as "less accurate".

### Polynomial acos / asin / atan (ShaderFastMathLib.h `acosFast4`, `asinFast4`, `atanFast4`)
- `acosFast4`: Abramowitz & Stegun 4.4.45, `sqrt(1 - |x|) * (1.5707288 - 0.2121144|x| + 0.0742610x^2 - 0.0187293|x|^3)`,
  mirrored for x < 0; max error 6.8e-5 rad (as stated). **fxc already writes `acos` / `asin` exactly this way** (the
  same four coefficients, checked with Microsoft's fxc -O3, ps_5_0), in Horner form with two multiplies fewer than the
  library's x2 / x3 form: no gain on DX10-12. On Vulkan / OpenGL ReShade emits GLSL.std.450 Acos and the driver decides;
  whether the polynomial is faster there needs a measurement.
- `atanFast4 = x * (1.0301 - 0.1784|x| - 0.0663x^2)`: max error **1.5e-3 rad** on [-1, 1] (the library says 7e-5, 20x
  less), and **only valid for |x| <= 1** (atan4(2) = 0.82 vs 1.11, atan4(10) = -73.8). fxc's `atan` covers the whole
  range (range reduction with a divide, a 4-term polynomial, ~1e-5 rad) at ~13 instructions.
- **Safe when** (atan) the argument is known to be in [-1, 1] and 1.5e-3 rad is fine.

### The `* 1e38` saturate forms (sign, and the floor / ceil / frac forms below)
- `sign(x) -> mad(saturate(mad(x, 1e38, 0.5)), 2.0, -1.0)` (mad_sat + mad, 2 instructions; found
  by sopt), `saturate(x * 1e38) - saturate(x * -1e38)`, `clamp(x * 1e38, -1.0, 1.0)`.
- Cost of the 2-instruction form: ~10 on Ampere / Blackwell / Intel (sign 28 / 18 / 14.5).
- **What's wrong:** nothing where fp32 denormals are flushed to zero (D3D10+ requires that for
  math instructions): any normal x times 1e38 is at least 1 in magnitude. Where denormals are kept
  (possible on some Vulkan / OpenGL drivers), a denormal x gives a value between -1 and 1 instead
  of +-1. Checked on every float in [-2^22, 2^22] with FTZ: exact.

### Add-round: `(x + 12582912.0) - 12582912.0` = `round(x)`
- 12582912 = 1.5 * 2^23: x + 1.5 * 2^23 lands where the float spacing is 1, so the add rounds x
  to the nearest integer (ties to even, like HLSL round). 2 adds.
- Cost: 8 (round: Ampere 24, Blackwell 23, Turing 12; Intel and AMD 4, no gain there).
- Built on it (5 instructions each, exact, every float in [-2^22, 2^22] checked):
  `floor(x) = r - saturate((r - x) * 1e38)`, `ceil(x) = r + saturate((x - r) * 1e38)`,
  `frac(x) = d + saturate(d * -1e38)` with `r` the add-round and `d = x - r`. Ampere / Blackwell
  23-24 -> 20-21.
- **What's wrong:** only valid for |x| <= 2^22 (4194304); beyond that the add no longer rounds to
  integers. And **fxc -O3 folds `(x + c) - c` back to `x`** (see Pitfalls), which silently gives
  a wrong result (`mad(uv.x, 1000, C) - C` became `uv.x * 1000`). Write it as `precise`:
  `precise float r = (x + 12582912.0) - 12582912.0;` sopt-fx does this automatically.

### `floor(SV_Position.xy)` = `SV_Position.xy - 0.5`
- Pixel centres are at n + 0.5, so subtracting 0.5 is exact and gives the integer pixel index.
  1 instruction instead of floor (Ampere / Blackwell 23-24).
- **What's wrong:** only for positions at pixel centres. With per-sample shading (MSAA sample
  frequency) the positions are not n + 0.5. Not yet a fact sopt knows (SV_Position is treated as
  any value in [0, 7680]).
- The general form `floor(x) -> round(x - 0.5)` is **wrong at every odd integer** (n - 0.5 is a
  tie and rounds to the even neighbour n - 1): 1000 of the 2001 integers in [-1000, 1000].

### Short ceil: `(x + C) - ((x + C) - (x + 0.5))`, C = 12582912
- Found by sopt for Blackwell: 4 adds (cost 16 against ceil 23 and the exact form's 21).
- **What's wrong:** gives 0 instead of 1 for 0 < x <= 2^-25 (2.98e-8): there `x + 0.5` rounds to
  0.5 and the tie in the second subtraction goes to the even value. Exact for every other float
  in [-2^22, 2^22] (checked exhaustively). Needs `precise` like the add-round.
- **Safe when** x cannot be a tiny positive value (e.g. x on a grid such as a texel or pixel
  coordinate, or x known to be >= 2^-24 or <= 0).
- Passed sopt's sampled verification as bit-exact until the sampler got tiny magnitudes
  (2026-10-02); now rejected.

### `frac(frac(a)) -> frac(a)` (from Mesa)
- **What's wrong:** for tiny negative a, `frac(a)` rounds to 1.0 (`frac(-1e-20) = 1.0` in float)
  and `frac(1.0)` is 0. Exact for a >= 0 (the library rule now says so).

## Not exact: avoid unless the error does not matter

### Clamp by scaling: `mad(saturate(mad(x, 1/(b - a), -a/(b - a))), b - a, a)`
- Replaces `clamp(x, a, b)` with constant a, b: 2 instructions (mad_sat + mad).
- **What's wrong:** the scale there and back rounds twice, so values inside (a, b) move by a few
  ulps of max(|a|, |b|); near 0 (when a < 0 < b) the relative error gets large; the end point b
  can come out one ulp off.
- Also not faster: ~8 against clamp 7.8 on Blackwell and ~8 on Intel; worse on Turing (clamp 4.1,
  min / max nearly free) and on AMD (clamp is one v_med3). Only Ampere gains a little (8 vs 10).

### Reassociating additions: `(a + b) + c -> a + (b + c)` and the like
- **What's wrong:** where two terms nearly cancel, the order decides the result
  (58.6359 + -58.9101 + 1.206 differs between the orders). fxc may reassociate anyway; `precise`
  stops it.

### `lerp` <-> `a + t * (b - a)` / `a * (1 - t) + b * t` and related forms
- **What's wrong:** the forms round differently and cancel differently once t is outside [0, 1]
  (failing points had t = -3.9, -39.6, -86.8, 2.002); GPUs also implement lerp in different ways
  (sopt's `mix` profile). Interchangeable for t in [0, 1] (the library rules that pass say
  `where t in [0, 1]`). Mesa rules rejected for this: `a + c * (b - a) -> lerp(a, b, c)`,
  `lerp(a + b, a + c, d) -> lerp(b, c, d) + a`, `mad(b - a, a, a) -> lerp(a, b, a)`,
  `a * (2 - a) -> lerp(a, 1, a)`, `1 - (1 - a)(1 - b) -> lerp(b, 1, a)`,
  `1 + c * (b - 1) -> (1 - c) + b * c`.

### `t * (1 - t) -> t - t * t`
- **What's wrong:** loses relative precision near t = 1 without fma (t - t * t cancels).

### pow / exp2 / log2 identities
- `pow(a, b) -> exp2(log2(a) * b)`: this *is* how fxc lowers pow, so on the GPU it changes
  nothing; it differs from C's pow only for a < 0 (GPU pow is undefined there).
- `exp2(log2(a) * b + log2(c) * d) -> pow(a, b) * pow(c, d)`: each pow can overflow or underflow
  where the combined exponent is fine.
- `pow(abs(pow(a, 2.2)), 0.454545) -> abs(a)`: 0.454545 is not exactly 1 / 2.2, so the gamma
  round trip is not the identity (error grows with |log a|).
- `log2(pow(a, b)) -> b * log2(a)`: differs at a = 0, b = 0 (log2(1) = 0 vs 0 * -inf = NaN) and
  where pow overflows.
- `exp(x * c) -> exp2(x * (c * log2(e)))`: folding the constant changes the rounding; the
  relative error grows with |x * c|. The library rules only allow it for |x * c| <= 8.

### Cancelling rational forms (ReShade.fxh reversed depth)
- The partial-fraction rewrite of the depth linearisation ends in a cancellation
  (~0.00105 - 0.00100). With the GPU's approximate rcp its relative error at small depths is ~10x
  the original's: DisplayDepth's normals got visibly noisier. sopt rejects it. The
  cancellation-free form `(1.0 - d) * rcp(mad(d, F - 1.0, 1.0))` is *more* accurate than the
  original (an accuracy fix, not a speedup).

### Noise hashes: `frac(sin(dot(uv, k)) * 43758.5)`
- float32 sin of large arguments is chaotic: the original is off from exact math by up to 1, so
  "mathematically equal" rewrites give different noise. Only rewrites that keep the same float32
  noise pass (sopt's noise guard). Swapping in a cheaper hash would need a noise mode that checks
  distribution instead of values (planned, not done).

### Compiler cancellations: `(a - b) + b -> a`, `(y / x) * x -> y`, `(x * y) / x -> y` (spirv-opt)
- spirv-opt's folding rules apply these (MergeGenericAddSubArithmetic, MergeMulDivArithmetic,
  MergeDivMulArithmetic); they save one or two instructions.
- **What's wrong:** they equal real math, not float math: `(a - b) + b` rounds `a - b` first, which is
  exactly what rounding tricks rely on (e.g. the add-round above, `(x + C) - C`). The compiler only
  keeps such code as written when it is marked `precise`.
- **Safe when** the code is ordinary arithmetic, not a deliberate rounding step. sopt's check passes
  them (as close to exact math as the original), so they are left out of the library on purpose.

### Constant multiply pushed into an add: `(x + b) * c -> mad(x, c, b * c)` (LLVM InstCombine)
- One instruction instead of two (add + mul -> mad).
- **What's wrong:** where x is close to -b, x + b cancels exactly in the original, but the mad adds
  two rounded products and keeps their rounding error (relative error up to 1 near the zero).
- **Safe when** x and b have the same sign (no cancellation): library rules with `x >= 0, b >= 0`
  and `x <= 0, b <= 0`. `(x * a + b) * c -> mad(x, a * c, b * c)` passes sopt's check without a condition.

### Divide chains: `(x / y) / z -> x / (y * z)`, `z / (x / y) -> (z * y) / x` (LLVM InstCombine)
- One divide fewer (a divide is rcp + mul, the rcp at quarter rate on most GPUs).
- **What's wrong:** `y * z` can overflow or flush to zero where the two divides would not (e.g. y = z =
  1e-20: y * z = 1e-40 is a denormal and flushes to 0).
- **Safe when** |y| and |z| stay in [1e-15, 1e15] (library rules use positive ranges).

## Pitfalls (compiler behaviour, not tricks)
- **fxc -O3 reassociates float math.** It folds `(x + c) - c` to `x`, even with c a uniform, and
  merges constant chains. Any trick that relies on a rounding step must be `precise` (ReShade FX
  supports it and passes it to HLSL, GLSL and SPIR-V). `precise` also applies backwards to the
  operations that compute the value's inputs, which then are not contracted into mads.
- **fxc writes `clamp` as `max` + `min`.** Only AMD's driver turns that back into one v_med3.
- **fxc writes `sign` as lt, lt, iadd, itof.** The int -> float conversion is what makes sign
  slow on Ampere / Blackwell (see the 1e38 forms above).

## NaN tests and `!=` (2026-10-06)

`(x != x)` as `isnan(x)`: on Direct3D 10-12 both compile to the same `ne r, x, x` where they survive, so neither is
faster. But ReShade compiles with fxc -O3 without `D3DCOMPILE_IEEE_STRICTNESS`, and fxc then assumes inputs (constant
buffers, textures) are never NaN or infinite: `isnan(x)` and `x != x` on such a value compile to `false`, `x / x` to 1,
`x - x` to 0, `isnan(inf - inf)` to false. A NaN from a division of different values survives, and `precise` keeps any
check (`precise float v = x; isnan(v)`). On ReShade's Vulkan path `x != x` does not work either: ReShade 6.8.0's SPIR-V
generator writes a float `!=` as `OpFOrdNotEqual`, false when either side is NaN, while `isnan()` becomes `OpIsNan`. So:
use `isnan()` on a `precise` value. The bit test `(asuint(x) & 0x7FFFFFFF) > 0x7F800000` is exact on every backend but one
instruction longer (`and` + `ult`), and fxc folds it too when it assumes the value cannot be NaN. `!(a == b)` and `a != b`
differ on ReShade's Vulkan path when an operand is NaN. That is ReShade's bug, not the rewrite's (IEEE 754 `!=` is
unordered, as on D3D): the library keeps `!(a == b) -> a != b` and `!(a != b) -> a == b` (owner, 2026-10-06: do not rule
out variants for a ReShade bug). The same generator converts float -> bool (`if (x)`) as `x != 0` with `OpFOrdNotEqual`
too, so a NaN counts as false on Vulkan and true on D3D. Test effect and findings: tools/reshade/sopt_IEEE754.fx,
tools/reshade/IEEE754.md.

NaN ordering: every ordered comparison with NaN is false (`NaN > +Inf` is false). Only the raw bits are ordered: read
as signed integers, +NaN is above +Inf and -NaN below -Inf.
