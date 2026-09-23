#pragma once
#include <array>
#include <cstddef>
#include <vector>

#include "ir/expr.hpp"
#include "verify/points.hpp"

namespace sopt {

// Exact reference: an expression evaluated in double precision with exact operations
// (no contraction; a / b, rcp and rsqrt exact). It only measures accuracy (owner's rule:
// a candidate may differ from the float32 original where it is at least as close to
// the exact value); the float32 evaluation stays the semantics reference.
struct ExactEvaluator {
  using Cols = std::array<const double*, 4>;
  std::vector<double> scratch;
  std::vector<Cols> ptr;
  // Root component columns over points [begin, begin + count); valid until the next call.
  const Cols& eval(const Expr& e, const PointSet& ps, size_t begin, size_t count);
};

// All exact values of e over ps, component-major: [c * ps.size() + point].
std::vector<double> evalExactAll(const Expr& e, const PointSet& ps);

}  // namespace sopt
