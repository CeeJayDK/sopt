#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "search/enumerator.hpp"
#include "verify/problems.hpp"
#include "verify/verify.hpp"

namespace sopt {

struct Options {
  uint32_t numTests = 32;        // fingerprint points
  uint32_t stage2Points = 4096;  // CEGIS filter
  size_t v1Points = 1u << 20;    // dense sampling verification
  // V2: after V1, the cheapest v2Candidates alternatives are checked on every point of
  // the input domain when it has at most v2Max points (e.g. 256^3 for an 8-bit RGB
  // input). 0 disables.
  uint64_t v2Max = 1ull << 24;
  uint32_t v2Candidates = 20;
  uint32_t maxIterations = 8;    // CEGIS restarts
  uint32_t maxCexPerIteration = 64;
  uint32_t maxAlternatives = 50; // stop V1 verification after this many distinct variants
  uint64_t seed = 1;
  // Programs with compile-time inputs (preprocessor definitions): search with them set
  // to their current value, then generalize the candidates' numeric constants into
  // expressions of those inputs (e.g. 0.001001001 = 1 / (F - 1)) and verify over the
  // whole range. The search itself cannot build such constants at useful depths.
  bool specialize = true;
  // Owner's accuracy rule (Budget::vsExact): off = candidates must stay within the
  // budget of the float32 original everywhere.
  bool exactRule = true;
  // Also keep less accurate candidates (Budget::loose; 0 = off): up to maxLoose of them,
  // classified Klass::LessAccurate.
  double loose = 0.0;
  uint32_t maxLoose = 10;
  // Accuracy variants (owner, 2026-09-26): candidates that are not cheaper but clearly more
  // accurate than the original against exact math (Accepted::moreAccurate: error at most
  // 1/4 of the original's, needs the accuracy rule) are kept up to accuracySlack above the
  // target's static cost. Also enables SearchConfig::rational.
  bool accuracyVariants = true;
  uint32_t accuracySlack = 8;
  unsigned threads = 0;          // 0 = hardware concurrency
  SearchConfig search;
};

struct Accepted {
  Expr expr;
  std::string text;
  uint32_t cost = 0;
  Klass klass = Klass::Within;
  Metrics worst;  // worst case over all semantic profiles
  bool exhaustive = false;  // verified on the whole domain (V2), worst is over all of it
  // Input values where it still fails (e.g. a division by zero at one value); owner:
  // kept and marked, the user decides; never picked by SOPT_AUTO.
  std::vector<ProblemRange> problems;
  // Clearly more accurate than the original against exact math (Options::accuracyVariants).
  bool moreAccurate = false;
};

struct RunResult {
  uint32_t targetCost = 0;
  std::string targetText;
  std::vector<Accepted> accepted;
  Metrics targetExact;  // the original's error against the exact values (accuracy rule)
  // The float32 original does not follow exact math (max error vs exact > 10% of its
  // range, e.g. frac(sin(x) * 43758.5) noise): the accuracy rule and the error-scale
  // floor of Rel budgets were off.
  bool exactOff = false;
  SearchStats search;  // stats of the final iteration
  uint32_t iterations = 0;
  uint64_t counterexamples = 0;
  uint64_t rejectedStage2 = 0;
  uint64_t rejectedV1 = 0;
  uint64_t rejectedProfiles = 0;
  uint64_t rejectedV2 = 0;
  uint64_t v2Points = 0;  // domain size when V2 applied, else 0  // passed ref but failed mix/fma/gpu
  double searchSec = 0.0;
  double verifySec = 0.0;
  double totalSec = 0.0;
};

RunResult optimize(const Program& prog, const Options& opt);
// optimize() for programs with compile-time inputs (see Options::specialize).
RunResult optimizeSpecialized(const Program& prog, const Options& opt);

}  // namespace sopt
