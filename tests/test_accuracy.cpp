#include <cmath>
#include <string>

#include "ir/parser.hpp"
#include "test.hpp"
#include "verify/exact.hpp"
#include "verify/points.hpp"
#include "verify/verify.hpp"

using namespace sopt;

namespace {

Metrics check(const Program& p, const char* cand) {
  const PointSet ps = makeRandomPoints(p, 1u << 16, 7, true);
  return compare(p, parseExpr(cand, p.inputs), ps, kProfileRef, 1);
}

}  // namespace

TEST(accuracy_exact_values) {
  Program p = parseProgram("input a : float in [1, 2]\noutput r = a / 3.0 + a * a\nbudget r : rel 1e-6\n");
  const PointSet ps = makeRandomPoints(p, 64, 3, true);
  const std::vector<double> x = evalExactAll(p.target, ps);
  for (size_t i = 0; i < ps.size(); ++i) {
    const double a = ps.cols[0][i];
    CHECK(std::fabs(x[i] - (a / 3.0 + a * a)) <= 1e-15 * x[i]);
  }
}

TEST(accuracy_rule_depth) {
  // ReShade's reversed depth linearization: the float32 original is off by up to 4e-4
  // from exact math at F = 10000, the rewrite by ~1.5e-7.
  Program p = parseProgram(
      "input t : float in [0, 1] grid 16777215\n"
      "input F : const float in [100, 10000] = 1000\n"
      "output r = (1.0 - t) / (F - (1.0 - t) * (F - 1.0))\n"
      "budget r : rel 1e-6\n");
  const char* rewrite = "mad(rcp(t + rcp(F - 1.0)), F / ((F - 1.0) * (F - 1.0)), rcp(1.0 - F))";
  const Metrics m = check(p, rewrite);
  CHECK(m.pass && m.viaExact > 0 && m.exactAbs < 1e-6);
  CHECK(classify(p, parseExpr(rewrite, p.inputs), m) == Klass::Accurate);
  const Metrics orig = check(p, "(1.0 - t) / (F - (1.0 - t) * (F - 1.0))");
  CHECK(orig.exactAbs > 1e-4);
  // Without the rule the rewrite is outside the budget of the float original.
  p.budget.vsExact = false;
  CHECK(!check(p, rewrite).pass);
}

TEST(accuracy_loose) {
  Program p = parseProgram("input a : float in [0, 1]\noutput r = a * 3.0\nbudget r : abs 1e-6\n");
  p.budget.loose = 100;
  const Metrics near = check(p, "a * 3.00001");  // error 1e-5: within 100x the budget
  CHECK(!near.pass && near.loosePass);
  CHECK(classify(p, parseExpr("a * 3.00001", p.inputs), near) == Klass::LessAccurate);
  const Metrics far = check(p, "a * 3.001");     // error 1e-3
  CHECK(!far.pass && !far.loosePass);
  // Exact budgets have neither the rule nor loose candidates.
  p.budget.kind = Budget::Kind::Exact;
  CHECK(!check(p, "a * 3.00001").loosePass);
}
