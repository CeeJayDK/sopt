#pragma once
#include <vector>

#include "ir/expr.hpp"

namespace sopt {

// Reshaped forms for the scheduling measures (owner, 2026-10-08; scheduleMetrics): sums and
// products are flattened (through single-use nodes of the same type) and rebuilt in rate order,
// constants first, then compile-time inputs, uniforms, per-pixel values and the texture fetches
// in source order, so that what can be precomputed is grouped (performance mode folds uniforms)
// and the last fetch enters last. The second form also multiplies a constant / uniform weight
// into the terms of a sum that reads a fetch ((v0 + v1) * w -> v0 * w + v1 * w: one fma per late
// term instead of an add and a final mul). Forms no better than e by any of model's measures (cost,
// performance mode cost, tail, critical path) are left out; every form is verified like any
// candidate (reassociation changes rounding).
std::vector<Expr> reshapeForms(const Expr& e, const std::vector<InputDecl>& inputs, const CostModel& model);

}  // namespace sopt
