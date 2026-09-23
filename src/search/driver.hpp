#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "search/enumerator.hpp"
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
};

struct RunResult {
  uint32_t targetCost = 0;
  std::string targetText;
  std::vector<Accepted> accepted;
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

}  // namespace sopt
