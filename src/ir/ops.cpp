#include "ir/ops.hpp"

namespace sopt {
namespace {

constexpr Type F = Type::Float;
constexpr Type B = Type::Bool;

// clang-format off
const std::array<OpInfo, static_cast<size_t>(Op::Count)> kInfo = {{
  //  name        sym   syntax           ar res  args       comm   exact  base   prec
  {"input",    "",   Syntax::Leaf,    0, F, {F, F, F}, false, true,  true,  9},
  {"const",    "",   Syntax::Leaf,    0, F, {F, F, F}, false, true,  true,  9},
  {"neg",      "-",  Syntax::Prefix,  1, F, {F, F, F}, false, true,  true,  7},
  {"abs",      "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  9},
  {"saturate", "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  9},
  {"floor",    "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  9},
  {"frac",     "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  9},
  {"sign",     "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  9},
  {"sqrt",     "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  9},
  {"rsqrt",    "",   Syntax::Call,    1, F, {F, F, F}, false, false, true,  9},
  {"rcp",      "",   Syntax::Call,    1, F, {F, F, F}, false, false, true,  9},
  {"exp",      "",   Syntax::Call,    1, F, {F, F, F}, false, false, false, 9},
  {"log",      "",   Syntax::Call,    1, F, {F, F, F}, false, false, false, 9},
  {"sin",      "",   Syntax::Call,    1, F, {F, F, F}, false, false, false, 9},
  {"cos",      "",   Syntax::Call,    1, F, {F, F, F}, false, false, false, 9},
  {"add",      "+",  Syntax::Infix,   2, F, {F, F, F}, true,  true,  true,  5},
  {"sub",      "-",  Syntax::Infix,   2, F, {F, F, F}, false, true,  true,  5},
  {"mul",      "*",  Syntax::Infix,   2, F, {F, F, F}, true,  true,  true,  6},
  // GPUs lower a / b to a * rcp(b) with an approximate rcp: never bit-exact.
  {"div",      "/",  Syntax::Infix,   2, F, {F, F, F}, false, false, true,  6},
  {"min",      "",   Syntax::Call,    2, F, {F, F, F}, true,  true,  true,  9},
  {"max",      "",   Syntax::Call,    2, F, {F, F, F}, true,  true,  true,  9},
  {"step",     "",   Syntax::Call,    2, F, {F, F, F}, false, true,  true,  9},
  {"pow",      "",   Syntax::Call,    2, F, {F, F, F}, false, false, false, 9},
  {"lt",       "<",  Syntax::Infix,   2, B, {F, F, F}, false, true,  true,  3},
  {"le",       "<=", Syntax::Infix,   2, B, {F, F, F}, false, true,  true,  3},
  {"gt",       ">",  Syntax::Infix,   2, B, {F, F, F}, false, true,  true,  3},
  {"ge",       ">=", Syntax::Infix,   2, B, {F, F, F}, false, true,  true,  3},
  {"eq",       "==", Syntax::Infix,   2, B, {F, F, F}, true,  true,  true,  3},
  {"ne",       "!=", Syntax::Infix,   2, B, {F, F, F}, true,  true,  true,  3},
  {"mad",      "",   Syntax::Call,    3, F, {F, F, F}, false, true,  true,  9},
  {"lerp",     "",   Syntax::Call,    3, F, {F, F, F}, false, true,  true,  9},
  {"clamp",    "",   Syntax::Call,    3, F, {F, F, F}, false, true,  true,  9},
  {"select",   "?:", Syntax::Ternary, 3, F, {B, F, F}, false, true,  true,  2},
}};

// Per-op costs in Op order (input, const, neg, abs, saturate, floor, frac, sign, sqrt,
// rsqrt, rcp, exp, log, sin, cos, add, sub, mul, div, min, max, step, pow,
// lt, le, gt, ge, eq, ne, mad, lerp, clamp, select).
const CostModel kGeneric{"generic",
  {0, 0, 1, 1, 1, 2, 3, 2, 5,
   5, 5, 6, 6, 6, 6, 2, 2, 3, 5, 2, 2, 2, 8,
   2, 2, 2, 2, 2, 2, 4, 6, 3, 2},
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
   16, 16, 20, 20, 20, 20, 4, 4, 4, 20, 4, 4, 8, 36,
   4, 4, 4, 4, 4, 4, 4, 8, 4, 4},
  1, true};
// Enumeration order for the rdna3 objective: the same cheap ops, transcendentals at
// half cost (rcp/sqrt/rsqrt 8, exp/log/sin/cos/div 12, pow 20, sign 8). Ordering them
// at full cost puts one rsqrt behind every program of ~4 VALU ops; generic order
// reaches them but misorders cheap ops (loses planted problems). Chosen on the bench
// against uniform op count and quarter-cost transcendentals.
const CostModel kRdna3Search{"rdna3-search",
  {0, 0, 1, 1, 1, 4, 4, 8, 8,
   8, 8, 12, 12, 12, 12, 4, 4, 4, 12, 4, 4, 8, 20,
   4, 4, 4, 4, 4, 4, 4, 8, 4, 4},
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

const CostModel& costGeneric() { return kGeneric; }
const CostModel& costRdna3() { return kRdna3; }
const CostModel& defaultCostModel() { return kRdna3; }
const CostModel& defaultOrderFor(const CostModel& objective) {
  return &objective == &kRdna3 ? kRdna3Search : objective;
}

const CostModel* costModelByName(std::string_view name) {
  if (name == kGeneric.name) return &kGeneric;
  if (name == kRdna3.name) return &kRdna3;
  if (name == kRdna3Search.name) return &kRdna3Search;
  return nullptr;
}

}  // namespace sopt
