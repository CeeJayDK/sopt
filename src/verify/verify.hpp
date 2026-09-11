#pragma once
#include <cstdint>
#include <vector>

#include "ir/expr.hpp"
#include "verify/points.hpp"

namespace sopt {

// Evaluates an Expr over points [begin, begin+count). Returns a pointer to the
// root values (valid until the next call with the same scratch buffers).
struct BlockEvaluator {
  std::vector<float> scratch;
  std::vector<const float*> ptr;
  const float* eval(const Expr& e, const PointSet& ps, size_t begin, size_t count,
                    const Profile& profile);
};

enum class Klass { BitExact, Identical8, Within };
const char* klassName(Klass k);

struct Metrics {
  bool pass = true;
  bool bitExact = true;
  int maxCodeDiff = 0;
  double maxAbs = 0.0;
  double maxRel = 0.0;
  uint64_t checked = 0;       // points where the target is finite
  uint64_t codeChanged = 0;   // points whose 8-bit code differs
  std::vector<float> failPoint;  // first failing point, if any
  uint64_t valueHash = 0;        // order-independent hash of candidate values

  void merge(const Metrics& o);
  double changedFraction() const {
    return checked ? static_cast<double>(codeChanged) / static_cast<double>(checked) : 0.0;
  }
};

// Compares candidate against the program target under one profile.
// Points where the target is not finite are don't-care.
// targetVals: optional precomputed target values for this profile (see evalAll).
Metrics compare(const Program& prog, const Expr& cand, const PointSet& ps, const Profile& profile,
                unsigned threads = 0, const std::vector<float>* targetVals = nullptr);

std::vector<float> evalAll(const Expr& e, const PointSet& ps, const Profile& profile);

// Single-point budget check (shared with the enumerator).
inline int code8(float v) {
  const double c = v < 0.0f ? 0.0 : (v > 1.0f ? 1.0 : static_cast<double>(v));
  return static_cast<int>(c * 255.0 + 0.5);
}
bool pointWithinBudget(const Budget& b, float target, float cand);

Klass classify(const Program& prog, const Expr& cand, const Metrics& worst);

}  // namespace sopt
