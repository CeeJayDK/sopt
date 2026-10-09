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
};
enum { kGroupshared = 1, kLocalArray = 2 };

// Constants keep every chain finite and away from denormals (x stays roughly in [0.3, 3]).
const Test kTests[] = {
    {"mad", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, nullptr, "reference: one fma per step"},
    {"add", "mad(x + c.z, c.x, c.y)", 0.5f, 0.5f, 0.1f, 0.0f, "mad", ""},
    {"sub", "mad(c.z - x, c.x, c.y)", 0.5f, 1.0f, 2.0f, 0.0f, "mad", ""},
    {"mul", "mad(x * c.z, c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.0f, "mad", ""},
    // One fma with a single constant (x stays near the fixed point -0.37): the fp32 score's rate on GPUs where
    // the reference's two constants cost a second instruction (GCN's constant bus: an extra v_mov).
    {"fma1", "mad(x, x, c.x)", -0.5f, 0.0f, 0.0f, 0.0f, nullptr, "one fma, one constant: the fp32 score where it is faster"},
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
    {"rcpmax", "mad(max(rcp(x), c.z), c.x, c.y)", 0.5f, 0.5f, 0.1f, 0.0f, "mad", "base for the omod tests"},
    {"omod2", "mad(max(rcp(x) * 2.0, c.z), c.x, c.y)", 0.5f, 0.5f, 0.1f, 0.0f, "rcpmax", "AMD: output modifier, ~0"},
    {"omodhalf", "mad(max(rcp(x) * 0.5, c.z), c.x, c.y)", 0.5f, 1.0f, 0.1f, 0.0f, "rcpmax", "AMD: output modifier, ~0"},
    {"omod3", "mad(max(rcp(x) * 3.0, c.z), c.x, c.y)", 0.5f, 0.5f, 0.1f, 0.0f, "rcpmax", "control: not an omod scale"},
    {"omod4", "mad(max(rcp(x) * 4.0, c.z), c.x, c.y)", 0.125f, 0.5f, 0.1f, 0.0f, "rcpmax", "AMD: output modifier, ~0"},
    {"omod8", "mad(max(rcp(x) * 8.0, c.z), c.x, c.y)", 0.0625f, 0.5f, 0.1f, 0.0f, "rcpmax", "old GPUs only (DX9 _x8)?"},
    {"omod0.25", "mad(max(rcp(x) * 0.25, c.z), c.x, c.y)", 2.0f, 0.5f, 0.1f, 0.0f, "rcpmax", "old GPUs only (DX9 _d4)?"},
    {"omod0.125", "mad(max(rcp(x) * 0.125, c.z), c.x, c.y)", 4.0f, 0.5f, 0.01f, 0.0f, "rcpmax", "old GPUs only (DX9 _d8)?"},
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
    {"radians", "mad(radians(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "a mul (also degrees)"},
    {"ldexp", "mad(ldexp(x, x - c.z), c.x, c.y)", 0.5f, 0.3f, 2.0f, 0.0f, "sub", "x * exp2(e): exp2 + mul"},
    {"frexp", "mad(frexpM(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "mantissa + exponent * 0.01"},
    {"modf", "mad(modfS(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "fraction + integer part * 0.5"},
    {"isnan", "mad(isnan(x) ? c.z : x, c.x, c.y)", 0.5f, 0.5f, 1.0f, 0.0f, "mad", "ne + movc (a compiler may drop it)"},
    {"isinf", "mad(isinf(x) ? c.z : x, c.x, c.y)", 0.5f, 0.5f, 1.0f, 0.0f, "mad", "abs, eq + movc"},
    {"f16round", "mad(f16tof32(f32tof16(x)), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "f32tof16 + f16tof32"},
    {"bitcast", "mad(asfloat(asint(x)), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "asint / asfloat: free"},
    // Integer and bit operations (uint chains: x = (x ^ c.y) * c.x mixes, nothing reassociates)
    // and conversions between int and float.
    {"ixmul", "(x ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "mad", "base: xor + imul", "uint"},
    {"iadd", "((x + asuint(c.z)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul", "", "uint"},
    {"imul", "((x * asuint(c.z)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul", "", "uint"},
    {"iand", "((x & asuint(c.z)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul", "NVIDIA: LOP3 can merge it with the xor",
     "uint"},
    {"imin", "(min(x, asuint(c.z)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul", "umin", "uint"},
    {"ishr", "((x ^ (x >> 13)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul", "ushr + xor", "uint"},
    {"irot", "(((x << 7) | (x >> 25)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul",
     "rotate: shl, shr, or (or one funnel shift / alignbit)", "uint"},
    {"popc", "((x + countbits(x)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "iadd", "countbits", "uint"},
    {"fbh", "((x + firstbithigh(x)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "iadd", "firstbithigh", "uint"},
    {"bitrev", "(reversebits(x) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul", "reversebits", "uint"},
    {"fbl", "((x + firstbitlow(x)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "iadd", "firstbitlow", "uint"},
    {"icmpsel", "((x > asuint(c.z) ? x : ~x) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul", "ult, not, movc", "uint"},
    {"udiv", "((x / ((asuint(c.z) >> 24) | 1u)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul",
     "unsigned divide (no instruction on most GPUs), plus shr, or", "uint"},
    {"umod", "((x % ((asuint(c.z) >> 24) | 1u)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul", "unsigned modulo", "uint"},
    {"idiv", "(asuint(asint(x) / (asint(asuint(c.z) >> 24) | 1)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul",
     "signed divide", "uint"},
    {"imod", "(asuint(asint(x) % (asint(asuint(c.z) >> 24) | 1)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul",
     "signed modulo", "uint"},
    {"itof", "(asuint(float(asint(x))) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul", "int -> float conversion", "uint"},
    {"utof", "(asuint(float(x)) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul", "uint -> float conversion", "uint"},
    {"unitf", "(asuint(asfloat((x >> 9) | 0x3f800000u) * 1.5) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "ixmul",
     "uint -> [1, 2) by bits: ushr, or, plus a mul", "uint"},
    {"ftou", "(uint(asfloat((x >> 9) | 0x3f800000u) * 4194304.0) ^ asuint(c.y)) * asuint(c.x)", 0, 0, 0, 0, "unitf",
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
};

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
     "how many of each instruction the GPU finishes per second (the number sopt's cost models use)"},
    {"dep", 1, kGroupsFull, "Cost, one dependent chain",
     "every step waits for the one before it; the GPU hides the wait by switching between threads"},
    {"lat", 1, 1, "Latency, one at a time", "how long one step takes until its result is ready (one group of threads, nothing to hide it)"}};

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
  s += "[numthreads(64, 1, 1)]\n"
       "void main(uint3 id : SV_DispatchThreadID)\n{\n"
       "  const uint g = id.x / 64u, l = id.x % 64u;\n";
  if (t.setup & kGroupshared)
    s += "  for (uint k = l; k < 2048u; k += 64u) GS[k] = 0.5 + float(k) * 1e-4 + seed;\n"
         "  GSI[l] = l;\n  GroupMemoryBarrierWithGroupSync();\n";
  if (t.setup & kLocalArray) s += "  float A[16];\n  [unroll] for (uint k = 0; k < 16u; ++k) A[k] = 0.5 + float(k) * 0.01 + seed;\n";
  // The type and step of chain k (pair tests: the odd chains run the pair step).
  auto typeOf = [&](int k) { return std::string(t.pairStep && (k & 1) ? t.pairType : t.type); };
  auto stepOf = [&](int k) { return std::string(t.pairStep && (k & 1) ? t.pairStep : t.step); };
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
    s += "    {\n      const float4 c = U[" + std::to_string(r) + "];\n";
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
  s += "  }\n  O[id.x] = 0.0";
  for (int k = 0; k < chains; ++k) {
    const std::string x = "x" + std::to_string(k);
    const std::string type = typeOf(k);
    if (type == "float") s += " + " + x;
    else if (type == "uint") s += " + float(" + x + " & 1023u)";
    else if (type == "min16float") s += " + (float)" + x;
    else s += " + dot(" + x + ", 1.0)";
  }
  if (t.setup & kGroupshared) s += " + GS[(l * 7u) & 2047u] + float(GSI[l] & 1u)";  // keeps the writes
  s += ";\n}\n";
  return s;
}

struct Gpu {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* out = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  Timer timer;
};

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
  return g.timer.time(g.ctx, [&] { g.ctx->Dispatch(groups, 1, 1); });
}

struct Result {
  UINT iters = 0;
  double ms = 0.0;
  double nsPerStep = 0.0;  // per step of one chain in one thread (tput: of all chains together / chains)
};

// Doubles the iteration count until a dispatch takes >= 2 ms.
UINT calibrate(Gpu& g, ID3D11ComputeShader* cs, const Test& t, const Config& c) {
  g.ctx->CSSetShader(cs, nullptr, 0);
  UINT iters = 2;
  for (;;) {
    setConstants(g, t, iters);
    const double ms = timeDispatch(g, c.groups);  // < 0: no valid reading, keep the run length
    if (ms < 0.0 || ms >= 2.0 || iters >= (1u << 20)) return iters;
    iters *= 2;
  }
}

// The median of reps runs at a known iteration count.
Result measureAt(Gpu& g, ID3D11ComputeShader* cs, const Test& t, const Config& c, UINT iters, int reps) {
  g.ctx->CSSetShader(cs, nullptr, 0);
  setConstants(g, t, iters);
  std::vector<double> runs;
  for (int k = 0; k < reps * 3 && int(runs.size()) < reps; ++k)
    if (const double m = timeDispatch(g, c.groups); m > 0.0) runs.push_back(m);
  Result r;
  r.iters = iters;
  r.ms = median(runs);
  const double steps = double(iters) * kUnroll * double(c.groups) * kGroupSize * c.chains;
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
    "#Modifiers and folds", "neg", "abs", "negabs", "saturate", "satmad", "mul", "omod2", "omodhalf", "omod4",
    "omod8", "omod0.25", "omod0.125", "omod3",
    "#Basic arithmetic", "min", "max", "step", "add", "sub", "fma1", "mad2", "contract", "max3", "minmax", "clamp", "select",
    "lerp",
    "#Rounding and sign", "floor", "ceil", "round", "trunc", "frac", "roundadd", "flooradd", "fracadd", "signsel2", "signbits",
    "signsat", "signmad", "signclamp", "signsel", "sign",
    "#Division and transcendentals", "divxy", "rcp", "rsqrt", "sqrt", "div", "exp2", "log2", "log", "exp", "cos", "sin",
    "rcpmax", "pow",
    "#Fast approximations (ShaderFastMathLib, bit tricks)", "rsqrtnr0", "rsqrtnr1", "rcpnr0", "rcpnr1", "sqrtnr0",
    "pow2floor", "exp2floor", "pow2ceil", "exp2ceil", "acos4", "acos1", "atan4", "atan5a", "atan2q",
    "#Vector (float2 / float3 / float4)", "mad2v", "mad3v", "mad4v", "dot2", "dot3", "dot4", "cross", "length",
    "distance", "normalize", "reflect", "refract", "faceforward", "det3", "matmul4", "transpose",
    "#Written out by fxc", "smoothstep", "fmod", "sincos", "tan", "atan", "atan2", "asin", "acos",
    "#More intrinsics", "bitcast", "radians", "log10", "isnan", "isinf", "modf", "frexp", "ldexp", "f16round", "tanh",
    "sinh", "cosh",
    "#Integer and conversions", "bitor", "ixmul", "iadd", "iand", "imin", "ishr", "irot", "imul", "popc", "fbh",
    "bitrev", "fbl", "icmpsel", "unitf", "utof", "itof", "ftou", "ftoitof", "udiv", "umod", "idiv", "imod",
    "#Half precision (min16float)", "mad16", "add16", "mul16", "rcp16", "sqrt16", "exp2_16",
    "#Compute: groupshared memory and barriers", "gsread", "gswrite", "gswriteread", "gsread32", "gswrite32", "barrier",
    "groupbarrier", "membarrier",
    "#Compute: groupshared atomics (aAdd = atomicAdd ...; 1 = 64 threads on one address)", "aAdd", "aAnd", "aOr", "aXor",
    "aMin", "aMax", "aXchg", "aCmpXchg", "aAdd1", "aAnd1", "aOr1", "aXor1", "aMin1", "aMax1",
    "aXchg1", "aCmpXchg1",
    "#Compute: local arrays and branches", "arrayread", "arraywrite", "constarray", "selectboth", "branchuni", "branchdiv",
    "#Parallel issue: an fma and X together (Cost = both; % = of the two one after the other)", "fma+fma", "fma+int",
    "fma+minmax", "fma+cvt", "fma+rcp", "fma+half"};

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
    if (std::strcmp(f.model, model) == 0) fc.family = &f;
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
  gProgram = "OpBench";
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // progress shows while it runs
  const Style st = initConsole();
  int adapterIndex = -1;
  bool list = false;
  std::string filter, outPath;
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
      kConfigs[0].groups = kConfigs[1].groups = n;
    } else {
      std::printf("OpBench %s\nusage: OpBench [--adapter N] [--list] [--adapters] [--filter text] [--reps N] [--out file.csv] [--groups N]\n",
                  SOPT_VERSION);
      return a == "-h" || a == "--help" ? 0 : 1;
    }
  }

  if (!list) {
    printBox(st, std::string("OpBench ") + SOPT_VERSION + "  -  by CeeJay.dk");
    std::printf("\n");
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
      half16 = (mp.AllOtherShaderStagesMinPrecision & D3D11_SHADER_MIN_PRECISION_16_BIT) != 0;
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
  }

  const std::filesystem::path reports = reportsDir();
  std::string safeName = gpuName;
  for (char& ch : safeName)
    if (!isalnum((unsigned char)ch)) ch = '_';
  if (outPath.empty()) outPath = (reports / ("opbench-" + safeName + ".csv")).string();
  const std::filesystem::path dxbcDir = reports / "Shaders" / "OpBench";
  std::filesystem::create_directories(dxbcDir);

  // Compile every test for every configuration (chains differ).
  std::vector<const Test*> tests;
  for (const Test& t : kTests)
    if (filter.empty() || std::string(t.name).find(filter) != std::string::npos || !t.base || std::strcmp(t.name, "mad") == 0)
      tests.push_back(&t);
  // Bases of the selected tests are needed too.
  for (bool added = true; added;) {
    added = false;
    for (const Test* t : std::vector<const Test*>(tests))
      if (t->base && std::none_of(tests.begin(), tests.end(), [&](const Test* u) { return std::strcmp(u->name, t->base) == 0; }))
        for (const Test& u : kTests)
          if (std::strcmp(u.name, t->base) == 0) {
            tests.push_back(&u);
            added = true;
          }
  }

  // Shaders compile in the background, section by section in measuring order (owner, 2026-10-05: results
  // start sooner); each section waits for its own. The map is filled with every key first, so the worker only
  // writes values.
  std::map<std::string, std::map<std::string, ID3D11ComputeShader*>> shaders;
  for (const Config& c : kConfigs)
    for (const Test* t : tests) shaders[c.name][t->name] = nullptr;
  auto compileTest = [&](const Test* t) {
    for (const Config& c : kConfigs) {
      const std::string src = shaderSource(*t, c.chains);
      ID3DBlob* code = nullptr;
      ID3DBlob* err = nullptr;
      if (FAILED(D3DCompile(src.data(), src.size(), t->name, nullptr, nullptr, "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        fail(std::string("cannot compile test ") + t->name + ":\n" + (err ? (const char*)err->GetBufferPointer() : ""));
      if (c.chains == 8) {  // disassembly of the throughput version
        ID3DBlob* dis = nullptr;
        if (SUCCEEDED(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &dis))) {
          if (FILE* f = std::fopen((dxbcDir / (std::string(t->name) + ".txt")).string().c_str(), "wb")) {
            std::fwrite(src.data(), 1, src.size(), f);
            std::fputs("\n// ---- DXBC (D3DCompile -O3) ----\n", f);
            std::fwrite(dis->GetBufferPointer(), 1, dis->GetBufferSize() - 1, f);
            std::fclose(f);
          }
          dis->Release();
        }
      }
      ID3D11ComputeShader* cs = nullptr;
      if (FAILED(g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs)))
        fail(std::string("cannot create test ") + t->name);
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
  std::printf("\n");
  BackgroundJobs compiling(std::move(jobList));
  compiling.wait(0);  // the reference, for the warm-up

  std::printf("%zu tests, each measured in three ways:\n", tests.size());
  for (const Config& c : kConfigs)
    std::printf("  %s%-26s%s %s\n", st.c("\x1b[1;96m"), c.title, st.reset(), c.what);
  std::printf("Each is measured twice (forward, then backward through its section) and more often when its two\n"
              "readings disagree. Only the first is shown (lower is better); the CSV has all three. The results appear section\n"
              "by section while the rest is measured.\n"
              "Warming up the GPU for 2 seconds so its clock settles ...");
  {
    g.ctx->CSSetShader(shaders["tput"]["mad"], nullptr, 0);
    setConstants(g, *madTest, 256);
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 2000) timeDispatch(g, kConfigs[0].groups);
  }
  std::printf(" done\n");

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
    std::printf("\n  %s%s%s\n  %s%-*s %6s  %-*s  %5s  %s%s\n", st.c("\x1b[1;96m"), gr.title, st.reset(), st.c("\x1b[90m"),
                nameW, "Test", "Cost", kBarWidth, "Graph", "Ops", "Comment", st.reset());
    for (const char* name : gr.shown) {
      const Measured& x = results["tput"][name];
      const Test* self = find(name);
      // Parallel issue: Cost = one fma and one X together (two chain steps of the pair test); the comment
      // compares it with the two one after the other (4 for the fma + X measured alone).
      const bool pair = self->pairStep && results["tput"].count(self->solo);
      const double together = pair ? 2.0 * x.units.value : 0.0;
      const double apart = pair ? 4.0 + results["tput"][self->solo].units.value : 0.0;
      const double v = pair ? together : x.vsBase;
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
      const auto e = fam.cost.find(name);
      if (!shaky && !pair && e != fam.cost.end()) ++compared;
      if (!shaky && !pair && e != fam.cost.end() && differs(v, e->second)) {
        different.push_back({name, v, e->second});
        char buf[48];
        std::snprintf(buf, sizeof(buf), "  usually %.1f", e->second);
        note += std::string(st.c("\x1b[95m")) + buf + st.reset();
      }
      const double shown = std::fabs(v) < 0.05 ? 0.0 : v;  // no "-0.0"
      const double ops = std::fabs(v / 4.0) < 0.05 ? 0.0 : v / 4.0;
      std::printf("  %-*s %6.1f  %s  %5.1f  %s%s%s%s\n", nameW, name, shown, b.c_str(), ops, color.c_str(), comment,
                  st.reset(), note.c_str());
    }
    std::printf("%s%s(shorter is better)%s\n", graphIndent.c_str(), st.c("\x1b[90m"), st.reset());
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
  std::fprintf(csv, "# OpBench %s\n# gpu: %s\n# vendor: 0x%04X\n# device: 0x%04X\n# driver: %s\n", SOPT_VERSION,
               gpuName.c_str(), desc.VendorId, desc.DeviceId, driver.c_str());
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
  std::printf("\n\n   reference drift:");
  for (const Config& c : kConfigs) {
    const std::vector<double>& v = mads[c.name];
    const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    madDrift[c.name] = 100.0 * (*hi - *lo) / median(v);
    double mean = 0.0;
    for (double x : v) mean += x / double(v.size());
    madNs[c.name] = mean;
    std::printf(" %s %.1f%%", c.name, madDrift[c.name]);
  }
  std::printf("\n");

  writeCsv();


  // Summary: banner and the GPU (the sections are shown above).
  std::printf("\n");
  printBox(st, std::string("OpBench ") + SOPT_VERSION + "  -  " + gpuName);
  std::printf("  driver %s, vendor 0x%04X, device 0x%04X, %.1f TFLOPS fp32 (measured)\n", driver.c_str(), desc.VendorId,
              desc.DeviceId, score().fp32);
  std::printf("  min16float runs at %s\n", half16 ? "16 bits" : "32 bits on this driver (the half precision tests measure fp32)");
  // How to read the summary, for people who are not programmers (owner's wording review, 2026-10-04).
  std::printf("\n  %sHow to read this%s\n"
              "  Cost    How long the operation takes, compared with the simplest thing a GPU does:\n"
              "          a multiply-add, which counts as 4. The rest of the test is already subtracted.\n"
              "  Ops     Operations: the cost counted in multiply-adds.\n"
              "          2.0 means \"takes as long as two multiply-adds\".\n"
              "  Graph   Longer bar = slower. Free operations have no bar; a full bar is 25 multiply-adds or more.\n"
              "  usually The cost other cards of the same family measured, where this card differs by more than 25%%.\n"
              "\n"
              "  The numbers show how fast the GPU is when it is fully busy (as in a game).\n"
              "  Docs\\OpBench.html (README.html next to this program) explains every test in plain words.\n",
              st.c("\x1b[1;96m"), st.reset());

  bool warned = false;
  for (const Config& c : kConfigs)
    if (madDrift[c.name] > 5.0) {
      if (!warned) std::printf("\n");
      warned = true;
      std::printf("  %sWarning:%s the reference changed by %.0f%% during the %s tests: the GPU clock moved.\n",
                  st.c("\x1b[1;93m"), st.reset(), madDrift[c.name], c.name);
    }
  if (!unstable.empty()) {
    std::printf("%s  %sWarning:%s %zu test(s) without agreeing readings after %d passes:", warned ? "" : "\n",
                st.c("\x1b[1;93m"), st.reset(), unstable.size(), kMaxPasses);
    for (const std::string& n : unstable) std::printf(" %s", n.c_str());
    std::printf("\n");
    warned = true;
  }
  if (warned)
    std::printf("  For steadier numbers: close other programs, plug in a laptop, set \"Prefer maximum performance\"\n"
                "  (NVIDIA) or lock the clocks, and run it again.\n");
  // Compared with the family (owner, 2026-10-06): differences make the report interesting.
  std::printf("\n  %sCompared with other cards%s\n", st.c("\x1b[1;96m"), st.reset());
  if (!fam.family)
    std::printf("  No reports from this GPU's family yet: this report is especially interesting, please send it.\n");
  else {
    const int n = fam.family->cards;
    std::printf("  %s: %d card%s measured so far. ", fam.family->label, n, n == 1 ? "" : "s");
    if (different.empty())
      std::printf("This card matches %s in all %d tests compared.\n", n == 1 ? "it" : "them", compared);
    else {
      std::printf("This card differs in %s%zu of %d test%s%s (\"usually\" above):\n", st.c("\x1b[95m"),
                  different.size(), compared, compared == 1 ? "" : "s", st.reset());
      std::string line = "   ";
      for (const Difference& d : different) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), " %s %.1f (usually %.1f)", d.test.c_str(), d.measured, d.expected);
        if (line.size() + std::strlen(buf) > size_t(std::max(40, cols - 2))) {
          std::printf("%s\n", line.c_str());
          line = "   ";
        }
        line += buf;
      }
      std::printf("%s\n", line.c_str());
      if (warned) std::printf("  The warnings above may explain some of them.\n");
      std::printf("  Such reports are especially interesting: please send this one.\n");
    }
  }
  std::printf("\n  CSV:  %s\n  DXBC: %s\n", outPath.c_str(), dxbcDir.string().c_str());
  if (const Score sc = score(); sc.fp32 > 0.0) {
    std::vector<std::pair<std::string, std::string>> more;
    more.push_back({"fp16 (min16float)", sc.fp16 > 0.0 ? threeDigits(sc.fp16) + " TFLOPS" : std::string("runs at fp32")});
    if (sc.special > 0.0) more.push_back({"Special functions (rcp)", threeDigits(sc.special) + " Gops/s"});
    printScore(st, "OpBench score", gpuName, desc.VendorId, sc.fp32, "TFLOPS", "fp32, measured", more);
  }
  gCurrent.clear();
  setTitle("done");
  return 0;
}
