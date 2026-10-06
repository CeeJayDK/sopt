#pragma once
#include <string>
#include <vector>
#include "ir/expr.hpp"

namespace sopt {

// Input values where a verified candidate still fails, e.g. a division by zero at one
// value that random sampling cannot hit (rcp(0.01 * F - 2) at F = 200). Owner: such
// variants are kept and marked with the failing values and the ranges where they are
// fine; the user decides. Found at the zeros / domain edges of the candidate's rcp,
// div, rsqrt, sqrt, log and pow operands that depend on a single scalar input, and at
// -0.0 for candidates with integer ops / bit casts (random sampling never draws -0).
struct ProblemRange {
  uint32_t input = 0;       // index into Program::inputs
  double lo = 0, hi = 0;    // failing values (lo == hi: a single value)
  bool notFinite = false;   // the candidate is NaN/inf there (else: outside the budget)
  // Fails only at -0.0 (owner, 2026-10-06: bit tricks that read the sign bit, e.g. 1.0 with x's
  // sign copied in for x >= 0 ? 1 : -1; kept and marked "differs at x = -0.0", the user decides).
  bool negZero = false;
};

// loose: the candidate is accepted by the loose budget (Klass::LessAccurate).
std::vector<ProblemRange> findProblemRanges(const Program& prog, const Expr& cand, bool loose);

// "fails at F = [200, 200.00002] (NaN/inf at some), fine on [100, 200) and
// (200.00002, 10000]"; problems of different inputs joined by "; ". Empty if none.
std::string describeProblems(const Program& prog, const std::vector<ProblemRange>& problems);

}  // namespace sopt
