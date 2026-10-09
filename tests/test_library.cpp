#include <string>

#include "ir/parser.hpp"
#include "search/driver.hpp"
#include "search/library.hpp"
#include "test.hpp"
#include "verify/points.hpp"

using namespace sopt;

namespace {

bool hasForm(const std::vector<LibraryForm>& forms, const Program& p, const std::string& text) {
  for (const auto& f : forms)
    if (toString(f.expr, p.inputs) == text) return true;
  return false;
}

std::vector<LibraryForm> rewrite(const Program& p, const std::string& rules) {
  const Library lib = parseLibrary(rules, "test");
  const PointSet ps = makeRandomPoints(p, 4096, 3, true);
  return libraryRewrites(p, lib, ps, defaultCostModel());
}

}  // namespace

TEST(library_parse) {
  const Library lib = parseLibrary(
      "# comment\n"
      "lerp(a, b, t) -> mad(t, b - a, a)   # expansion\n"
      "\n"
      "saturate(x) -> x   where x in [0, 1]\n"
      "x / c -> x * (1.0 / c)   where c const, c != 0\n"
      "length(v) * length(v) -> dot(v, v)   where v : float3\n",
      "test");
  CHECK(lib.rules.size() == 4);
  CHECK(lib.rules[0].vars.size() == 3);
  CHECK(lib.rules[1].conds.size() == 1 && lib.rules[1].vars[0].lo == 0.0 && lib.rules[1].vars[0].hi == 1.0);
  CHECK(lib.rules[2].conds.size() == 2 && lib.rules[2].conds[0].kind == RuleCond::Kind::Const);
  CHECK(lib.rules[3].vars[0].type == Type::Float3);
  // A rule over several lines, as sopt-fx writes sopt-found.txt.
  const Library multi = parseLibrary(
      "# region\n"
      "saturate(x)\n"
      "  ->\n"
      "x\n"
      "  where x in [0, 1]\n"
      "\n"
      "lerp(a, b, t) ->\n"
      "mad(t, b - a, a)\n",
      "multi");
  CHECK(multi.rules.size() == 2);
  CHECK(multi.rules[0].conds.size() == 1 && multi.rules[0].vars[0].hi == 1.0);
  CHECK(multi.rules[0].source == "multi:2");
  CHECK(multi.rules[1].vars.size() == 3);
  bool threw = false;
  try {
    parseLibrary("lerp(a, b, t) mad(t, b - a, a)\n", "bad");
  } catch (const ParseError&) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    parseLibrary("abs(x) -> x   where y >= 0\n", "bad");  // not a variable of the pattern
  } catch (const ParseError&) {
    threw = true;
  }
  CHECK(threw);
}

// Every rule of the shipped library holds (rel 1e-6 or at least as close to exact math,
// every semantic profile), and the built-in copy is the same file.
TEST(library_rules_hold) {
  const Library lib = loadLibrary(SOPT_LIBRARY_FILE);
  CHECK(lib.rules.size() >= 20);
  for (const auto& r : lib.rules) {
    const RuleCheck c = checkRule(r);
    if (!c.pass) std::printf("  %s: %s (%s)\n", r.source.c_str(), r.text.c_str(), c.note.c_str());
    CHECK(c.pass);
  }
  CHECK(defaultLibrary().rules.size() == lib.rules.size());
}

TEST(library_check_rejects_wrong_rule) {
  const Library lib = parseLibrary("lerp(a, b, t) -> mad(t, b, a)\n", "wrong");
  CHECK(!checkRule(lib.rules[0]).pass);
}

TEST(library_rewrites_fold_and_match) {
  const Program p = parseProgram("input x : float in [0, 2]\noutput r = lerp(1.0, 2.5, saturate(x))\nbudget r : rel 1e-6\n");
  const auto forms = rewrite(p, "lerp(a, b, t) -> mad(t, b - a, a)\n");
  CHECK(hasForm(forms, p, "mad(saturate(x), 1.5, 1.0)"));  // 2.5 - 1.0 folded
}

TEST(library_conditions) {
  const std::string rules = "saturate(x) -> x   where x in [0, 1]\n";
  const Program in01 = parseProgram("input a : float in [0, 1]\ninput b : float in [0, 1]\noutput r = saturate(a * b)\nbudget r : rel 1e-6\n");
  CHECK(hasForm(rewrite(in01, rules), in01, "a * b"));
  const Program in02 = parseProgram("input a : float in [0, 2]\ninput b : float in [0, 1]\noutput r = saturate(a * b)\nbudget r : rel 1e-6\n");
  CHECK(rewrite(in02, rules).empty());
  // The same variable twice must be the same subexpression; commutative operands match
  // in either order.
  const std::string fac = "a * b + a * c -> a * (b + c)\n";
  const Program f = parseProgram("input x : float in [-1, 1]\ninput y : float in [-1, 1]\ninput z : float in [-1, 1]\noutput r = y * x + x * z\nbudget r : rel 1e-6\n");
  CHECK(hasForm(rewrite(f, fac), f, "x * (y + z)"));
  const Program g = parseProgram("input x : float in [-1, 1]\ninput y : float in [-1, 1]\ninput z : float in [-1, 1]\noutput r = y * x + y * z\nbudget r : rel 1e-6\n");
  CHECK(hasForm(rewrite(g, fac), g, "y * (x + z)"));
}

TEST(library_in_driver) {
  // pow(abs(u), 2.0) * pow(abs(u), 2.0) costs two pows; the library gives (u * u) * (u * u).
  const Program p = parseProgram(
      "input a : float in [0, 1]\ninput b : float in [0, 1]\ninput c : float in [0, 1]\n"
      "output r = pow(abs(a * b - c), 4.0) + lerp(a, b, step(0.5, c))\nbudget r : rel 1e-6\n");
  Options opt;
  opt.search.timeLimitSec = 2.0;
  opt.subtrees = false;
  opt.cuts = false;
  opt.v3 = false;
  opt.library = true;
  const RunResult r = optimize(p, opt);
  CHECK(r.libraryForms > 0);
  CHECK(r.libraryBest > 0 && r.libraryBest < r.targetCost);
  CHECK(!r.accepted.empty() && r.accepted[0].cost <= r.libraryBest);
}
