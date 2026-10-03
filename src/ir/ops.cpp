#include "ir/ops.hpp"

namespace sopt {
namespace {

// clang-format off
const std::array<OpInfo, static_cast<size_t>(Op::Count)> kInfo = {{
  //  name         sym   syntax             ar shape           comm   exact  base   prec
  {"input",     "",   Syntax::Leaf,      0, Shape::Leaf,     false, true,  true,  9},
  {"const",     "",   Syntax::Leaf,      0, Shape::Leaf,     false, true,  true,  9},
  {"neg",       "-",  Syntax::Prefix,    1, Shape::Comp,     false, true,  true,  7},
  {"abs",       "",   Syntax::Call,      1, Shape::Comp,     false, true,  true,  9},
  {"saturate",  "",   Syntax::Call,      1, Shape::Comp,     false, true,  true,  9},
  {"floor",     "",   Syntax::Call,      1, Shape::Comp,     false, true,  true,  9},
  {"frac",      "",   Syntax::Call,      1, Shape::Comp,     false, true,  true,  9},
  {"sign",      "",   Syntax::Call,      1, Shape::Comp,     false, true,  true,  9},
  {"sqrt",      "",   Syntax::Call,      1, Shape::Comp,     false, true,  true,  9},
  {"rsqrt",     "",   Syntax::Call,      1, Shape::Comp,     false, false, true,  9},
  {"rcp",       "",   Syntax::Call,      1, Shape::Comp,     false, false, true,  9},
  {"exp",       "",   Syntax::Call,      1, Shape::Comp,     false, false, false, 9},
  {"log",       "",   Syntax::Call,      1, Shape::Comp,     false, false, false, 9},
  {"sin",       "",   Syntax::Call,      1, Shape::Comp,     false, false, false, 9},
  {"cos",       "",   Syntax::Call,      1, Shape::Comp,     false, false, false, 9},
  // exp2 / log2: the hardware's transcendentals (exp and log are exp2(x * log2 e) and
  // log2(x) * ln 2); round (to nearest even) and ceil: one instruction each.
  {"exp2",      "",   Syntax::Call,      1, Shape::Comp,     false, false, false, 9},
  {"log2",      "",   Syntax::Call,      1, Shape::Comp,     false, false, false, 9},
  {"round",     "",   Syntax::Call,      1, Shape::Comp,     false, true,  false, 9},
  {"ceil",      "",   Syntax::Call,      1, Shape::Comp,     false, true,  false, 9},
  {"add",       "+",  Syntax::Infix,     2, Shape::Comp,     true,  true,  true,  5},
  {"sub",       "-",  Syntax::Infix,     2, Shape::Comp,     false, true,  true,  5},
  {"mul",       "*",  Syntax::Infix,     2, Shape::Comp,     true,  true,  true,  6},
  // GPUs lower a / b to a * rcp(b) with an approximate rcp: never bit-exact.
  {"div",       "/",  Syntax::Infix,     2, Shape::Comp,     false, false, true,  6},
  {"min",       "",   Syntax::Call,      2, Shape::Comp,     true,  true,  true,  9},
  {"max",       "",   Syntax::Call,      2, Shape::Comp,     true,  true,  true,  9},
  {"step",      "",   Syntax::Call,      2, Shape::Comp,     false, true,  true,  9},
  {"pow",       "",   Syntax::Call,      2, Shape::Comp,     false, false, false, 9},
  {"lt",        "<",  Syntax::Infix,     2, Shape::Cmp,      false, true,  true,  3},
  {"le",        "<=", Syntax::Infix,     2, Shape::Cmp,      false, true,  true,  3},
  {"gt",        ">",  Syntax::Infix,     2, Shape::Cmp,      false, true,  true,  3},
  {"ge",        ">=", Syntax::Infix,     2, Shape::Cmp,      false, true,  true,  3},
  {"eq",        "==", Syntax::Infix,     2, Shape::Cmp,      true,  true,  true,  3},
  {"ne",        "!=", Syntax::Infix,     2, Shape::Cmp,      true,  true,  true,  3},
  {"mad",       "",   Syntax::Call,      3, Shape::Comp,     false, true,  true,  9},
  {"lerp",      "",   Syntax::Call,      3, Shape::Comp,     false, true,  true,  9},
  {"clamp",     "",   Syntax::Call,      3, Shape::Comp,     false, true,  true,  9},
  {"select",    "?:", Syntax::Ternary,   3, Shape::Select,   false, true,  true,  2},
  // smoothstep(a, b, x): a pure helper (not enumerated); inexact through its division.
  {"smoothstep","",   Syntax::Call,      3, Shape::Comp,     false, false, true,  9},
  // dot/length/distance: a sum of products and a sqrt (inexact through sqrt/rsqrt and
  // the order of the sum); exact only as far as the ops they expand to.
  {"dot",       "",   Syntax::Call,      2, Shape::Reduce,   true,  true,  true,  9},
  {"length",    "",   Syntax::Call,      1, Shape::Reduce,   false, true,  true,  9},
  {"normalize", "",   Syntax::Call,      1, Shape::Same,     false, false, true,  9},
  {"distance",  "",   Syntax::Call,      2, Shape::Reduce,   true,  true,  true,  9},
  {"swizzle",   ".",  Syntax::Swizzle,   1, Shape::Swizzle,  false, true,  true,  10},
  {"construct", "",   Syntax::Construct, 0, Shape::Construct,false, true,  true,  9},
}};

// Per-op costs in Op order (input, const, neg, abs, saturate, floor, frac, sign, sqrt,
// rsqrt, rcp, exp, log, sin, cos, exp2, log2, round, ceil, add, sub, mul, div, min, max,
// step, pow, lt, le, gt, ge, eq, ne, mad, lerp, clamp, select, smoothstep, dot, length,
// normalize, distance, swizzle, construct). Costs are per float1; CostModel::opCost
// scales them to floatN; smoothstep and dot..distance are computed there (their entries
// are placeholders >= 1).
const CostModel kGeneric{"generic",
  {0, 0, 1, 1, 1, 2, 3, 2, 5,
   5, 5, 6, 6, 6, 6, 5, 5, 2, 2, 2, 2, 3, 5, 2, 2, 2, 8,
   2, 2, 2, 2, 2, 2, 4, 6, 3, 2, 1,
   3, 8, 11, 10, 1, 1},
  0, false};

// Quarter-VALU units, checked op by op against RGA gfx1100 ISA (fxstat COST x 4):
// VALU ops 4 (floor, frac, min, max, mad, clamp = v_med3), transcendentals 16,
// exp/log/sin/cos 20 (scale + transcendental), div 20 (rcp + mul), pow 36 (log, mul,
// exp), sign 16 (4 VALU), step 8 and comparison + select 8 (v_cmp + v_cndmask),
// lerp 8 (sub + fma). neg/abs/saturate are free modifiers in context (1 VALU only
// when applied to a bare input), so 1. Not modeled (the ISA ranking catches these):
// min(max(a, b), c) is one v_med3, x < y ? x : y is one v_min, and some constant
// combinations need an extra v_mov (a * 999.0 + 1.0 is 2 VALU, a * 0.3 + 0.7 is 1).
const CostModel kRdna3{"rdna3",
  {0, 0, 1, 1, 1, 4, 4, 16, 16,
   16, 16, 20, 20, 20, 20, 16, 16, 4, 4, 4, 4, 4, 20, 4, 4, 8, 36,
   4, 4, 4, 4, 4, 4, 4, 8, 4, 4, 1,
   4, 20, 24, 24, 1, 1},
  1, true, true};
// NVIDIA Ada (sm_89) SASS via ptxas + nvdisasm, in quarter-ALU units (4 = one FP32
// instruction), MUFU at 8x (FP32 : MUFU throughput 128 : 16 per SM per clock).
// Measured op by op like rdna3. Differences from rdna3: clamp = two FMNMX (no med3),
// step = one FSET, sign = 3 instructions, sqrt = one MUFU.SQRT; neg/abs/saturate are
// free modifiers (FADD/FMUL/FFMA.SAT); contraction to FFMA as on AMD. Not modeled:
// FFMA takes one non-inline immediate, a second constant costs a MOV.
const CostModel kNvidia{"nvidia",
  {0, 0, 1, 1, 1, 4, 8, 12, 32,
   32, 32, 36, 36, 36, 36, 32, 32, 4, 4, 4, 4, 4, 36, 4, 4, 4, 68,
   4, 4, 4, 4, 4, 4, 4, 8, 8, 4, 1,
   4, 36, 40, 40, 1, 1},
  1, true};

// Intel Gen9 (Iris 540, Skylake), measured with sopt-opbench on the owner's NUC (D3D11,
// docs/opbench/intel-iris-540.csv), quarter units (4 = one fma, throughput). Not Arc (Xe-HPG
// differs). add/sub/mul/mad/floor/frac/round/ceil/step/min/max one op; neg/abs/saturate free
// modifiers; clamp two ops (no med3), lerp and compare + select two ops, sign ~3.5 ops; the math
// unit (rcp, rsqrt, sqrt, exp2, log2, cos, div as rcp + mul) ~3x an fma, sin a little more;
// pow = log2 + mul + exp2. Contraction to fma as on AMD; no omod, no 3-operand min / max.
const CostModel kIntelGen9{"intel-gen9",
  {0, 0, 1, 1, 1, 4, 4, 14, 12,
   12, 12, 14, 12, 13, 12, 12, 12, 4, 4, 4, 4, 4, 12, 4, 4, 4, 30,
   4, 4, 4, 4, 4, 4, 4, 8, 8, 4, 1,
   4, 16, 20, 20, 1, 1},
  1, true};

// NVIDIA Turing (GTX 1660, TU116, sm_75), measured with sopt-opbench on the owner's PC (D3D11,
// docs/opbench/nvidia-gtx-1660.csv), quarter units (4 = one fma, throughput). add/sub/mul/mad one
// op; neg/abs/saturate free modifiers; the quarter-rate unit (rcp, rsqrt, sqrt, exp2, log2, sin,
// cos, and on Turing also floor/ceil/round/frac) 3 more than the fma it overlaps, exp/log/div the
// same (their mul hides under it), pow two of them. min/max/step/compare/select run on the ALU pipe
// beside the FMAs: one is ~free next to a mad, two cost one op (max3, clamp), so 2 each as an
// additive approximation; lerp two ops, sign ~2 ops. Contraction to fma; no omod, no max3.
const CostModel kNvidiaTuring{"nvidia-turing",
  {0, 0, 1, 1, 1, 12, 12, 8, 12,
   12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 4, 4, 4, 12, 2, 2, 2, 28,
   2, 2, 2, 2, 2, 2, 4, 8, 4, 2, 1,
   4, 16, 20, 20, 1, 1},
  1, true};

// NVIDIA Pascal (GT 1030 = GP108 and GTX 1060 6GB = GP106, docs/opbench/nvidia-gt-1030.csv /
// nvidia-gtx-1060-6gb.csv, the two agree within ~0.3), sopt-opbench, quarter units (4 = one fma,
// throughput). add/sub/mul/mad one op (measured 3.5); neg and saturate free modifiers, but abs is not
// (3.5, one op; compiledCost still counts abs as a free source modifier for every model); the quarter-rate
// unit (rcp, rsqrt, sqrt, exp2, log2, sin, cos) and floor/ceil/round/frac 10, exp/log/div the same (the
// mul hides), pow two of them (24); min/max/step/compares ~1.75 ops (6.8: no free min / max on a second
// pipe as on Turing), so clamp 14 (two of them, no med3) and compare + select 12 (select 5 after the
// 7-unit compare); lerp two ops, sign 10 (~2.5 ops). Contraction to fma; no omod, no max3.
const CostModel kNvidiaPascal{"nvidia-pascal",
  {0, 0, 1, 4, 1, 10, 10, 10, 10,
   10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 4, 4, 4, 10, 7, 7, 7, 24,
   7, 7, 7, 7, 7, 7, 4, 8, 14, 5, 1,
   4, 14, 18, 18, 1, 1},
  1, true};

// NVIDIA Ampere (RTX 3050, docs/opbench/nvidia-rtx-3050.csv) and Blackwell (RTX 5080 / 5090,
// nvidia-rtx-5080.csv / nvidia-rtx-5090.csv), sopt-opbench, quarter units (4 = one fma, throughput).
// Provisional (one Ampere card; Ada not measured yet). Both: neg/abs/saturate free modifiers; the
// quarter-rate unit (rcp, rsqrt, sqrt, exp2, log2, sin, cos) and floor/ceil/round/frac ~6x an fma
// (the second FP32 pipe makes fmas relatively cheaper than on Turing), exp/log/div the same (the mul
// hides), pow two of them; sign ~7x on Ampere / ~4.5x on Blackwell (fxc's sign ends in an int->float
// conversion). min/max/step/compares ~1.25 ops on Ampere, ~0.8 on Blackwell (min/max 1 there, so clamp =
// min + max: fxc writes clamp as max + min); compare + select 10.6 / 8; clamp two min/max. Contraction to fma; no omod, no max3.
const CostModel kNvidiaAmpere{"nvidia-ampere",
  {0, 0, 1, 1, 1, 24, 24, 28, 24,
   24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 4, 4, 4, 24, 5, 5, 5, 52,
   5, 5, 5, 5, 5, 5, 4, 8, 10, 5, 1,
   4, 28, 32, 32, 1, 1},
  1, true};
const CostModel kNvidiaBlackwell{"nvidia-blackwell",
  {0, 0, 1, 1, 1, 23, 23, 18, 23,
   23, 23, 24, 23, 25, 24, 23, 23, 23, 23, 4, 4, 4, 23, 4, 4, 3, 50,
   3, 3, 3, 3, 3, 3, 4, 8, 8, 5, 1,
   4, 27, 31, 31, 1, 1},
  1, true};

// Enumeration order for the measured objectives (rdna3, the nvidia models, intel-gen9): rdna3's cheap ops,
// transcendentals at half cost (rcp/sqrt/rsqrt 8, exp/log/sin/cos/div 12, pow 20, sign 8). Ordering them
// at full cost puts one rsqrt behind every program of ~4 VALU ops; generic order
// reaches them but misorders cheap ops (loses planted problems). Chosen on the bench
// against uniform op count and quarter-cost transcendentals. For nvidia it beats
// nvidia's own costs as order (10/11 examples vs 8/11, planted equal).
const CostModel kSearch{"search",
  {0, 0, 1, 1, 1, 4, 4, 8, 8,
   8, 8, 12, 12, 12, 12, 8, 8, 4, 4, 4, 4, 4, 12, 4, 4, 8, 20,
   4, 4, 4, 4, 4, 4, 4, 8, 4, 4, 1,
   4, 12, 16, 16, 1, 1},
  1, true};
// clang-format on

}  // namespace

const OpInfo& info(Op op) { return kInfo[static_cast<size_t>(op)]; }

std::optional<Op> opFromCall(std::string_view name, uint8_t arity) {
  for (size_t i = 0; i < kInfo.size(); ++i) {
    const auto& oi = kInfo[i];
    if (oi.syntax == Syntax::Call && oi.name == name && oi.arity == arity)
      return static_cast<Op>(i);
  }
  return std::nullopt;
}

uint32_t CostModel::opCost(Op op, unsigned w) const {
  const CostModel& m = *this;
  switch (op) {
    case Op::Input:
    case Op::Const: return 0;
    case Op::Dot: return m[Op::Mul] + (w - 1) * m[Op::Mad];
    case Op::Length: return opCost(Op::Dot, w) + m[Op::Sqrt];
    case Op::Normalize: return opCost(Op::Dot, w) + m[Op::Rsqrt] + w * m[Op::Mul];
    case Op::Distance: return w * m[Op::Sub] + opCost(Op::Length, w);
    // DXC: (x - a) / (b - a), saturate, 3 - 2s (one fma), two multiplies.
    case Op::Smoothstep:
      return w * (2 * m[Op::Sub] + m[Op::Div] + m[Op::Saturate] + m[Op::Mad] + 2 * m[Op::Mul]);
    case Op::Swizzle:
    case Op::Construct: return m[op];
    default: return info(op).shape == Shape::Cmp ? m[op] : w * m[op];
  }
}


// AMD models from sopt-opbench (throughput), scaled so that one plain VALU instruction (each
// card's measured add) is 4: OpBench's mad base carries extra issue cost on AMD (two scalar
// constants: an 8-byte VOP3 fma on RDNA, an extra v_mov on GCN), so its "4 = one fma" overstates
// a plain instruction there.
// RDNA 2 (Radeon 680M iGPU, RX 6950 XT; docs/opbench/amd-radeon-680m-rembrandt-v3.csv /
// amd-radeon-rx-6950-xt.csv, OpBench 0.3.0, the two agree within ~0.2): plain ops 4, neg / abs /
// saturate free, the transcendental unit (rcp, rsqrt, sqrt, exp2, log2) ~10.5, exp / log / sin /
// cos / div ~11, pow 26, sign 19, compare + cndmask 7, lerp 9, clamp 8 (max + min with uniform
// bounds). Output modifier (x * 2 / 4 / 0.5) and v_max3 / v_min3 fold; min over max does not (no
// v_minmax before RDNA 3).
const CostModel kAmdRdna2{"amd-rdna2",
  {0, 0, 1, 1, 1, 4, 4, 19, 10,
   10, 10, 11, 11, 11, 11, 10, 10, 4, 4, 4, 4, 4, 11, 4, 4, 7, 26,
   4, 4, 4, 4, 4, 4, 4, 9, 8, 3, 1,
   4, 14, 18, 18, 1, 1},
  1, true, true, true};
// RDNA 4 (RX 9070 XT, amd-radeon-rx-9070-xt.csv; one card): OpBench's fma base dual-issues, and
// one plain add also measures 4, so units are used as measured: ops that cannot dual-issue cost
// more (floor / ceil / round / frac / clamp 8, min / max 5), the transcendental unit 26 (exp /
// log / sin / cos / div alike), pow 58, sign 38, compare + cndmask 12, lerp 10. Output modifier
// free; max3 / minmax measure 3 extra (folded here like rdna3: v_max3 / v_minmax exist).
const CostModel kAmdRdna4{"amd-rdna4",
  {0, 0, 1, 1, 1, 8, 8, 38, 26,
   26, 26, 26, 26, 26, 26, 26, 26, 8, 8, 4, 5, 4, 26, 5, 5, 12, 58,
   5, 5, 5, 5, 5, 5, 4, 10, 8, 7, 1,
   4, 30, 34, 34, 1, 1},
  1, true, true};
// GCN 5 (Vega iGPU in Renoir, amd-radeon-vega-renoir.csv; one card): plain ops 4, neg / abs /
// saturate free, the transcendental unit 16 (quarter rate), exp / sin / cos 20, pow 35, sign 20,
// compare + cndmask 8, lerp 12, clamp 8. Output modifier and v_max3 / v_min3 fold; min over max is
// an instruction (v_med3 only for constant bounds).
const CostModel kAmdGcn5{"amd-gcn5",
  {0, 0, 1, 1, 1, 4, 4, 20, 16,
   16, 16, 20, 16, 20, 20, 16, 16, 4, 4, 4, 4, 4, 16, 4, 4, 8, 35,
   4, 4, 4, 4, 4, 4, 4, 12, 8, 4, 1,
   4, 20, 24, 24, 1, 1},
  1, true, true, true};
// TeraScale 2 (VLIW5: Radeon HD 7400M, amd-radeon-hd-7400m-as-intel-hd-3000.csv; one card): every
// ALU op one slot (4), abs included (not a free modifier here; compiledCost still treats it as one),
// the transcendental (t) slot 16, sin 25, cos 21, pow 37, sign 17, compare + select 8, lerp 8,
// clamp 8. No output modifier or 3-operand min / max gain (omod2 = omod3).
const CostModel kAmdTerascale2{"amd-terascale2",
  {0, 0, 1, 4, 1, 4, 4, 17, 16,
   16, 16, 16, 16, 25, 21, 16, 16, 4, 4, 4, 4, 4, 16, 4, 4, 4, 37,
   4, 4, 4, 4, 4, 4, 4, 8, 8, 4, 1,
   4, 20, 24, 24, 1, 1},
  1, true};

const CostModel& costGeneric() { return kGeneric; }
const CostModel& costRdna3() { return kRdna3; }
const CostModel& costNvidia() { return kNvidia; }
const CostModel& costNvidiaPascal() { return kNvidiaPascal; }
const CostModel& costNvidiaTuring() { return kNvidiaTuring; }
const CostModel& costNvidiaAmpere() { return kNvidiaAmpere; }
const CostModel& costNvidiaBlackwell() { return kNvidiaBlackwell; }
const CostModel& costIntelGen9() { return kIntelGen9; }
const CostModel& costAmdRdna2() { return kAmdRdna2; }
const CostModel& costAmdRdna4() { return kAmdRdna4; }
const CostModel& costAmdGcn5() { return kAmdGcn5; }
const CostModel& costAmdTerascale2() { return kAmdTerascale2; }
const CostModel& defaultCostModel() { return kRdna3; }
// rdna3 without the context effects (--no-amd-folds).
const CostModel kRdna3NoFolds = [] {
  CostModel m = kRdna3;
  m.amdFolds = false;
  return m;
}();

const CostModel& defaultOrderFor(const CostModel& objective) {
  return &objective == &kRdna3 || &objective == &kRdna3NoFolds || &objective == &kNvidia ||
                 &objective == &kNvidiaPascal || &objective == &kNvidiaTuring || &objective == &kNvidiaAmpere ||
                 &objective == &kNvidiaBlackwell || &objective == &kIntelGen9 || &objective == &kAmdRdna2 ||
                 &objective == &kAmdRdna4 || &objective == &kAmdGcn5 || &objective == &kAmdTerascale2
             ? kSearch
             : objective;
}

const CostModel* withoutAmdFolds(const CostModel* m) { return m == &kRdna3 ? &kRdna3NoFolds : m; }

const CostModel* costModelByName(std::string_view name) {
  if (name == kGeneric.name) return &kGeneric;
  if (name == kRdna3.name) return &kRdna3;
  if (name == kSearch.name || name == "rdna3-search") return &kSearch;
  if (name == kNvidia.name) return &kNvidia;
  if (name == kNvidiaPascal.name) return &kNvidiaPascal;
  if (name == kNvidiaTuring.name) return &kNvidiaTuring;
  if (name == kNvidiaAmpere.name) return &kNvidiaAmpere;
  if (name == kNvidiaBlackwell.name) return &kNvidiaBlackwell;
  if (name == kIntelGen9.name) return &kIntelGen9;
  if (name == kAmdRdna2.name) return &kAmdRdna2;
  if (name == kAmdRdna4.name) return &kAmdRdna4;
  if (name == kAmdGcn5.name) return &kAmdGcn5;
  if (name == kAmdTerascale2.name) return &kAmdTerascale2;
  return nullptr;
}

}  // namespace sopt
