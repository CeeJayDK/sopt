#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sopt {

enum class Type : uint8_t { Float, Bool };

enum class Op : uint8_t {
  Input, Const,
  // unary
  Neg, Abs, Saturate, Floor, Frac, Sign, Sqrt, Rsqrt, Rcp, Exp, Log, Sin, Cos,
  // binary
  Add, Sub, Mul, Div, Min, Max, Step, Pow,
  Lt, Le, Gt, Ge, Eq, Ne,
  // ternary
  Mad, Lerp, Clamp, Select,
  Count
};

enum class Syntax : uint8_t { Leaf, Call, Prefix, Infix, Ternary };

struct OpInfo {
  std::string_view name;    // FX function name (Call) or internal name
  std::string_view symbol;  // operator symbol (Prefix/Infix)
  Syntax syntax;
  uint8_t arity;
  Type result;
  std::array<Type, 3> args;
  bool commutative;
  bool exact;     // bit-exact on IEEE 754 GPUs (given a semantic profile)
  bool base;      // enumerated by default; others only if present in the target
  uint8_t prec;   // printing precedence, higher binds tighter
};

const OpInfo& info(Op op);
// Intrinsics that are only shorthand for several instructions (lerp = sub + fma,
// step = cmp + cndmask), as opposed to single instructions or modifiers.
inline bool isPureHelper(Op op) { return op == Op::Lerp || op == Op::Step; }
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

  uint16_t operator[](Op op) const { return cost[static_cast<size_t>(op)]; }
  bool fusesIntoAdd(Op operand) const {
    return fusedAdd && (operand == Op::Mul || (divIsMul && operand == Op::Div));
  }
};

// generic: the M1 placeholder weights, no contraction.
// rdna3: AMD RDNA3 ISA (RGA gfx1100) in quarter-VALU units: 4 = one VALU op,
//   transcendentals 16 (VALU + 3 for quarter rate, as fxstat's COST), free source
//   and output modifiers (neg, abs, saturate) 1, clamp = v_med3, contraction on.
const CostModel& costGeneric();
const CostModel& costRdna3();
// Default is rdna3 (searched in rdna3-search order); --isa ranks by the real ISA.
const CostModel& defaultCostModel();
const CostModel* costModelByName(std::string_view name);
// Enumeration order used when none is given: rdna3-search for rdna3, else the model.
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
};

inline constexpr Profile kProfileRef{"ref", false, false};
inline constexpr Profile kProfileMix{"mix", true, false};
inline constexpr Profile kProfileFma{"fma", false, true};
inline constexpr Profile kProfileGpu{"gpu", false, true, true, true};
inline constexpr std::array<Profile, 4> kAllProfiles{kProfileRef, kProfileMix, kProfileFma,
                                                     kProfileGpu};

}  // namespace sopt
