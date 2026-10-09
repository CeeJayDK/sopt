#include "verify/bound.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>

#include "verify/verify.hpp"

namespace sopt {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kU = 0x1p-24;       // unit roundoff of float32
constexpr double kInexact = 0x1p-22;  // rcp, rsqrt, sqrt, exp, log, sin, cos, pow: 2 ulp
constexpr double kDiv = 0x1p-21;      // a * rcp(b): 4 ulp
constexpr double kFtz = 0x1p-126;     // denormals flushed, per rounded op
constexpr double kFltMax = 3.4028234663852886e38;
constexpr unsigned kMaxSlots = 16;
constexpr unsigned kMaxHessSlots = 8;  // second-order centered form up to this many slots
constexpr unsigned kHess = kMaxHessSlots * (kMaxHessSlots + 1) / 2;
unsigned hidx(unsigned s, unsigned r) { return s >= r ? s * (s + 1) / 2 + r : r * (r + 1) / 2 + s; }

double down(double x) { return std::nextafter(x, -kInf); }
double up(double x) { return std::nextafter(x, kInf); }

// Closed interval of doubles; every operation rounds outward.
struct I {
  double lo = 0.0, hi = 0.0;
};
I pt(double v) { return {v, v}; }
I out(double lo, double hi) { return {down(lo), up(hi)}; }
bool finite(I a) { return std::isfinite(a.lo) && std::isfinite(a.hi); }
bool zero(I a) { return a.lo == 0.0 && a.hi == 0.0; }
bool has0(I a) { return a.lo <= 0.0 && a.hi >= 0.0; }
double mag(I a) { return std::max(std::fabs(a.lo), std::fabs(a.hi)); }
double mig(I a) { return has0(a) ? 0.0 : std::min(std::fabs(a.lo), std::fabs(a.hi)); }
I widen(I a, double e) { return e > 0.0 ? out(a.lo - e, a.hi + e) : a; }

I add(I a, I b) { return out(a.lo + b.lo, a.hi + b.hi); }
I sub(I a, I b) { return out(a.lo - b.hi, a.hi - b.lo); }
I neg(I a) { return {-a.hi, -a.lo}; }
double mulp(double x, double y) { return x == 0.0 || y == 0.0 ? 0.0 : x * y; }
I mul(I a, I b) {
  if (zero(a) || zero(b)) return pt(0.0);
  const double p[4] = {mulp(a.lo, b.lo), mulp(a.lo, b.hi), mulp(a.hi, b.lo), mulp(a.hi, b.hi)};
  return out(*std::min_element(p, p + 4), *std::max_element(p, p + 4));
}
I recip(I b) {
  if (has0(b)) return {-kInf, kInf};
  return out(1.0 / b.hi, 1.0 / b.lo);
}
I div(I a, I b) { return zero(a) && !has0(b) ? pt(0.0) : mul(a, recip(b)); }
I sqr(I a) {
  const double l = mulp(mig(a), mig(a)), h = mulp(mag(a), mag(a));
  return out(l, h);
}
I isqrt(I a) { return out(std::sqrt(std::max(a.lo, 0.0)), std::sqrt(std::max(a.hi, 0.0))); }
I iexp(I a) { return {std::max(0.0, down(down(std::exp(a.lo)))), up(up(std::exp(a.hi)))}; }
I ilog(I a) {
  return {a.lo <= 0.0 ? -kInf : down(down(std::log(a.lo))), a.hi <= 0.0 ? -kInf : up(up(std::log(a.hi)))};
}
// sin over an interval: endpoints and the extrema inside (with slack for libm's error).
I isin(I a) {
  constexpr double kPi = 3.141592653589793, k2Pi = 6.283185307179586;
  if (!finite(a) || a.hi - a.lo >= k2Pi) return {-1.0, 1.0};
  const double s0 = std::sin(a.lo), s1 = std::sin(a.hi);
  double lo = std::min(s0, s1) - 1e-15, hi = std::max(s0, s1) + 1e-15;
  auto contains = [&](double base) {  // base + 2k pi in [lo - slack, hi + slack] for some k
    const double k = std::ceil((a.lo - base) / k2Pi - 1e-12);
    return base + k * k2Pi <= a.hi + 1e-12;
  };
  if (contains(kPi / 2)) hi = 1.0;
  if (contains(-kPi / 2)) lo = -1.0;
  return {std::max(lo, -1.0), std::min(hi, 1.0)};
}
I icos(I a) { return isin(add(a, pt(1.5707963267948966))); }
I imin(I a, I b) { return {std::min(a.lo, b.lo), std::min(a.hi, b.hi)}; }
I imax(I a, I b) { return {std::max(a.lo, b.lo), std::max(a.hi, b.hi)}; }
I iabs(I a) {
  if (a.lo >= 0.0) return a;
  if (a.hi <= 0.0) return neg(a);
  return {0.0, std::max(-a.lo, a.hi)};
}
I isat(I a) { return {std::clamp(a.lo, 0.0, 1.0), std::clamp(a.hi, 0.0, 1.0)}; }

// One scalar value: exact-value enclosure, its gradient over the input slots (for the
// centered form), and a bound on the float32 evaluation's error.
struct Val {
  I x;
  std::array<I, kMaxSlots> g{};
  std::array<I, kHess> h{};  // Hessian (lower triangle, hidx), second-order centered form
  double e = 0.0;
  bool gok = true;   // g is valid (the value is differentiable or Lipschitz on the box)
  bool hok = true;   // h is valid (twice differentiable on the box)
  bool fail = false; // undefined somewhere in the box, or a step that cannot be decided
};

// Result enclosure and partial derivatives of an op at given argument intervals.
struct OpEval {
  I v;
  I d[3];
  I dd[3][3]{};        // second partials (symmetric)
  bool smooth = true;  // dd valid: no kink inside the argument intervals
  bool fail = false;
};

OpEval evalOp(Op op, const I* a, unsigned nargs) {
  OpEval r;
  const I x = a[0], y = nargs > 1 ? a[1] : I{}, z = nargs > 2 ? a[2] : I{};
  switch (op) {
    case Op::Neg: r.v = neg(x); r.d[0] = pt(-1.0); break;
    case Op::Abs:
      r.v = iabs(x);
      r.d[0] = x.lo >= 0.0 ? pt(1.0) : (x.hi <= 0.0 ? pt(-1.0) : I{-1.0, 1.0});
      r.smooth = x.lo > 0.0 || x.hi < 0.0;
      break;
    case Op::Saturate:
      r.v = isat(x);
      r.d[0] = (x.lo >= 0.0 && x.hi <= 1.0) ? pt(1.0) : ((x.hi <= 0.0 || x.lo >= 1.0) ? pt(0.0) : I{0.0, 1.0});
      r.smooth = (x.lo > 0.0 && x.hi < 1.0) || x.hi < 0.0 || x.lo > 1.0;
      break;
    case Op::Sqrt:
      if (x.lo < 0.0) r.fail = true;
      r.v = isqrt(x);
      r.d[0] = div(pt(0.5), r.v);
      r.dd[0][0] = div(pt(-0.25), mul(r.v, x));  // -x^(-3/2) / 4
      break;
    case Op::Rsqrt:
      if (x.lo <= 0.0) r.fail = true;
      r.v = recip(isqrt(x));
      r.d[0] = mul(pt(-0.5), div(r.v, x));
      r.dd[0][0] = mul(pt(0.75), div(r.v, sqr(x)));  // 3/4 x^(-5/2)
      break;
    case Op::Rcp:
      if (has0(x)) r.fail = true;
      r.v = recip(x);
      r.d[0] = neg(sqr(r.v));
      r.dd[0][0] = mul(pt(2.0), mul(r.v, sqr(r.v)));
      break;
    case Op::Exp: r.v = iexp(x); r.d[0] = r.dd[0][0] = r.v; break;
    case Op::Log:
      if (x.lo <= 0.0) r.fail = true;
      r.v = ilog(x);
      r.d[0] = recip(x);
      r.dd[0][0] = neg(sqr(r.d[0]));
      break;
    case Op::Sin: r.v = isin(x); r.d[0] = icos(x); r.dd[0][0] = neg(r.v); break;
    case Op::Cos: r.v = icos(x); r.d[0] = neg(isin(x)); r.dd[0][0] = neg(r.v); break;
    case Op::Exp2: {  // 2^x = e^(x ln 2)
      const double l2 = 0.6931471805599453;
      r.v = iexp(mul(x, I{down(l2), up(l2)}));
      r.d[0] = mul(r.v, I{down(l2), up(l2)});
      r.dd[0][0] = mul(r.d[0], I{down(l2), up(l2)});
      break;
    }
    case Op::Log2: {
      if (x.lo <= 0.0) r.fail = true;
      const I il2 = I{down(1.4426950408889634), up(1.4426950408889634)};
      r.v = mul(ilog(x), il2);
      r.d[0] = mul(recip(x), il2);
      r.dd[0][0] = neg(mul(sqr(recip(x)), il2));
      break;
    }
    case Op::Add: r.v = add(x, y); r.d[0] = r.d[1] = pt(1.0); break;
    case Op::Sub: r.v = sub(x, y); r.d[0] = pt(1.0); r.d[1] = pt(-1.0); break;
    case Op::Mul: r.v = mul(x, y); r.d[0] = y; r.d[1] = x; r.dd[0][1] = r.dd[1][0] = pt(1.0); break;
    case Op::Div:
      if (has0(y)) r.fail = true;
      r.v = div(x, y);
      r.d[0] = recip(y);
      r.d[1] = neg(div(r.v, y));
      r.dd[0][1] = r.dd[1][0] = neg(sqr(r.d[0]));             // -1 / y^2
      r.dd[1][1] = mul(pt(2.0), div(r.v, sqr(y)));            // 2x / y^3
      break;
    case Op::Min:
    case Op::Max: {
      const bool mx = op == Op::Max;
      r.v = mx ? imax(x, y) : imin(x, y);
      const bool xWins = mx ? x.lo > y.hi : x.hi < y.lo, yWins = mx ? y.lo > x.hi : y.hi < x.lo;
      r.d[0] = xWins ? pt(1.0) : (yWins ? pt(0.0) : I{0.0, 1.0});
      r.d[1] = yWins ? pt(1.0) : (xWins ? pt(0.0) : I{0.0, 1.0});
      r.smooth = xWins || yWins;
      break;
    }
    case Op::Pow: {
      // x^y = exp(y log x) for x > 0; x = 0 only with y > 0 (0^y = 0); x < 0 undefined.
      if (x.lo < 0.0 || (x.lo == 0.0 && y.lo <= 0.0)) r.fail = true;
      const I lx = ilog(x);
      r.v = iexp(mul(y, lx));
      if (x.lo == 0.0) r.v.lo = 0.0;
      r.d[0] = mul(y, iexp(mul(sub(y, pt(1.0)), lx)));  // y x^(y-1)
      r.d[1] = mul(lx, r.v);                            // log(x) x^y
      const I xm1 = iexp(mul(sub(y, pt(1.0)), lx));     // x^(y-1)
      r.dd[0][0] = mul(mul(y, sub(y, pt(1.0))), iexp(mul(sub(y, pt(2.0)), lx)));
      r.dd[0][1] = r.dd[1][0] = mul(xm1, add(pt(1.0), mul(y, lx)));
      r.dd[1][1] = mul(sqr(lx), r.v);
      break;
    }
    case Op::Mad:
      r.v = add(mul(x, y), z);
      r.d[0] = y;
      r.d[1] = x;
      r.d[2] = pt(1.0);
      r.dd[0][1] = r.dd[1][0] = pt(1.0);
      break;
    case Op::Lerp: {  // x + z (y - x) = (1 - z) x + z y
      const I f1 = add(x, mul(z, sub(y, x))), f2 = add(mul(sub(pt(1.0), z), x), mul(z, y));
      r.v = {std::max(f1.lo, f2.lo), std::min(f1.hi, f2.hi)};
      r.d[0] = sub(pt(1.0), z);
      r.d[1] = z;
      r.d[2] = sub(y, x);
      r.dd[0][2] = r.dd[2][0] = pt(-1.0);
      r.dd[1][2] = r.dd[2][1] = pt(1.0);
      break;
    }
    default: r.fail = true; break;
  }
  if (!finite(r.v)) r.fail = true;
  return r;
}

// Relative rounding of one op's result (see the model in bound.hpp); 0 = exact.
double roundRel(Op op, const Profile*) {
  switch (op) {
    case Op::Add: case Op::Sub: case Op::Mul: return kU;
    case Op::Sqrt: case Op::Rsqrt: case Op::Rcp: case Op::Exp: case Op::Log: case Op::Sin: case Op::Cos:
    case Op::Pow: case Op::Exp2: case Op::Log2: return kInexact;
    case Op::Div: return kDiv;
    default: return 0.0;
  }
}

class Evaluator {
 public:
  explicit Evaluator(unsigned n) : n_(n) {}

  Val input(I box, unsigned slot) const {
    Val v;
    v.x = box;
    v.g[slot] = pt(1.0);
    // A denormal input is flushed to zero.
    if (box.lo < kFtz && box.hi > -kFtz && !zero(box)) v.e = kFtz;
    return v;
  }
  Val constant(float c) const {
    Val v;
    v.x = pt(c);
    return v;
  }

  // Smooth / Lipschitz ops through OpEval.
  Val apply(Op op, const Val* const* a, unsigned nargs) const {
    Val r;
    I xs[3], ws[3];
    for (unsigned k = 0; k < nargs; ++k) {
      xs[k] = a[k]->x;
      ws[k] = widen(a[k]->x, a[k]->e);
      r.fail = r.fail || a[k]->fail;
      r.gok = r.gok && a[k]->gok;
    }
    if (r.fail) return r;
    const OpEval ex = evalOp(op, xs, nargs);
    const OpEval wi = evalOp(op, ws, nargs);
    if (ex.fail || wi.fail) {
      r.fail = true;
      return r;
    }
    r.x = ex.v;
    // Gradient (forward mode).
    if (r.gok)
      for (unsigned s = 0; s < n_; ++s) {
        I g = pt(0.0);
        for (unsigned k = 0; k < nargs; ++k)
          if (!zero(a[k]->g[s])) g = add(g, mul(ex.d[k], a[k]->g[s]));
        r.gok = r.gok && finite(g);
        r.g[s] = g;
      }
    // Hessian: sum_k f_k H_k + sum_kl f_kl g_k g_l^T.
    r.hok = r.gok && ex.smooth && n_ <= kMaxHessSlots;
    for (unsigned k = 0; k < nargs && r.hok; ++k) r.hok = a[k]->hok;
    if (r.hok)
      for (unsigned s = 0; s < n_; ++s)
        for (unsigned q = 0; q <= s; ++q) {
          I hv = pt(0.0);
          const unsigned id = hidx(s, q);
          for (unsigned k = 0; k < nargs; ++k) {
            if (!zero(a[k]->h[id])) hv = add(hv, mul(ex.d[k], a[k]->h[id]));
            for (unsigned l = 0; l < nargs; ++l)
              if (!zero(ex.dd[k][l]) && !zero(a[k]->g[s]) && !zero(a[l]->g[q]))
                hv = add(hv, mul(ex.dd[k][l], mul(a[k]->g[s], a[l]->g[q])));
          }
          r.hok = r.hok && finite(hv);
          r.h[id] = hv;
        }
    // Error: propagated (mean value theorem over the widened arguments) plus rounding.
    double e = 0.0;
    for (unsigned k = 0; k < nargs; ++k)
      if (a[k]->e > 0.0) e += up(mag(wi.d[k]) * a[k]->e);
    double rel = roundRel(op, nullptr);
    double round = rel * mag(wi.v);
    if (op == Op::Mad) round = kU * mag(mul(ws[0], ws[1])) + kU * mag(wi.v);  // unfused: two roundings
    if (op == Op::Lerp) {
      // mad(t, b - a, a) and (1 - t) a + t b lowerings; the larger bound.
      const I d = sub(ws[1], ws[0]), omt = sub(pt(1.0), ws[2]);
      const double m1 = mag(ws[2]) * mag(d) + mag(mul(ws[2], d)) + mag(wi.v);
      const double m2 = mag(ws[0]) * mag(omt) + mag(mul(ws[0], omt)) + mag(mul(ws[2], ws[1])) + mag(wi.v);
      round = kU * std::max(m1, m2);
    }
    if (rel > 0.0 || op == Op::Mad || op == Op::Lerp) round += kFtz;
    e = up(up(e + round) * (1.0 + 4.0 * kU));
    r.e = e;
    if (!std::isfinite(e) || mag(widen(r.x, e)) > kFltMax) r.fail = true;
    return r;
  }

  Val unary(Op op, const Val& a) const {
    const Val* p[1] = {&a};
    return apply(op, p, 1);
  }
  Val binary(Op op, const Val& a, const Val& b) const {
    const Val* p[2] = {&a, &b};
    return apply(op, p, 2);
  }
  Val ternary(Op op, const Val& a, const Val& b, const Val& c) const {
    const Val* p[3] = {&a, &b, &c};
    return apply(op, p, 3);
  }

  // Piecewise constant / discontinuous ops: only where the step is decided over the
  // widened argument intervals (the computed values), so the float result is exact.
  Val constVal(double v) const {
    Val r;
    r.x = pt(v);
    return r;
  }
  Val failed() const {
    Val r;
    r.fail = true;
    return r;
  }
  Val floorOp(const Val& a, bool frac) const {
    if (a.fail) return failed();
    const I w = widen(a.x, a.e);
    const double f = std::floor(w.lo);
    if (std::floor(w.hi) != f) return failed();
    if (!frac) return constVal(f);
    Val r = a;  // x - k: exact shift (rounded once)
    r.x = sub(a.x, pt(f));
    r.e = up(a.e + kU * mag(widen(r.x, a.e)) + kFtz);
    return r;
  }
  // ceil / round: exact where the widened argument stays within one step.
  Val stepOp(Op op, const Val& a) const {
    if (a.fail) return failed();
    const I w = widen(a.x, a.e);
    const double lo = op == Op::Ceil ? std::ceil(w.lo) : std::nearbyint(w.lo);
    const double hi = op == Op::Ceil ? std::ceil(w.hi) : std::nearbyint(w.hi);
    if (lo != hi) return failed();
    return constVal(lo);
  }
  // 1 if the relation holds everywhere, 0 if nowhere, fail otherwise.
  Val compare(Op op, const Val& a, const Val& b) const {
    if (a.fail || b.fail) return failed();
    const I d = sub(widen(a.x, a.e), widen(b.x, b.e));  // a - b over the computed values
    bool t = false, f = false;
    switch (op) {
      case Op::Lt: t = d.hi < 0.0; f = d.lo >= 0.0; break;
      case Op::Le: t = d.hi <= 0.0; f = d.lo > 0.0; break;
      case Op::Gt: t = d.lo > 0.0; f = d.hi <= 0.0; break;
      case Op::Ge: t = d.lo >= 0.0; f = d.hi < 0.0; break;
      case Op::Eq: t = zero(d); f = !has0(d); break;
      case Op::Ne: t = !has0(d); f = zero(d); break;
      default: break;
    }
    return t ? constVal(1.0) : (f ? constVal(0.0) : failed());
  }
  Val signOp(const Val& a) const {
    if (a.fail) return failed();
    const I w = widen(a.x, a.e);
    if (w.lo > 0.0) return constVal(1.0);
    if (w.hi < 0.0) return constVal(-1.0);
    if (zero(w)) return constVal(0.0);
    return failed();
  }

 private:
  unsigned n_;
};

// All component values of an expression over one box.
class BoxEval {
 public:
  BoxEval(const Expr& e, const std::vector<uint32_t>& inputSlot) : e_(e), slot_(inputSlot) {}

  const std::vector<std::array<Val, 4>>& eval(const std::vector<I>& box) {
    const Evaluator ev(static_cast<unsigned>(box.size()));
    vals_.resize(e_.nodes.size());
    for (uint32_t i = 0; i < e_.nodes.size(); ++i) {
      const Node& n = e_.nodes[i];
      const unsigned w = width(n.type);
      auto& o = vals_[i];
      auto arg = [&](unsigned k, unsigned c) -> const Val& {
        const uint32_t a = n.args[k];
        return vals_[a][width(e_.nodes[a].type) == 1 ? 0 : c];
      };
      switch (n.op) {
        case Op::Input:
          for (unsigned c = 0; c < w; ++c) o[c] = ev.input(box[slot_[n.input] + c], slot_[n.input] + c);
          break;
        case Op::Const:
          for (unsigned c = 0; c < w; ++c) o[c] = ev.constant(n.value[c]);
          break;
        case Op::Swizzle:
          for (unsigned c = 0; c < w; ++c) o[c] = vals_[n.args[0]][n.swz[c]];
          break;
        case Op::Construct: {
          unsigned c = 0;
          for (unsigned k = 0; k < n.nargs; ++k)
            for (unsigned j = 0; j < width(e_.nodes[n.args[k]].type); ++j) o[c++] = vals_[n.args[k]][j];
          break;
        }
        case Op::Dot:
        case Op::Length:
        case Op::Distance:
        case Op::Normalize: {
          const unsigned aw = width(e_.nodes[n.args[0]].type);
          std::array<Val, 4> v;
          for (unsigned c = 0; c < aw; ++c)
            v[c] = n.op == Op::Distance ? ev.binary(Op::Sub, arg(0, c), arg(1, c)) : arg(0, c);
          auto other = [&](unsigned c) -> const Val& { return n.op == Op::Dot ? arg(1, c) : v[c]; };
          Val s = ev.binary(Op::Mul, v[0], other(0));
          for (unsigned c = 1; c < aw; ++c) s = ev.ternary(Op::Mad, v[c], other(c), s);
          if (n.op == Op::Dot) o[0] = s;
          if (n.op == Op::Length || n.op == Op::Distance) o[0] = ev.unary(Op::Sqrt, s);
          if (n.op == Op::Normalize) {
            const Val r = ev.unary(Op::Rsqrt, s);
            for (unsigned c = 0; c < w; ++c) o[c] = ev.binary(Op::Mul, v[c], r);
          }
          break;
        }
        default:
          for (unsigned c = 0; c < w; ++c) {
            switch (n.op) {
              case Op::Floor: o[c] = ev.floorOp(arg(0, c), false); break;
              case Op::Ceil: case Op::Round: o[c] = ev.stepOp(n.op, arg(0, c)); break;
              case Op::Smoothstep: {  // DXC's expansion over the ops V3 knows
                const Val s = ev.unary(Op::Saturate, ev.binary(Op::Div, ev.binary(Op::Sub, arg(2, c), arg(0, c)),
                                                               ev.binary(Op::Sub, arg(1, c), arg(0, c))));
                const Val t = ev.ternary(Op::Mad, ev.constant(-2.0f), s, ev.constant(3.0f));
                o[c] = ev.binary(Op::Mul, s, ev.binary(Op::Mul, s, t));
                break;
              }
              case Op::Frac: o[c] = ev.floorOp(arg(0, c), true); break;
              case Op::Sign: o[c] = ev.signOp(arg(0, c)); break;
              case Op::Step: o[c] = ev.compare(Op::Ge, arg(1, c), arg(0, c)); break;  // step(e, x) = x >= e
              case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Eq: case Op::Ne:
                o[c] = ev.compare(n.op, arg(0, c), arg(1, c));
                break;
              case Op::LAnd: case Op::LOr: case Op::LNot: {  // conditions are constVal(0 / 1) or failed
                const Val& x = arg(0, c);
                if (n.op == Op::LNot) {
                  o[c] = x.fail ? ev.failed() : ev.constant(x.x.lo == 1.0 ? 0.0f : 1.0f);
                  break;
                }
                const Val& y = arg(1, c);
                const float decides = n.op == Op::LAnd ? 0.0f : 1.0f;  // a value that settles it alone
                if ((!x.fail && x.x.lo == decides) || (!y.fail && y.x.lo == decides)) o[c] = ev.constant(decides);
                else if (x.fail || y.fail) o[c] = ev.failed();
                else o[c] = ev.constant(1.0f - decides);
                break;
              }
              case Op::Select: {
                const Val& cond = arg(0, c);
                o[c] = cond.fail ? ev.failed() : (cond.x.lo == 1.0 ? arg(1, c) : arg(2, c));
                break;
              }
              case Op::Clamp:
                o[c] = ev.binary(Op::Min, ev.binary(Op::Max, arg(0, c), arg(1, c)), arg(2, c));
                break;
              default:
                if (n.nargs == 1) o[c] = ev.unary(n.op, arg(0, c));
                else if (n.nargs == 2) o[c] = ev.binary(n.op, arg(0, c), arg(1, c));
                else o[c] = ev.ternary(n.op, arg(0, c), arg(1, c), arg(2, c));
            }
          }
      }
    }
    return vals_;
  }
  uint32_t root() const { return e_.root; }

 private:
  const Expr& e_;
  const std::vector<uint32_t>& slot_;
  std::vector<std::array<Val, 4>> vals_;
};

// The largest |c_f - t_f| (or, accuracy rule, |c_f - exact|) the budget allows where the
// original's exact value lies in t with rounding error et. <= 0: not provable.
double allowed(const Budget& b, I t, double et, bool vsExact) {
  switch (b.kind) {
    case Budget::Kind::Exact: return 0.0;
    case Budget::Kind::Abs:
    case Budget::Kind::Texcoord: return b.eps;
    case Budget::Kind::Color8:
    case Budget::Kind::Color10: {
      // |c - t| < k / N  =>  the codes round(N * sat(.)) differ by at most k.
      const double n = static_cast<double>((1 << b.codeBits()) - 1);
      return down(b.maxCodeDiff / n);
    }
    case Budget::Kind::Rel: {
      // Relative to |t_f| (>= mig(t) - et) or, accuracy rule, to |exact| (>= mig(t)).
      const double base = vsExact ? mig(t) : mig(t) - et;
      return base > 0.0 ? b.eps * base * (1.0 - 4.0 * kU) : 0.0;
    }
  }
  return 0.0;
}

}  // namespace

BoundResult proveBound(const Program& prog, const Expr& cand, const BoundOptions& opt) {
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  BoundResult res;
  const Budget& b = prog.budget;
  const std::vector<InputDecl> slots = slotDecls(prog.inputs);
  const std::vector<uint32_t> inputSlot = inputSlots(prog.inputs);
  const unsigned n = static_cast<unsigned>(slots.size());
  const Type rt = prog.target.nodes[prog.target.root].type;
  if (b.kind == Budget::Kind::Exact || n > kMaxSlots || cand.nodes[cand.root].type != rt) return res;
  // No interval model for integer ops / bit casts (bit tricks): no proof.
  for (const Expr* x : {&prog.target, &cand})
    for (const Node& nd : x->nodes)
      if (isIntShape(info(nd.op).shape)) return res;
  const unsigned w = width(rt);
  const bool rule = accuracyRule(b);

  std::vector<I> init(n);
  std::vector<double> initW(n);
  for (unsigned k = 0; k < n; ++k) {
    init[k] = {slots[k].lo, slots[k].hi};
    initW[k] = slots[k].hi - slots[k].lo;
  }
  BoxEval te(prog.target, inputSlot), ce(cand, inputSlot), tm(prog.target, inputSlot), cm(cand, inputSlot);

  struct Box {
    double vol;
    std::vector<I> x;
    bool operator<(const Box& o) const { return vol < o.vol; }
  };
  // Both ending in the same 1-Lipschitz op (saturate, abs, neg): |f(a) - f(b)| <= |a - b|,
  // so the difference is bounded on the arguments (no kink in the way), with their errors;
  // the budget still uses the whole original.
  uint32_t tpeel = prog.target.root, cpeel = cand.root;
  for (;;) {
    const Node &tn = prog.target.nodes[tpeel], &cn = cand.nodes[cpeel];
    if (tn.op != cn.op || tn.type != cn.type || (tn.op != Op::Saturate && tn.op != Op::Abs && tn.op != Op::Neg))
      break;
    if (prog.target.nodes[tn.args[0]].type != tn.type || cand.nodes[cn.args[0]].type != cn.type) break;
    tpeel = tn.args[0];
    cpeel = cn.args[0];
  }
  std::priority_queue<Box> queue;
  queue.push({1.0, init});
  double proven = 0.0;
  bool failed = false;
  std::vector<I> mid(n);
  while (!queue.empty()) {
    if (res.boxes >= opt.maxBoxes ||
        std::chrono::duration<double>(Clock::now() - t0).count() > opt.seconds)
      break;
    Box box = queue.top();
    queue.pop();
    ++res.boxes;
    const auto& tall = te.eval(box.x);
    const auto& call = ce.eval(box.x);
    for (unsigned k = 0; k < n; ++k) mid[k] = pt(0.5 * (box.x[k].lo + box.x[k].hi));
    const auto& tc = tm.eval(mid)[tpeel];
    const auto& cc = cm.eval(mid)[cpeel];
    const auto &tv = tall[te.root()], &cv = call[ce.root()];      // errors, budget
    const auto &tp = tall[tpeel], &cp = call[cpeel];              // the difference
    bool pass = true;
    double bound = 0.0;
    std::vector<double> score(n, 0.0);
    for (unsigned c = 0; c < w; ++c) {
      const Val &tf = tv[c], &vf = cv[c];
      const Val &t = tp[c], &v = cp[c];
      if (tf.fail || vf.fail || t.fail || v.fail || tc[c].fail || cc[c].fail) {
        pass = false;
        continue;
      }
      I d = sub(v.x, t.x);
      if (t.gok && v.gok) {
        I dc = sub(cc[c].x, tc[c].x);
        for (unsigned k = 0; k < n; ++k) {
          const I gd = sub(v.g[k], t.g[k]);
          dc = add(dc, mul(gd, sub(box.x[k], mid[k])));
          score[k] += (mag(gd) + 0x1p-20 * (mag(v.g[k]) + mag(t.g[k]))) * (box.x[k].hi - box.x[k].lo);
        }
        d = {std::max(d.lo, dc.lo), std::min(d.hi, dc.hi)};
      }
      // Second order (Taylor with Lagrange remainder, needs C^2 on the box): the
      // gradient at the midpoint is tight, the Hessian difference cancels exactly where
      // the two share their structure (identities), so the remainder shrinks as width^3.
      if (t.hok && v.hok && tc[c].gok && cc[c].gok && n <= kMaxHessSlots) {
        I d2 = sub(cc[c].x, tc[c].x);
        for (unsigned k = 0; k < n; ++k) {
          const I hk = sub(box.x[k], mid[k]);
          const I gm = sub(cc[c].g[k], tc[c].g[k]);
          d2 = add(d2, mul(gm, hk));
          double hs = 0.0;
          for (unsigned q = 0; q < n; ++q) {
            const I hd = sub(v.h[hidx(k, q)], t.h[hidx(k, q)]);
            if (zero(hd)) continue;
            hs += mag(hd) * (box.x[q].hi - box.x[q].lo);
            if (q > k) continue;
            const I prod = q == k ? sqr(hk) : mul(hk, sub(box.x[q], mid[q]));
            d2 = add(d2, mul(q == k ? pt(0.5) : pt(1.0), mul(hd, prod)));
          }
          score[k] += (mag(gm) + hs) * (box.x[k].hi - box.x[k].lo);
        }
        d = {std::max(d.lo, d2.lo), std::min(d.hi, d2.hi)};
      }
      const double diff = mag(d);
      // |f(c_f) - f(t_f)| <= |c_f - t_f| <= |c - t| + e_c + e_t with the peeled nodes'
      // errors (the whole expression's error can be 0 where f is flat, e.g. saturate).
      const double vsT = up(up(diff + v.e) + t.e);
      const double vsX = up(diff + v.e);
      const bool ok = vsT <= allowed(b, tf.x, tf.e, false) || (rule && vsX <= allowed(b, tf.x, tf.e, true));
      if (!ok || !std::isfinite(vsT)) pass = false;
      bound = std::max(bound, vsT);
    }
    if (pass) {
      proven += box.vol;
      res.maxBound = std::max(res.maxBound, bound);
      continue;
    }
    // Split the dimension that contributes most (gradients x widths), else the widest.
    unsigned best = n;
    double bestScore = 0.0;
    for (unsigned k = 0; k < n; ++k) {
      const double wk = box.x[k].hi - box.x[k].lo;
      if (!(wk > 0.0) || static_cast<float>(box.x[k].lo) == static_cast<float>(box.x[k].hi)) continue;
      if (wk <= initW[k] * 0x1p-40) continue;
      const double s = score[k] > 0.0 ? score[k] : wk / initW[k] * 1e-300;
      if (best == n || s > bestScore) best = k, bestScore = s;
    }
    if (best == n) {
      failed = true;  // cannot be split further
      continue;
    }
    const double m = 0.5 * (box.x[best].lo + box.x[best].hi);
    Box a{0.5 * box.vol, box.x}, c{0.5 * box.vol, box.x};
    a.x[best].hi = m;
    c.x[best].lo = m;
    queue.push(std::move(a));
    queue.push(std::move(c));
  }
  res.fraction = std::min(1.0, proven);
  res.proven = !failed && queue.empty();
  res.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
  return res;
}

}  // namespace sopt
