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
  Neg, Abs, Saturate, Floor, Frac, Sign, Sqrt, Rsqrt, Exp, Log, Sin, Cos,
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
  uint16_t cost;  // static cost, placeholder weights (calibrated in M6)
  uint8_t prec;   // printing precedence, higher binds tighter
};

const OpInfo& info(Op op);
std::optional<Op> opFromCall(std::string_view name, uint8_t arity);

// Backend semantic profiles. The same FX source can evaluate differently:
// HLSL lerp is a + t*(b-a), GLSL/SPIR-V mix is a*(1-t) + b*t, and mad may be fused.
struct Profile {
  std::string_view name;
  bool lerpMix;
  bool madFused;
};

inline constexpr Profile kProfileRef{"ref", false, false};
inline constexpr Profile kProfileMix{"mix", true, false};
inline constexpr Profile kProfileFma{"fma", false, true};
inline constexpr std::array<Profile, 3> kAllProfiles{kProfileRef, kProfileMix, kProfileFma};

}  // namespace sopt
