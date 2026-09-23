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

}  // namespace

const ExactEvaluator::Cols& ExactEvaluator::eval(const Expr& e, const PointSet& ps, size_t begin,
                                                 size_t count) {
  const size_t nn = e.nodes.size();
  size_t total = 0;
  for (const auto& n : e.nodes) total += width(n.type);
  scratch.assign(total * count, 0.0);
  ptr.assign(nn, Cols{});
  double* next = scratch.data();
  for (size_t i = 0; i < nn; ++i) {
    const Node& n = e.nodes[i];
    const unsigned w = width(n.type);
    double* out[4] = {nullptr, nullptr, nullptr, nullptr};
    for (unsigned c = 0; c < w; ++c, next += count) out[c] = next, ptr[i][c] = next;
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
        for (unsigned c = 0; c < w; ++c) std::copy(ptr[n.args[0]][n.swz[c]], ptr[n.args[0]][n.swz[c]] + count, out[c]);
        continue;
      case Op::Construct: {
        unsigned c = 0;
        for (unsigned k = 0; k < n.nargs; ++k)
          for (unsigned j = 0; j < width(e.nodes[n.args[k]].type); ++j, ++c)
            std::copy(ptr[n.args[k]][j], ptr[n.args[k]][j] + count, out[c]);
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
        continue;
      }
      default:
        for (unsigned c = 0; c < w; ++c) {
          const double* a = n.nargs > 0 ? arg(0, c) : nullptr;
          const double* b = n.nargs > 1 ? arg(1, c) : nullptr;
          const double* d = n.nargs > 2 ? arg(2, c) : nullptr;
          for (size_t j = 0; j < count; ++j)
            out[c][j] = exactOp(n.op, a ? a[j] : 0.0, b ? b[j] : 0.0, d ? d[j] : 0.0);
        }
    }
  }
  return ptr[e.root];
}

std::vector<double> evalExactAll(const Expr& e, const PointSet& ps) {
  constexpr size_t kBlock = 4096;
  const unsigned w = width(e.nodes[e.root].type);
  const size_t total = ps.size();
  std::vector<double> out(w * total);
  ExactEvaluator ev;
  for (size_t b = 0; b < total; b += kBlock) {
    const size_t count = std::min(kBlock, total - b);
    const ExactEvaluator::Cols& v = ev.eval(e, ps, b, count);
    for (unsigned c = 0; c < w; ++c)
      std::copy(v[c], v[c] + count, out.begin() + static_cast<std::ptrdiff_t>(c * total + b));
  }
  return out;
}

}  // namespace sopt
