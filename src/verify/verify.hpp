#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
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
// Accurate: passes only by the accuracy rule at some points (closer to the exact value
// than the budget allows around the float32 original, but at least as accurate).
// LessAccurate: passes only the loose budget (Budget::loose).
enum class Klass { BitExact, Identical8, Within, Accurate, LessAccurate };
const char* klassName(Klass k, int codeBits = 8);

struct Metrics {
  bool pass = true;
  bool loosePass = true;  // passes the loose budget (== pass when Budget::loose is off)
  std::vector<float> looseFailPoint;
  bool bitExact = true;
  int maxCodeDiff = 0;
  double maxAbs = 0.0;
  double maxRel = 0.0;
  uint64_t checked = 0;       // points where the target is finite
  uint64_t codeChanged = 0;   // points whose 8-bit code differs
  uint64_t viaExact = 0;      // points accepted only by the accuracy rule
  double exactAbs = 0.0;      // max |value - exact value| (when the rule applies)
  double exactRel = 0.0;      // ... relative to max(1, |exact value|)
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
// exactVals: optional precomputed exact target values (evalExactAll), used when the
// accuracy rule applies (accuracyRule); computed per block otherwise.
Metrics compare(const Program& prog, const Expr& cand, const PointSet& ps, const Profile& profile,
                unsigned threads = 0, const std::vector<float>* targetVals = nullptr,
                const std::vector<double>* exactVals = nullptr);

// All values of e over ps, component-major: [c * ps.size() + point].
std::vector<float> evalAll(const Expr& e, const PointSet& ps, const Profile& profile);

// Single-point budget check (shared with the enumerator).
inline int codeN(float v, int bits) {
  const double c = v < 0.0f ? 0.0 : (v > 1.0f ? 1.0 : static_cast<double>(v));
  return static_cast<int>(c * ((1 << bits) - 1) + 0.5);
}
inline int code8(float v) { return codeN(v, 8); }
inline bool pointWithinBudget(const Budget& b, float t, float c) {
  if (!std::isfinite(c)) return false;
  switch (b.kind) {
    case Budget::Kind::Exact: return c == t;
    case Budget::Kind::Color8:
    case Budget::Kind::Color10:
      return std::abs(codeN(c, b.codeBits()) - codeN(t, b.codeBits())) <= b.maxCodeDiff;
    case Budget::Kind::Texcoord: return std::fabs(static_cast<double>(c) - t) <= b.eps;
    case Budget::Kind::Abs: return std::fabs(static_cast<double>(c) - t) <= b.eps;
    case Budget::Kind::Rel:
      return std::fabs(static_cast<double>(c) - t) <= b.eps * std::max(1.0, std::fabs(double(t)));
  }
  return false;
}
inline bool accuracyRule(const Budget& b) { return b.vsExact && b.kind != Budget::Kind::Exact; }
// The accuracy rule at one point: cand is at least as close to the exact value as the
// original target is (times `scale`), or within the budget of the exact value.
bool pointAccurate(const Budget& b, float target, double exact, float cand, double scale = 1.0);
// Budget::loose applied: eps times loose, color budgets one more code.
Budget looseBudget(const Budget& b);
inline bool pointLoose(const Budget& b, float target, const double* exact, float cand) {
  if (!(b.loose > 1.0) || b.kind == Budget::Kind::Exact) return false;
  const Budget lb = looseBudget(b);
  return pointWithinBudget(lb, target, cand) || (exact && pointAccurate(lb, target, *exact, cand, b.loose));
}
inline bool pointAcceptable(const Budget& b, float target, const double* exact, float cand) {
  return pointWithinBudget(b, target, cand) || (exact && pointAccurate(b, target, *exact, cand));
}

Klass classify(const Program& prog, const Expr& cand, const Metrics& worst);

// V2: the whole input domain (every grid value / every float32 of each component).
// domainSize returns its number of points, or 0 if that exceeds cap.
uint64_t domainSize(const Program& prog, uint64_t cap);
// Compares against the target on every point of the domain under one profile.
Metrics compareExhaustive(const Program& prog, const Expr& cand, const Profile& profile,
                          unsigned threads = 0);

}  // namespace sopt
