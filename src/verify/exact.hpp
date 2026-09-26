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
//
// With withScale, it also computes the error scale S of e's float32 evaluation: a running
// rounding-error bound divided by the unit roundoff, i.e. the summed size of the values
// whose rounding the result carries (S >= |value|; much larger where the result is a
// cancellation of larger terms, e.g. x + y - 2xy near 0). Rel budgets are relative to
// max(|t|, S) (owner, 2026-09-26): relative near zeros that are computed exactly (1 - d),
// absolute-like at zero crossings where the original itself cancels.
struct ExactEvaluator {
  using Cols = std::array<const double*, 4>;
  std::vector<double> scratch, sscratch;
  std::vector<Cols> ptr, sptr;
  bool withScale = false;
  // Root component columns over points [begin, begin + count); valid until the next call.
  const Cols& eval(const Expr& e, const PointSet& ps, size_t begin, size_t count);
  // The root's error scale columns of the last eval (withScale).
  const Cols& scale(const Expr& e) const { return sptr[e.root]; }
};

// All exact values of e over ps, component-major: [c * ps.size() + point]; with scales,
// also the error scales (see ExactEvaluator), same layout.
std::vector<double> evalExactAll(const Expr& e, const PointSet& ps, std::vector<double>* scales = nullptr);

}  // namespace sopt
