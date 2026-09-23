#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "ir/expr.hpp"
#include "verify/points.hpp"

namespace sopt {

// Evaluates an Expr over points [begin, begin+count). Returns the root's component
// columns (width(root type) of them; valid until the next call).
struct BlockEvaluator {
  using Cols = std::array<const float*, 4>;
  std::vector<float> scratch;
  std::vector<Cols> ptr;
  std::vector<float> tmp;
  const Cols& eval(const Expr& e, const PointSet& ps, size_t begin, size_t count,
                   const Profile& profile);
};

// Identical8: identical after quantization to the budget's code values (8 or 10 bits).
enum class Klass { BitExact, Identical8, Within };
const char* klassName(Klass k, int codeBits = 8);

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

// All values of e over ps, component-major: [c * ps.size() + point].
std::vector<float> evalAll(const Expr& e, const PointSet& ps, const Profile& profile);

// Single-point budget check (shared with the enumerator).
inline int codeN(float v, int bits) {
  const double c = v < 0.0f ? 0.0 : (v > 1.0f ? 1.0 : static_cast<double>(v));
  return static_cast<int>(c * ((1 << bits) - 1) + 0.5);
}
inline int code8(float v) { return codeN(v, 8); }
bool pointWithinBudget(const Budget& b, float target, float cand);

Klass classify(const Program& prog, const Expr& cand, const Metrics& worst);

// V2: the whole input domain (every grid value / every float32 of each component).
// domainSize returns its number of points, or 0 if that exceeds cap.
uint64_t domainSize(const Program& prog, uint64_t cap);
// Compares against the target on every point of the domain under one profile.
Metrics compareExhaustive(const Program& prog, const Expr& cand, const Profile& profile,
                          unsigned threads = 0);

}  // namespace sopt
