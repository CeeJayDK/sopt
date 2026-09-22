#include <string>

#include "ir/parser.hpp"
#include "test.hpp"

using namespace sopt;

namespace {

std::vector<InputDecl> abcx() {
  return {{"a", 0, 1, 0}, {"b", 0, 1, 0}, {"c", 0, 1, 0}, {"x", 0, 1, 0}};
}

std::string roundtrip(const char* s) { return toString(parseExpr(s, abcx()), abcx()); }

bool throws(const char* s) {
  try {
    parseExpr(s, abcx());
  } catch (const ParseError&) {
    return true;
  }
  return false;
}

}  // namespace

TEST(parser_roundtrip) {
  CHECK(roundtrip("1.0 - (1.0 - a) * (1.0 - b)") == "1.0 - (1.0 - a) * (1.0 - b)");
  CHECK(roundtrip("a - (b - c)") == "a - (b - c)");
  CHECK(roundtrip("(a - b) - c") == "a - b - c");
  CHECK(roundtrip("a / (b * c)") == "a / (b * c)");
  CHECK(roundtrip("x >= 0.5 ? b : a") == "x >= 0.5 ? b : a");
  CHECK(roundtrip("lerp(a, b, step(0.5, x))") == "lerp(a, b, step(0.5, x))");
  CHECK(roundtrip("-(a + b)") == "-(a + b)");
  CHECK(roundtrip("a * -2.0") == "a * -2.0");
}

TEST(parser_constant_folding) {
  CHECK(roundtrip("a * (2.0 * 0.5)") == "a * 1.0");
  CHECK(roundtrip("-0.25 + a") == "-0.25 + a");
  CHECK(roundtrip("saturate(1.5)") == "1.0");
}

TEST(parser_errors) {
  CHECK(throws("foo(a)"));
  CHECK(throws("a +"));
  CHECK(throws("q"));
  CHECK(throws("a ? b : c"));         // condition must be a comparison
  CHECK(throws("(a < b) + c"));       // bool used as float
}

TEST(parser_dag_cost_counts_shared_nodes_once) {
  const auto e = parseExpr("a * b + a * b", abcx());
  for (const CostModel* m : {&costGeneric(), &costRdna3()})
    CHECK(dagCost(e, *m) == (*m)[Op::Mul] + (*m)[Op::Add]);  // shared mul: no contraction
}

TEST(cost_contraction) {
  const CostModel& m = costRdna3();
  // Single-use mul (or div) under add/sub is one fma.
  CHECK(dagCost(parseExpr("a * b + c", abcx()), m) == m[Op::Mul] + m.fusedAdd);
  CHECK(dagCost(parseExpr("c - a * b", abcx()), m) == m[Op::Mul] + m.fusedAdd);
  CHECK(dagCost(parseExpr("a / b - c", abcx()), m) == m[Op::Div] + m.fusedAdd);
  CHECK(dagCost(parseExpr("a * b + a * c", abcx()), m) == 2u * m[Op::Mul] + m.fusedAdd);
  CHECK(dagCost(parseExpr("a * b + c", abcx()), costGeneric()) ==
        costGeneric()[Op::Mul] + costGeneric()[Op::Add]);
  // Every non-leaf op costs >= 1 in every model.
  for (const CostModel* cm : {&costGeneric(), &costRdna3()})
    for (size_t i = 2; i < static_cast<size_t>(Op::Count); ++i) CHECK(cm->cost[i] >= 1);
}

TEST(parser_program) {
  const Program p = parseProgram(
      "# comment\n"
      "input a : float in [0, 1] grid 255\n"
      "input t : float in [-1, 2]\n"
      "output r = lerp(a, 1.0, t)   # trailing\n"
      "budget r : color8 maxdiff 0\n");
  CHECK(p.inputs.size() == 2);
  CHECK(p.inputs[0].grid == 255);
  CHECK(p.inputs[1].lo == -1.0);
  CHECK(p.budget.kind == Budget::Kind::Color8);
  CHECK(p.budget.maxCodeDiff == 0);
  CHECK(toString(p.target, p.inputs) == "lerp(a, 1.0, t)");
}
