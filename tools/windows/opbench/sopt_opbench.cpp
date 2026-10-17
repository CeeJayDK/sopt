// OpBench (sopt-opbench until 0.2.0): measures what single GPU instructions and instruction patterns cost on the
// GPU in this machine (owner's idea, 2026-10-01), to calibrate sopt's cost models.
//
//   OpBench [--adapter N] [--list] [--filter text] [--reps N] [--out file.csv] [--groups N]
//
// Every test is one HLSL step x = f(x, c) repeated in long dependent chains in a D3D11 compute
// shader (compiled at run time with Microsoft's D3DCompile -O3, as ReShade does on D3D9-12; the
// driver then compiles the DXBC for the GPU). The constants c come from a constant buffer and
// change with every unrolled step, so neither compiler can fold or reassociate the chain. Each
// step ends in mad(y, c.x, c.y), which keeps the values in range; the plain step
// x = mad(x, c.x, c.y) is the reference, and a test's cost is its time minus its base test's
// time per step, in sopt's units (4 = the reference mad, i.e. one fma).
//
// Three configurations:
//   tput  8 independent chains per thread, 1M threads: throughput (the cost model's question)
//   dep   1 chain per thread, 1M threads: dependent issue with full occupancy
//   lat   1 chain, one thread group: latency of a dependent step, relative to mad's
// Results go to the console and a CSV (default Reports\opbench-<gpu>.csv next to the exe); the DXBC
// disassembly of every test goes to Reports\Shaders\OpBench\ so a folded test can be spotted.
//
// Version 2: every test is measured twice (forward, then backward through the list) with a fresh
// reference mad right before it, so a GPU clock change only affects the tests around it and shows
// as drift or as a disagreement between the two passes; the summary lists the throughput costs in
// a fixed order with a bar per test; the CSV has the GPU / driver once in '#' header lines.
//
// Version 3 (0.2.0): vector chains (dot, cross, length, normalize), intrinsics fxc writes out
// (atan, asin, tan, fmod, smoothstep, sincos), integer / bit operations and int <-> float
// conversions on uint chains, half precision (min16float); summary in sections. Docs/OpBench.html
// describes every test.
//
// Version 4 (0.3.0, owner 2026-10-03): renamed OpBench; block graphics only from full and half
// blocks (4 levels per cell from bright / dark color pairs), a gradient progress bar, an
// Ops column; tests whose passes disagree are measured again (up to kMaxPasses) until most
// readings agree.
//
// Version 6 (0.5.0, owner 2026-10-04: test everything ReShade FX and HLSL can do): the rest of the
// intrinsics (hyperbolic, ldexp / frexp / modf, isnan / isinf, f16 conversions, refract,
// faceforward, matrices, determinant), integer divide / modulo, int -> float, compare + select;
// "(shorter is better)" under each section; the progress bar stays within 70 characters.

#include "../benchkit.hpp"
#include "../expected.hpp"
#include <d3dcompiler.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <regex>
#include <sstream>

namespace {

using namespace benchkit;


struct Test {
  const char* name;
  const char* step;  // HLSL expression of x (the chain value) and c (this step's float4 constants)
  float cx, cy, cz, cw;
  const char* base;  // the test whose time is subtracted
  const char* note;
  // Type of the chain value x: float, float2..4, uint (constants are then random odd 32-bit
  // patterns, read with asuint) or min16float.
  const char* type = "float";
  // Compute setup: kGroupshared (groupshared float GS[2048] / uint GSI[64], filled before the loop and read
  // after it, or fxc drops writes nothing reads), kLocalArray (a local float A[16]). A step with ';' is
  // statements that assign x, not an expression.
  int setup = 0;
  // Parallel issue: the odd chains run pairStep (of pairType) instead of step, so a throughput run
  // interleaves 4 chains of each; solo names the test that runs pairStep alone (and is the base).
  const char* pairStep = nullptr;
  const char* pairType = "float";
  const char* solo = nullptr;
  // How many ops run at once (owner, 2026-10-10): chains overrides the throughput run's 8 independent chains per
  // thread (1..6 show a VLIW GPU's width: a scalar GPU costs the same with 1 chain, a VLIW5 one up to 5x more);
  // pairCount > 0 lets the last pairCount chains run pairStep instead of the odd ones (4 fma + 1 rcp: VLIW's t slot).
  int chains = 0;
  int pairCount = 0;
  // Compiled without fxc's optimizer (/Od, D3DCOMPILE_SKIP_OPTIMIZATION): an "-od" twin of a test (see kOdPairs).
  bool od = false;
};
enum { kGroupshared = 1, kLocalArray = 2, kPixelOnly = 4 };  // kPixelOnly: derivatives / clip (StageBench's pixel run)

// StageBench (owner, 2026-10-13: GPU Blueprint researches GPU architectures, not only what ReShade uses): this file
// built with SOPT_STAGEBENCH runs the same tests in a pixel shader (one thread per pixel of a 1024 x 1024 target) or a
// vertex shader (one thread per point, drawn off-screen) instead of a compute shader; throughput only. Compute-only
// tests (groupshared memory, barriers, atomics) are left out there, the derivative tests are pixel shader only.
enum class Stage { Compute, Vertex, Pixel };
#ifdef SOPT_STAGEBENCH
Stage gStage = Stage::Pixel;
constexpr const char* kTool = "StageBench";
#else
Stage gStage = Stage::Compute;
constexpr const char* kTool = "OpBench";
#endif
const char* stageName() { return gStage == Stage::Pixel ? "ps" : gStage == Stage::Vertex ? "vs" : "cs"; }
bool runsHere(const Test& t) {
  if (gStage != Stage::Compute && (t.setup & kGroupshared)) return false;
  return gStage == Stage::Pixel || !(t.setup & kPixelOnly);
}

// Constants keep every chain finite and away from denormals (x stays roughly in [0.3, 3]).
const Test kTests[] = {
    {"mad", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, nullptr, "reference: one fma per step"},
    {"add", "mad(x + c.z, c.x, c.y)", 0.5f, 0.5f, 0.1f, 0.0f, "mad", ""},
    {"sub", "mad(c.z - x, c.x, c.y)", 0.5f, 1.0f, 2.0f, 0.0f, "mad", ""},
    {"mul", "mad(x * c.z, c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.0f, "mad", "~0 where the driver merges it: x * (c.z * c.x)"},
    // One fma with a single constant (x stays near the fixed point -0.37): the fp32 score's rate on GPUs where
    // the reference's two constants cost a second instruction (GCN's constant bus: an extra v_mov).
    {"fma1", "mad(x, x, c.x)", -0.5f, 0.0f, 0.0f, 0.0f, nullptr, "one fma, one constant: the fp32 score where it is faster"},
    // Movs (owner, 2026-10-15: they compute nothing; can sopt avoid them?): GCN reads one scalar operand per
    // instruction and no literal in a 3-operand one (a v_mov first), RDNA two scalars and one literal (RGA: v_fmaak /
    // v_fmamk). Each step is clamped by saturate (free everywhere, and active: x climbs above 1), so no driver can merge
    // steps whose constants are all literals (Intel does: x * k1 + b1 then * k2 + b2 is one mad, even through abs). The
    // clamp must be able to go either way: each step maps [0, 1] over all of [0, 1] (slope ~2, offset -0.5); fxc tracks
    // intervals through the unrolled steps and folds a chain whose interval shrinks to a point.
    // madsatu: two uniforms (the base), madlit: two literals (k: this step's own), madlit1: a literal and a uniform,
    // madinl: two values AMD encodes for free (inline constants: +-0.5, 1, 2, 4, 1 / 2pi).
    {"madsatu", "saturate(mad(x, c.x, c.y))", 2.1f, -0.5f, 0.0f, 0.0f, "mad", "base for the literal tests: two uniforms"},
    {"madlit", "saturate(mad(x, k + 1.5, -0.5 - k * 0.1))", 0.0f, 0.0f, 0.0f, 0.0f, "madsatu", "two literals: a v_mov on GCN?"},
    {"madlit1", "saturate(mad(x, k + 1.5, c.y))", 0.0f, -0.5f, 0.0f, 0.0f, "madsatu", "a literal and a uniform"},
    {"madinl", "saturate(mad(x, 2.0, -0.5))", 0.0f, 0.0f, 0.0f, 0.0f, "madsatu", "inline constants: free to encode on AMD"},
    {"mad2", "mad(mad(x, c.z, c.w), c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.1f, "mad", "a second mad"},
    {"min", "mad(min(x, c.z), c.x, c.y)", 0.5f, 0.5f, 1.2f, 0.0f, "mad", ""},
    {"max", "mad(max(x, c.z), c.x, c.y)", 0.5f, 0.5f, 0.8f, 0.0f, "mad", ""},
    {"neg", "mad(-x, c.x, c.y)", 0.5f, 1.5f, 0.0f, 0.0f, "mad", "source modifier"},
    {"abs", "mad(abs(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "source modifier"},
    {"negabs", "mad(-abs(x), c.x, c.y)", 0.5f, 1.5f, 0.0f, 0.0f, "mad", "source modifiers"},
    {"saturate", "mad(saturate(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "output modifier of the previous mad"},
    {"clamp", "mad(clamp(x, c.z, c.w), c.x, c.y)", 0.5f, 0.5f, 0.8f, 1.2f, "mad", "AMD: v_med3"},
    {"floor", "mad(floor(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"ceil", "mad(ceil(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", ""},
    {"round", "mad(round(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"trunc", "mad(trunc(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "round toward zero (fxc: round_z)"},
    {"frac", "mad(frac(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"sign", "mad(sign(x - c.z), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub", "sign of a difference"},
    {"step", "mad(step(c.z, x), c.x, c.y)", 0.5f, 1.0f, 1.2f, 0.0f, "mad", ""},
    {"select", "mad(x < c.z ? c.w : x, c.x, c.y)", 0.5f, 0.5f, 1.0f, 0.7f, "mad", "compare + cndmask"},
    {"lerp", "mad(lerp(c.z, x, c.w), c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.6f, "mad", ""},
    {"rcp", "mad(rcp(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"div", "mad(c.z / x, c.x, c.y)", 0.5f, 1.0f, 1.3f, 0.0f, "mad", "c / x"},
    {"divxy", "mad(x / (x + c.z), c.x, c.y)", 0.5f, 1.0f, 0.5f, 0.0f, "add", "x / y"},
    {"sqrt", "mad(sqrt(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", ""},
    {"rsqrt", "mad(rsqrt(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", ""},
    {"exp2", "mad(exp2(x), c.x, c.y)", 0.25f, 0.0f, 0.0f, 0.0f, "mad", ""},
    // Fast approximations (ShaderFastMathLib.h, M. Drobot 2014; bit tricks from Bit Twiddling Hacks): integer guesses on
    // the float bits, with and without one Newton-Raphson step, polynomial acos / atan, power-of-2 rounding.
    {"rsqrtnr0", "mad(asfloat(0x5F3759DFu - (asuint(x) >> 1)), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad",
     "rsqrt guess, no NR step (3.4% off): shift, int sub"},
    {"rsqrtnr1", "mad(rsqrtNR1(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "rsqrt guess + 1 NR step (0.18% off)"},
    {"rcpnr0", "mad(asfloat(0x7EF311C2u - asuint(x)), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad",
     "rcp guess, no NR step (5% off): int sub"},
    {"rcpnr1", "mad(rcpNR1(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "rcp guess + 1 NR step (0.26% off)"},
    {"sqrtnr0", "mad(asfloat(0x1FBD1DF5u + (asuint(x) >> 1)), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad",
     "sqrt guess, no NR step (4.5% off): shift, int add"},
    {"pow2floor", "mad(asfloat(asuint(x) & 0x7F800000u), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad",
     "exp2(floor(log2(x))) from the bits: one and (exact)"},
    {"exp2floor", "mad(exp2(floor(log2(x))), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "the same written out"},
    {"pow2ceil", "mad(asfloat((asuint(x) + 0x007FFFFFu) & 0x7F800000u), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "mad",
     "exp2(ceil(log2(x))) from the bits: add, and (exact)"},
    {"exp2ceil", "mad(exp2(ceil(log2(x))), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "mad", "the same written out"},
    {"log2", "mad(log2(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"exp", "mad(exp(x), c.x, c.y)", 0.25f, 0.0f, 0.0f, 0.0f, "mad", "fxc: mul + exp2"},
    {"log", "mad(log(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "fxc: log2 + mul"},
    {"pow", "mad(pow(x, c.z), c.x, c.y)", 0.5f, 0.5f, 1.3f, 0.0f, "mad", "fxc: log2, mul, exp2"},
    {"sin", "mad(sin(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"cos", "mad(cos(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    // Context effects (sopt's rdna3 model assumes them; compare with the plain forms above).
    // Free scales (owner, 2026-10-13; replaces the omod tests over rcp, whose multiply hid under the rcp's issue rate
    // on NVIDIA and Intel: the x3 control measured 0 there). The scaled value is a plain x * x, so nothing slow hides
    // the multiply, and max with x keeps the driver from pulling the constant out (max(k*a, c) = k*max(a, c/k)).
    // x settles at 0.586 (k >= 2: x -> x*x/4 + 0.5) or 0.667 (k <= 0.5: max = x, x -> x/4 + 0.5).
    {"sqmax", "mad(max(x * x, x), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "mad", "base for the scale tests"},
    {"scale2", "mad(max(x * x * 2.0, x), c.x, c.y)", 0.125f, 0.5f, 0.0f, 0.0f, "sqmax", "fxc writes * 2 as x + x (here dp2): what D3D gets"},
    {"scalehalf", "mad(max(x * x * 0.5, x), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "sqmax", "AMD: output modifier, ~0"},
    {"scale4", "mad(max(x * x * 4.0, x), c.x, c.y)", 0.0625f, 0.5f, 0.0f, 0.0f, "sqmax", "AMD: output modifier, ~0"},
    {"scale8", "mad(max(x * x * 8.0, x), c.x, c.y)", 0.03125f, 0.5f, 0.0f, 0.0f, "sqmax", "DX9 _x8; NVIDIA FMUL .M8?"},
    {"scale0.25", "mad(max(x * x * 0.25, x), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "sqmax", "DX9 _d4; NVIDIA FMUL .D4?"},
    {"scale0.125", "mad(max(x * x * 0.125, x), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "sqmax", "DX9 _d8"},
    {"scale3", "mad(max(x * x * 3.0, x), c.x, c.y)", 0.25f / 3.0f, 0.5f, 0.0f, 0.0f, "sqmax", "control: a full multiply"},
    {"mulxx", "mad(x * x, c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "mad", "a multiply no driver can merge"},
    // Where free scales end (owner, 2026-10-13: confirm, then take out what is settled). Mesa's nouveau: NVIDIA's
    // FMUL scales its result by 2^-3 .. 2^3 (post-factor), only a multiply's. AMD's omod: 2, 4, 0.5 on most VALU ops.
    // Beyond the range (x16, /16), a negative scale, and the scale after an fma and after an add, each with an x3
    // control. x settles at 0.586 / 0.667 / 1.0 as above (cx keeps the scaled step at x*x/4 + 0.5 or x/4 + 0.5).
    {"scale16", "mad(max(x * x * 16.0, x), c.x, c.y)", 0.015625f, 0.5f, 0.0f, 0.0f, "sqmax", "beyond x8"},
    {"scale0.0625", "mad(max(x * x * 0.0625, x), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "sqmax", "beyond /8"},
    {"scaleneg4", "mad(max(x * x * -4.0, x), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "sqmax", "a negative scale"},
    {"scaleneg0.5", "mad(max(x * x * -0.5, x), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "sqmax", "a negative scale"},
    {"scaleneg8", "mad(max(x * x * -8.0, x), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "sqmax", "a negative scale"},
    {"fmamax", "mad(max(mad(x, x, x), x), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "mad", "base for the fma scale tests"},
    {"fmascale4", "mad(max(mad(x, x, x) * 4.0, x), c.x, c.y)", 0.0625f, 0.5f, 0.0f, 0.0f, "fmamax", "AMD: omod on v_fma"},
    {"fmascale0.25", "mad(max(mad(x, x, x) * 0.25, x), c.x, c.y)", 0.25f, 0.5f, 0.0f, 0.0f, "fmamax", "not an AMD scale"},
    {"fmascale3", "mad(max(mad(x, x, x) * 3.0, x), c.x, c.y)", 0.25f / 3.0f, 0.5f, 0.0f, 0.0f, "fmamax", "control"},
    {"addmax", "mad(max(x + abs(x), x), c.x, c.y)", 0.125f, 0.5f, 0.0f, 0.0f, "mad", "base for the add scale tests"},
    {"addscale4", "mad(max((x + abs(x)) * 4.0, x), c.x, c.y)", 0.03125f, 0.5f, 0.0f, 0.0f, "addmax", "AMD: omod on v_add"},
    {"addscale3", "mad(max((x + abs(x)) * 3.0, x), c.x, c.y)", 0.125f / 3.0f, 0.5f, 0.0f, 0.0f, "addmax", "control"},
    {"max3", "mad(max(max(x, c.z), c.w), c.x, c.y)", 0.5f, 0.5f, 0.8f, 0.9f, "max", "AMD: v_max3, ~0"},
    {"minmax", "mad(min(max(x, c.z), c.w), c.x, c.y)", 0.5f, 0.5f, 0.8f, 1.2f, "max", "AMD: v_minmax / v_med3, ~0"},
    {"contract", "mad(x * c.z + c.w, c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.1f, "mad", "mul + add: one fma like mad2?"},
    {"satmad", "mad(saturate(mad(x, c.z, c.w)), c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.1f, "mad2", "saturate as output modifier, ~0"},
    // Fast forms of expensive ops (sign: no int -> float conversion; round: two precise adds).
    {"signsat", "mad(saturate((x - c.z) * 1e38) - saturate((x - c.z) * -1e38), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub",
     "exact sign: mul_sat + add"},
    {"signmad", "mad(mad(saturate(mad(x - c.z, 1e38, 0.5)), 2.0, -1.0), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub",
     "exact sign: mad_sat + mad"},
    {"signclamp", "mad(clamp((x - c.z) * 1e38, -1.0, 1.0), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub", "exact sign: mul, max, min"},
    {"signsel", "mad((x - c.z) > 0.0 ? 1.0 : ((x - c.z) < 0.0 ? -1.0 : 0.0), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub",
     "exact sign: lt, lt, and, movc"},
    {"signsel2", "mad((x - c.z) >= 0.0 ? 1.0 : -1.0, c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub", "not sign (1 at 0): ge, movc"},
    {"roundadd", "mad(roundAdd(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "round: (x + 1.5 * 2^23) - 1.5 * 2^23, precise"},
    {"flooradd", "mad(floorAdd(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "floor: r - saturate((r - x) * 1e38), precise"},
    {"fracadd", "mad(fracAdd(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "frac: d + saturate(d * -1e38), d = x - r, precise"},
    // Vector chains (version 3): dot / cross / length / normalize are no hardware instructions on
    // scalar GPUs; these show whether they cost their expansions. Bases: 2-4 fmas.
    {"mad2v", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "2 fmas (float2)", "float2"},
    {"mad3v", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "3 fmas (float3)", "float3"},
    {"mad4v", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "4 fmas (float4)", "float4"},
    {"dot2", "mad(x.yx, dot(x, c.zw), c.y)", 0.5f, 0.5f, 0.2f, 0.2f, "mad2v", "dot2: mul + fma", "float2"},
    {"dot3", "mad(x.yzx, dot(x, c.zwz), c.y)", 0.5f, 0.5f, 0.13f, 0.13f, "mad3v", "dot3: mul + 2 fma", "float3"},
    {"dot4", "mad(x.yzwx, dot(x, c.zwzw), c.y)", 0.5f, 0.5f, 0.1f, 0.1f, "mad4v", "dot4: mul + 3 fma", "float4"},
    {"cross", "mad(cross(x, c.zwy), c.x, c.y)", 0.5f, 1.0f, 0.8f, 0.6f, "mad3v", "cross: 3 mul + 3 fma", "float3"},
    {"normalize", "mad(normalize(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad3v", "dot3, rsqrt, 3 mul", "float3"},
    {"length", "mad(x.yzx, c.x, c.y) - length(x) * c.z", 0.5f, 1.0f, 0.3f, 0.0f, "mad3v", "dot3, sqrt, plus one fma",
     "float3"},
    {"distance", "mad(x.yzx, c.x, c.y) - distance(x, c.zwz) * c.z", 0.5f, 1.0f, 0.3f, 0.2f, "mad3v",
     "3 sub, dot3, sqrt, plus one fma", "float3"},
    {"reflect", "mad(reflect(x, c.zwz), c.x, c.y)", 0.5f, 1.0f, 0.6f, 0.8f, "mad3v", "i - 2 * dot(i, n) * n", "float3"},
    {"refract", "mad(refract(x, c.zwz, c.z), c.x, c.y)", 0.5f, 1.0f, 0.6f, 0.8f, "mad3v", "dot, sqrt, select ...", "float3"},
    {"faceforward", "mad(faceforward(x, c.zwz, x.yzx), c.x, c.y)", 0.5f, 1.0f, 0.6f, 0.8f, "mad3v", "dot, compare, select",
     "float3"},
    {"matmul4", "mad(mul(float4x4(c, c.yzwx, c.zwxy, c.wxyz), x), 0.25, c.y)", 0.5f, 0.5f, 0.1f, 0.1f, "mad4v",
     "float4x4 * float4: 4 dot4", "float4"},
    {"transpose", "mad(mul(transpose(float4x4(c, c.yzwx, c.zwxy, c.wxyz)), x), 0.25, c.y)", 0.5f, 0.5f, 0.1f, 0.1f,
     "matmul4", "the same with transpose: free?", "float4"},
    {"det3", "mad(x, c.x, determinant(float3x3(x, c.zwz, c.wzw)) * c.w + c.y)", 0.5f, 0.5f, 0.2f, 0.3f, "mad3v",
     "float3x3 determinant", "float3"},
    // Intrinsics fxc writes out as instruction sequences (sopt has no op for most of them yet).
    {"fmod", "mad(fmod(x, c.z), c.x, c.y)", 0.5f, 1.0f, 0.7f, 0.0f, "mad", "fxc: div, frac, mul, select"},
    {"smoothstep", "mad(smoothstep(c.z, c.w, x), c.x, c.y)", 0.5f, 1.0f, 0.5f, 2.5f, "mad", "fxc: add, mul_sat, mad, 2 mul"},
    {"atan", "mad(atan(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "fxc: polynomial"},
    {"atan2", "mad(atan2(x, c.z), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "mad", "fxc: polynomial + quadrant fixes"},
    {"asin", "mad(asin(x * c.z), c.x, c.y)", 0.5f, 1.0f, 0.3f, 0.0f, "mul", "fxc: polynomial + sqrt"},
    {"acos", "mad(acos(x * c.z), c.x, c.y)", 0.5f, 1.0f, 0.3f, 0.0f, "mul", "fxc: polynomial + sqrt"},
    {"acos4", "mad(acosFast4(x * c.z), c.x, c.y)", 0.5f, 1.0f, 0.3f, 0.0f, "mul",
     "ShaderFastMathLib acosFast4 (fxc's own acos uses the same polynomial)"},
    {"atan4", "mad(atanFast4(x * c.z), c.x, c.y)", 0.5f, 1.0f, 0.3f, 0.0f, "mul",
     "ShaderFastMathLib atanFast4: |x| <= 1 only, 1.5e-3 rad off"},
    // Sebastien Lagarde's minimax forms (2014): acos degree 1, atan odd degree 5 "alternate" (pi/4 + p((x-1)/(x+1))),
    // the first-quadrant atan2 (x, y > 0 only).
    {"acos1", "mad(acosP1(x * c.z), c.x, c.y)", 0.5f, 1.0f, 0.3f, 0.0f, "mul", "Lagarde acos degree 1 (6.1e-3 rad off)"},
    {"atan5a", "mad(atanOP5A(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "Lagarde atan odd degree 5 alternate (1.2e-3 rad)"},
    {"atan2q", "mad(atan2Q(x, c.z), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "mad",
     "Lagarde atan2, first quadrant only (1.2e-3 rad)"},
    {"tan", "mad(tan(x * c.z), c.x, c.y)", 0.5f, 1.0f, 0.4f, 0.0f, "mul", "fxc: sincos + div"},
    {"sincos", "mad(sin(x) + cos(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "add", "sin and cos of one value"},
    // More intrinsics (version 5): the rest of ReShade FX's math.
    {"cosh", "mad(cosh(x), c.x, c.y)", 0.25f, 0.0f, 0.0f, 0.0f, "mad", "fxc: 2 exp"},
    {"sinh", "mad(sinh(x), c.x, c.y)", 0.3f, 0.5f, 0.0f, 0.0f, "mad", "fxc: 2 exp"},
    {"tanh", "mad(tanh(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "fxc: exp, div"},
    {"log10", "mad(log10(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "fxc: log2 + mul"},
    {"radians", "mad(radians(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "a mul by a constant (also degrees); ~0 where merged"},
    {"ldexp", "mad(ldexp(x, x - c.z), c.x, c.y)", 0.5f, 0.3f, 2.0f, 0.0f, "sub", "x * exp2(e): exp2 + mul"},
    {"frexp", "mad(frexpM(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "mantissa + exponent * 0.01"},
    {"modf", "mad(modfS(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "fraction + integer part * 0.5"},
    {"isnan", "mad(isnan(x) ? c.z : x, c.x, c.y)", 0.5f, 0.5f, 1.0f, 0.0f, "mad", "ne + movc (a compiler may drop it)"},
    {"isinf", "mad(isinf(x) ? c.z : x, c.x, c.y)", 0.5f, 0.5f, 1.0f, 0.0f, "mad", "abs, eq + movc"},
    // Not in ReShade FX, in HLSL or GLSL (owner, 2026-10-10: tools/reshade/INTRINSICS.md; sopt reads HLSL and GLSL too).
    {"isfinite", "mad(isfinite(x) ? x : c.z, c.x, c.y)", 0.5f, 0.5f, 1.0f, 0.0f, "mad", "HLSL isfinite + select"},
    {"mod", "mad(x - c.z * floor(x / c.z), c.x, c.y)", 0.5f, 1.0f, 0.7f, 0.0f, "mad", "GLSL mod (floor-based): div, floor, fma"},
    {"asinh", "mad(log(x + sqrt(mad(x, x, 1.0))), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "GLSL asinh: log(x + sqrt(x x + 1))"},
    {"acosh", "mad(log(x + sqrt(mad(x, x, -1.0))), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "GLSL acosh: log(x + sqrt(x x - 1))"},
    {"atanh", "mad(0.5 * log((1.0 + x * c.z) / (1.0 - x * c.z)), c.x, c.y)", 0.5f, 1.0f, 0.25f, 0.0f, "mad",
     "GLSL atanh: 0.5 log((1 + x) / (1 - x))"},
    // any() of three values: compares joined by ||, or the zero-comparison trick's sum / max of absolute values
    // (library/rewrites.txt; max3 with abs is one instruction on AMD).
    {"any3or", "mad((x - c.z != 0.0 || x - c.w != 0.0 || x * c.z - c.w != 0.0) ? x : c.z, c.x, c.y)", 0.5f, 0.5f, 1.1f,
     0.9f, "mad", "3 subs, 3 compares, 2 ors, select"},
    {"all3and", "mad((x - c.z != 0.0 && x - c.w != 0.0 && x * c.z - c.w != 0.0) ? x : c.z, c.x, c.y)", 0.5f, 0.5f, 1.1f,
     0.9f, "mad", "all(): 3 subs, 3 compares, 2 ands, select"},
    {"any3abs", "mad(abs(x - c.z) + abs(x - c.w) + abs(x * c.z - c.w) != 0.0 ? x : c.z, c.x, c.y)", 0.5f, 0.5f, 1.1f,
     0.9f, "mad", "3 subs, 2 adds of abs, 1 compare, select"},
    {"any3max", "mad(max(max(abs(x - c.z), abs(x - c.w)), abs(x * c.z - c.w)) != 0.0 ? x : c.z, c.x, c.y)", 0.5f, 0.5f,
     1.1f, 0.9f, "mad", "3 subs, max3 of abs, 1 compare, select"},
    // any() of four (owner, 2026-10-11): the trick on two pairs joined by || (NVIDIA can fold the || into a compare),
    // or the trick on a max3 of the others.
    {"any4or", "mad((x - c.z != 0.0 || x - c.w != 0.0 || x * c.z - c.w != 0.0 || x * c.w - c.z != 0.0) ? x : c.z, c.x, c.y)",
     0.5f, 0.5f, 1.1f, 0.9f, "mad", "4 subs, 4 compares, 3 ors, select"},
    {"any4pairs", "mad((abs(x - c.z) != -abs(x - c.w) || abs(x * c.z - c.w) != -abs(x * c.w - c.z)) ? x : c.z, c.x, c.y)",
     0.5f, 0.5f, 1.1f, 0.9f, "mad", "4 subs, 2 pair compares, 1 or, select"},
    {"any4max3", "mad(max(max(abs(x - c.z), abs(x - c.w)), abs(x * c.z - c.w)) != -abs(x * c.w - c.z) ? x : c.z, c.x, c.y)",
     0.5f, 0.5f, 1.1f, 0.9f, "mad", "4 subs, max3 of abs, 1 compare, select"},
    {"any4abs", "mad(abs(x - c.z) + abs(x - c.w) + (abs(x * c.z - c.w) + abs(x * c.w - c.z)) != 0.0 ? x : c.z, c.x, c.y)",
     0.5f, 0.5f, 1.1f, 0.9f, "mad", "4 subs, 3 adds of abs, 1 compare, select"},
    {"f16round", "mad(f16tof32(f32tof16(x)), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "f32tof16 + f16tof32"},
    // Each half alone (owner, 2026-10-13: test everything), over bitor (one integer op on the float's bits).
    {"f32tof16", "mad(asfloat(f32tof16(x) + 0x3f800000u), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "bitor", "f32tof16 + iadd"},
    {"f16tof32", "mad(f16tof32(asuint(x) & 0x3bffu), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "bitor", "iand + f16tof32 (finite halves)"},
    {"degrees", "mad(degrees(x), c.x, c.y)", 0.5f / 57.29578f, 0.5f, 0.0f, 0.0f, "mad", "a mul by a constant, like radians"},
    {"bitcast", "mad(asfloat(asint(x)), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "asint / asfloat: free"},
    // Integer and bit operations (uint chains: x = (x OP ...) ^ c.y) + c.x over the base int, xor + add) and conversions
    // between int and float. Until 0.7.4 the base was ixmul, (x ^ c.y) * c.x: an integer multiply is quarter rate on GCN
    // and several XMADs on Maxwell / Pascal, so cheaper ops hid under it (owner, 2026-10-15; build.py skips those readings).
    {"ixmul", "(x ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "mad", "xor + imul (the integer base until 0.7.4)", "uint"},
    {"iadd", "((x + asuint(c.z)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "", "uint"},
    {"imul", "((x * asuint(c.z)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "", "uint"},
    {"iand", "((x & asuint(c.z)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "NVIDIA: LOP3 can merge it with the xor",
     "uint"},
    {"imin", "(min(x, asuint(c.z)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "umin", "uint"},
    {"ishr", "((x ^ (x >> 13)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "ushr + xor", "uint"},
    {"irot", "(((x << 7) | (x >> 25)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int",
     "rotate: shl, shr, or (or one funnel shift / alignbit)", "uint"},
    {"popc", "((x + countbits(x)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "iadd", "countbits", "uint"},
    {"fbh", "((x + firstbithigh(x)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "iadd", "firstbithigh", "uint"},
    {"bitrev", "(reversebits(x) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "reversebits", "uint"},
    {"fbl", "((x + firstbitlow(x)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "iadd", "firstbitlow", "uint"},
    {"ishl", "((x ^ (x << 11)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "shl + xor", "uint"},
    {"ineg", "(asuint(-asint(x)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "integer negate", "uint"},
    {"iabs", "(asuint(abs(asint(x))) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "integer abs", "uint"},
    {"bfe", "(((x >> 5) & 0xffu) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "shr + and: one bitfield extract?", "uint"},
    {"xor3", "((x ^ (x >> 7) ^ (x << 11)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "2 shifts + 2 xors: v_xor3, LOP3", "uint"},
    {"or3", "((x | (x >> 7) | (x << 11)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "2 shifts + 2 ors: v_or3, LOP3", "uint"},
    {"andor", "(((x & (x >> 7)) | (x << 11)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "2 shifts, and, or: v_and_or, LOP3", "uint"},
    {"bitsel", "(((x & (x >> 7)) | (~x & (x << 11))) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int",
     "bit select (and, not, and, or): v_bfi, LOP3", "uint"},
    {"maj3", "((((x & (x >> 7)) | (x & (x << 11))) | ((x >> 7) & (x << 11))) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0,
     "int", "majority (3 ands, 2 ors): LOP3", "uint"},
    {"iadd3", "((x + (x >> 7) + (x << 11)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "2 shifts + 2 adds: IADD3, v_add3", "uint"},
    {"lshladd", "(((x << 3) + (x >> 7)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "shl + add: LEA, v_lshl_add", "uint"},
    // fxc writes (x << 2) + x as imul x, 5 (Emil Persson, "Low-level Shader Optimization for Next-Gen and DX11",
    // slides 53-55): an integer multiply is quarter rate on AMD, shl + add one v_lshl_add (see shladd5-od).
    {"shladd5", "(((x << 2) + x) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "fxc: imul x, 5", "uint"},
    {"addlshl", "(((x + (x >> 7)) << 3) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "add + shl: v_add_lshl", "uint"},
    {"imad", "((x * asuint(c.z) + (x >> 7)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "imul + add: IMAD", "uint"},
    {"icmpsel", "((x > asuint(c.z) ? x : ~x) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "ult, not, movc", "uint"},
    {"udiv", "((x / ((asuint(c.z) >> 24) | 1u)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int",
     "unsigned divide (no instruction on most GPUs), plus shr, or", "uint"},
    {"umod", "((x % ((asuint(c.z) >> 24) | 1u)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "unsigned modulo", "uint"},
    {"idiv", "(asuint(asint(x) / (asint(asuint(c.z) >> 24) | 1)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int",
     "signed divide", "uint"},
    {"imod", "(asuint(asint(x) % (asint(asuint(c.z) >> 24) | 1)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int",
     "signed modulo", "uint"},
    {"itof", "(asuint(float(asint(x))) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "int -> float conversion", "uint"},
    {"utof", "(asuint(float(x)) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int", "uint -> float conversion", "uint"},
    {"unitf", "(asuint(asfloat((x >> 9) | 0x3f800000u) * 1.5) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "int",
     "uint -> [1, 2) by bits: ushr, or, plus a mul", "uint"},
    {"ftou", "(uint(asfloat((x >> 9) | 0x3f800000u) * 4194304.0) ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "unitf",
     "float -> uint conversion", "uint"},
    // The | 1 keeps the int: fxc writes float(int(v)) alone as one round_z (truncation).
    {"ftoitof", "mad(float(int(x * c.z) | 1), c.x, c.y)", 0.0005f, 0.5f, 1000.0f, 0.0f, "mul", "ftoi, or, itof"},
    {"bitor", "mad(asfloat(asuint(x) | 1u), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "one int op on a float value"},
    {"signbits", "mad(asfloat((asuint(x - c.z) & 0x80000000u) | 0x3f800000u), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub",
     "+-1 from the sign bit (1 at +0, like signsel2): and, iadd"},
    // Half precision: min16float (drivers may run it at 32 bits; see the CSV header).
    {"mad16", "mad(x, (min16float)c.x, (min16float)c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "fp16 fma", "min16float"},
    {"add16", "mad(x + (min16float)c.z, (min16float)c.x, (min16float)c.y)", 0.5f, 0.5f, 0.1f, 0.0f, "mad16", "", "min16float"},
    {"mul16", "mad(x * (min16float)c.z, (min16float)c.x, (min16float)c.y)", 0.5f, 0.5f, 0.9f, 0.0f, "mad16", "",
     "min16float"},
    {"rcp16", "mad(rcp(x), (min16float)c.x, (min16float)c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad16", "", "min16float"},
    {"sqrt16", "mad(sqrt(x), (min16float)c.x, (min16float)c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad16", "", "min16float"},
    {"exp2_16", "mad(exp2(x), (min16float)c.x, (min16float)c.y)", 0.25f, 0.0f, 0.0f, 0.0f, "mad16", "", "min16float"},
    {"log2_16", "mad(log2(x), (min16float)c.x, (min16float)c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad16", "", "min16float"},
    {"max16", "mad(max(x, (min16float)c.z), (min16float)c.x, (min16float)c.y)", 0.5f, 0.5f, 0.8f, 0.0f, "mad16", "",
     "min16float"},
    // Where fp16 pays (owner, 2026-10-09): packed math pairs components (AMD: float3 = 2 instructions?), and every
    // switch between float and min16float may cost a conversion. A bare (float)(min16float)x is dropped by fxc, so the
    // conversions are measured around an fp16 fma (mix16; DXBC marks them "def32 as min16f" / "min16f as def32").
    {"mad16v3", "mad(x, (min16float)c.x, (min16float)c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad3v", "3 fp16 fmas (min16float3)",
     "min16float3"},
    {"mix16", "mad((float)mad((min16float)x, (min16float)c.z, (min16float)c.w), c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.1f, "mad",
     "an fp16 fma between fp32 fmas"},
    // Compute (moved from TexBench, owner 2026-10-04: ops in OpBench, texture work in TexBench). gi / ai index
    // groupshared memory / a local array from x and the thread's lane l, so the address differs per lane.
    {"gsbase", "uint gi = (l + uint(x * 64.0)) & 2047u; float v = asfloat((gi & 1023u) | 0x3f000000u); x = mad(x, c.x, c.y + v * 0.01);",
     0.5f, 0.25f, 0.7f, 0.3f, "mad", "base: groupshared index, a stand-in value", "float", kGroupshared},
    {"gsbase32", "uint gi = (l * 32u + uint(x * 64.0)) & 2047u; float v = asfloat((gi & 1023u) | 0x3f000000u); x = mad(x, c.x, c.y + v * 0.01);",
     0.5f, 0.25f, 0.7f, 0.3f, "mad", "base: index with stride 32", "float", kGroupshared},
    {"gsread", "uint gi = (l + uint(x * 64.0)) & 2047u; float v = GS[gi]; x = mad(x, c.x, c.y + v * 0.01);", 0.5f, 0.25f, 0.7f,
     0.3f, "gsbase", "groupshared read, neighbouring lanes in neighbouring words", "float", kGroupshared},
    {"gsread32", "uint gi = (l * 32u + uint(x * 64.0)) & 2047u; float v = GS[gi]; x = mad(x, c.x, c.y + v * 0.01);", 0.5f, 0.25f,
     0.7f, 0.3f, "gsbase32", "lanes 32 words apart: bank conflicts", "float", kGroupshared},
    {"gswrite", "uint gi = (l + uint(x * 64.0)) & 2047u; GS[gi] = x; float v = asfloat((gi & 1023u) | 0x3f000000u); x = mad(x, c.x, c.y + v * 0.01);",
     0.5f, 0.25f, 0.7f, 0.3f, "gsbase", "groupshared write", "float", kGroupshared},
    {"gswrite32", "uint gi = (l * 32u + uint(x * 64.0)) & 2047u; GS[gi] = x; float v = asfloat((gi & 1023u) | 0x3f000000u); x = mad(x, c.x, c.y + v * 0.01);",
     0.5f, 0.25f, 0.7f, 0.3f, "gsbase32", "bank conflicts", "float", kGroupshared},
    {"gswriteread", "uint gi = (l + uint(x * 64.0)) & 2047u; GS[gi] = x; float v = GS[gi ^ 1u]; x = mad(x, c.x, c.y + v * 0.01);",
     0.5f, 0.25f, 0.7f, 0.3f, "gsbase", "write, then read the neighbour's word", "float", kGroupshared},
    {"barrier", "uint gi = (l + uint(x * 64.0)) & 2047u; GS[gi] = x; GroupMemoryBarrierWithGroupSync(); float v = asfloat((gi & 1023u) | 0x3f000000u); x = mad(x, c.x, c.y + v * 0.01);",
     0.5f, 0.25f, 0.7f, 0.3f, "gswrite", "barrier() after a groupshared write", "float", kGroupshared},
    {"groupbarrier", "uint gi = (l + uint(x * 64.0)) & 2047u; GS[gi] = x; GroupMemoryBarrier(); float v = asfloat((gi & 1023u) | 0x3f000000u); x = mad(x, c.x, c.y + v * 0.01);",
     0.5f, 0.25f, 0.7f, 0.3f, "gswrite", "groupMemoryBarrier() after a write", "float", kGroupshared},
    {"membarrier", "uint gi = (l + uint(x * 64.0)) & 2047u; GS[gi] = x; AllMemoryBarrier(); float v = asfloat((gi & 1023u) | 0x3f000000u); x = mad(x, c.x, c.y + v * 0.01);",
     0.5f, 0.25f, 0.7f, 0.3f, "gswrite", "memoryBarrier() after a write", "float", kGroupshared},
    // Atomics on groupshared memory: each thread its own address, and "1": the group's 64 threads on one.
    {"atombase", "uint o = asuint(x) & 255u; x = mad(x, c.x, c.y + float(o & 255u) * 1e-6);", 0.5f, 0.25f, 0.7f, 0.3f, "mad",
     "base: the atomic's stand-in", "float", kGroupshared},
#define SOPT_ATOMIC(N, CALL, CALL1, NOTE)                                                                                 \
  {N, "uint o; " CALL " x = mad(x, c.x, c.y + float(o & 255u) * 1e-6);", 0.5f, 0.25f, 0.7f, 0.3f, "atombase", NOTE, "float",  \
   kGroupshared},                                                                                                         \
  {N "1", "uint o; " CALL1 " x = mad(x, c.x, c.y + float(o & 255u) * 1e-6);", 0.5f, 0.25f, 0.7f, 0.3f, "atombase",          \
   NOTE ", 64 threads on one address", "float", kGroupshared},
    SOPT_ATOMIC("aAdd", "InterlockedAdd(GSI[l], asuint(x) & 255u, o);", "InterlockedAdd(GSI[0], asuint(x) & 255u, o);", "atomicAdd")
    SOPT_ATOMIC("aAnd", "InterlockedAnd(GSI[l], asuint(x) & 255u, o);", "InterlockedAnd(GSI[0], asuint(x) & 255u, o);", "atomicAnd")
    SOPT_ATOMIC("aOr", "InterlockedOr(GSI[l], asuint(x) & 255u, o);", "InterlockedOr(GSI[0], asuint(x) & 255u, o);", "atomicOr")
    SOPT_ATOMIC("aXor", "InterlockedXor(GSI[l], asuint(x) & 255u, o);", "InterlockedXor(GSI[0], asuint(x) & 255u, o);", "atomicXor")
    SOPT_ATOMIC("aMin", "InterlockedMin(GSI[l], asuint(x) & 255u, o);", "InterlockedMin(GSI[0], asuint(x) & 255u, o);", "atomicMin")
    SOPT_ATOMIC("aMax", "InterlockedMax(GSI[l], asuint(x) & 255u, o);", "InterlockedMax(GSI[0], asuint(x) & 255u, o);", "atomicMax")
    SOPT_ATOMIC("aXchg", "InterlockedExchange(GSI[l], asuint(x) & 255u, o);", "InterlockedExchange(GSI[0], asuint(x) & 255u, o);",
                "atomicExchange")
    SOPT_ATOMIC("aCmpXchg", "InterlockedCompareExchange(GSI[l], asuint(x) & 255u, 7u, o);",
                "InterlockedCompareExchange(GSI[0], asuint(x) & 255u, 7u, o);", "atomicCompareExchange")
#undef SOPT_ATOMIC
    // Local arrays indexed at run time (fxc: indexable temps), a constant array (an immediate constant
    // buffer), and branches: 4 sin / 4 cos per side (short sides become selects), uniform within a group
    // (from the group) or divergent (from the lane), against both sides computed and one picked.
    {"arraybase", "uint ai = (uint(x * 64.0) + l) & 15u; float v = asfloat(ai | 0x3f000000u); x = mad(x, c.x, c.y + v * 0.01);",
     0.5f, 0.25f, 0.7f, 0.3f, "mad", "base: array index, a stand-in value", "float", kLocalArray},
    {"arrayread", "uint ai = (uint(x * 64.0) + l) & 15u; float v = A[ai]; x = mad(x, c.x, c.y + v * 0.01);", 0.5f, 0.25f, 0.7f,
     0.3f, "arraybase", "float A[16][i], i differs per lane", "float", kLocalArray},
    {"arraywrite", "uint ai = (uint(x * 64.0) + l) & 15u; A[ai] = x; float v = A[ai ^ 1u]; x = mad(x, c.x, c.y + v * 0.01);",
     0.5f, 0.25f, 0.7f, 0.3f, "arraybase", "A[i] = x, then a read", "float", kLocalArray},
    {"constarray", "uint ai = (uint(x * 64.0) + l) & 15u; float v = K[ai]; x = mad(x, c.x, c.y + v * 0.01);", 0.5f, 0.25f, 0.7f,
     0.3f, "arraybase", "static const float K[16][i]", "float", kLocalArray},
    {"selectboth", "bool sel = ((asuint(c.w) ^ g) & 1u) != 0; float v = sel ? sin(sin(sin(sin(x)))) : cos(cos(cos(cos(x)))); x = mad(v, c.x, c.y);",
     0.5f, 0.25f, 0.7f, 0.3f, "mad", "4 sin and 4 cos computed, one result picked"},
    {"branchuni", "bool sel = ((asuint(c.w) ^ g) & 1u) != 0; float v; [branch] if (sel) v = sin(sin(sin(sin(x)))); else v = cos(cos(cos(cos(x)))); x = mad(v, c.x, c.y);",
     0.5f, 0.25f, 0.7f, 0.3f, "mad", "a branch, one side per group"},
    {"branchdiv", "bool sel = ((asuint(c.w) ^ l) & 1u) != 0; float v; [branch] if (sel) v = sin(sin(sin(sin(x)))); else v = cos(cos(cos(cos(x)))); x = mad(v, c.x, c.y);",
     0.5f, 0.25f, 0.7f, 0.3f, "mad", "a branch, both sides in every group"},
    // Parallel issue (owner, 2026-10-04): can the GPU run an fma and another kind of instruction at the same
    // time (VLIW slots, Turing's integer pipe beside the float pipe, a second FP32 pipe, dual issue)? Each
    // "fma+X" test runs 4 chains of mad and 4 chains of X; "X" alone is measured too. If the pair takes less
    // than the two one after the other, they overlap.
    {"int", "(x ^ asuint(c.y)) + asuint(c.x)", 0, 0, 0, 0, "mad", "integer xor + add, alone", "uint"},
    {"rcp1", "rcp(x + c.x)", 0.5f, 0.5f, 1.2f, 0.8f, "mad", "add + rcp, alone"},
    {"minmax1", "max(min(x, c.z), c.w)", 0.5f, 0.5f, 1.2f, 0.8f, "mad", "min + max, alone"},
    {"cvt1", "asuint(float(x) * 0.7)", 0, 0, 0, 0, "mad", "uint -> float + mul, alone", "uint"},
    {"half1", "mad(x, (min16float)c.x, (min16float)c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "fp16 fma, alone", "min16float"},
    {"fma+fma", "mad(x, c.x, c.y)", 0.5f, 0.5f, 1.2f, 0.8f, "mad", "control: the same fma on both halves", "float", 0,
     "mad(x, c.x, c.y)", "float", "mad"},
    {"fma+int", "mad(x, c.x, c.y)", 0.5f, 0.5f, 1.2f, 0.8f, "int", "fma beside integer ops", "float", 0,
     "(x ^ asuint(c.y)) + asuint(c.x)", "uint", "int"},
    {"fma+rcp", "mad(x, c.x, c.y)", 0.5f, 0.5f, 1.2f, 0.8f, "rcp1", "fma beside the transcendental unit", "float", 0,
     "rcp(x + c.x)", "float", "rcp1"},
    {"fma+minmax", "mad(x, c.x, c.y)", 0.5f, 0.5f, 1.2f, 0.8f, "minmax1", "fma beside min / max", "float", 0,
     "max(min(x, c.z), c.w)", "float", "minmax1"},
    {"fma+cvt", "mad(x, c.x, c.y)", 0.5f, 0.5f, 1.2f, 0.8f, "cvt1", "fma beside conversions", "float", 0,
     "asuint(float(x) * 0.7)", "uint", "cvt1"},
    {"fma+half", "mad(x, c.x, c.y)", 0.5f, 0.5f, 1.2f, 0.8f, "half1", "fp32 fma beside fp16 fma", "float", 0,
     "mad(x, (min16float)c.x, (min16float)c.y)", "min16float", "half1"},
    // VLIW slots: 4 fma chains beside 1 rcp chain fill a VLIW5 bundle (x, y, z, w + the t unit).
    {"fma4+rcp", "mad(x, c.x, c.y)", 0.5f, 0.5f, 1.2f, 0.8f, "rcp1", "4 fmas beside 1 rcp (VLIW5: one bundle)", "float", 0,
     "rcp(x + c.x)", "float", "rcp1", 5, 1},
    // How many fmas at once: the same fma with fewer independent chains per thread (mad itself has 8).
    {"ilp1", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "1 chain per thread", "float", 0, nullptr, "float", nullptr, 1},
    {"ilp2", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "2 chains per thread", "float", 0, nullptr, "float", nullptr, 2},
    {"ilp3", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "3 chains per thread", "float", 0, nullptr, "float", nullptr, 3},
    {"ilp4", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "4 chains per thread", "float", 0, nullptr, "float", nullptr, 4},
    {"ilp5", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "5 chains per thread", "float", 0, nullptr, "float", nullptr, 5},
    {"ilp6", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "6 chains per thread", "float", 0, nullptr, "float", nullptr, 6},
    // Pixel shaders only (StageBench): differences to the neighbour pixel of the 2 x 2 quad, and clip.
    {"ddx", "mad(x + ddx(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "add", "ddx (coarse or fine, the driver's choice)", "float", kPixelOnly},
    {"ddy", "mad(x + ddy(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "add", "ddy", "float", kPixelOnly},
    {"ddx_coarse", "mad(x + ddx_coarse(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "add", "one value per quad", "float", kPixelOnly},
    {"ddy_coarse", "mad(x + ddy_coarse(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "add", "one value per quad", "float", kPixelOnly},
    {"ddx_fine", "mad(x + ddx_fine(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "add", "per row of the quad", "float", kPixelOnly},
    {"ddy_fine", "mad(x + ddy_fine(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "add", "per column of the quad", "float", kPixelOnly},
    {"fwidth", "mad(x + fwidth(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "add", "abs(ddx) + abs(ddy)", "float", kPixelOnly},
    {"clip", "clip(x + 2.0); x = mad(x, c.x, c.y);", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "clip that never discards", "float", kPixelOnly},
};

// fxc's rewrites (owner, 2026-10-16: data for a separate fxc fix-up project, docs/notes/fxc-fixup-brief.md): these
// tests also run as "<name>-od", the same step compiled without fxc's optimizer, so the driver sees the step as
// written. fxc writes x * 2 as add x, x / dp2 (the GTX 1660 then loses its free x2), sign as lt, lt, iadd, itof, clamp
// as max + min, and expands lerp / smoothstep / fmod / atan2 its own way. A twin's base is the base's twin when the
// base is in the list too.
const char* const kOdPairs[] = {"sqmax", "scale2", "scale4", "sign", "clamp", "lerp", "smoothstep", "fmod", "atan2",
                                 "shladd5", "irot"};

// kTests and the -od twins.
const std::vector<Test>& allTests() {
  static const std::vector<Test> all = [] {
    static std::vector<std::string> names;  // the twins' names and bases (Test holds const char*)
    names.reserve(2 * std::size(kOdPairs));
    std::vector<Test> v(std::begin(kTests), std::end(kTests));
    for (const char* n : kOdPairs)
      for (const Test& t : kTests)
        if (std::strcmp(t.name, n) == 0) {
          Test u = t;
          u.name = names.emplace_back(std::string(n) + "-od").c_str();
          if (t.base && std::find_if(std::begin(kOdPairs), std::end(kOdPairs), [&](const char* b) { return std::strcmp(b, t.base) == 0; }) != std::end(kOdPairs))
            u.base = names.emplace_back(std::string(t.base) + "-od").c_str();
          u.note = "fxc /Od: the step as written";
          u.od = true;
          v.push_back(u);
        }
    return v;
  }();
  return all;
}

constexpr int kUnroll = 16;          // steps per loop iteration, each with its own constants
constexpr UINT kGroupSize = 64;
constexpr UINT kGroupsFull = 16384;  // 1M threads

struct Config {
  const char* name;
  int chains;
  UINT groups;
  const char* title;  // console heading
  const char* what;   // what it shows
};
Config kConfigs[] = {
    {"tput", 8, kGroupsFull, "Cost, many in parallel",
     "how many the GPU finishes per second (what sopt's cost models use)"},
#ifndef SOPT_STAGEBENCH
    {"dep", 1, kGroupsFull, "Cost, one dependent chain",
     "each step waits for the one before; other threads hide the wait"},
    {"lat", 1, 1, "Latency, one at a time", "how long one step takes, with nothing to hide the wait"},
#endif
};

std::string shaderSource(const Test& t, int chains) {
  std::string s =
      "cbuffer C : register(b0) { float4 U[16]; uint iters; float seed; float2 pad; };\n"
      "RWStructuredBuffer<float> O : register(u0);\n"
      // precise keeps fxc from folding (v + c) - c to v
      "float roundAdd(float v) { precise float t = v + 12582912.0; precise float r = t - 12582912.0; return r; }\n"
      "float floorAdd(float v) { precise float r = (v + 12582912.0) - 12582912.0; precise float f = r - saturate((r - v) * 1e38); return f; }\n"
      "float frexpM(float v) { float e; float m = frexp(v, e); return m + e * 0.01; }\n"
      "float modfS(float v) { float i; float f = modf(v, i); return f + i * 0.5; }\n"
      "float rsqrtNR1(float v) { float g = asfloat(0x5F375A86u - (asuint(v) >> 1)); return g * (1.5 - 0.5 * v * g * g); }\n"
      "float rcpNR1(float v) { float g = asfloat(0x7EF311C3u - asuint(v)); return g * (2.0 - v * g); }\n"
      "float acosFast4(float v) { float x1 = abs(v); float x2 = x1 * x1; float x3 = x2 * x1;\n"
      "  float s = -0.2121144 * x1 + 1.5707288; s = 0.0742610 * x2 + s; s = -0.0187293 * x3 + s;\n"
      "  s = sqrt(1.0 - x1) * s; return v >= 0.0 ? s : 3.14159265 - s; }\n"
      "float atanFast4(float v) { return v * (-0.1784 * abs(v) - 0.0663 * v * v + 1.0301); }\n"
      "float acosP1(float v) { float x = abs(v); float r = -0.155972 * x + 1.56467; r *= sqrt(1.0 - x); return v >= 0.0 ? r : 3.141593 - r; }\n"
      "float atanOP5A(float v) { float a = abs(v); float t = (a - 1.0) / (a + 1.0); float t1 = t * t;\n"
      "  float r = 0.785398 + ((0.0892423 * t1 - 0.301029) * t1 + 0.998422) * t; return v < 0.0 ? -r : r; }\n"
      "float atan2Q(float y, float x) { float t = (y - x) / (y + x); float t1 = t * t;\n"
      "  return 0.785398 + ((0.0892423 * t1 - 0.301029) * t1 + 0.998422) * t; }\n"
      "float fracAdd(float v) { precise float d = v - ((v + 12582912.0) - 12582912.0); precise float f = d + saturate(d * -1e38); return f; }\n"
      "static const float K[16] = {0.51, 0.52, 0.53, 0.54, 0.55, 0.56, 0.57, 0.58, 0.59, 0.60, 0.61, 0.62, 0.63, 0.64, 0.65, 0.66};\n";
  if (t.setup & kGroupshared) s += "groupshared float GS[2048];\ngroupshared uint GSI[64];\n";
  if (gStage == Stage::Pixel)  // one thread per pixel of the 1024-wide target
    s += "float4 main(float4 pos : SV_Position) : SV_Target\n{\n"
         "  const uint3 id = uint3(uint(pos.x) + uint(pos.y) * 1024u, 0u, 0u);\n";
  else if (gStage == Stage::Vertex)  // one thread per point
    s += "float4 main(uint vid : SV_VertexID) : SV_Position\n{\n"
         "  const uint3 id = uint3(vid, 0u, 0u);\n";
  else
    s += "[numthreads(64, 1, 1)]\n"
         "void main(uint3 id : SV_DispatchThreadID)\n{\n";
  s += "  const uint g = id.x / 64u, l = id.x % 64u;\n";
  if (t.setup & kGroupshared)
    s += "  for (uint k = l; k < 2048u; k += 64u) GS[k] = 0.5 + float(k) * 1e-4 + seed;\n"
         "  GSI[l] = l;\n  GroupMemoryBarrierWithGroupSync();\n";
  if (t.setup & kLocalArray) s += "  float A[16];\n  [unroll] for (uint k = 0; k < 16u; ++k) A[k] = 0.5 + float(k) * 0.01 + seed;\n";
  // The type and step of chain k (pair tests: the odd chains run the pair step, or the last pairCount of them).
  auto paired = [&](int k) { return t.pairStep && (t.pairCount && chains > t.pairCount ? k >= chains - t.pairCount : (k & 1)); };
  auto typeOf = [&](int k) { return std::string(paired(k) ? t.pairType : t.type); };
  auto stepOf = [&](int k) { return std::string(paired(k) ? t.pairStep : t.step); };
  for (int k = 0; k < chains; ++k) {
    const std::string type = typeOf(k);
    const std::string ks = std::to_string(k);
    std::string init;
    if (type == "uint") init = "id.x * 2654435761u + " + ks + "u * 40503u + 1u";
    else if (type == "float") init = "1.0 + frac(id.x * 0.000123 + " + ks + " * 0.137) * seed";
    else if (type == "min16float") init = "(min16float)(1.0 + frac(id.x * 0.000123 + " + ks + " * 0.137) * seed)";
    else {  // float2..4: different start values per component
      const std::string sw = std::string("xyzw").substr(0, size_t(type.back() - '0'));
      init = "1.0 + frac(id.x * 0.000123 + " + ks + " * 0.137 + float4(0.0, 0.31, 0.53, 0.71)." + sw + ") * seed";
    }
    s += "  " + type + " x" + ks + " = " + init + ";\n";
  }
  s += "  [loop] for (uint i = 0; i < iters; ++i)\n  {\n";
  for (int r = 0; r < kUnroll; ++r) {
    // k: a literal of this step (K[r] folds to a constant; distinct per step, so no two steps merge).
    s += "    {\n      const float4 c = U[" + std::to_string(r) + "];\n      const float k = K[" + std::to_string(r) + "];\n";
    for (int k = 0; k < chains; ++k) {
      const std::string x = "x" + std::to_string(k);
      std::string step = stepOf(k);
      // Replace the chain variable x (a lone identifier) with xk.
      std::string out;
      for (size_t p = 0; p < step.size(); ++p) {
        const bool lone = step[p] == 'x' && (p == 0 || !(isalnum((unsigned char)step[p - 1]) || step[p - 1] == '_' || step[p - 1] == '.')) &&
                          (p + 1 == step.size() || !(isalnum((unsigned char)step[p + 1]) || step[p + 1] == '_'));
        out += lone ? x : std::string(1, step[p]);
      }
      if (out.find(';') != std::string::npos) s += "      { " + out + " }\n";  // statements that assign x
      else s += "      " + x + " = " + out + ";\n";
    }
    s += "    }\n";
  }
  s += "  }\n";
  // The chains' sum: written to the buffer, returned as the pixel, or as an off-screen point's position (x = 4 + |sum|;
  // 4 + 1e-30 * sum let fxc fold bounded sums, the integer chains' float(x & 1023u), into a constant: StageBench 0.7.8).
  s += gStage == Stage::Pixel ? "  return float4(0.0" : gStage == Stage::Vertex ? "  return float4(4.0 + abs(0.0" : "  O[id.x] = 0.0";
  for (int k = 0; k < chains; ++k) {
    const std::string x = "x" + std::to_string(k);
    const std::string type = typeOf(k);
    if (type == "float") s += " + " + x;
    else if (type == "uint") s += " + float(" + x + " & 1023u)";
    else if (type == "min16float") s += " + (float)" + x;
    else s += " + dot(" + x + ", 1.0)";
  }
  if (t.setup & kGroupshared) s += " + GS[(l * 7u) & 2047u] + float(GSI[l] & 1u)";  // keeps the writes
  s += gStage == Stage::Pixel ? ", 0.0, 0.0, 1.0);\n}\n" : gStage == Stage::Vertex ? "), 4.0, 0.5, 1.0);\n}\n" : ";\n}\n";
  return s;
}

struct Gpu {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* out = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;   // StageBench's pixel run: 1024 x 1024 R32_FLOAT
  ID3D11VertexShader* fullVS = nullptr;    // its full-screen triangle
  Timer timer;
};

// Binds a test's shader for the current stage (the map keeps every stage's shader as a device child).
void bindShader(Gpu& g, ID3D11DeviceChild* sh) {
  if (gStage == Stage::Compute) {
    g.ctx->CSSetShader(static_cast<ID3D11ComputeShader*>(sh), nullptr, 0);
  } else if (gStage == Stage::Pixel) {
    g.ctx->VSSetShader(g.fullVS, nullptr, 0);
    g.ctx->PSSetShader(static_cast<ID3D11PixelShader*>(sh), nullptr, 0);
  } else {
    g.ctx->VSSetShader(static_cast<ID3D11VertexShader*>(sh), nullptr, 0);
    g.ctx->PSSetShader(nullptr, nullptr, 0);
  }
}

struct CbData {
  float U[16][4];
  UINT iters;
  float seed;
  float pad[2];
};

void setConstants(Gpu& g, const Test& t, UINT iters) {
  CbData d = {};
  const bool bits = std::strcmp(t.type, "uint") == 0;
  for (int r = 0; r < 16; ++r) {
    if (bits) {  // random 32-bit patterns, read with asuint; c.x odd (a multiplier that loses no bits)
      for (int i = 0; i < 4; ++i) {
        uint32_t h = uint32_t(r * 4 + i + 1) * 2654435761u;
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        if (i == 0) h |= 1u;
        std::memcpy(&d.U[r][i], &h, 4);
      }
      continue;
    }
    // Distinct constants per step (within 0.1%), so no two steps can be merged.
    const float e = 1.0f + 0.0001f * float(r);
    d.U[r][0] = t.cx * e;
    d.U[r][1] = t.cy * e;
    d.U[r][2] = t.cz * e;
    d.U[r][3] = t.cw * e;
  }
  d.iters = iters;
  d.seed = 0.1f;
  g.ctx->UpdateSubresource(g.cb, 0, nullptr, &d, 0, 0);
}

// One timed dispatch in milliseconds (negative if the timestamps were disjoint).
double timeDispatch(Gpu& g, UINT groups) {
  const UINT threads = groups * 64u;
  if (gStage == Stage::Pixel) {  // the same threads as pixels: 1024 wide (fewer: one row)
    D3D11_VIEWPORT vp = {0.0f, 0.0f, float(std::min(threads, 1024u)), float(std::max(1u, threads / 1024u)), 0.0f, 1.0f};
    g.ctx->RSSetViewports(1, &vp);
    return g.timer.time(g.ctx, [&] { g.ctx->Draw(3, 0); });
  }
  if (gStage == Stage::Vertex) return g.timer.time(g.ctx, [&] { g.ctx->Draw(threads, 0); });
  return g.timer.time(g.ctx, [&] { g.ctx->Dispatch(groups, 1, 1); });
}

struct Result {
  UINT iters = 0;
  double ms = 0.0;
  double nsPerStep = 0.0;  // per step of one chain in one thread (tput: of all chains together / chains)
};

// Chains per thread of a test in a configuration (a test's own count applies to the throughput run).
int chainsOf(const Test& t, const Config& c) { return t.chains && std::strcmp(c.name, "tput") == 0 ? t.chains : c.chains; }

// Doubles the iteration count until a dispatch takes >= 2 ms.
UINT calibrate(Gpu& g, ID3D11DeviceChild* cs, const Test& t, const Config& c) {
  bindShader(g, cs);
  UINT iters = 2;
  for (;;) {
    setConstants(g, t, iters);
    const double ms = timeDispatch(g, c.groups);  // < 0: no valid reading, keep the run length
    if (ms < 0.0 || ms >= 2.0 || iters >= (1u << 20)) return iters;
    iters *= 2;
  }
}

// The median of reps runs at a known iteration count.
Result measureAt(Gpu& g, ID3D11DeviceChild* cs, const Test& t, const Config& c, UINT iters, int reps) {
  bindShader(g, cs);
  setConstants(g, t, iters);
  std::vector<double> runs;
  for (int k = 0; k < reps * 3 && int(runs.size()) < reps; ++k)
    if (const double m = timeDispatch(g, c.groups); m > 0.0) runs.push_back(m);
  Result r;
  r.iters = iters;
  r.ms = median(runs);
  const double steps = double(iters) * kUnroll * double(c.groups) * kGroupSize * chainsOf(t, c);
  r.nsPerStep = r.ms * 1e6 / steps;
  return r;
}

// One test in one configuration: both passes, each relative to the mad measured just before it.
// Version 4: tests whose passes disagree are measured again until most readings agree (like
// redundant sensors: two show that one is wrong, three or more which one).
struct Measured {
  Result r;                      // averaged over all readings
  std::vector<double> readings;  // 4 * time / the reference mad's time, one per pass
  Consensus units;               // of the readings
  double vsBase = 0.0;           // units minus the base test's units (the reported cost)
  double vsBasePass[2] = {0, 0}; // the same from the forward and the backward pass alone
};

// The summary's fixed order: sections ("#" entries), within each cheapest to most expensive as
// most GPUs measure it, the same on every GPU so results can be compared line by line.
const char* const kDisplayOrder[] = {
    // One section for the few, fast basics (owner, 2026-10-10: like the web page's group).
    "#Modifiers and basic arithmetic", "neg", "abs", "negabs", "saturate", "satmad", "mul", "mulxx", "sqmax", "scale2", "scalehalf",
    "scale4", "scale8", "scale0.25", "scale0.125", "scale3", "scale16", "scale0.0625", "scaleneg4", "scaleneg0.5", "scaleneg8",
    "fmamax", "fmascale4", "fmascale0.25", "fmascale3", "addmax", "addscale4", "addscale3", "min", "max", "step", "add", "sub", "fma1", "madsatu", "madlit", "madlit1", "madinl", "mad2", "contract", "max3", "minmax", "clamp", "select",
    "lerp",
    "#Rounding and sign", "floor", "ceil", "round", "trunc", "frac", "roundadd", "flooradd", "fracadd", "signsel2", "signbits",
    "signsat", "signmad", "signclamp", "signsel", "sign",
    "#Division and transcendentals", "divxy", "rcp", "rsqrt", "sqrt", "div", "exp2", "log2", "log", "exp", "cos", "sin",
    "pow",
    "#Fast approximations (ShaderFastMathLib, bit tricks)", "rsqrtnr0", "rsqrtnr1", "rcpnr0", "rcpnr1", "sqrtnr0",
    "pow2floor", "exp2floor", "pow2ceil", "exp2ceil", "acos4", "acos1", "atan4", "atan5a", "atan2q",
    "#Vector (float2 / float3 / float4)", "mad2v", "mad3v", "mad4v", "dot2", "dot3", "dot4", "cross", "length",
    "distance", "normalize", "reflect", "refract", "faceforward", "det3", "matmul4", "transpose",
    "#Written out by fxc", "smoothstep", "fmod", "sincos", "tan", "atan", "atan2", "asin", "acos",
    "#More intrinsics", "bitcast", "radians", "log10", "isnan", "isinf", "isfinite", "modf", "frexp", "ldexp", "f16round", "f32tof16", "f16tof32", "degrees",
    "tanh", "sinh", "cosh", "mod", "asinh", "acosh", "atanh", "any3or", "all3and", "any3abs", "any3max", "any4or",
    "any4pairs", "any4max3", "any4abs",
    "#Integer and conversions", "bitor", "ixmul", "iadd", "iand", "imin", "ishr", "irot", "imul", "popc", "fbh",
    "bitrev", "fbl", "ishl", "ineg", "iabs", "bfe", "xor3", "or3", "andor", "bitsel", "maj3", "iadd3",
    "lshladd", "addlshl", "imad", "icmpsel", "unitf", "utof", "itof", "ftou", "ftoitof", "udiv", "umod", "idiv", "imod",
    "#Half precision (min16float)", "mad16", "add16", "mul16", "max16", "rcp16", "sqrt16", "exp2_16", "log2_16",
    "mad16v3", "mix16",
    "#Compute: groupshared memory and barriers", "gsread", "gswrite", "gswriteread", "gsread32", "gswrite32", "barrier",
    "groupbarrier", "membarrier",
    "#Compute: groupshared atomics (aAdd = atomicAdd ...; 1 = 64 threads on one address)", "aAdd", "aAnd", "aOr", "aXor",
    "aMin", "aMax", "aXchg", "aCmpXchg", "aAdd1", "aAnd1", "aOr1", "aXor1", "aMin1", "aMax1",
    "aXchg1", "aCmpXchg1",
    "#Compute: local arrays and branches", "arrayread", "arraywrite", "constarray", "selectboth", "branchuni", "branchdiv",
    "#Parallel issue: an fma and X together (Cost = both; % = of the two one after the other)", "fma+fma", "fma+int",
    "fma+minmax", "fma+cvt", "fma+rcp", "fma+half", "fma4+rcp",
    "#Parallel issue: how many fmas at once (Cost per fma with 1 to 6 independent chains per thread; the same as 8 = scalar)",
    "ilp1", "ilp2", "ilp3", "ilp4", "ilp5", "ilp6",
    "#fxc's rewrites: as fxc -O3 writes the step, then as written (-od: compiled without fxc's optimizer)",
    "sqmax", "sqmax-od", "scale2", "scale2-od", "scale4", "scale4-od", "sign", "sign-od", "clamp", "clamp-od", "lerp",
    "lerp-od", "smoothstep", "smoothstep-od", "fmod", "fmod-od", "atan2", "atan2-od", "shladd5", "shladd5-od", "irot",
    "irot-od",
    "#Pixel shaders only", "ddx", "ddy", "ddx_coarse", "ddy_coarse", "ddx_fine", "ddy_fine", "fwidth", "clip"};

// What other cards of this card's family measure (expected.hpp, from the reports in docs/opbench; owner,
// 2026-10-06: a card that differs from its family makes an interesting report).
struct FamilyCosts {
  const expected::Family* family = nullptr;
  std::map<std::string, double> cost;  // test -> throughput cost
};

FamilyCosts familyOf(const std::string& gpuName, unsigned deviceId) {
  FamilyCosts fc;
  std::string name = std::regex_replace(gpuName, std::regex(R"(\((R|TM)\))"), "");
  name = std::regex_replace(name, std::regex(R"(\s+)"), " ");
  const char* model = nullptr;
  for (const expected::Device& d : expected::kDevices)
    if (d.id == deviceId) model = d.model;
  for (const expected::Rule& r : expected::kRules)
    if (!model && std::regex_search(name, std::regex(r.pattern))) model = r.model;
  if (!model) return fc;
  for (const expected::Family& f : expected::kFamilies)
    if (std::strcmp(f.model, model) == 0 && std::strcmp(f.stage, stageName()) == 0) fc.family = &f;
  if (!fc.family) return fc;
  std::istringstream in(fc.family->costs);
  std::string test;
  double v;
  while (in >> test >> v) fc.cost[test] = v;
  return fc;
}

// Off by more than 25% (and more than 1.5 units: small costs are noisy).
bool differs(double measured, double expected) {
  return std::fabs(measured - expected) > std::max(0.25 * std::fabs(expected), 1.5);
}

}  // namespace

int main(int argc, char** argv) {
  gProgram = kTool;
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // progress shows while it runs
  const Style st = initConsole();
  int adapterIndex = -1;
  bool list = false;
  std::string filter, outPath;
  [[maybe_unused]] bool stageGiven = false;
  int reps = 5;  // runs per reading (median); readings agree within ~0.1% on clean runs
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) fail("missing value for " + a);
      return argv[++i];
    };
    if (a == "--adapter") adapterIndex = std::atoi(next());
    else if (a == "--list") list = true;
    else if (a == "--adapters") {  // hardware GPUs, each once (for GPU-Blueprint.bat, every card)
      printUniqueAdapters();
      return 0;
    }
    else if (a == "--filter") filter = next();
    else if (a == "--reps") reps = std::max(1, std::atoi(next()));
    else if (a == "--out") outPath = next();
    else if (a == "--groups") {  // thread groups of 64 for tput / dep (default 16384; small for a quick check)
      const UINT n = UINT(std::max(1, std::min(int(kGroupsFull), std::atoi(next()))));
      for (Config& c : kConfigs)
        if (std::strcmp(c.name, "lat") != 0) c.groups = n;
#ifdef SOPT_STAGEBENCH
    } else if (a == "--stage") {
      const std::string v = next();
      if (v != "ps" && v != "vs") fail("--stage takes ps or vs");
      gStage = v == "ps" ? Stage::Pixel : Stage::Vertex;
      stageGiven = true;
#endif
    } else {
      print("%s %s\nusage: %s [--adapter N] [--list] [--adapters] [--filter text] [--reps N] [--out file.csv] [--groups N]%s\n",
            kTool, SOPT_VERSION, kTool, gStage == Stage::Compute ? "" : " [--stage ps|vs]");
      return a == "-h" || a == "--help" ? 0 : 1;
    }
  }

#ifdef SOPT_STAGEBENCH
  // Without --stage: the pixel run, then the vertex run, each a run of its own (own CSV, own summary).
  if (!stageGiven && !list) {
    std::vector<std::string> args(argv, argv + argc);
    for (const char* stage : {"ps", "vs"}) {
      std::string cmd = "\"\"" + args[0] + "\"";
      for (size_t k = 1; k < args.size(); ++k) cmd += " \"" + args[k] + "\"";
      cmd += std::string(" --stage ") + stage + "\"";
      if (const int rc = std::system(cmd.c_str()); rc != 0) return rc;
    }
    return 0;
  }
#endif
  if (!list) {
    printBox(st, std::string(kTool) + " " + SOPT_VERSION + "  -  by CeeJay.dk" +
                     (gStage == Stage::Pixel ? "  -  pixel shaders" : gStage == Stage::Vertex ? "  -  vertex shaders" : ""));
    print("\n");
  }
  const Adapter ad = selectAdapter(st, list, adapterIndex);
  if (list) return 0;
  const DXGI_ADAPTER_DESC1& desc = ad.desc;
  const std::string& gpuName = ad.name;
  const std::string& driver = ad.driver;
  const FamilyCosts fam = familyOf(gpuName, desc.DeviceId);

  Gpu g;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(ad.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &g.dev,
                               nullptr, &g.ctx)))
    fail("D3D11CreateDevice failed");
  // Whether the driver runs min16float at 16 bits in compute shaders (else at 32: the half tests
  // then measure fp32).
  bool half16 = false;
  {
    D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT mp = {};
    if (SUCCEEDED(g.dev->CheckFeatureSupport(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, &mp, sizeof(mp))))
      half16 = ((gStage == Stage::Pixel ? mp.PixelShaderMinPrecision : mp.AllOtherShaderStagesMinPrecision) &
                D3D11_SHADER_MIN_PRECISION_16_BIT) != 0;
  }
  {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CbData);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.cb))) fail("cannot create the constant buffer");
    D3D11_BUFFER_DESC ob = {};
    ob.ByteWidth = kGroupsFull * kGroupSize * 4;
    ob.Usage = D3D11_USAGE_DEFAULT;
    ob.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ob.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ob.StructureByteStride = 4;
    if (FAILED(g.dev->CreateBuffer(&ob, nullptr, &g.out))) fail("cannot create the output buffer");
    if (FAILED(g.dev->CreateUnorderedAccessView(g.out, nullptr, &g.uav))) fail("cannot create the output view");
    g.timer.create(g.dev);
    g.ctx->CSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &g.uav, nullptr);
    if (gStage != Stage::Compute) {
      g.ctx->VSSetConstantBuffers(0, 1, &g.cb);
      g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
      g.ctx->IASetPrimitiveTopology(gStage == Stage::Pixel ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    }
    if (gStage == Stage::Pixel) {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = td.Height = 1024;
      td.MipLevels = td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R32_FLOAT;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      ID3D11Texture2D* rt = nullptr;
      if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &rt)) || FAILED(g.dev->CreateRenderTargetView(rt, nullptr, &g.rtv)))
        fail("cannot create the render target");
      g.ctx->OMSetRenderTargets(1, &g.rtv, nullptr);
      const char* vs = "float4 main(uint v : SV_VertexID) : SV_Position\n"
                       "{ float2 p = float2((v << 1) & 2, v & 2); return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }\n";
      ID3DBlob* code = nullptr;
      if (FAILED(D3DCompile(vs, std::strlen(vs), "fullscreen", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, nullptr)) ||
          FAILED(g.dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.fullVS)))
        fail("cannot create the full-screen vertex shader");
      code->Release();
    }
  }

  const std::filesystem::path reports = reportsDir();
  std::string safeName = gpuName;
  for (char& ch : safeName)
    if (!isalnum((unsigned char)ch)) ch = '_';
  std::string prefix = kTool;
  for (char& ch : prefix) ch = char(std::tolower((unsigned char)ch));
  if (gStage != Stage::Compute) prefix += std::string("-") + stageName();
  if (outPath.empty()) outPath = (reports / (prefix + "-" + safeName + ".csv")).string();
  startReport(outPath, kTool, gStage == Stage::Compute ? "" : stageName(), ad);
  const std::filesystem::path dxbcDir =
      reports / "Shaders" / (std::string(kTool) + (gStage == Stage::Compute ? "" : std::string("-") + stageName()));
  std::filesystem::create_directories(dxbcDir);

  // Compile every test for every configuration (chains differ).
  std::vector<const Test*> tests;
  for (const Test& t : allTests())
    if (runsHere(t) && (filter.empty() || std::string(t.name).find(filter) != std::string::npos || !t.base ||
                        std::strcmp(t.name, "mad") == 0))
      tests.push_back(&t);
  // Bases of the selected tests are needed too.
  for (bool added = true; added;) {
    added = false;
    for (const Test* t : std::vector<const Test*>(tests))
      if (t->base && std::none_of(tests.begin(), tests.end(), [&](const Test* u) { return std::strcmp(u->name, t->base) == 0; }))
        for (const Test& u : allTests())
          if (std::strcmp(u.name, t->base) == 0) {
            tests.push_back(&u);
            added = true;
          }
  }

  // Shaders compile in the background, section by section in measuring order (owner, 2026-10-05: results
  // start sooner); each section waits for its own. The map is filled with every key first, so the worker only
  // writes values.
  std::map<std::string, std::map<std::string, ID3D11DeviceChild*>> shaders;
  for (const Config& c : kConfigs)
    for (const Test* t : tests) shaders[c.name][t->name] = nullptr;
  auto compileTest = [&](const Test* t) {
    for (const Config& c : kConfigs) {
      const std::string src = shaderSource(*t, chainsOf(*t, c));
      ID3DBlob* code = nullptr;
      ID3DBlob* err = nullptr;
      const char* profile = gStage == Stage::Pixel ? "ps_5_0" : gStage == Stage::Vertex ? "vs_5_0" : "cs_5_0";
      if (FAILED(D3DCompile(src.data(), src.size(), t->name, nullptr, nullptr, "main", profile,
                            t->od ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        fail(std::string("cannot compile test ") + t->name + ":\n" + (err ? (const char*)err->GetBufferPointer() : ""));
      if (std::strcmp(c.name, "tput") == 0) {  // disassembly of the throughput version
        ID3DBlob* dis = nullptr;
        if (SUCCEEDED(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &dis))) {
          if (FILE* f = std::fopen((dxbcDir / (std::string(t->name) + ".txt")).string().c_str(), "wb")) {
            std::fwrite(src.data(), 1, src.size(), f);
            std::fputs(t->od ? "\n// ---- DXBC (D3DCompile /Od) ----\n" : "\n// ---- DXBC (D3DCompile -O3) ----\n", f);
            std::fwrite(dis->GetBufferPointer(), 1, dis->GetBufferSize() - 1, f);
            std::fclose(f);
          }
          dis->Release();
        }
      }
      ID3D11DeviceChild* cs = nullptr;
      HRESULT hr;
      if (gStage == Stage::Pixel)
        hr = g.dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, reinterpret_cast<ID3D11PixelShader**>(&cs));
      else if (gStage == Stage::Vertex)
        hr = g.dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, reinterpret_cast<ID3D11VertexShader**>(&cs));
      else
        hr = g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, reinterpret_cast<ID3D11ComputeShader**>(&cs));
      if (FAILED(hr)) fail(std::string("cannot create test ") + t->name);
      code->Release();
      shaders[c.name][t->name] = cs;
    }
  };

  const Test* madTest = nullptr;
  for (const Test* t : tests)
    if (std::strcmp(t->name, "mad") == 0) madTest = t;
  // Warm up (clocks ramp up): two seconds of the reference test.
  // Sections in display order, each with the tests it shows plus the bases and solo tests they need
  // (measured with the first section that needs them); tests no section shows (bases only) go last.
  // Each section is shown as soon as it is measured (owner: users can read while the rest runs).
  struct Group {
    const char* title = nullptr;
    std::vector<const char*> shown;
    std::vector<const Test*> run;
  };
  auto find = [&](const char* name) -> const Test* {
    for (const Test* t : tests)
      if (name && std::strcmp(t->name, name) == 0) return t;
    return nullptr;
  };
  std::vector<Group> groups(1);
  std::vector<const Test*> assigned;
  auto assign = [&](Group& gr, const Test* t, auto& self) -> void {
    if (!t || std::find(assigned.begin(), assigned.end(), t) != assigned.end()) return;
    assigned.push_back(t);
    self(gr, find(t->base), self);
    if (t->pairStep) self(gr, find(t->solo), self);
    gr.run.push_back(t);
  };
  assign(groups[0], madTest, assign);
  for (const char* name : kDisplayOrder) {
    if (name[0] == '#') {
      if (groups.back().title || !groups.back().shown.empty()) groups.emplace_back();
      groups.back().title = name + 1;
    } else if (const Test* t = find(name)) {
      groups.back().shown.push_back(name);
      assign(groups.back(), t, assign);
    }
  }
  groups.emplace_back();
  for (const Test* t : tests) assign(groups.back(), t, assign);

  // Job 0 is the reference alone, so the warm-up starts as soon as it is compiled and the first section
  // compiles during the warm-up (owner, 2026-10-05: a small first batch).
  std::vector<std::function<void()>> jobList;
  jobList.push_back([&compileTest, madTest] { compileTest(madTest); });
  for (const Group& gr : groups)
    jobList.push_back([&compileTest, &gr, madTest] {
      for (const Test* t : gr.run)
        if (t != madTest) compileTest(t);
    });
  print("\n");
  BackgroundJobs compiling(std::move(jobList));
  compiling.wait(0);  // the reference, for the warm-up

  print(std::size(kConfigs) == 1 ? "%zu tests:\n" : "%zu tests, each measured in three ways:\n", tests.size());
  for (const Config& c : kConfigs)
    print("  %s%-26s%s %s\n", st.c("\x1b[1;96m"), c.title, st.reset(), c.what);
  print("Each is measured twice (forward, then backward through its section), more often when the two\n"
              "readings disagree. Lower is better; the CSV has every reading. Results appear section by\n"
              "section while the rest is measured.\n"
              "Warming up the GPU for 2 seconds so its clock settles ...");
  {
    gCurrent = "the 2-second warm-up (mad)";  // where a stop happened (the report's "# stopped:" line)
    bindShader(g, shaders["tput"]["mad"]);
    setConstants(g, *madTest, 256);
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 2000) timeDispatch(g, kConfigs[0].groups);
  }
  print(" done\n");

  // Section tables: throughput costs, graphs on one fixed scale (sections appear before the largest
  // cost is known): 100 = 25 mads, longer costs fill the graph.
  const int cols = consoleColumns();  // the graph shrinks in a narrow window so lines do not wrap
  constexpr double kGraphMax = 100.0;
  int nameW = 10;  // the longest name, so the graphs line up
  for (const char* name : kDisplayOrder)
    if (name[0] != '#') nameW = std::max(nameW, int(std::strlen(name)));
  const std::string graphIndent(size_t(2 + nameW + 1 + 6 + 2), ' ');  // where the graphs start
  // Fixed columns: indent, name, cost, Ops, comment ("expensive") and a note ("3 passes").
  const int kBarWidth = std::clamp(cols - 1 - (2 + nameW + 1 + 6 + 2 + 2 + 5 + 2 + 10 + 10), 10, 40);
  std::map<std::string, std::map<std::string, Measured>> results;  // config -> test -> result
  std::vector<std::string> unstable;
  struct Difference {
    std::string test;
    double measured, expected;
  };
  std::vector<Difference> different;  // tests where this card differs from its family
  int compared = 0;                   // tests compared with the family
  auto printSection = [&](const Group& gr) {
    print("\n  %s%s%s\n  %s%-*s %6s  %-*s  %5s  %s%s\n", st.c("\x1b[1;96m"), gr.title, st.reset(), st.c("\x1b[90m"),
                nameW, "Test", "Cost", kBarWidth, "Graph", "Ops", "Comment", st.reset());
    for (const char* name : gr.shown) {
      const Measured& x = results["tput"][name];
      const Test* self = find(name);
      // Parallel issue: Cost = the fmas and one X together (n fma chains beside m X chains: (n + m) / m chain
      // steps of the pair test); the comment compares it with them one after the other (4 per fma + X alone).
      // A chain count test (ilpN) shows its cost per fma.
      const bool pair = self->pairStep && results["tput"].count(self->solo);
      const int all = self->chains ? self->chains : 8, xs = self->pairCount ? self->pairCount : all / 2;
      const double together = pair ? double(all) / xs * x.units.value : 0.0;
      const double apart = pair ? 4.0 * (all - xs) / xs + results["tput"][self->solo].units.value : 0.0;
      const double v = pair ? together : self->chains ? x.units.value : x.vsBase;
      Shade shade;
      const char* comment = costComment(v, &shade);
      char pairComment[64];
      if (pair) {
        const double pct = apart > 0.0 ? 100.0 * together / apart : 100.0;
        std::snprintf(pairComment, sizeof(pairComment), "%3.0f%% of %.1f: %s", pct, apart,
                      pct < 85.0 ? "in parallel" : "one after the other");
        comment = pairComment;
        shade = pct < 85.0 ? kGreen : kWhite;
      }
      const std::string color = st.vt ? "\x1b[" + std::to_string(shade.bright) + "m" : "";
      const std::string b = bar(std::min(v, kGraphMax), kGraphMax, kBarWidth, st, shade);
      // A test (or its base) without a majority among its readings, or settled by extra passes.
      const Measured* base = self->base ? &results["tput"][self->base] : nullptr;
      const bool shaky = !x.units.ok || (base && !base->units.ok);
      if (shaky) unstable.push_back(name);
      std::string note;
      if (shaky) note = st.vt ? "  \x1b[93m! no consensus\x1b[0m" : "  ! no consensus";
      else if (x.readings.size() > 2)
        note = std::string("  ") + st.c("\x1b[90m") + std::to_string(x.readings.size()) + " passes" + st.reset();
      // The family's costs are over the base (expected.hpp, units_vs_base); chain tests show their whole cost, so
      // their base's cost is added before comparing (ilp1-6 read "4.3, usually 0.3" in 0.7.3).
      const auto e = fam.cost.find(name);
      const double usual = e != fam.cost.end() ? e->second + (v - x.vsBase) : 0.0;
      if (!shaky && !pair && e != fam.cost.end()) ++compared;
      if (!shaky && !pair && e != fam.cost.end() && differs(v, usual)) {
        different.push_back({name, v, usual});
        char buf[48];
        std::snprintf(buf, sizeof(buf), "  usually %.1f", usual);
        note += std::string(st.c("\x1b[95m")) + buf + st.reset();
      }
      const double shown = std::fabs(v) < 0.05 ? 0.0 : v;  // no "-0.0"
      const double ops = std::fabs(v / 4.0) < 0.05 ? 0.0 : v / 4.0;
      print("  %-*s %6.1f  %s  %5.1f  %s%s%s%s\n", nameW, name, shown, b.c_str(), ops, color.c_str(), comment,
                  st.reset(), note.c_str());
    }
    print("%s%s(shorter is better)%s\n", graphIndent.c_str(), st.c("\x1b[90m"), st.reset());
  };

  std::map<std::string, double> madDrift;                     // config -> spread of the reference
  std::map<std::string, double> madNs;                        // config -> mean reference time
  std::map<std::string, std::vector<double>> mads;            // config -> reference readings
  std::map<std::string, std::map<std::string, UINT>> iters;   // config -> test -> run length
  std::map<std::string, std::map<std::string, Result>> sums;  // config -> test -> summed readings
  // Score (owner: a number to show others, in the units GPU spec lists use): fp32 TFLOPS from the reference
  // fma or the one-constant fma1, whichever is faster, fp16 TFLOPS from mad16 (min16float; 0 when the driver runs it at 32 bits), special functions from
  // the rcp step's time (one rcp per step; the fma beside it runs in parallel where the GPU can).
  auto driftOf = [&](const char* cname) {
    const std::vector<double>& v = mads[cname];
    if (v.empty()) return 0.0;
    const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    return 100.0 * (*hi - *lo) / median(v);
  };
  struct Score {
    double fp32 = 0.0, fp16 = 0.0, special = 0.0;
  };
  auto score = [&] {
    Score sc;
    // fp32 from every reference reading; fp16 and rcp from their costs relative to the reference (units are
    // measured against the reference right before each reading, so a drifting clock cancels; their own raw
    // timings come from other moments of the run).
    double mean = 0.0;
    for (double v : mads["tput"]) mean += v / double(mads["tput"].size());
    if (mean > 0.0) sc.fp32 = 2.0 / mean / 1000.0;
    auto units = [&](const char* name) {
      return results["tput"].count(name) && !results["tput"][name].readings.empty() ? results["tput"][name].units.value : 0.0;
    };
    const double ref = sc.fp32;  // the reference's rate: fp16 and rcp are relative to it
    if (half16 && units("mad16") > 0.0) sc.fp16 = ref * 4.0 / units("mad16");
    if (units("rcp") > 0.0) sc.special = ref * 1000.0 / 2.0 * 4.0 / units("rcp");  // G fma/s * 4 / cost
    // fp32: the faster of the two fma forms (owner, 2026-10-05: GCN APUs showed a third of their rate, the
    // reference's second constant costing an instruction there).
    if (units("fma1") > 0.0) sc.fp32 = std::max(sc.fp32, ref * 4.0 / units("fma1"));
    return sc;
  };
  // CSV: the GPU once in header lines, then one row per configuration and test. Written again whenever a
  // section is shown (between measurements, the GPU idle) and at the end, so a run stopped early still
  // leaves its results.
  auto writeCsv = [&] {
  FILE* csv = std::fopen(outPath.c_str(), "wb");
  if (!csv) return;
  // StageBench's own header (the report intake files only OpBench / TexBench reports).
  std::fprintf(csv, "# %s %s\n", kTool, SOPT_VERSION);
  if (gStage != Stage::Compute) std::fprintf(csv, "# stage: %s\n", stageName());
  std::fprintf(csv, "# gpu: %s\n# vendor: 0x%04X\n# device: 0x%04X\n# driver: %s\n", gpuName.c_str(), desc.VendorId,
               desc.DeviceId, driver.c_str());
  std::fprintf(csv, "# min16float: %s\n", half16 ? "16-bit" : "32-bit (no 16-bit min precision reported)");
  for (const Config& c : kConfigs) std::fprintf(csv, "# reference drift %s: %.2f%%\n", c.name, driftOf(c.name));
  const Score sc = score();
  if (fam.family) {
    std::fprintf(csv, "# family: %s (%s, %d card%s)\n", fam.family->model, fam.family->label, fam.family->cards,
                 fam.family->cards == 1 ? "" : "s");
    std::fprintf(csv, "# differs from the family:");
    for (const Difference& d : different) std::fprintf(csv, " %s %.1f (usually %.1f);", d.test.c_str(), d.measured, d.expected);
    std::fprintf(csv, "\n");
  } else
    std::fprintf(csv, "# family: none yet\n");
  std::fprintf(csv, "# fp32: %.3f TFLOPS\n# fp16 (min16float): %.3f TFLOPS\n# special functions (rcp): %.1f Gops/s\n", sc.fp32,
               sc.fp16, sc.special);
  std::fprintf(csv,
               "config,test,base,iters,ms,ns_per_step,units,units_vs_base,vs_base_fwd,vs_base_bwd,passes,consensus,readings,"
               "step,note,expected\n");
  for (const Config& c : kConfigs)
    for (const Test* t : tests) {
      if (!results[c.name].count(t->name) || results[c.name][t->name].readings.empty()) continue;
      const Measured& x = results[c.name][t->name];
      std::string readings;
      for (double v : x.readings) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%.3f", readings.empty() ? "" : ";", v);
        readings += buf;
      }
      std::string expect;
      if (const auto e = fam.cost.find(t->name); e != fam.cost.end() && std::strcmp(c.name, "tput") == 0 && !t->pairStep) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", e->second);
        expect = buf;
      }
      std::fprintf(csv, "%s,%s,%s,%u,%.4f,%.6f,%.3f,%.3f,%.3f,%.3f,%zu,%s,%s,\"%s\",\"%s\",%s\n", c.name, t->name,
                   t->base ? t->base : "", x.r.iters, x.r.ms, x.r.nsPerStep, x.units.value, x.vsBase, x.vsBasePass[0],
                   x.vsBasePass[1], x.readings.size(), x.units.ok ? "yes" : "no", readings.c_str(), t->step,
                   t->note ? t->note : "", expect.c_str());
    }
  std::fclose(csv);
  };
  Progress progress{&st, 2 * static_cast<int>(tests.size() * std::size(kConfigs))};  // passes 1 and 2
  progress.start();
  for (size_t gi = 0; gi < groups.size(); ++gi) {
    const Group& gr = groups[gi];
    compiling.wait(gi + 1);
    for (const Config& c : kConfigs) {
      std::map<std::string, UINT>& it = iters[c.name];
      if (!it.count("mad")) it["mad"] = calibrate(g, shaders[c.name]["mad"], *madTest, c);
      for (const Test* t : gr.run)
        if (!it.count(t->name)) it[t->name] = calibrate(g, shaders[c.name][t->name], *t, c);
      // Passes 1 and 2 measure every test of the section (forward, then backward); later passes only the
      // tests whose readings have no majority yet, alternating the direction.
      for (int pass = 0; pass < kMaxPasses; ++pass) {
        std::vector<const Test*> order;
        for (const Test* t : gr.run)
          if (pass < 2 || !consensus(results[c.name][t->name].readings).ok) order.push_back(t);
        if (order.empty()) break;
        if (pass % 2 == 1) std::reverse(order.begin(), order.end());
        for (const Test* t : order) {
          gCurrent = std::string(t->name) + " (" + c.name + ")";
          // A fresh reference right before the test: a clock change moves both.
          const Result m = measureAt(g, shaders[c.name]["mad"], *madTest, c, it["mad"], reps);
          const Result r = t == madTest ? m : measureAt(g, shaders[c.name][t->name], *t, c, it[t->name], reps);
          mads[c.name].push_back(m.nsPerStep);
          results[c.name][t->name].readings.push_back(4.0 * r.nsPerStep / m.nsPerStep);
          Result& acc = sums[c.name][t->name];
          acc.iters = r.iters;
          acc.ms += r.ms;
          acc.nsPerStep += r.nsPerStep;
          progress.step();
        }
      }
      for (const Test* t : gr.run) {
        Measured& x = results[c.name][t->name];
        const double n = double(x.readings.size());
        x.r = sums[c.name][t->name];
        x.r.ms /= n;
        x.r.nsPerStep /= n;
        x.units = consensus(x.readings);
      }
      // Bases are measured in this section or an earlier one.
      for (const Test* t : gr.run) {
        Measured& x = results[c.name][t->name];
        const Measured* b = t->base ? &results[c.name][t->base] : nullptr;
        x.vsBase = b ? x.units.value - b->units.value : x.units.value;
        for (int pass = 0; pass < 2; ++pass)
          x.vsBasePass[pass] = b ? x.readings[size_t(pass)] - b->readings[size_t(pass)] : x.readings[size_t(pass)];
      }
    }
    if (!gr.shown.empty()) {
      progress.pause();
      printSection(gr);
      writeCsv();
      progress.resume();
    }
  }
  progress.pause();  // the tests are done: the bar is not needed any more (owner, 2026-10-12)
  print("\n   reference drift:");
  for (const Config& c : kConfigs) {
    const std::vector<double>& v = mads[c.name];
    const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    madDrift[c.name] = 100.0 * (*hi - *lo) / median(v);
    double mean = 0.0;
    for (double x : v) mean += x / double(v.size());
    madNs[c.name] = mean;
    print(" %s %.1f%%", c.name, madDrift[c.name]);
  }
  print("\n");

  writeCsv();


  // Summary: banner and the GPU (the sections are shown above).
  print("\n");
  printBox(st, std::string(kTool) + " " + SOPT_VERSION + "  -  " + gpuName);
  print("  driver %s, vendor 0x%04X, device 0x%04X, %.1f TFLOPS fp32 (measured)\n", driver.c_str(), desc.VendorId,
              desc.DeviceId, score().fp32);
  print("  min16float runs at %s\n", half16 ? "16 bits" : "32 bits on this driver (the half precision tests measure fp32)");
  // How to read the summary, for people who are not programmers (owner's wording review, 2026-10-04).
  print("\n  %sHow to read this%s\n"
              "  Cost    How long the operation takes, compared with the simplest thing a GPU does:\n"
              "          a multiply-add, which counts as 4. The rest of the test is already subtracted.\n"
              "  Ops     Operations: the cost counted in multiply-adds.\n"
              "          2.0 means \"takes as long as two multiply-adds\".\n"
              "  Graph   Longer bar = slower. Free operations have no bar; a full bar is 25 multiply-adds or more.\n"
              "  usually What other cards of this family measured, where this card differs by over 25%%.\n"
              "\n"
              "  The numbers show how fast the GPU is when it is fully busy (as in a game).\n"
              "  Docs\\OpBench.html (README.html next to this program) explains every test in plain words.\n",
              st.c("\x1b[1;96m"), st.reset());

  // The drift itself is printed at the very end (printDrift).
  double maxDrift = 0.0;
  for (const Config& c : kConfigs) maxDrift = std::max(maxDrift, madDrift[c.name]);
  bool warned = maxDrift > 50.0;
  if (!unstable.empty()) {
    print("\n  %sWarning:%s %zu test(s) without agreeing readings after %d passes:", st.c("\x1b[1;93m"), st.reset(),
          unstable.size(), kMaxPasses);
    for (const std::string& n : unstable) print(" %s", n.c_str());
    print("\n");
    warned = true;
  }
  if (!unstable.empty())
    print("  For steadier numbers: close other programs, plug in a laptop, set \"Prefer maximum performance\"\n"
                "  (NVIDIA) or lock the clocks, and run it again.\n");
  // Compared with the family (owner, 2026-10-06): differences make the report interesting.
  print("\n  %sCompared with other cards%s\n", st.c("\x1b[1;96m"), st.reset());
  if (!fam.family)
    print("  No reports from this GPU's family yet: this report is especially interesting, please send it.\n");
  else {
    const int n = fam.family->cards;
    print("  %s: %d card%s measured so far. ", fam.family->label, n, n == 1 ? "" : "s");
    if (different.empty())
      print("This card matches %s in all %d tests compared.\n", n == 1 ? "it" : "them", compared);
    else {
      print("This card differs in %s%zu of %d test%s%s (\"usually\" above):\n", st.c("\x1b[95m"),
                  different.size(), compared, compared == 1 ? "" : "s", st.reset());
      std::string line = "   ";
      for (const Difference& d : different) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), " %s %.1f (usually %.1f)", d.test.c_str(), d.measured, d.expected);
        if (line.size() + std::strlen(buf) > size_t(std::max(40, cols - 2))) {
          print("%s\n", line.c_str());
          line = "   ";
        }
        line += buf;
      }
      print("%s\n", line.c_str());
      if (warned) print("  The warnings above may explain some of them.\n");
      print("  Such reports are especially interesting: please send this one.\n");
    }
  }
  std::printf("\n  CSV:  %s\n  DXBC: %s\n", outPath.c_str(), dxbcDir.string().c_str());  // not in the shared results page
  if (const Score sc = score(); sc.fp32 > 0.0) {
    std::vector<std::pair<std::string, std::string>> more;
    more.push_back({"fp16 (min16float)", sc.fp16 > 0.0 ? threeDigits(sc.fp16) + " TFLOPS" : std::string("runs at fp32")});
    if (sc.special > 0.0) more.push_back({"Special functions (rcp)", threeDigits(sc.special) + " Gops/s"});
    printScore(st, std::string(kTool) + " score", gpuName, desc.VendorId, sc.fp32, "TFLOPS", "fp32, measured", more);
  }
  finishRun(st, maxDrift, outPath);
  gCurrent.clear();
  setTitle("done");
  return 0;
}
