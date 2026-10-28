#pragma once
#include <vector>

#include "search/driver.hpp"
#include "search/enumerator.hpp"

namespace sopt {

// STOKE-style stochastic search (Schkufza, Sharma, Aiken 2013; owner's go to try behind a flag, 2026-10-28,
// Options::stoke, --stoke): a Metropolis walk over expressions from the target, one random edit a step (an op for
// another of its shape, an operand for an earlier node or a constant, a node for one of its operands, operands
// swapped), scored by the cost plus a penalty per test point outside the budget. Forms right on every point and
// cheaper than the target are ordinary candidates (the driver verifies them like any other). `seconds` of walking.
std::vector<Candidate> stokeCandidates(const Program& prog, const CostModel& model, uint32_t targetCost, double seconds,
                                       uint64_t seed);

}  // namespace sopt
