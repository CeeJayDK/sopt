// Integer and bit-cast ops (bit tricks, --bits).
#include <bit>
#include <cmath>
#include <string>

#include "ir/eval.hpp"
#include "ir/parser.hpp"
#include "test.hpp"
#include "verify/points.hpp"
#include "verify/verify.hpp"

using namespace sopt;

namespace {

std::vector<InputDecl> xy() { return {{"x", -4, 4, 0}, {"y", -4, 4, 0}}; }

std::string roundtrip(const char* s) { return toString(parseExpr(s, xy()), xy()); }

float eval1(const char* s, float x, float y = 0.0f) {
  const Expr e = parseExpr(s, xy());
  PointSet ps;
  ps.add({x, y});
  return evalAll(e, ps, kProfileRef)[0];
}

}  // namespace

TEST(bits_parse_print) {
  CHECK(roundtrip("asfloat(asuint(x) & 0x7FFFFFFFu)") == "asfloat(asuint(x) & 0x7FFFFFFFu)");
  CHECK(roundtrip("asfloat((asuint(x) >> 9) | 0x3f800000u) - 1.0") == "asfloat((asuint(x) >> 9) | 0x3F800000u) - 1.0");
  CHECK(roundtrip("asfloat(asuint(x) ^ (asuint(y) & 0x80000000u))") ==
        "asfloat(asuint(x) ^ (asuint(y) & 0x80000000u))");
  CHECK(roundtrip("float(asint(asuint(x) + 1u))") == "float(asint(asuint(x) + 1u))");
  CHECK(roundtrip("asfloat(asuint(asint(asuint(x)) >> 31) | 0x3F800000u)") ==
        "asfloat(asuint(asint(asuint(x)) >> 31) | 0x3F800000u)");
  CHECK(roundtrip("float(uint(x * 255.0))") == "float(uint(x * 255.0))");
  CHECK(roundtrip("float(asint(asuint(int(x))))") == "float(asint(asuint(int(x))))");
  // Printed forms parse back to the same DAG.
  const char* forms[] = {"asfloat(asuint(asint(asuint(x)) >> 31) | 0x3F800000u)", "float(asint(asuint(int(x))))",
                         "asfloat((asuint(x) << 1u) - (asuint(y) * 3u))"};
  for (const char* f : forms) CHECK(roundtrip(roundtrip(f).c_str()) == roundtrip(f));
  // Constant folding of integer subexpressions.
  CHECK(roundtrip("asfloat(asuint(x) & (0xFFu << 4))") == "asfloat(asuint(x) & 4080u)");
}

TEST(bits_eval) {
  CHECK(eval1("asfloat(asuint(x) & 0x7FFFFFFFu)", -2.5f) == 2.5f);  // abs
  CHECK(eval1("asfloat((asuint(x) & 0x80000000u) | 0x3F800000u)", -0.0f) == -1.0f);  // copysign(1, x)
  CHECK(eval1("asfloat((asuint(x) & 0x80000000u) | 0x3F800000u)", 3.0f) == 1.0f);
  CHECK(eval1("float(asint(asuint(asint(asuint(x)) >> 31)))", -1.0f) == -1.0f);  // arithmetic shift
  CHECK(eval1("float(asuint(x) >> 31)", -1.0f) == 1.0f);                         // logical shift
  CHECK(eval1("float(uint(x))", -3.0f) == 0.0f);                                 // D3D: negative -> 0
  CHECK(eval1("float(uint(x))", 3.9f) == 3.0f);
  CHECK(eval1("float(asint(asuint(int(x))))", -3.9f) == -3.0f);
  CHECK(eval1("float(asuint(x) << 33u)", std::bit_cast<float>(1u)) == 2.0f);  // count & 31
  CHECK(eval1("asfloat(asuint(x) - asuint(y) * 2u)", 1.0f, 0.0f) == 1.0f);
}

TEST(bits_costs) {
  // Every integer op costs >= 1 in every model (levels stay well-founded).
  const char* models[] = {"generic", "rdna3", "search", "nvidia", "nvidia-maxwell", "nvidia-pascal", "nvidia-turing",
                          "nvidia-ampere", "nvidia-blackwell", "intel-gen9", "intel-gen7.5", "amd-rdna2", "amd-rdna4",
                          "amd-gcn5", "amd-terascale2"};
  for (const char* name : models) {
    const CostModel* m = costModelByName(name);
    CHECK(m != nullptr);
    for (Op op = Op::AsUint; op < Op::Count; op = static_cast<Op>(static_cast<int>(op) + 1)) CHECK((*m)[op] >= 1);
  }
  CHECK(dagCost(parseExpr("asfloat(asuint(x) & 0x7FFFFFFFu)", xy()), costRdna3()) == 1 + 4 + 1);
}
