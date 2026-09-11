#include "ir/eval.hpp"

#include <cmath>

namespace sopt {
namespace {

// All literals are float (f suffix) so nothing is promoted to double.
inline float fSaturate(float a) { return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); }
inline float fMin(float a, float b) { return b < a ? b : a; }
inline float fMax(float a, float b) { return a < b ? b : a; }
inline float fSign(float a) { return a > 0.0f ? 1.0f : (a < 0.0f ? -1.0f : 0.0f); }
inline float fBool(bool v) { return v ? 1.0f : 0.0f; }

#define SOPT_LOOP1(expr)                      \
  for (size_t i = 0; i < n; ++i) {            \
    const float x = a[i];                     \
    out[i] = (expr);                          \
  }                                           \
  return;
#define SOPT_LOOP2(expr)                      \
  for (size_t i = 0; i < n; ++i) {            \
    const float x = a[i], y = b[i];           \
    out[i] = (expr);                          \
  }                                           \
  return;
#define SOPT_LOOP3(expr)                      \
  for (size_t i = 0; i < n; ++i) {            \
    const float x = a[i], y = b[i], z = c[i]; \
    out[i] = (expr);                          \
  }                                           \
  return;

}  // namespace

void evalArray(Op op, const float* a, const float* b, const float* c, float* out, size_t n,
               const Profile& profile) {
  switch (op) {
    case Op::Input:
    case Op::Const:
    case Op::Count:
      return;
    case Op::Neg: SOPT_LOOP1(-x)
    case Op::Abs: SOPT_LOOP1(std::fabs(x))
    case Op::Saturate: SOPT_LOOP1(fSaturate(x))
    case Op::Floor: SOPT_LOOP1(std::floor(x))
    case Op::Frac: SOPT_LOOP1(x - std::floor(x))
    case Op::Sign: SOPT_LOOP1(fSign(x))
    case Op::Sqrt: SOPT_LOOP1(std::sqrt(x))
    case Op::Rsqrt: SOPT_LOOP1(1.0f / std::sqrt(x))
    case Op::Exp: SOPT_LOOP1(std::exp(x))
    case Op::Log: SOPT_LOOP1(std::log(x))
    case Op::Sin: SOPT_LOOP1(std::sin(x))
    case Op::Cos: SOPT_LOOP1(std::cos(x))
    case Op::Add: SOPT_LOOP2(x + y)
    case Op::Sub: SOPT_LOOP2(x - y)
    case Op::Mul: SOPT_LOOP2(x * y)
    case Op::Div: SOPT_LOOP2(x / y)
    case Op::Min: SOPT_LOOP2(fMin(x, y))
    case Op::Max: SOPT_LOOP2(fMax(x, y))
    case Op::Step: SOPT_LOOP2(fBool(y >= x))  // step(edge, x)
    case Op::Pow: SOPT_LOOP2(std::pow(x, y))
    case Op::Lt: SOPT_LOOP2(fBool(x < y))
    case Op::Le: SOPT_LOOP2(fBool(x <= y))
    case Op::Gt: SOPT_LOOP2(fBool(x > y))
    case Op::Ge: SOPT_LOOP2(fBool(x >= y))
    case Op::Eq: SOPT_LOOP2(fBool(x == y))
    case Op::Ne: SOPT_LOOP2(fBool(x != y))
    case Op::Mad:
      if (profile.madFused) { SOPT_LOOP3(std::fma(x, y, z)) }
      SOPT_LOOP3(x * y + z)
    case Op::Lerp:
      if (profile.lerpMix) { SOPT_LOOP3(x * (1.0f - z) + y * z) }
      SOPT_LOOP3(x + z * (y - x))
    case Op::Clamp: SOPT_LOOP3(fMin(fMax(x, y), z))
    case Op::Select: SOPT_LOOP3(x != 0.0f ? y : z)
  }
}

float evalScalar(Op op, float a, float b, float c, const Profile& profile) {
  float out = 0.0f;
  evalArray(op, &a, &b, &c, &out, 1, profile);
  return out;
}

}  // namespace sopt
