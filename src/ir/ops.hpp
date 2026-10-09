#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sopt {

// Float is float1; FloatN are HLSL floatN vectors. Bool is a scalar condition. Uint is a
// scalar 32-bit unsigned integer (bit tricks, --bits); its value is stored in the float slot
// as the same bits (std::bit_cast), never as a converted number.
enum class Type : uint8_t { Float, Bool, Float2, Float3, Float4, Uint };
inline constexpr size_t kNumTypes = 6;
inline constexpr uint8_t width(Type t) {
  return t == Type::Float2 ? 2 : t == Type::Float3 ? 3 : t == Type::Float4 ? 4 : 1;
}
inline constexpr bool isFloat(Type t) { return t != Type::Bool && t != Type::Uint; }
inline constexpr Type floatType(unsigned w) {
  return w == 2 ? Type::Float2 : w == 3 ? Type::Float3 : w == 4 ? Type::Float4 : Type::Float;
}

enum class Op : uint8_t {
  Input, Const,
  // unary
  Neg, Abs, Saturate, Floor, Frac, Sign, Sqrt, Rsqrt, Rcp, Exp, Log, Sin, Cos,
  Exp2, Log2, Round, Ceil,
  // binary
  Add, Sub, Mul, Div, Min, Max, Step, Pow,
  Lt, Le, Gt, Ge, Eq, Ne,
  // ternary
  Mad, Lerp, Clamp, Select,
  Smoothstep,  // pure helper: s * s * (3 - 2s), s = saturate((x - a) / (b - a)) (DXC's lowering)
  // vectors: pure helpers (dot = mul + fmas, length = sqrt(dot), normalize = v *
  // rsqrt(dot(v, v)), distance = length(a - b)), component selection and construction
  Dot, Length, Normalize, Distance, Swizzle, Construct,
  // integers and bit casts (scalar; owner, 2026-10-06: bit tricks are hard for people to find).
  // AsUint / AsFloat reinterpret the bits; FToU / FToI convert (truncate, D3D clamps out of range:
  // NaN -> 0); UToF / IToF convert the unsigned / signed value; IShr is the arithmetic shift of
  // the signed value. Shift counts use their low 5 bits, as on GPUs.
  AsUint, AsFloat, FToU, FToI, UToF, IToF,
  UAnd, UOr, UXor, UShl, UShr, IShr, UAdd, USub, UMul,
  // logical and / or / not on Bool conditions (a < b && c < d)
  LAnd, LOr, LNot,
  Count
};

enum class Syntax : uint8_t { Leaf, Call, Prefix, Infix, Ternary, Swizzle, Construct };

// How an op's operand and result types relate.
//   Comp:    componentwise on floatN; operands are float1 (broadcast) or floatN.
//   Cmp:     scalar float comparison, result Bool.
//   Select:  Bool condition, componentwise branches.
//   Reduce:  floatN operands of equal width, float1 result (dot, length, distance).
//   Same:    floatN -> floatN (normalize).
//   Swizzle: components of one operand (Node::swz), result width = count.
//   Construct: operands concatenated, result width = sum (2..4).
//   Int:     uint operands, uint result.
//   ToUint:  float1 -> uint (bit cast or conversion).
//   ToFloat: uint -> float1.
//   Logic:   Bool operands, Bool result (&&, ||, !).
enum class Shape : uint8_t { Leaf, Comp, Cmp, Select, Reduce, Same, Swizzle, Construct, Int, ToUint, ToFloat, Logic };
inline constexpr bool isIntShape(Shape s) { return s == Shape::Int || s == Shape::ToUint || s == Shape::ToFloat; }

struct OpInfo {
  std::string_view name;    // FX function name (Call) or internal name
  std::string_view symbol;  // operator symbol (Prefix/Infix)
  Syntax syntax;
  uint8_t arity;            // Construct: 0 = variable (Node::nargs)
  Shape shape;
  bool commutative;
  bool exact;     // bit-exact on IEEE 754 GPUs (given a semantic profile)
  bool base;      // enumerated by default; others only if present in the target
  uint8_t prec;   // printing precedence, higher binds tighter
};

const OpInfo& info(Op op);
// Intrinsics that are only shorthand for several instructions (lerp = sub + fma,
// step = cmp + cndmask), as opposed to single instructions or modifiers.
inline bool isPureHelper(Op op) {
  return op == Op::Lerp || op == Op::Step || op == Op::Dot || op == Op::Length ||
         op == Op::Normalize || op == Op::Distance || op == Op::Smoothstep;
}
std::optional<Op> opFromCall(std::string_view name, uint8_t arity);

// Static cost model. Costs are integers and every non-leaf op costs >= 1 so that
// cost levels are well-founded (operands of a level-c node have cost < c).
struct CostModel {
  std::string_view name;
  std::array<uint16_t, static_cast<size_t>(Op::Count)> cost;
  // Contraction: an add/sub whose operand is a single-use mul (or div, if divIsMul)
  // becomes one fma, and the add/sub then costs fusedAdd instead. 0 = no contraction.
  uint16_t fusedAdd;
  bool divIsMul;  // a / b is lowered to a * rcp(b)
  // AMD context effects (rdna3; owner, 2026-10-01, from ACO): a multiply by +-2, +-4 or +-0.5 is
  // the output modifier (omod) of the instruction producing the other operand, and min / max over
  // a min / max is one instruction (v_max3 / v_min3 / v_minmax / v_maxmin / v_med3). The folded
  // node costs 1 per component, like the other modifiers. See amdFoldedNodes (expr.hpp).
  bool amdFolds = false;
  // With amdFolds: only min over min and max over max fold (v_min3 / v_max3); min over max is an
  // instruction of its own (RDNA 2 and older have no v_minmax / v_maxmin; v_med3 needs lo <= hi).
  bool sameMinMaxOnly = false;

  uint16_t operator[](Op op) const { return cost[static_cast<size_t>(op)]; }
  // Cost of one node of this op producing / reducing floatN (w = operand width for
  // Reduce ops, result width otherwise). GPUs are scalar per lane: componentwise ops
  // cost w times the scalar op; dot = mul + (w-1) fma; swizzle/construct are register
  // moves (cost 1, like a modifier, to keep levels well-founded).
  uint32_t opCost(Op op, unsigned w) const;
  // A division as the compilers lower it, a * rcp(b) (RGA and ptxas, 2026-10-09): floatN / float1
  // takes one reciprocal of the divisor and N multiplies. wb = the divisor's width; equals opCost(Div, w)
  // when wb == w.
  uint32_t divCost(unsigned w, unsigned wb) const { return rcpPart(wb) + opCost(Op::Mul, w); }
  // The reciprocal's share of a division by a floatN divisor: what a second division by the same
  // divisor, which reuses it, does not pay again.
  uint32_t rcpPart(unsigned wb) const {
    const uint32_t d = opCost(Op::Div, wb), m = opCost(Op::Mul, wb);
    return d > m ? d - m : 0;
  }
  // opCost of a binary node from its operand widths (only a division by a broadcast scalar differs).
  uint32_t binaryCost(Op op, unsigned w, unsigned wb) const {
    return op == Op::Div && wb < w ? divCost(w, wb) : opCost(op, w);
  }
  bool fusesIntoAdd(Op operand) const {
    return fusedAdd && (operand == Op::Mul || (divIsMul && operand == Op::Div));
  }
};

// Half precision (min16float; owner, 2026-10-09: "the cost models must tell where fp16 math is actually a win: many
// cards support it but just run it as fp32"). ReShade writes min16float as min16float on D3D10-12 only (plain float on
// D3D9, "mediump float" on OpenGL, float + RelaxedPrecision on Vulkan: no effect there); the D3D driver decides whether it
// runs at 16 bits. Provisional, from the OpBench (D3D11) family medians of mad16 / add16 / rcp16 / sqrt16 / exp2_16 and the
// CSV header "# min16float"; the conversion cost waits for mix16 (OpBench 0.7.0) reports.
struct HalfCosts {
  bool sixteenBit = false;  // the driver runs min16float at 16 bits (else at 32: same cost, no conversions)
  bool packed = false;      // two components per instruction (AMD packed math): floatN costs ceil(N / 2) instructions
  uint16_t aluPct = 100;    // add / sub / mul / mad / min / max / lerp / clamp / dot, % of fp32 (per pair when packed)
  uint16_t mufuPct = 100;   // rcp / rsqrt / sqrt / exp2 / log2 / exp / log / sin / cos / pow / div, % of fp32
  uint16_t cvtOps = 0;      // one float <-> min16float conversion per component, in plain instructions (add costs)
};
const HalfCosts& halfCosts(const CostModel& m);
// Cost of one node computed in min16float; ops OpBench has no fp16 test for (floor, compares, select ...) cost as fp32.
uint32_t halfOpCost(const CostModel& m, Op op, unsigned w);
// Converting n components between float and min16float (one direction).
uint32_t halfConvertCost(const CostModel& m, unsigned n);

// generic: the M1 placeholder weights, no contraction.
// rdna3: AMD RDNA 3 measured with OpBench (RX 7900 GRE), units as measured (4 = one dual-issued fma; ops that
//   cannot dual-issue cost more), free source and output modifiers, contraction on.
const CostModel& costGeneric();
const CostModel& costRdna3();
// rdna3-rga: the earlier rdna3 from RGA gfx1100 instruction counts (VALU 4, MUFU 16), for comparisons and tests.
const CostModel& costRdna3Rga();
// nvidia: NVIDIA Ada SASS (ptxas + nvdisasm) in quarter-ALU units, MUFU at 8x.
const CostModel& costNvidia();
// nvidia-maxwell: NVIDIA Maxwell (GTX 860M, Quadro M5000M) from OpBench timings, quarter units.
const CostModel& costNvidiaMaxwell();
// nvidia-pascal: NVIDIA Pascal (GT 1030, GTX 1060) from sopt-opbench timings, quarter units.
const CostModel& costNvidiaPascal();
// nvidia-turing: NVIDIA Turing (GTX 1660) from sopt-opbench timings, quarter units, MUFU ~3x extra.
const CostModel& costNvidiaTuring();
// nvidia-ampere / nvidia-blackwell: provisional, from sopt-opbench (RTX 3050; RTX 5080 / 5090).
const CostModel& costNvidiaAmpere();
const CostModel& costNvidiaBlackwell();
// intel-gen9: Intel Gen9 (Iris 540) from sopt-opbench timings, quarter units, math unit ~3x.
const CostModel& costIntelGen9();
// intel-gen7.5: Intel Gen7.5 (HD Graphics 4600, Haswell) from OpBench timings, quarter units, math unit ~1 op.
const CostModel& costIntelGen75();
// intel-gen12: Intel Gen12 / Xe-LP (Iris Xe) from OpBench timings, quarter units, math unit ~2.75 ops.
const CostModel& costIntelGen12();
// amd-rdna2 / amd-rdna4 / amd-gcn5 / amd-terascale2: AMD from sopt-opbench timings (680M + RX 6950 XT;
// RX 9070 XT; Renoir Vega; HD 7400M), quarter units with one plain VALU instruction = 4.
const CostModel& costAmdRdna2();
const CostModel& costAmdRdna4();
const CostModel& costAmdGcn5();
const CostModel& costAmdTerascale2();
// Default is rdna3 (searched in search order); --isa / --sass rank by real machine code.
const CostModel& defaultCostModel();
const CostModel* costModelByName(std::string_view name);
// --no-amd-folds: rdna3 without CostModel::amdFolds (other models unchanged).
const CostModel* withoutAmdFolds(const CostModel* m);
// Enumeration order used when none is given: search for rdna3 and the nvidia / intel models, else the model.
const CostModel& defaultOrderFor(const CostModel& objective);

// Backend semantic profiles. The same FX source can evaluate differently:
// HLSL lerp is a + t*(b-a), GLSL/SPIR-V mix is a*(1-t) + b*t, and mad may be fused.
// gpu models what the drivers actually emit (seen in RDNA3 ISA): a*b + c contracted
// to fma when the product has a single use, and a / b as a * rcp(b).
struct Profile {
  std::string_view name;
  bool lerpMix;
  bool madFused;
  bool contract = false;  // fuse single-use mul/div into add/sub (Expr-level, see verify)
  bool divRcp = false;    // a / b = a * (1 / b)
  // GPU approximations: inexact ops (rcp, rsqrt, sqrt, div, exp, log, sin, cos, pow) are
  // not correctly rounded on GPUs. +1 / -1 moves each of their results one float step up /
  // down, so a candidate that amplifies such errors (e.g. by cancellation) shows it.
  int ulpStep = 0;
};

inline constexpr Profile kProfileRef{"ref", false, false};
inline constexpr Profile kProfileMix{"mix", true, false};
inline constexpr Profile kProfileFma{"fma", false, true};
inline constexpr Profile kProfileGpu{"gpu", false, true, true, true};
inline constexpr Profile kProfileGpuUp{"gpu+", false, true, true, true, +1};
inline constexpr Profile kProfileGpuDown{"gpu-", false, true, true, true, -1};
inline constexpr std::array<Profile, 6> kAllProfiles{kProfileRef, kProfileMix, kProfileFma,
                                                     kProfileGpu, kProfileGpuUp, kProfileGpuDown};

}  // namespace sopt
