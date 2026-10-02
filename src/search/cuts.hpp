#pragma once
#include <vector>

#include "search/driver.hpp"
#include "search/enumerator.hpp"

namespace sopt {

// Cut points (M7, Options::cuts; owner's idea): a node v of the region's expression that
// all of the rest depends on for the inputs below it. The expression is then
// top(v, other inputs) with sub = v's subexpression, and the two are searched on their
// own: top over v's sampled range and the inputs sub does not read (so the box domain is
// exact), sub as in the subtree search. Each cheaper top is combined with sub and with
// sub's cheaper forms; the results are ordinary candidates for the region, verified
// against its own budget.
struct Cut {
  uint32_t node = 0;
  uint32_t subOps = 0, topOps = 0;  // operations below v (with v) and above it
  uint32_t subCost = 0, topCost = 0;
  double lo = 0.0, hi = 0.0;        // v's sampled range (all components)
};

// Cut points with at least two operations on each side, most balanced first.
std::vector<Cut> findCuts(const Program& prog, const CostModel& model, uint64_t seed);

// top as a program: inputs = the region's inputs top still reads, then v.
// inputMap[k] = the region's input index of top's input k (v: UINT32_MAX).
Program topProgram(const Program& prog, const Cut& cut, std::vector<uint32_t>& inputMap);

std::vector<Candidate> cutCandidates(const Program& prog, const Options& opt, uint32_t targetCost,
                                     uint32_t* searches = nullptr);

}  // namespace sopt
