#pragma once
#include <vector>

#include "search/driver.hpp"
#include "search/enumerator.hpp"

namespace sopt {

// Subtree search (M7, Options::subtrees): when a region is too large for the bottom-up
// search to reach its cost, search its small subexpressions on their own (each over the
// region's input domain, budget rel 1e-6 plus the accuracy rule, or exact for exact
// regions) and put the cheaper forms back into the whole expression: each single
// replacement, and the best disjoint ones combined. The results are ordinary candidates
// for the region; the driver verifies them against the region's own budget like any
// other.
std::vector<Candidate> subtreeCandidates(const Program& prog, const Options& opt, uint32_t targetCost,
                                         uint32_t* searches = nullptr);

// Copy of e with the nodes in `repl` replaced by the given expressions (same inputs).
Expr replaceNodes(const Expr& e, const std::vector<std::pair<uint32_t, const Expr*>>& repl);
// Inserts all of e into b (hash-consed with what b holds); returns e's root there.
uint32_t insertExpr(const Expr& e, ExprBuilder& b);
// Trivial identities removed (same value; printed variants read better):
// mad(a, b, 0) -> a * b, mad(1, a, c) -> a + c, mad(-1, a, c) -> c - a, a * 1 -> a,
// a + 0 -> a, a - 0 -> a. Only where the result keeps its type.
Expr simplifyIdentities(const Expr& e);
// The subexpression rooted at node i.
Expr subexpr(const Expr& e, uint32_t i);

}  // namespace sopt
