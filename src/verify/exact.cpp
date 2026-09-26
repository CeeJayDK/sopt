#include "verify/exact.hpp"

#include <algorithm>
#include <cmath>

namespace sopt {
namespace {

double exactOp(Op op, double x, double y, double z) {
  switch (op) {
    case Op::Neg: return -x;
    case Op::Abs: return std::fabs(x);
    case Op::Saturate: return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    case Op::Floor: return std::floor(x);
    case Op::Frac: return x - std::floor(x);
    case Op::Sign: return x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0);
    case Op::Sqrt: return std::sqrt(x);
    case Op::Rsqrt: return 1.0 / std::sqrt(x);
    case Op::Rcp: return 1.0 / x;
    case Op::Exp: return std::exp(x);
    case Op::Log: return std::log(x);
    case Op::Sin: return std::sin(x);
    case Op::Cos: return std::cos(x);
    case Op::Add: return x + y;
    case Op::Sub: return x - y;
    case Op::Mul: return x * y;
    case Op::Div: return x / y;
    case Op::Min: return y < x ? y : x;
    case Op::Max: return x < y ? y : x;
    case Op::Step: return y >= x ? 1.0 : 0.0;
    case Op::Pow: return std::pow(x, y);
    case Op::Lt: return x < y ? 1.0 : 0.0;
    case Op::Le: return x <= y ? 1.0 : 0.0;
    case Op::Gt: return x > y ? 1.0 : 0.0;
    case Op::Ge: return x >= y ? 1.0 : 0.0;
    case Op::Eq: return x == y ? 1.0 : 0.0;
    case Op::Ne: return x != y ? 1.0 : 0.0;
    case Op::Mad: return x * y + z;
    case Op::Lerp: return x + z * (y - x);
    case Op::Clamp: return std::min(std::max(x, y), z);
    case Op::Select: return x != 0.0 ? y : z;
    default: return 0.0;  // leaves and vector ops: see eval()
  }
}

// Error scale of one scalar op (see ExactEvaluator): operand values x, y, z with scales
// sx, sy, sz, exact result v. Each rounding adds |value|; errors propagate first-order.
double scaleOp(Op op, double x, double y, double z, double sx, double sy, double sz, double v) {
  const double av = std::fabs(v);
  double s;
  switch (op) {
    case Op::Neg: case Op::Abs: return sx;
    case Op::Saturate: return (x < 0.0 || x > 1.0) ? 0.0 : sx;
    case Op::Clamp: return (x < y) ? sy : (x > z ? sz : sx);
    case Op::Min: return y < x ? sy : sx;
    case Op::Max: return x < y ? sy : sx;
    case Op::Select: return x != 0.0 ? sy : sz;
    case Op::Floor: case Op::Sign: case Op::Step:
    case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Eq: case Op::Ne:
      return 0.0;  // piecewise constant: errors move the steps, they do not scale
    case Op::Frac: return sx;
    case Op::Add: case Op::Sub: s = sx + sy + av; break;
    case Op::Mul: s = std::fabs(y) * sx + std::fabs(x) * sy + av; break;
    case Op::Div: s = (sx + av * sy) / std::fabs(y) + av; break;
    case Op::Mad: s = std::fabs(y) * sx + std::fabs(x) * sy + std::fabs(x * y) + sz + av; break;
    case Op::Lerp: {  // x + z * (y - x)
      const double d = y - x, sd = sx + sy + std::fabs(d), p = z * d;
      s = sx + std::fabs(d) * sz + std::fabs(z) * sd + std::fabs(p) + av;
      break;
    }
    case Op::Rcp: s = av * sx / std::fabs(x) + av; break;
    case Op::Sqrt: s = (av > 0.0 ? sx / (2.0 * av) : 0.0) + av; break;
    case Op::Rsqrt: s = av * sx / (2.0 * std::fabs(x)) + av; break;
    case Op::Exp: s = av * sx + av; break;
    case Op::Log: s = sx / std::fabs(x) + av; break;
    case Op::Sin: case Op::Cos: s = sx + av; break;
    case Op::Pow: s = av * (std::fabs(y) * sx / std::fabs(x) + std::fabs(std::log(std::fabs(x))) * sy) + av; break;
    default: s = av; break;
  }
  return std::isfinite(s) ? s : av;
}

}  // namespace

const ExactEvaluator::Cols& ExactEvaluator::eval(const Expr& e, const PointSet& ps, size_t begin,
                                                 size_t count) {
  const size_t nn = e.nodes.size();
  size_t total = 0;
  for (const auto& n : e.nodes) total += width(n.type);
  scratch.assign(total * count, 0.0);
  ptr.assign(nn, Cols{});
  double* next = scratch.data();
  if (withScale) {
    sscratch.assign(total * count, 0.0);  // leaves (inputs, constants) are exact: scale 0
    sptr.assign(nn, Cols{});
  }
  double* snext = sscratch.data();
  for (size_t i = 0; i < nn; ++i) {
    const Node& n = e.nodes[i];
    const unsigned w = width(n.type);
    double* out[4] = {nullptr, nullptr, nullptr, nullptr};
    for (unsigned c = 0; c < w; ++c, next += count) out[c] = next, ptr[i][c] = next;
    double* sout[4] = {nullptr, nullptr, nullptr, nullptr};
    if (withScale)
      for (unsigned c = 0; c < w; ++c, snext += count) sout[c] = snext, sptr[i][c] = snext;
    auto sarg = [&](unsigned k, unsigned c) {
      const uint32_t a = n.args[k];
      return sptr[a][width(e.nodes[a].type) == 1 ? 0 : c];
    };
    // Component c of operand k (a float1 operand is broadcast).
    auto arg = [&](unsigned k, unsigned c) {
      const uint32_t a = n.args[k];
      return ptr[a][width(e.nodes[a].type) == 1 ? 0 : c];
    };
    switch (n.op) {
      case Op::Input: {
        const uint32_t slot = ps.slotOf(n.input);
        for (unsigned c = 0; c < w; ++c)
          for (size_t j = 0; j < count; ++j) out[c][j] = ps.cols[slot + c][begin + j];
        continue;
      }
      case Op::Const:
        for (unsigned c = 0; c < w; ++c) std::fill(out[c], out[c] + count, double(n.value[c]));
        continue;
      case Op::Swizzle:
        for (unsigned c = 0; c < w; ++c) {
          std::copy(ptr[n.args[0]][n.swz[c]], ptr[n.args[0]][n.swz[c]] + count, out[c]);
          if (withScale) std::copy(sptr[n.args[0]][n.swz[c]], sptr[n.args[0]][n.swz[c]] + count, sout[c]);
        }
        continue;
      case Op::Construct: {
        unsigned c = 0;
        for (unsigned k = 0; k < n.nargs; ++k)
          for (unsigned j = 0; j < width(e.nodes[n.args[k]].type); ++j, ++c) {
            std::copy(ptr[n.args[k]][j], ptr[n.args[k]][j] + count, out[c]);
            if (withScale) std::copy(sptr[n.args[k]][j], sptr[n.args[k]][j] + count, sout[c]);
          }
        continue;
      }
      case Op::Dot:
      case Op::Length:
      case Op::Distance:
      case Op::Normalize: {
        const unsigned aw = width(e.nodes[n.args[0]].type);
        std::vector<double> s(count, 0.0);
        for (unsigned c = 0; c < aw; ++c)
          for (size_t j = 0; j < count; ++j) {
            const double a = arg(0, c)[j];
            const double b = n.op == Op::Dot ? arg(1, c)[j] : (n.op == Op::Distance ? a - arg(1, c)[j] : a);
            s[j] += (n.op == Op::Distance ? b * b : a * b);
          }
        if (n.op == Op::Dot) std::copy(s.begin(), s.end(), out[0]);
        if (n.op == Op::Length || n.op == Op::Distance)
          for (size_t j = 0; j < count; ++j) out[0][j] = std::sqrt(s[j]);
        if (n.op == Op::Normalize)
          for (unsigned c = 0; c < w; ++c)
            for (size_t j = 0; j < count; ++j) out[c][j] = arg(0, c)[j] / std::sqrt(s[j]);
        if (withScale) {
          // Sum of products (each product and partial sum rounded), then sqrt / division.
          std::vector<double> ss(count, 0.0);
          for (unsigned c = 0; c < aw; ++c)
            for (size_t j = 0; j < count; ++j) {
              const double a = arg(0, c)[j], sa = sarg(0, c)[j];
              const double b = n.op == Op::Dot ? arg(1, c)[j] : (n.op == Op::Distance ? a - arg(1, c)[j] : a);
              const double sb = n.op == Op::Dot ? sarg(1, c)[j]
                                                : (n.op == Op::Distance ? sa + sarg(1, c)[j] + std::fabs(b) : sa);
              ss[j] += std::fabs(b) * sa + std::fabs(a) * sb + 2.0 * std::fabs(a * b);
            }
          for (size_t j = 0; j < count; ++j) {
            if (n.op == Op::Dot) sout[0][j] = ss[j];
            if (n.op == Op::Length || n.op == Op::Distance)
              sout[0][j] = (out[0][j] > 0.0 ? ss[j] / (2.0 * out[0][j]) : 0.0) + out[0][j];
            if (n.op == Op::Normalize)
              for (unsigned c = 0; c < w; ++c) {
                const double v = std::fabs(out[c][j]), a = std::fabs(arg(0, c)[j]);
                const double rel = (s[j] > 0.0 ? ss[j] / (2.0 * s[j]) : 0.0) + (a > 0.0 ? sarg(0, c)[j] / a : 0.0);
                sout[c][j] = std::isfinite(rel) ? v * rel + 2.0 * v : v;
              }
          }
        }
        continue;
      }
      default:
        for (unsigned c = 0; c < w; ++c) {
          const double* a = n.nargs > 0 ? arg(0, c) : nullptr;
          const double* b = n.nargs > 1 ? arg(1, c) : nullptr;
          const double* d = n.nargs > 2 ? arg(2, c) : nullptr;
          for (size_t j = 0; j < count; ++j)
            out[c][j] = exactOp(n.op, a ? a[j] : 0.0, b ? b[j] : 0.0, d ? d[j] : 0.0);
          if (withScale) {
            const double* sa = n.nargs > 0 ? sarg(0, c) : nullptr;
            const double* sb = n.nargs > 1 ? sarg(1, c) : nullptr;
            const double* sd = n.nargs > 2 ? sarg(2, c) : nullptr;
            for (size_t j = 0; j < count; ++j)
              sout[c][j] = scaleOp(n.op, a ? a[j] : 0.0, b ? b[j] : 0.0, d ? d[j] : 0.0, sa ? sa[j] : 0.0,
                                   sb ? sb[j] : 0.0, sd ? sd[j] : 0.0, out[c][j]);
          }
        }
    }
  }
  return ptr[e.root];
}

std::vector<double> evalExactAll(const Expr& e, const PointSet& ps, std::vector<double>* scales) {
  constexpr size_t kBlock = 4096;
  const unsigned w = width(e.nodes[e.root].type);
  const size_t total = ps.size();
  std::vector<double> out(w * total);
  if (scales) scales->assign(w * total, 0.0);
  ExactEvaluator ev;
  ev.withScale = scales != nullptr;
  for (size_t b = 0; b < total; b += kBlock) {
    const size_t count = std::min(kBlock, total - b);
    const ExactEvaluator::Cols& v = ev.eval(e, ps, b, count);
    for (unsigned c = 0; c < w; ++c)
      std::copy(v[c], v[c] + count, out.begin() + static_cast<std::ptrdiff_t>(c * total + b));
    if (scales) {
      const ExactEvaluator::Cols& s = ev.scale(e);
      for (unsigned c = 0; c < w; ++c)
        std::copy(s[c], s[c] + count, scales->begin() + static_cast<std::ptrdiff_t>(c * total + b));
    }
  }
  return out;
}

}  // namespace sopt
