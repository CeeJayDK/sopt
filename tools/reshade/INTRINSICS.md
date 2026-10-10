# ReShade FX intrinsics: what is missing, and how the workarounds hold up

A report for crosire. Checked against ReShade 6.8.0's source (`effect_symbol_table_intrinsics.inl`,
`effect_codegen_hlsl.cpp`, `effect_codegen_glsl.cpp`, `effect_codegen_spirv.cpp`) and REFERENCE.md, the HLSL (SM5)
intrinsic list and GLSL 4.60. The GPU code below was produced with fxc (`d3dcompiler_47`, constants read as hex) and
AMD's RGA 2.15 (`vk-offline`, gfx1100 / RDNA 3); the numeric comparisons emulate float32.

## 1. Documentation

REFERENCE.md lists all 104 intrinsics of the source except `ddx_coarse`, `ddx_fine`, `ddy_coarse` and `ddy_fine`.

## 2. Intrinsics that could be added

From HLSL (code ported from HLSL fails to compile without them):

| Intrinsic | D3D9 (SM3) | D3D10-12 (SM4 / 5) | OpenGL (GLSL) | Vulkan (SPIR-V) |
|---|---|---|---|---|
| `fmod(x, y)` | `fmod` (fxc writes it out) | `fmod` | the existing `fmodHLSL` helper | `OpFRem` (sign of x, as HLSL) |
| `isfinite(x)` | `abs(x) <= 3.402823466e38` | `isfinite` | `!(isinf(x) \|\| isnan(x))` | `OpLogicalNot(OpLogicalOr(OpIsInf, OpIsNan))` (`OpIsFinite` needs the Kernel capability) |
| `clip(x)` (pixel shaders only) | `clip` | `clip` | `if (any(lessThan(x, vec(0.0)))) discard;` | the same condition and the path `discard` already uses |

From GLSL (neither ReShade nor HLSL has them; GLSL shaders ported to ReShade need them):

| Intrinsic | HLSL (all shader models) | OpenGL | Vulkan |
|---|---|---|---|
| `mod(x, y)` (floor-based: the result takes y's sign, unlike `%`) | `x - y * floor(x / y)` | `mod` | `OpFMod` |
| `asinh`, `acosh`, `atanh` | `log(x + sqrt(x * x + 1))`, `log(x + sqrt(x * x - 1))`, `0.5 * log((1 + x) / (1 - x))` | native (GLSL 1.30) | `GLSLstd450Asinh / Acosh / Atanh` |
| `packUnorm4x8(v)` / `unpackUnorm4x8(u)` | SM4+: `uint4 b = uint4(round(saturate(v) * 255.0)); b.x \| b.y << 8 \| b.z << 16 \| b.w << 24` / `float4((u >> uint4(0, 8, 16, 24)) & 255) / 255.0`; SM3: not possible (no integers) | native (GLSL 4.00) | `GLSLstd450PackUnorm4x8 / UnpackUnorm4x8` |

Lower priority: `textureQueryLevels` (the mip count; `tex2Dsize` gives sizes only), `inverse` (matrix; HLSL lacks it too).

## 3. The workarounds, checked

**3.1 `round` differs between APIs at .5 (recommended fix, no cost).**
- D3D9: fxc writes `round(x)` as `floor(x + 0.5)` (`add`, `frc`, `add`): halves round up (2.5 -> 3, -2.5 -> -2).
- D3D10-12: `round_ne`: halves to even (2.5 -> 2, 3.5 -> 4).
- OpenGL `round()` and SPIR-V `GLSLstd450Round`: the direction for .5 is implementation-defined.

Using `roundEven()` (GLSL) and `GLSLstd450RoundEven` (SPIR-V) makes OpenGL and Vulkan match D3D10-12 exactly; both are
one instruction on current hardware (RDNA `v_rndne_f32`). D3D9 can stay as it is (document the half-up rounding).

**3.2 `clamp(x, min, max)` with min > max differs on AMD under Vulkan / OpenGL.**
HLSL defines clamp as `min(max(x, min), max)` (fxc writes `max` + `min`), so min > max gives max. GLSL `clamp` and
SPIR-V `FClamp` are undefined for min > max, and AMD compiles them to one `v_med3_f32` (RGA, gfx1100: `clamp(x, u.x,
u.y)` -> `v_med3_f32 v1, v1, s0, s1`), which returns the median: x = 0.5, min = 1, max = 0 gives 0 under D3D and 0.5
under Vulkan. It happens when a user sets two uniforms the wrong way round. Options: emit `min(max(x, a), b)` in GLSL /
SPIR-V when the bounds are not constants (AMD then spends 2 instructions instead of 1), or document it.

**3.3 `smoothstep(a, b, x)` with a >= b is undefined in GLSL / SPIR-V (no cost to fix).**
HLSL defines it for any edges, and reversed edges (`smoothstep(1.0, 0.0, x)`) are common in ReShade effects. GLSL and
`GLSLstd450SmoothStep` leave a >= b undefined. AMD happens to compile the plain formula (RGA: `v_sub`, `v_rcp`,
`v_subrev`, `v_mul ... clamp`, `v_mul`, `v_fmaak`, `v_mul`), so it works there, but other drivers may not. Emitting
the formula in GLSL / SPIR-V costs nothing extra, since drivers expand smoothstep anyway:
`t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t);`

**3.4 `saturate` and NaN in SPIR-V (likely no cost).**
D3D10+ defines `saturate(NaN) = 0`. SPIR-V `FClamp(x, 0, 1)` is undefined for a NaN x; `GLSLstd450NClamp(x, 0, 1)` is
defined and returns 0, matching D3D. On AMD and NVIDIA the clamp is an output modifier either way (RGA: `clamp(x, 0.0,
1.0)` -> `v_max_f32_e64 v1, v1, v1 clamp`), so NClamp should cost nothing; worth confirming with RGA in SPIR-V mode.
GLSL has no NClamp; the GLSL path would need `x >= 0.0 ? min(x, 1.0) : 0.0` to match, which costs a compare.

**3.5 Float `%` in GLSL: slightly different from D3D at quotient boundaries (fine as it is).**
GLSL uses `fmodHLSL(x, y) = x - y * trunc(x / y)`: on AMD 4 instructions (`v_rcp`, `v_mul`, `v_trunc`, `v_fma`); fxc's
own `%` is 7 (`mul`, `ge`, `movc`, `div`, `mul`, `frc`, `mul`). Over 200,000 float32 points per case:

| Case | fxc `%`: max error, results outside [0, \|y\|) or with the wrong sign | `x - y * trunc(x / y)` |
|---|---|---|
| x in [-1000, 1000], y in [0.1, 3] | 1 \|y\| (a wrap at a boundary), 0 | 1 \|y\|, 7 |
| x in [0, 100], y = 1.001 (AmbientLight.fx) | 5.2e-6 \|y\|, 0 | 0, 0 |
| x in [0, 1e5], y = 289 (Deband.fx's noise) | 2.6e-5 \|y\|, 0 | 0, 0 |

The trunc form is usually more accurate, but where x / y rounds onto an integer it can return a tiny value with the
wrong sign (-1e-7 instead of 0): harmless for most uses, visible only where the result wraps a coordinate. No change
needed; if D3D-identical results matter, the GLSL helper could mirror fxc's form instead.

**3.6 Fine as they are.**
- `rcp` -> `1.0 / x` (GLSL) and `log10` -> `log2(x) / log2(10.0)`: AMD compiles them to `v_rcp_f32` and `v_log_f32` +
  `v_mul_f32` (the division by a constant becomes a multiply), as good as it gets.
- SM3 `floor(x)` -> `x - frac(x)`: exact.
- SM3 bit fallbacks (`__countbits`, `__reversebits`, `__firstbitlow`, loop versions): correct, since fxc emulates
  integer division with truncation on ps_3_0 (`frc` + `add` after the divide).

**3.7 SM3 `asint` / `asfloat` fallbacks: inherent limits (low priority).**
SM3 has no integers, so the emulated bit patterns are floats: exact only below 2^24, so the low mantissa bits are lost
for most values (bit tricks such as `0x5F3759DF - (asint(x) >> 1)` cannot work on D3D9). -0, NaN, infinity and
denormals are not handled (as the code's comments say). `__asfloat(int)` with a negative int argument: the sign test
`v > 2147483647` is never true for a negative int, so the result is wrong; testing `v < 0` as well (or adding 2^32 first)
would fix it.
