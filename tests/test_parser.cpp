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
  CHECK(roundtrip("-0.25 + a") == "a - 0.25");  // a + -c prints as a - c
  CHECK(roundtrip("saturate(1.5)") == "1.0");
}

// Intrinsics written out the way DXC lowers them.
TEST(parser_sugar) {
  CHECK(roundtrip("radians(a)") == "a * 0.017453292");
  CHECK(roundtrip("degrees(a)") == "a * 57.29578");
  CHECK(roundtrip("log10(a)") == roundtrip("log2(a) * 0.3010299956639812"));
  CHECK(roundtrip("tan(a)") == "sin(a) / cos(a)");
  CHECK(throws("cross(a, b)"));  // float3 only
  InputDecl u{"u", -1.0, 1.0, 0}, v{"v", -1.0, 1.0, 0};
  u.type = v.type = Type::Float3;
  const std::vector<InputDecl> in = {u, v};
  CHECK(toString(parseExpr("cross(u, v)", in), in) == toString(parseExpr("u.yzx * v.zxy - u.zxy * v.yzx", in), in));
}

TEST(needs_precise) {
  const std::vector<InputDecl> in = {{"a", -1e6, 1e6, 0}};
  CHECK(needsPrecise(parseExpr("(a + 12582912.0) - 12582912.0", in)));
  CHECK(needsPrecise(parseExpr("-12582912.0 + (12582912.0 + a)", in)));
  CHECK(needsPrecise(parseExpr("mad(a, 1000.0, 12582912.0) - 12582912.0", in)));
  CHECK(!needsPrecise(parseExpr("(a + 1.5) - 1.5", in)));             // too small to round to integers
  CHECK(!needsPrecise(parseExpr("(a + 12582912.0) - 8388608.0", in)));  // different constants
  CHECK(!needsPrecise(parseExpr("round(a)", in)));
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

// rdna3 context effects (CostModel::amdFolds): omod and three-operand min / max cost 1.
TEST(cost_amd_folds) {
  const CostModel& m = costRdna3();
  const CostModel& off = *withoutAmdFolds(&m);
  auto cost = [&](const char* s, const CostModel& model) { return dagCost(parseExpr(s, abcx()), model); };
  CHECK(cost("rcp(a) * 2.0", m) == 17u);   // v_rcp_f32 with omod
  CHECK(cost("rcp(a) * 2.0", off) == 20u);
  CHECK(cost("rcp(a) * -0.5", m) == 17u);
  CHECK(cost("rcp(a) * 3.0", m) == 20u);   // not an omod scale
  CHECK(cost("a * 2.0", m) == 4u);         // nothing to fold into
  CHECK(cost("(a + b) * 4.0", m) == 5u);
  CHECK(cost("a * 2.0 + b", m) == 5u);     // an fma, not an omod
  CHECK(cost("rcp(a) * 2.0 * rcp(a)", m) == 24u);  // shared rcp: no omod
  CHECK(cost("max(max(a, b), c)", m) == 5u);       // v_max3
  CHECK(cost("min(max(a, b), c)", m) == 5u);       // v_minmax / v_med3
  CHECK(cost("max(max(max(a, b), c), x)", m) == 9u);  // max3 + max
  CHECK(cost("max(max(a, b), c)", off) == 8u);
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
  for (const CostModel* cm : {&costGeneric(), &costRdna3(), &costNvidia(), &costNvidiaPascal(), &costNvidiaTuring(), &costNvidiaAmpere(), &costNvidiaBlackwell(), &costIntelGen9()})
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
