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
  // from exact math at F = 10000. The cancellation-free rewrite (1 - t) / (1 + (F - 1) t)
  // is off by ~1.5e-7; the partial fraction form shipped earlier is not accepted any more
  // (relative error near t = 1, where the value goes to 0).
  Program p = parseProgram(
      "input t : float in [0, 1] grid 16777215\n"
      "input F : const float in [100, 10000] = 1000\n"
      "output r = (1.0 - t) / (F - (1.0 - t) * (F - 1.0))\n"
      "budget r : rel 1e-6\n");
  // The original cancels near t = 0 (F - (1 - t)(F - 1) ~ 1), so its error scale there is
  // large and the rewrite is within the budget of it (relBase), rule or not.
  const char* rewrite = "(t - 1.0) * rcp(mad(t, 1.0 - F, -1.0))";
  const Metrics m = check(p, rewrite);
  CHECK(m.pass && m.exactAbs < 1e-6);
  const Metrics orig = check(p, "(1.0 - t) / (F - (1.0 - t) * (F - 1.0))");
  CHECK(orig.exactAbs > 1e-4);
  const char* partial = "mad(rcp(t + rcp(F - 1.0)), F / ((F - 1.0) * (F - 1.0)), rcp(1.0 - F))";
  CHECK(!check(p, partial).pass);
  p.budget.vsExact = false;
  CHECK(check(p, rewrite).pass);
  CHECK(!check(p, partial).pass);
}

TEST(error_scale_budget) {
  // Rel budgets are relative to max(|t|, the original's error scale): absolute-like where the
  // original itself cancels (XOR x + y - 2xy near 0), relative where the small value is
  // computed exactly (t * (1 - t) near t = 1).
  Program x = parseProgram("input x : float in [0, 2.5]\ninput y : float in [0, 2.5]\n"
                           "output r = dot(float4(-x, -x, x, y), float4(y, y, 1.0, 1.0))\nbudget r : rel 1e-6\n");
  CHECK(check(x, "mad(x, 1.0 - (y + y), y)").pass);
  Program t = parseProgram("input t : float in [0, 1]\noutput r = t * (1.0 - t)\nbudget r : rel 1e-6\n");
  CHECK(!check(t, "t - t * t").pass);
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
  // ReShade.fxh's reversed depth on the far plane's hard limits [1, ...]. The partial
  // fraction rewrite shipped earlier fails near t = 1 for every F (relative error, GPU rcp);
  // the cancellation-free form (accuracy variant) is fine everywhere, F = 1 included.
  Program p = parseProgram(
      "input t : float in [0, 1] grid 16777215\n"
      "input F : const float in [1, 10000] = 1000\n"
      "output r = (1.0 - t) / (F - (1.0 - t) * (F - 1.0))\n"
      "budget r : rel 1e-6\n");
  const Expr old = parseExpr("mad(rcp(t + rcp(F - 1.0)), rcp(F - (2.0 - rcp(F))), rcp(1.0 - F))", p.inputs);
  const auto probs = findProblemRanges(p, old, false);
  CHECK(!probs.empty() && probs[0].lo == 1.0);
  CHECK(findProblemRanges(p, parseExpr("(t - 1.0) * rcp(mad(t, 1.0 - F, -1.0))", p.inputs), false).empty());
}
