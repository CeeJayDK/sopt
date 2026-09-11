#include <string>

#include "ir/parser.hpp"
#include "search/driver.hpp"
#include "test.hpp"

using namespace sopt;

namespace {

// M1 done criterion: the enumerator finds each known rewrite on its own, i.e. an
// accepted alternative at least as cheap as the "# expect:" expression.
void expectRewrite(const char* file) {
  const std::string path = std::string(SOPT_EXAMPLES_DIR) + "/" + file;
  const Program prog = loadProgram(path);
  const std::string expect = readExpect(path);
  CHECK(!expect.empty());
  const uint32_t expectCost = dagCost(parseExpr(expect, prog.inputs));

  Options opt;
  opt.v1Points = 1u << 16;
  const RunResult r = optimize(prog, opt);
  CHECK(!r.accepted.empty());
  if (r.accepted.empty()) return;
  bool foundText = false;
  for (const auto& a : r.accepted) foundText = foundText || a.text == expect;
  std::printf("  %s: target cost %u, best cost %u (%s: %s), expected %u (%s)%s\n", file,
              r.targetCost, r.accepted[0].cost, klassName(r.accepted[0].klass),
              r.accepted[0].text.c_str(), expectCost, expect.c_str(),
              foundText ? ", expected text found" : "");
  CHECK(r.accepted[0].cost <= expectCost);
}

}  // namespace

TEST(rewrite_step_lerp) { expectRewrite("step_lerp.sopt"); }
TEST(rewrite_screen) { expectRewrite("screen.sopt"); }
TEST(rewrite_pow2) { expectRewrite("pow2.sopt"); }
TEST(rewrite_rsqrt) { expectRewrite("rsqrt.sopt"); }
TEST(rewrite_saturate_range) { expectRewrite("saturate_range.sopt"); }
TEST(rewrite_factor) { expectRewrite("factor.sopt"); }
