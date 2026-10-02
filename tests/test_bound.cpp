#include <cmath>
#include <string>

#include "ir/parser.hpp"
#include "test.hpp"
#include "verify/bound.hpp"
#include "verify/points.hpp"
#include "verify/verify.hpp"

using namespace sopt;

namespace {

// Largest |cand - target| over random points under every semantic profile.
double sampledMax(const Program& p, const Expr& cand) {
  const PointSet ps = makeRandomPoints(p, 1u << 16, 11, true);
  double m = 0.0;
  for (const auto& prof : kAllProfiles) {
    const std::vector<float> t = evalAll(p.target, ps, prof), c = evalAll(cand, ps, prof);
    for (size_t i = 0; i < t.size(); ++i)
      if (std::isfinite(t[i])) m = std::max(m, std::fabs(double(c[i]) - t[i]));
  }
  return m;
}

}  // namespace

TEST(bound_identity_proven) {
  const Program p = parseProgram(
      "input x : float in [0, 1]\ninput y : float in [0, 1]\ninput z : float in [-1, 1]\n"
      "output r = x * y + z\nbudget r : abs 1e-6\n");
  const Expr c = parseExpr("mad(y, x, z)", p.inputs);
  const BoundResult b = proveBound(p, c);
  CHECK(b.proven && b.fraction == 1.0);
  CHECK(b.maxBound > 0.0 && b.maxBound <= 1e-6);
  CHECK(sampledMax(p, c) <= b.maxBound);  // the bound holds where we can look
}

TEST(bound_wrong_not_proven) {
  const Program p = parseProgram(
      "input x : float in [0, 1]\ninput y : float in [0, 1]\n"
      "output r = x * y\nbudget r : abs 1e-6\n");
  // Off by 2e-6 everywhere: nothing can be proven.
  BoundOptions o;
  o.seconds = 0.5;
  const BoundResult everywhere = proveBound(p, parseExpr("x * y + 2e-6", p.inputs), o);
  CHECK(!everywhere.proven && everywhere.fraction == 0.0);
  // Off only for x > 0.999: most of the domain, never all of it.
  const BoundResult corner = proveBound(p, parseExpr("x > 0.999 ? x * y + 0.001 : y * x", p.inputs), o);
  CHECK(!corner.proven && corner.fraction > 0.9 && corner.fraction < 1.0);
}

TEST(bound_rational_and_vector) {
  // A solved-constant rewrite (not an identity): the centered form bounds the difference
  // of the exact functions; the bound must cover what sampling sees under every profile.
  const Program p = parseProgram("input t : float in [0, 1]\noutput r = (t + 1.0) / (3.0 * t + 2.0)\n"
                                 "budget r : rel 1e-5\n");
  const Expr c = parseExpr("mad(rcp(t + 0.6666667), 0.11111111, 0.33333334)", p.inputs);
  const BoundResult b = proveBound(p, c);
  CHECK(b.proven);
  CHECK(sampledMax(p, c) <= b.maxBound);

  const Program v = parseProgram("input c : float3 in [0, 1]\ninput k : float in [0, 2]\n"
                                 "output r = saturate(c * k * 0.5 + 0.25)\nbudget r : color8\n");
  const Expr vc = parseExpr("saturate(mad(c, k * 0.5, 0.25))", v.inputs);
  const BoundResult bv = proveBound(v, vc);
  CHECK(bv.proven);
  CHECK(sampledMax(v, vc) <= bv.maxBound);
}

TEST(bound_relative_near_zero) {
  // x * (a + b) vs a * x + b * x: near a + b = 0 the rewrite's rounding is not relative
  // to the (tiny) result, so rel 1e-6 cannot hold there; the rest of the domain can.
  const Program p = parseProgram(
      "input a : float in [0, 1]\ninput b : float in [-1, 0]\ninput x : float in [1, 2]\n"
      "output r = x * (a + b)\nbudget r : rel 1e-6\n");
  BoundOptions o;
  o.seconds = 1.0;
  const BoundResult b = proveBound(p, parseExpr("mad(a, x, b * x)", p.inputs), o);
  CHECK(!b.proven && b.fraction < 1.0);
  // Undefined points of the original (rcp through 0) are never proven either.
  const Program q = parseProgram("input t : float in [-1, 1]\noutput r = rcp(t) * 2.0\nbudget r : rel 1e-5\n");
  const BoundResult bq = proveBound(q, parseExpr("2.0 / t", q.inputs), o);
  CHECK(!bq.proven && bq.fraction > 0.5);
}
