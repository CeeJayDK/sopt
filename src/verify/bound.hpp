#pragma once
#include <cstdint>
#include <vector>

#include "ir/expr.hpp"

namespace sopt {

// V3 (M7): a formal upper bound on |candidate - original| over a continuous input domain
// by interval arithmetic and box subdivision. Per box:
//   |c_f - t_f| <= |c - t| + E_c + E_t
// c, t: the exact (real-number) values, c - t enclosed by the centered form
// (c - t)(mid) + sum_k d(c - t)/dx_k(box) * (x_k - mid_k) (interval gradients, forward
// mode) intersected with the naive enclosure; E_c, E_t: bounds on the float32 evaluations'
// rounding errors (first order through the mean value theorem over the widened argument
// intervals, so they hold rigorously). Rounding model, the same as the verification
// profiles: exact ops correctly rounded (u = 2^-24 each; unfused mad two roundings; lerp in
// both lowerings), inexact ops (rcp, rsqrt, sqrt, exp, log, sin, cos, pow) within 2 ulp of
// the exact value (libm 1 ulp + the gpu+/gpu- step), div as a * rcp(b) within 4 ulp,
// denormals flushed (2^-126 absolute per op). Not a statement about a real GPU's
// transcendental accuracy, which is not specified that tightly.
//
// A box passes when the bound is within the budget of the float32 original, or (accuracy
// rule) |c_f - exact| <= |c - t| + E_c is within the budget of the exact value. Exact
// budgets are not provable this way. Boxes that do not pass are split (largest
// contribution first) up to maxBoxes / seconds; boxes where the original is undefined
// (e.g. rcp through 0) or where a comparison cannot be decided never pass.
struct BoundOptions {
  uint64_t maxBoxes = 200000;
  double seconds = 2.0;
};

struct BoundResult {
  bool proven = false;     // every box passed
  double fraction = 0.0;   // share of the domain's volume proven
  double maxBound = 0.0;   // largest |c_f - t_f| bound over the passed boxes
  uint64_t boxes = 0;      // boxes evaluated
  double seconds = 0.0;
};

BoundResult proveBound(const Program& prog, const Expr& cand, const BoundOptions& opt = {});

}  // namespace sopt
