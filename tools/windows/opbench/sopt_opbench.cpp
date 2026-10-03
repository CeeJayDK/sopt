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
// Results go to the console and a CSV (default opbench-<gpu>.csv next to the exe); the DXBC
// disassembly of every test goes to opbench-dxbc\ so a folded test can be spotted.
//
// Version 2: every test is measured twice (forward, then backward through the list) with a fresh
// reference mad right before it, so a GPU clock change only affects the tests around it and shows
// as drift or as a disagreement between the two passes; the summary lists the throughput costs in
// a fixed order with a bar per test; the CSV has the GPU / driver once in '#' header lines.
//
// Version 3 (0.2.0): vector chains (dot, cross, length, normalize), intrinsics fxc writes out
// (atan, asin, tan, fmod, smoothstep, sincos), integer / bit operations and int <-> float
// conversions on uint chains, half precision (min16float); summary in sections. TESTS.txt
// describes every test.
//
// Version 4 (0.3.0, owner 2026-10-03): renamed OpBench; block graphics only from full and half
// blocks (4 levels per cell from bright / dark color pairs), a gradient progress bar, an
// Ops column; tests whose passes disagree are measured again (up to kMaxPasses) until most
// readings agree.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Laptops with switchable graphics: ask the NVIDIA (Optimus) and AMD (PowerXpress / Enduro)
// drivers for the discrete GPU instead of the integrated one.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

#ifndef SOPT_VERSION
#define SOPT_VERSION "dev"
#endif

namespace {

struct Test {
  const char* name;
  const char* step;  // HLSL expression of x (the chain value) and c (this step's float4 constants)
  float cx, cy, cz, cw;
  const char* base;  // the test whose time is subtracted
  const char* note;
  // Type of the chain value x: float, float2..4, uint (constants are then random odd 32-bit
  // patterns, read with asuint) or min16float.
  const char* type = "float";
};

// Constants keep every chain finite and away from denormals (x stays roughly in [0.3, 3]).
const Test kTests[] = {
    {"mad", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, nullptr, "reference: one fma per step"},
    {"add", "mad(x + c.z, c.x, c.y)", 0.5f, 0.5f, 0.1f, 0.0f, "mad", ""},
    {"sub", "mad(c.z - x, c.x, c.y)", 0.5f, 1.0f, 2.0f, 0.0f, "mad", ""},
    {"mul", "mad(x * c.z, c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.0f, "mad", ""},
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
    // Intrinsics fxc writes out as instruction sequences (sopt has no op for most of them yet).
    {"fmod", "mad(fmod(x, c.z), c.x, c.y)", 0.5f, 1.0f, 0.7f, 0.0f, "mad", "fxc: div, frac, mul, select"},
    {"smoothstep", "mad(smoothstep(c.z, c.w, x), c.x, c.y)", 0.5f, 1.0f, 0.5f, 2.5f, "mad", "fxc: add, mul_sat, mad, 2 mul"},
    {"atan", "mad(atan(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "fxc: polynomial"},
    {"atan2", "mad(atan2(x, c.z), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "mad", "fxc: polynomial + quadrant fixes"},
    {"asin", "mad(asin(x * c.z), c.x, c.y)", 0.5f, 1.0f, 0.3f, 0.0f, "mul", "fxc: polynomial + sqrt"},
    {"acos", "mad(acos(x * c.z), c.x, c.y)", 0.5f, 1.0f, 0.3f, 0.0f, "mul", "fxc: polynomial + sqrt"},
    {"tan", "mad(tan(x * c.z), c.x, c.y)", 0.5f, 1.0f, 0.4f, 0.0f, "mul", "fxc: sincos + div"},
    {"sincos", "mad(sin(x) + cos(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "add", "sin and cos of one value"},
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
    {"tput", 8, kGroupsFull, "Throughput",
     "how many of each instruction the GPU finishes per second (the number sopt's cost models use)"},
    {"dep", 1, kGroupsFull, "Dependent chains",
     "every step waits for the one before it; the GPU hides the wait by switching between threads"},
    {"lat", 1, 1, "Latency", "how long one step takes until its result is ready (one group of threads, nothing to hide it)"}};

[[noreturn]] void fail(const std::string& what) {
  std::fprintf(stderr, "OpBench: %s\n", what.c_str());
  std::exit(1);
}

std::string narrow(const wchar_t* w) {
  char buf[512];
  WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
  return buf;
}

std::string shaderSource(const Test& t, int chains) {
  std::string s =
      "cbuffer C : register(b0) { float4 U[16]; uint iters; float seed; float2 pad; };\n"
      "RWStructuredBuffer<float> O : register(u0);\n"
      // precise keeps fxc from folding (v + c) - c to v
      "float roundAdd(float v) { precise float t = v + 12582912.0; precise float r = t - 12582912.0; return r; }\n"
      "float floorAdd(float v) { precise float r = (v + 12582912.0) - 12582912.0; precise float f = r - saturate((r - v) * 1e38); return f; }\n"
      "float fracAdd(float v) { precise float d = v - ((v + 12582912.0) - 12582912.0); precise float f = d + saturate(d * -1e38); return f; }\n"
      "[numthreads(64, 1, 1)]\n"
      "void main(uint3 id : SV_DispatchThreadID)\n{\n";
  const std::string type = t.type;
  for (int k = 0; k < chains; ++k) {
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
      std::string step = t.step;
      // Replace the chain variable x (a lone identifier) with xk.
      std::string out;
      for (size_t p = 0; p < step.size(); ++p) {
        const bool lone = step[p] == 'x' && (p == 0 || !(isalnum((unsigned char)step[p - 1]) || step[p - 1] == '_' || step[p - 1] == '.')) &&
                          (p + 1 == step.size() || !(isalnum((unsigned char)step[p + 1]) || step[p + 1] == '_'));
        out += lone ? x : std::string(1, step[p]);
      }
      s += "      " + x + " = " + out + ";\n";
    }
    s += "    }\n";
  }
  s += "  }\n  O[id.x] = 0.0";
  for (int k = 0; k < chains; ++k) {
    const std::string x = "x" + std::to_string(k);
    if (type == "float") s += " + " + x;
    else if (type == "uint") s += " + float(" + x + " & 1023u)";
    else if (type == "min16float") s += " + (float)" + x;
    else s += " + dot(" + x + ", 1.0)";
  }
  s += ";\n}\n";
  return s;
}

struct Gpu {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* out = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  ID3D11Query* disjoint = nullptr;
  ID3D11Query* t0 = nullptr;
  ID3D11Query* t1 = nullptr;
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
  g.ctx->Begin(g.disjoint);
  g.ctx->End(g.t0);
  g.ctx->Dispatch(groups, 1, 1);
  g.ctx->End(g.t1);
  g.ctx->End(g.disjoint);
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
  while (g.ctx->GetData(g.disjoint, &dj, sizeof(dj), 0) != S_OK) Sleep(0);
  UINT64 a = 0, b = 0;
  while (g.ctx->GetData(g.t0, &a, sizeof(a), 0) != S_OK) Sleep(0);
  while (g.ctx->GetData(g.t1, &b, sizeof(b), 0) != S_OK) Sleep(0);
  if (dj.Disjoint || dj.Frequency == 0) return -1.0;
  return double(b - a) * 1000.0 / double(dj.Frequency);
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0.0 : v[v.size() / 2];
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
    if (timeDispatch(g, c.groups) >= 2.0 || iters >= (1u << 20)) return iters;
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
constexpr int kMaxPasses = 6;

// Two readings agree within 0.75 units or 15%.
bool agree(double a, double b) { return std::fabs(a - b) <= std::max(0.75, 0.15 * std::max(std::fabs(a), std::fabs(b))); }

struct Consensus {
  bool ok = false;     // more than half of the readings agree
  double value = 0.0;  // their mean (without a majority: the median of all)
};
Consensus consensus(std::vector<double> v) {
  Consensus c;
  if (v.empty()) return c;
  std::sort(v.begin(), v.end());
  size_t best = 0, bestLo = 0;
  for (size_t lo = 0; lo < v.size(); ++lo)
    for (size_t hi = lo; hi < v.size() && agree(v[lo], v[hi]); ++hi)
      if (hi - lo + 1 > best) {
        best = hi - lo + 1;
        bestLo = lo;
      }
  c.ok = 2 * best > v.size();
  if (c.ok) {
    for (size_t k = bestLo; k < bestLo + best; ++k) c.value += v[k] / double(best);
  } else {
    c.value = v[v.size() / 2];
  }
  return c;
}

struct Measured {
  Result r;                      // averaged over all readings
  std::vector<double> readings;  // 4 * time / the reference mad's time, one per pass
  Consensus units;               // of the readings
  double vsBase = 0.0;           // units minus the base test's units (the reported cost)
  double vsBasePass[2] = {0, 0}; // the same from the forward and the backward pass alone
};

// Console output: ANSI colors and UTF-8 box / bar characters where the console supports virtual
// terminal sequences (Windows 10+), plain ASCII otherwise.
struct Style {
  bool vt = false;
  const char* c(const char* code) const { return vt ? code : ""; }
  const char* reset() const { return c("\x1b[0m"); }
};

Style initConsole() {
  Style st;
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) &&
      SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
    st.vt = true;
    SetConsoleOutputCP(CP_UTF8);
  }
  return st;
}

// The summary's fixed order: sections ("#" entries), within each cheapest to most expensive as
// most GPUs measure it, the same on every GPU so results can be compared line by line.
const char* const kDisplayOrder[] = {
    "#Modifiers and folds", "neg", "abs", "negabs", "saturate", "satmad", "mul", "omod2", "omodhalf", "omod4",
    "omod8", "omod0.25", "omod0.125", "omod3",
    "#Basic arithmetic", "min", "max", "step", "add", "sub", "mad2", "contract", "max3", "minmax", "clamp", "select",
    "lerp",
    "#Rounding and sign", "floor", "ceil", "round", "trunc", "frac", "roundadd", "flooradd", "fracadd", "signsel2", "signbits",
    "signsat", "signmad", "signclamp", "signsel", "sign",
    "#Division and transcendentals", "divxy", "rcp", "rsqrt", "sqrt", "div", "exp2", "log2", "log", "exp", "cos", "sin",
    "rcpmax", "pow",
    "#Vector (float2 / float3 / float4)", "mad2v", "mad3v", "mad4v", "dot2", "dot3", "dot4", "cross", "length",
    "distance", "normalize", "reflect",
    "#Written out by fxc", "smoothstep", "fmod", "sincos", "tan", "atan", "atan2", "asin", "acos",
    "#Integer and conversions", "bitor", "ixmul", "iadd", "iand", "imin", "ishr", "irot", "imul", "popc", "fbh",
    "bitrev", "unitf", "utof", "ftou", "ftoitof",
    "#Half precision (min16float)", "mad16", "add16", "mul16", "rcp16", "sqrt16", "exp2_16"};

// 1048576 -> "1,048,576".
std::string withCommas(unsigned long long v) {
  std::string s = std::to_string(v);
  for (int k = int(s.size()) - 3; k > 0; k -= 3) s.insert(size_t(k), ",");
  return s;
}


// Block graphics use only the full block and the half blocks (code page 437, in every console font;
// the 1/8 blocks of version 3 showed as boxes). A color is a bright / dark pair of the 16 console
// colors, and the dark one as a background gives 4 levels per cell (owner's design, 2026-10-03):
// space, left half dark, left half bright, left half bright on dark, full bright.
constexpr const char* kFull = "\u2588";
constexpr const char* kLeft = "\u258C";
struct Shade {
  int bright, dark;  // foreground codes; the dark background is dark + 10
};
constexpr Shade kRed = {91, 31}, kYellow = {93, 33}, kWhite = {97, 90}, kCyan = {96, 36}, kGreen = {92, 32};

// A comment and a color for a throughput cost (extra over the base, 4 = one fma).
const char* costComment(double v, Shade* shade) {
  if (v < 0.75) { *shade = kGreen; return "free"; }
  if (v < 3.0) { *shade = kCyan; return "cheap"; }
  if (v < 5.5) { *shade = kWhite; return "one op"; }
  if (v < 9.0) { *shade = kYellow; return "two ops"; }
  *shade = kRed;
  return "expensive";
}

// One cell of a bar at level 0..4.
std::string barCell(int level, Shade c) {
  char buf[48];
  switch (level) {
    case 0: return " ";
    case 1: std::snprintf(buf, sizeof(buf), "\x1b[%dm%s\x1b[0m", c.dark, kLeft); break;
    case 2: std::snprintf(buf, sizeof(buf), "\x1b[%dm%s\x1b[0m", c.bright, kLeft); break;
    case 3: std::snprintf(buf, sizeof(buf), "\x1b[%d;%dm%s\x1b[0m", c.bright, (c.dark == 90 ? 100 : c.dark + 10), kLeft); break;
    default: std::snprintf(buf, sizeof(buf), "\x1b[%dm%s\x1b[0m", c.bright, kFull); break;
  }
  return buf;
}

// A bar of width cells for v out of maxV (at least one level when v > 0); returns its text, the
// display width is always `width`.
std::string bar(double v, double maxV, int width, const Style& st, Shade c) {
  const int levels = maxV <= 0.0 ? 0 : int(std::lround(std::max(0.0, v) / maxV * width * 4));
  const int n = std::min(width * 4, v > 0.0 ? std::max(1, levels) : 0);
  std::string s;
  for (int k = 0; k < width; ++k) {
    const int lv = std::min(4, std::max(0, n - 4 * k));
    s += st.vt ? barCell(lv, c) : std::string(lv >= 2 ? "#" : " ");
  }
  return s;
}

// The progress bar while measuring: one level per measured test, 6 levels per cell (with light grey).
struct Progress {
  const Style* st;
  int steps = 0;
  void step() {
    ++steps;
    if (!st->vt) {
      std::printf(".");
      return;
    }
    static const char* const kCell[6] = {"\x1b[90m\u258C", "\x1b[37m\u258C", "\x1b[97m\u258C",
                                         "\x1b[97;100m\u258C", "\x1b[97;47m\u258C", "\x1b[97m\u2588"};
    const int sub = (steps - 1) % 6;
    std::printf("%s%s\x1b[0m", sub == 0 ? "" : "\b", kCell[sub]);
  }
};


// Display width of a UTF-8 string (one column per code point).
int columns(const std::string& s) {
  int n = 0;
  for (unsigned char ch : s) n += (ch & 0xC0) != 0x80;
  return n;
}

// A double-line box around a title (bright cyan frame, bright white title; ASCII without VT).
void printBox(const Style& st, const std::string& title) {
  const std::string hz = st.vt ? "\u2550" : "=";
  std::string line;
  for (int k = 0; k < columns(title) + 4; ++k) line += hz;
  std::printf("  %s%s%s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "\u2554" : "+", line.c_str(), st.vt ? "\u2557" : "+", st.reset());
  std::printf("  %s%s%s  %s%s%s  %s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "\u2551" : "|", st.reset(), st.c("\x1b[1;97m"),
              title.c_str(), st.reset(), st.c("\x1b[1;96m"), st.vt ? "\u2551" : "|", st.reset());
  std::printf("  %s%s%s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "\u255A" : "+", line.c_str(), st.vt ? "\u255D" : "+", st.reset());
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // progress shows while it runs
  const Style st = initConsole();
  int adapterIndex = -1;
  bool list = false;
  std::string filter, outPath;
  int reps = 7;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) fail("missing value for " + a);
      return argv[++i];
    };
    if (a == "--adapter") adapterIndex = std::atoi(next());
    else if (a == "--list") list = true;
    else if (a == "--filter") filter = next();
    else if (a == "--reps") reps = std::max(1, std::atoi(next()));
    else if (a == "--out") outPath = next();
    else if (a == "--groups") {  // thread groups of 64 for tput / dep (default 16384; small for a quick check)
      const UINT n = UINT(std::max(1, std::min(int(kGroupsFull), std::atoi(next()))));
      kConfigs[0].groups = kConfigs[1].groups = n;
    } else {
      std::printf("OpBench %s\nusage: OpBench [--adapter N] [--list] [--filter text] [--reps N] [--out file.csv] [--groups N]\n",
                  SOPT_VERSION);
      return a == "-h" || a == "--help" ? 0 : 1;
    }
  }

  if (!list) {
    printBox(st, std::string("OpBench ") + SOPT_VERSION + "  -  by CeeJay.dk");
    std::printf("\n");
  }
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) fail("CreateDXGIFactory1 failed");
  std::vector<IDXGIAdapter1*> adapters;
  for (UINT k = 0;; ++k) {
    IDXGIAdapter1* ad = nullptr;
    if (factory->EnumAdapters1(k, &ad) == DXGI_ERROR_NOT_FOUND) break;
    adapters.push_back(ad);
  }
  int pick = -1;
  SIZE_T bestMem = 0;
  std::vector<std::string> adapterNames;
  for (size_t k = 0; k < adapters.size(); ++k) {
    DXGI_ADAPTER_DESC1 d;
    adapters[k]->GetDesc1(&d);
    const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    adapterNames.push_back(narrow(d.Description) + (software ? " [software]" : ""));
    if (list)
      std::printf("%zu: %s (%zu MB)%s\n", k, narrow(d.Description).c_str(), size_t(d.DedicatedVideoMemory >> 20),
                  software ? " [software]" : "");
    if (!software && (pick < 0 || d.DedicatedVideoMemory > bestMem)) {
      pick = int(k);
      bestMem = d.DedicatedVideoMemory;
    }
  }
  if (list) return 0;
  if (adapterIndex >= 0) pick = adapterIndex;
  if (pick < 0 || pick >= int(adapters.size())) fail("no GPU adapter (try --list)");

  DXGI_ADAPTER_DESC1 desc;
  adapters[pick]->GetDesc1(&desc);
  const std::string gpuName = narrow(desc.Description);
  LARGE_INTEGER umd = {};
  std::string driver = "unknown";
  if (SUCCEEDED(adapters[pick]->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", unsigned(HIWORD(umd.HighPart)), unsigned(LOWORD(umd.HighPart)),
                  unsigned(HIWORD(umd.LowPart)), unsigned(LOWORD(umd.LowPart)));
    driver = buf;
  }
  std::printf("%sGPU: %s%s (vendor 0x%04X, device 0x%04X), driver %s\n", st.c("\x1b[1m"), gpuName.c_str(), st.reset(),
              desc.VendorId, desc.DeviceId, driver.c_str());
  if (adapters.size() > 1) {
    std::printf("Also detected in system:\n");
    size_t nameW = 0;
    for (size_t k = 0; k < adapters.size(); ++k)
      if (int(k) != pick) nameW = std::max(nameW, adapterNames[k].size());
    for (size_t k = 0; k < adapters.size(); ++k)
      if (int(k) != pick)
        std::printf("  %zu: %-*s   %sUse --adapter %zu to test this%s\n", k, int(nameW), adapterNames[k].c_str(),
                    st.c("\x1b[90m"), k, st.reset());
  }

  Gpu g;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(adapters[pick], D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &g.dev,
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
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    g.dev->CreateQuery(&qd, &g.disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    g.dev->CreateQuery(&qd, &g.t0);
    g.dev->CreateQuery(&qd, &g.t1);
    g.ctx->CSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &g.uav, nullptr);
  }

  char exePath[MAX_PATH];
  GetModuleFileNameA(nullptr, exePath, MAX_PATH);
  const std::filesystem::path here = std::filesystem::path(exePath).parent_path();
  std::string safeName = gpuName;
  for (char& ch : safeName)
    if (!isalnum((unsigned char)ch)) ch = '_';
  if (outPath.empty()) outPath = (here / ("opbench-" + safeName + ".csv")).string();
  const std::filesystem::path dxbcDir = here / "opbench-dxbc";
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

  std::map<std::string, std::map<std::string, ID3D11ComputeShader*>> shaders;
  for (const Config& c : kConfigs)
    for (const Test* t : tests) {
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

  const Test* madTest = nullptr;
  for (const Test* t : tests)
    if (std::strcmp(t->name, "mad") == 0) madTest = t;
  // Warm up (clocks ramp up): two seconds of the reference test.
  std::printf("\n%zu tests in three ways, each measured twice (forward, then backward through the list) and more\n"
              "often when its two readings disagree.\n"
              "Warming up the GPU for 2 seconds so its clock settles ...", tests.size());
  {
    g.ctx->CSSetShader(shaders["tput"]["mad"], nullptr, 0);
    setConstants(g, *madTest, 256);
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 2000) timeDispatch(g, kConfigs[0].groups);
  }
  std::printf(" done\n");

  std::map<std::string, std::map<std::string, Measured>> results;  // config -> test -> result
  std::map<std::string, double> madDrift;                          // config -> spread of the reference
  std::map<std::string, double> madNs;                             // config -> mean reference time
  for (const Config& c : kConfigs) {
    std::printf("\n%s== %s%s: %d %schain%s per thread, %s threads\n   %s%s%s\n   ", st.c("\x1b[1;96m"), c.title,
                st.reset(), c.chains, c.chains > 1 ? "independent " : "", c.chains > 1 ? "s" : "",
                withCommas(c.groups * kGroupSize).c_str(), st.c("\x1b[90m"), c.what, st.reset());
    std::map<std::string, UINT> iters;
    for (const Test* t : tests) iters[t->name] = calibrate(g, shaders[c.name][t->name], *t, c);
    std::vector<double> mads;
    std::map<std::string, Result> sum;
    Progress progress{&st};
    // Passes 1 and 2 measure every test (forward, then backward); later passes only the tests whose
    // readings have no majority yet, alternating the direction.
    for (int pass = 0; pass < kMaxPasses; ++pass) {
      std::vector<const Test*> order;
      for (const Test* t : tests)
        if (pass < 2 || !consensus(results[c.name][t->name].readings).ok) order.push_back(t);
      if (order.empty()) break;
      if (pass % 2 == 1) std::reverse(order.begin(), order.end());
      for (const Test* t : order) {
        // A fresh reference right before the test: a clock change moves both.
        const Result m = measureAt(g, shaders[c.name]["mad"], *madTest, c, iters["mad"], reps);
        const Result r = t == madTest ? m : measureAt(g, shaders[c.name][t->name], *t, c, iters[t->name], reps);
        mads.push_back(m.nsPerStep);
        results[c.name][t->name].readings.push_back(4.0 * r.nsPerStep / m.nsPerStep);
        Result& acc = sum[t->name];
        acc.iters = r.iters;
        acc.ms += r.ms;
        acc.nsPerStep += r.nsPerStep;
        progress.step();
      }
    }
    for (const Test* t : tests) {
      Measured& x = results[c.name][t->name];
      const double n = double(x.readings.size());
      x.r = sum[t->name];
      x.r.ms /= n;
      x.r.nsPerStep /= n;
      x.units = consensus(x.readings);
    }
    for (const Test* t : tests) {
      Measured& x = results[c.name][t->name];
      const Measured* b = t->base ? &results[c.name][t->base] : nullptr;
      x.vsBase = b ? x.units.value - b->units.value : x.units.value;
      for (int pass = 0; pass < 2; ++pass)
        x.vsBasePass[pass] = b ? x.readings[size_t(pass)] - b->readings[size_t(pass)] : x.readings[size_t(pass)];
    }
    const auto [lo, hi] = std::minmax_element(mads.begin(), mads.end());
    madDrift[c.name] = 100.0 * (*hi - *lo) / median(mads);
    double mean = 0.0;
    for (double v : mads) mean += v / double(mads.size());
    madNs[c.name] = mean;
    std::printf("\n   reference drift %.1f%%%s\n", madDrift[c.name], madDrift[c.name] > 5.0 ? " (the GPU clock moved)" : "");
  }

  // CSV: the GPU once in header lines, then one row per configuration and test.
  FILE* csv = std::fopen(outPath.c_str(), "wb");
  if (!csv) fail("cannot write " + outPath);
  std::fprintf(csv, "# OpBench %s\n# gpu: %s\n# vendor: 0x%04X\n# device: 0x%04X\n# driver: %s\n", SOPT_VERSION,
               gpuName.c_str(), desc.VendorId, desc.DeviceId, driver.c_str());
  std::fprintf(csv, "# min16float: %s\n", half16 ? "16-bit" : "32-bit (no 16-bit min precision reported)");
  for (const Config& c : kConfigs) std::fprintf(csv, "# reference drift %s: %.2f%%\n", c.name, madDrift[c.name]);
  std::fprintf(csv,
               "config,test,base,iters,ms,ns_per_step,units,units_vs_base,vs_base_fwd,vs_base_bwd,passes,consensus,readings,"
               "step,note\n");
  for (const Config& c : kConfigs)
    for (const Test* t : tests) {
      const Measured& x = results[c.name][t->name];
      std::string readings;
      for (double v : x.readings) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%.3f", readings.empty() ? "" : ";", v);
        readings += buf;
      }
      std::fprintf(csv, "%s,%s,%s,%u,%.4f,%.6f,%.3f,%.3f,%.3f,%.3f,%zu,%s,%s,\"%s\",\"%s\"\n", c.name, t->name,
                   t->base ? t->base : "", x.r.iters, x.r.ms, x.r.nsPerStep, x.units.value, x.vsBase, x.vsBasePass[0],
                   x.vsBasePass[1], x.readings.size(), x.units.ok ? "yes" : "no", readings.c_str(), t->step,
                   t->note ? t->note : "");
    }
  std::fclose(csv);

  // Summary: banner, other adapters, throughput costs in the fixed order.
  const double fmaRate = 1.0 / madNs["tput"];  // per ns
  std::printf("\n");
  printBox(st, std::string("OpBench ") + SOPT_VERSION + "  -  " + gpuName);
  std::printf("  driver %s, vendor 0x%04X, device 0x%04X, %.1f TFLOPS fp32 (measured)\n", driver.c_str(), desc.VendorId,
              desc.DeviceId, fmaRate * 2.0 / 1000.0);
  std::printf("  min16float runs at %s\n", half16 ? "16 bits" : "32 bits on this driver (the half precision tests measure fp32)");

  constexpr int kBarWidth = 28;
  double maxV = 0.0;
  for (const char* name : kDisplayOrder)
    if (results["tput"].count(name)) maxV = std::max(maxV, results["tput"][name].vsBase);
  std::printf("\n  %s%-10s %6s  %-*s  %5s  %s%s\n", st.c("\x1b[1m"), "Test", "Cost", kBarWidth, "Graph", "Ops", "Comment",
              st.reset());
  std::vector<std::string> unstable;
  const char* section = nullptr;
  for (const char* name : kDisplayOrder) {
    if (name[0] == '#') {
      section = name + 1;
      continue;
    }
    if (!results["tput"].count(name)) continue;
    if (section) {
      std::printf("\n  %s%s%s\n", st.c("\x1b[1;96m"), section, st.reset());
      section = nullptr;
    }
    const Measured& x = results["tput"][name];
    const double v = x.vsBase;
    Shade shade;
    const char* comment = costComment(v, &shade);
    const std::string color = st.vt ? "\x1b[" + std::to_string(shade.bright) + "m" : "";
    const std::string b = bar(v, maxV, kBarWidth, st, shade);
    // A test (or its base) without a majority among its readings, or settled by extra passes.
    const Measured* base = nullptr;
    for (const Test* t : tests)
      if (std::strcmp(t->name, name) == 0 && t->base) base = &results["tput"][t->base];
    const bool shaky = !x.units.ok || (base && !base->units.ok);
    if (shaky) unstable.push_back(name);
    std::string note;
    if (shaky) note = st.vt ? "  \x1b[93m! no consensus\x1b[0m" : "  ! no consensus";
    else if (x.readings.size() > 2)
      note = std::string("  ") + st.c("\x1b[90m") + std::to_string(x.readings.size()) + " passes" + st.reset();
    const double shown = std::fabs(v) < 0.05 ? 0.0 : v;  // no "-0.0"
    const double ops = std::fabs(v / 4.0) < 0.05 ? 0.0 : v / 4.0;
    std::printf("  %-10s %6.1f  %s  %5.1f  %s%s%s%s\n", name, shown, b.c_str(), ops, color.c_str(), comment, st.reset(),
                note.c_str());
  }
  std::printf("\n  Cost = extra over the test's base, in sopt units (4 = one fma); Ops = Cost / 4 (fma equivalents);\n"
              "  throughput, %d chains. What each test measures: TESTS.txt next to this program.\n",
              kConfigs[0].chains);

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
  std::printf("\n  CSV:  %s\n  DXBC: %s\n", outPath.c_str(), dxbcDir.string().c_str());
  return 0;
}
