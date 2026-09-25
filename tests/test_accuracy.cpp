#include <cmath>
#include <string>

#include "ir/parser.hpp"
#include "test.hpp"
#include "verify/exact.hpp"
#include "verify/points.hpp"
#include "verify/problems.hpp"
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

TEST(problem_ranges) {
  // iMMERSE LAUNCHPAD radius_scale; the second rewrite divides by 0.01 * F - 2.0,
  // which is 0 at F = 200: NaN there, fine everywhere else.
  Program p = parseProgram(
      "input F : const float in [100, 10000] = 1000\n"
      "input R : float in [0, 1]\n"
      "output r = (0.5 + F * 0.01 * saturate(R)) / 50.0 * 0.15\n"
      "budget r : rel 1e-6\n");
  const Expr good = parseExpr("mad(R, 0.15 / (50.0 / F / 0.01), 0.0015)", p.inputs);
  CHECK(findProblemRanges(p, good, false).empty());
  const Expr pole = parseExpr(
      "mad(mad(R, 2.0 / (0.01 * F - 2.0), R), (0.01 * F - 2.0) * 0.15 / 50.0, 0.0015000001)", p.inputs);
  const auto probs = findProblemRanges(p, pole, false);
  CHECK(probs.size() == 1);
  if (probs.size() == 1) {
    // At the next float above 200 the result is finite but far off.
    CHECK(probs[0].input == 0 && probs[0].lo == 200.0 && probs[0].notFinite);
    CHECK(probs[0].hi == static_cast<double>(std::nextafter(200.0f, 1e9f)));
    CHECK(describeProblems(p, probs) == "fails at F = [200, 200.00002] (NaN/inf at some), fine on [100, 200) and (200.00002, 10000]");
  }
  // The original has the same pole: not a problem of the candidate.
  Program q = parseProgram("input x : float in [0, 1]\noutput r = 1.0 / (x - 0.25)\nbudget r : rel 1e-6\n");
  CHECK(findProblemRanges(q, parseExpr("rcp(x - 0.25)", q.inputs), false).empty());
  // A domain edge: sqrt of a negative value on part of the range.
  Program s = parseProgram("input x : float in [0, 4]\noutput r = abs(x - 1.0)\nbudget r : abs 1e-3\n");
  const auto sq = findProblemRanges(s, parseExpr("sqrt((x - 1.0) * (x - 1.0))", s.inputs), false);
  CHECK(sq.empty());  // never negative
}

TEST(problem_ranges_depth) {
  // ReShade.fxh's reversed depth rewrite (shipped with FAR_PLANE in [100, 10000]) on the
  // hard limits' lower end: rcp(F - 1.0) at F = 1.
  Program p = parseProgram(
      "input t : float in [0, 1] grid 16777215\n"
      "input F : const float in [1, 10000] = 1000\n"
      "output r = (1.0 - t) / (F - (1.0 - t) * (F - 1.0))\n"
      "budget r : rel 1e-6\n");
  const Expr e = parseExpr("mad(rcp(t + rcp(F - 1.0)), rcp(F - (2.0 - rcp(F))), rcp(1.0 - F))", p.inputs);
  const auto probs = findProblemRanges(p, e, false);
  CHECK(describeProblems(p, probs) == "fails at F = [1, 1.2342985] (NaN/inf at some), fine on (1.2342985, 10000]");
  Program q = p;
  q.inputs[1].lo = 100.0;
  CHECK(findProblemRanges(q, e, false).empty());
}
