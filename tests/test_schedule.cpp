#include <string>

#include "ir/parser.hpp"
#include "search/driver.hpp"
#include "search/reshape.hpp"
#include "test.hpp"

using namespace sopt;

namespace {

bool hasForm(const std::vector<Expr>& forms, const Program& p, const std::string& text) {
  for (const auto& f : forms)
    if (toString(f, p.inputs) == text) return true;
  return false;
}

const char* kAverage =
    "input a : fetch float in [0, 1]\n"
    "input b : fetch float in [0, 1]\n"
    "input c : fetch float in [0, 1]\n"
    "input d : fetch float in [0, 1]\n"
    "output r = (a + b + c + d) * 0.25\n"
    "budget r : rel 1e-6\n";

const char* kUniforms =
    "input t : fetch float in [0, 1]\n"
    "input s : uniform float in [0, 2]\n"
    "input k : uniform float in [1, 4]\n"
    "output r = t * s * k + s\n"
    "budget r : rel 1e-6\n";

}  // namespace

TEST(schedule_rates_parsed) {
  const Program p = parseProgram(kUniforms);
  CHECK(p.inputs[0].rate == InputDecl::Rate::Fetch && p.inputs[0].fetchOrder == 0);
  CHECK(p.inputs[1].rate == InputDecl::Rate::Uniform && !p.inputs[1].compileTime);
  const Program q = parseProgram(kAverage);
  CHECK(q.inputs[3].fetchOrder == 3);
}

TEST(schedule_metrics) {
  const CostModel& m = defaultCostModel();
  // Sum then multiply: the last fetch d feeds the last add and the multiply.
  const Program p = parseProgram(kAverage);
  const ScheduleMetrics s = scheduleMetrics(p.target, m, p.inputs);
  CHECK(s.tail == m.opCost(Op::Add, 1) + m.opCost(Op::Mul, 1));
  CHECK(s.critical == 3 * m.opCost(Op::Add, 1) + m.opCost(Op::Mul, 1));
  CHECK(s.perfCost == dagCost(p.target, m, p.inputs));  // no uniforms
  // The mad chain: only the last mad waits for d.
  const Program q = parseProgram(std::string(kAverage).replace(
      std::string(kAverage).find("(a + b"), std::string("(a + b + c + d) * 0.25").size(),
      "mad(0.25, d, mad(0.25, c, mad(0.25, b, a * 0.25)))"));
  CHECK(scheduleMetrics(q.target, m, q.inputs).tail == m.opCost(Op::Mad, 1));
  // Performance mode folds the uniforms' product in t * (s * k) but not in (t * s) * k.
  const Program u = parseProgram(kUniforms);
  const ScheduleMetrics su = scheduleMetrics(u.target, m, u.inputs);
  CHECK(su.perfCost == dagCost(u.target, m, u.inputs));
  const Program v = parseProgram(std::string(kUniforms).replace(std::string(kUniforms).find("t * s * k + s"), 13,
                                                                 "mad(t, s * k, s)"));
  CHECK(scheduleMetrics(v.target, m, v.inputs).perfCost == m.opCost(Op::Mad, 1));
}

TEST(schedule_reshape_forms) {
  const Program p = parseProgram(kAverage);
  const std::vector<Expr> forms = reshapeForms(p.target, p.inputs, defaultCostModel());
  CHECK(forms.size() == 1);  // the regrouped sum is the target again
  CHECK(hasForm(forms, p, "mad(0.25, d, mad(0.25, c, mad(0.25, b, a * 0.25)))"));
  // Uniform factors first: s * k folds in performance mode.
  const Program u = parseProgram(kUniforms);
  CHECK(hasForm(reshapeForms(u.target, u.inputs, defaultCostModel()), u, "mad(t, s * k, s)"));
}

TEST(schedule_variants) {
  Options opt;
  opt.search.timeLimitSec = 1.0;
  // The average: nothing cheaper, but the mad chain has the shorter tail.
  {
    const Program p = parseProgram(kAverage);
    const RunResult r = optimize(p, opt);
    bool chain = false;
    for (const auto& a : r.accepted) chain = chain || (a.betterScheduling && a.tail < r.targetTail);
    CHECK(chain);
    Options off = opt;
    off.schedule = false;
    CHECK(optimize(p, off).accepted.empty());
  }
  // Uniforms: the cheapest alternatives tie; the one that folds in performance mode comes first.
  {
    const Program u = parseProgram(kUniforms);
    const RunResult r = optimize(u, opt);
    CHECK(!r.accepted.empty());
    CHECK(r.accepted[0].perfCost < r.targetPerfCost);
    for (const auto& a : r.accepted)
      if (a.cost == r.accepted[0].cost) CHECK(a.perfCost >= r.accepted[0].perfCost);
    // Performance mode first: the main cost is the folded one.
    Options pf = opt;
    pf.perfFirst = true;
    const RunResult q = optimize(u, pf);
    CHECK(q.targetCost == q.targetPerfCost && q.targetNormalCost == r.targetCost);
    CHECK(!q.accepted.empty() && q.accepted[0].cost == q.accepted[0].perfCost);
  }
}
