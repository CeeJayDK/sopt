#include "ir/ops.hpp"

namespace sopt {
namespace {

constexpr Type F = Type::Float;
constexpr Type B = Type::Bool;

// Costs are placeholder integers. All non-leaf costs must be >= 1 so that cost
// levels are well-founded (operands of a level-c node have cost < c).
// clang-format off
const std::array<OpInfo, static_cast<size_t>(Op::Count)> kInfo = {{
  //  name        sym   syntax           ar res  args       comm   exact  base   cost prec
  {"input",    "",   Syntax::Leaf,    0, F, {F, F, F}, false, true,  true,  0, 9},
  {"const",    "",   Syntax::Leaf,    0, F, {F, F, F}, false, true,  true,  0, 9},
  {"neg",      "-",  Syntax::Prefix,  1, F, {F, F, F}, false, true,  true,  1, 7},
  {"abs",      "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  1, 9},
  {"saturate", "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  1, 9},
  {"floor",    "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  2, 9},
  {"frac",     "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  3, 9},
  {"sign",     "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  2, 9},
  {"sqrt",     "",   Syntax::Call,    1, F, {F, F, F}, false, true,  true,  5, 9},
  {"rsqrt",    "",   Syntax::Call,    1, F, {F, F, F}, false, false, true,  5, 9},
  {"exp",      "",   Syntax::Call,    1, F, {F, F, F}, false, false, false, 6, 9},
  {"log",      "",   Syntax::Call,    1, F, {F, F, F}, false, false, false, 6, 9},
  {"sin",      "",   Syntax::Call,    1, F, {F, F, F}, false, false, false, 6, 9},
  {"cos",      "",   Syntax::Call,    1, F, {F, F, F}, false, false, false, 6, 9},
  {"add",      "+",  Syntax::Infix,   2, F, {F, F, F}, true,  true,  true,  2, 5},
  {"sub",      "-",  Syntax::Infix,   2, F, {F, F, F}, false, true,  true,  2, 5},
  {"mul",      "*",  Syntax::Infix,   2, F, {F, F, F}, true,  true,  true,  3, 6},
  {"div",      "/",  Syntax::Infix,   2, F, {F, F, F}, false, true,  true,  5, 6},
  {"min",      "",   Syntax::Call,    2, F, {F, F, F}, true,  true,  true,  2, 9},
  {"max",      "",   Syntax::Call,    2, F, {F, F, F}, true,  true,  true,  2, 9},
  {"step",     "",   Syntax::Call,    2, F, {F, F, F}, false, true,  true,  2, 9},
  {"pow",      "",   Syntax::Call,    2, F, {F, F, F}, false, false, false, 8, 9},
  {"lt",       "<",  Syntax::Infix,   2, B, {F, F, F}, false, true,  true,  2, 3},
  {"le",       "<=", Syntax::Infix,   2, B, {F, F, F}, false, true,  true,  2, 3},
  {"gt",       ">",  Syntax::Infix,   2, B, {F, F, F}, false, true,  true,  2, 3},
  {"ge",       ">=", Syntax::Infix,   2, B, {F, F, F}, false, true,  true,  2, 3},
  {"eq",       "==", Syntax::Infix,   2, B, {F, F, F}, true,  true,  true,  2, 3},
  {"ne",       "!=", Syntax::Infix,   2, B, {F, F, F}, true,  true,  true,  2, 3},
  {"mad",      "",   Syntax::Call,    3, F, {F, F, F}, false, true,  true,  4, 9},
  {"lerp",     "",   Syntax::Call,    3, F, {F, F, F}, false, true,  true,  6, 9},
  {"clamp",    "",   Syntax::Call,    3, F, {F, F, F}, false, true,  true,  3, 9},
  {"select",   "?:", Syntax::Ternary, 3, F, {B, F, F}, false, true,  true,  2, 2},
}};
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

}  // namespace sopt
