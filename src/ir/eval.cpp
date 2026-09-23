#include "ir/eval.hpp"

#include <algorithm>
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
    // vector ops go through evalNode
    case Op::Dot:
    case Op::Length:
    case Op::Normalize:
    case Op::Distance:
    case Op::Swizzle:
    case Op::Construct:
      return;
    case Op::Neg: SOPT_LOOP1(-x)
    case Op::Abs: SOPT_LOOP1(std::fabs(x))
    case Op::Saturate: SOPT_LOOP1(fSaturate(x))
    case Op::Floor: SOPT_LOOP1(std::floor(x))
    case Op::Frac: SOPT_LOOP1(x - std::floor(x))
    case Op::Sign: SOPT_LOOP1(fSign(x))
    case Op::Sqrt: SOPT_LOOP1(std::sqrt(x))
    case Op::Rsqrt: SOPT_LOOP1(1.0f / std::sqrt(x))
    case Op::Rcp: SOPT_LOOP1(1.0f / x)
    case Op::Exp: SOPT_LOOP1(std::exp(x))
    case Op::Log: SOPT_LOOP1(std::log(x))
    case Op::Sin: SOPT_LOOP1(std::sin(x))
    case Op::Cos: SOPT_LOOP1(std::cos(x))
    case Op::Add: SOPT_LOOP2(x + y)
    case Op::Sub: SOPT_LOOP2(x - y)
    case Op::Mul: SOPT_LOOP2(x * y)
    case Op::Div:
      if (profile.divRcp) { SOPT_LOOP2(x * (1.0f / y)) }
      SOPT_LOOP2(x / y)
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
      if (profile.contract) { SOPT_LOOP3(std::fma(z, y - x, x)) }
      SOPT_LOOP3(x + z * (y - x))
    case Op::Clamp: SOPT_LOOP3(fMin(fMax(x, y), z))
    case Op::Select: SOPT_LOOP3(x != 0.0f ? y : z)
  }
}

namespace {

// out = dot(a, b) over w components.
void dotArray(const float* const* a, const float* const* b, unsigned w, float* out, size_t n,
              const Profile& p) {
  for (size_t i = 0; i < n; ++i) out[i] = a[0][i] * b[0][i];
  for (unsigned c = 1; c < w; ++c) {
    if (p.madFused)
      for (size_t i = 0; i < n; ++i) out[i] = std::fma(a[c][i], b[c][i], out[i]);
    else
      for (size_t i = 0; i < n; ++i) out[i] = out[i] + a[c][i] * b[c][i];
  }
}

}  // namespace

void evalNode(const Node& node, const Type* argTypes, const float* const (*arg)[4],
              float* const* out, size_t n, const Profile& profile, std::vector<float>& tmp) {
  const Op op = node.op;
  const unsigned w = width(node.type);
  switch (info(op).shape) {
    case Shape::Leaf: return;
    case Shape::Comp:
    case Shape::Select:
    case Shape::Cmp:
      for (unsigned c = 0; c < w; ++c) {
        const float* p[3] = {nullptr, nullptr, nullptr};
        for (unsigned k = 0; k < node.nargs; ++k) p[k] = arg[k][width(argTypes[k]) == 1 ? 0 : c];
        evalArray(op, p[0], p[1], p[2], out[c], n, profile);
      }
      return;
    case Shape::Reduce: {
      const unsigned aw = width(argTypes[0]);
      const float* const* a = arg[0];
      if (op == Op::Distance) {  // length(a - b)
        tmp.resize(4 * n);
        const float* d[4];
        for (unsigned c = 0; c < aw; ++c) {
          evalArray(Op::Sub, arg[0][c], arg[1][c], nullptr, tmp.data() + c * n, n, profile);
          d[c] = tmp.data() + c * n;
        }
        dotArray(d, d, aw, out[0], n, profile);
      } else {
        dotArray(a, op == Op::Dot ? arg[1] : a, aw, out[0], n, profile);
      }
      if (op != Op::Dot) evalArray(Op::Sqrt, out[0], nullptr, nullptr, out[0], n, profile);
      return;
    }
    case Shape::Same: {  // normalize
      tmp.resize(n);
      dotArray(arg[0], arg[0], w, tmp.data(), n, profile);
      evalArray(Op::Rsqrt, tmp.data(), nullptr, nullptr, tmp.data(), n, profile);
      for (unsigned c = 0; c < w; ++c)
        evalArray(Op::Mul, arg[0][width(argTypes[0]) == 1 ? 0 : c], tmp.data(), nullptr, out[c], n,
                  profile);
      return;
    }
    case Shape::Swizzle:
      for (unsigned c = 0; c < w; ++c) {
        const float* src = arg[0][node.swz[c]];
        std::copy(src, src + n, out[c]);
      }
      return;
    case Shape::Construct: {
      unsigned c = 0;
      for (unsigned k = 0; k < node.nargs; ++k)
        for (unsigned j = 0; j < width(argTypes[k]); ++j, ++c) std::copy(arg[k][j], arg[k][j] + n, out[c]);
      return;
    }
  }
}

uint32_t foldCopyNode(const Expr& src, uint32_t i, const std::vector<uint32_t>& map, ExprBuilder& b) {
  const Node& n = src.nodes[i];
  const auto& ns = b.nodes();
  bool allConst = n.op != Op::Input && n.op != Op::Const;
  for (unsigned k = 0; k < operandCount(n); ++k) allConst = allConst && ns[map[n.args[k]]].op == Op::Const;
  if (allConst) {
    Node m = n;
    Type ts[4];
    float buf[4][4];
    const float* ptr[4][4];
    for (unsigned k = 0; k < operandCount(n); ++k) {
      m.args[k] = map[n.args[k]];
      ts[k] = ns[m.args[k]].type;
      for (unsigned c = 0; c < 4; ++c) {
        buf[k][c] = ns[m.args[k]].value[c];
        ptr[k][c] = &buf[k][c];
      }
    }
    float res[4] = {0, 0, 0, 0};
    float* out[4] = {&res[0], &res[1], &res[2], &res[3]};
    std::vector<float> tmp;
    evalNode(m, ts, ptr, out, 1, kProfileRef, tmp);
    return b.constant(n.type, res);
  }
  switch (n.op) {
    case Op::Const: return b.constant(n.type, n.value);
    case Op::Swizzle: return b.swizzle(map[n.args[0]], n.swz, width(n.type));
    case Op::Construct: {
      uint32_t a[4];
      for (unsigned k = 0; k < n.nargs; ++k) a[k] = map[n.args[k]];
      return b.construct(a, n.nargs);
    }
    default:
      return b.op(n.op, map[n.args[0]], operandCount(n) > 1 ? map[n.args[1]] : 0,
                  operandCount(n) > 2 ? map[n.args[2]] : 0);
  }
}

Expr specializeCompileTime(const Expr& e, const std::vector<InputDecl>& inputs,
                           std::vector<InputDecl>& remaining, std::vector<uint32_t>* oldIndex) {
  remaining.clear();
  std::vector<uint32_t> newIndex(inputs.size(), UINT32_MAX);
  for (uint32_t i = 0; i < inputs.size(); ++i)
    if (!inputs[i].compileTime) {
      newIndex[i] = static_cast<uint32_t>(remaining.size());
      if (oldIndex) oldIndex->push_back(i);
      remaining.push_back(inputs[i]);
    }
  ExprBuilder b;
  std::vector<uint32_t> map(e.nodes.size());
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    if (n.op == Op::Input) {
      const InputDecl& d = inputs[n.input];
      if (d.compileTime) {
        const float v[4] = {float(d.value), float(d.value), float(d.value), float(d.value)};
        map[i] = b.constant(d.type, v);
      } else {
        map[i] = b.input(newIndex[n.input], d.type);
      }
    } else {
      map[i] = foldCopyNode(e, i, map, b);
    }
  }
  return b.finish(map[e.root]);
}

float evalScalar(Op op, float a, float b, float c, const Profile& profile) {
  float out = 0.0f;
  evalArray(op, &a, &b, &c, &out, 1, profile);
  return out;
}

}  // namespace sopt
