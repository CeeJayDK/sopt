#include <filesystem>
#include <string>

#include "ir/parser.hpp"
#include "search/cuts.hpp"
#include "search/driver.hpp"
#include "search/subtrees.hpp"
#include "test.hpp"

using namespace sopt;

namespace {

// M1 done criterion: the enumerator finds each known rewrite on its own, i.e. an
// accepted alternative at least as cheap as the "# expect:" expression.
void expectRewrite(const char* file, bool affine = false, size_t maxBank = 2'000'000,
                   const CostModel* model = nullptr, const CostModel* order = nullptr,
                   bool inner = false) {
  const std::string path = std::string(SOPT_EXAMPLES_DIR) + "/" + file;
  const Program prog = loadProgram(path);
  const std::string expect = readExpect(path);
  CHECK(!expect.empty());
  if (!model) model = &defaultCostModel();
  const uint32_t expectCost = dagCost(parseExpr(expect, prog.inputs), *model, prog.inputs);

  Options opt;
  opt.v1Points = 1u << 16;
  opt.search.affine = affine;
  opt.search.maxBank = maxBank;
  opt.search.overflow = false;  // these check what the bank alone reaches
  opt.search.sharedLeaves = false;  // ... with the plain leaves
  opt.subtrees = false;
  opt.cuts = false;
  opt.search.topDown = false;
  opt.search.twoPhase = false;
  opt.library = false;
  opt.search.model = model;
  opt.search.order = order;
  opt.search.inner = inner;
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

// Symbolic constants (--affine): the same rewrites, plus ones that need solved constants.
TEST(affine_step_lerp) { expectRewrite("step_lerp.sopt", true); }
TEST(affine_screen) { expectRewrite("screen.sopt", true); }
TEST(affine_pow2) { expectRewrite("pow2.sopt", true); }
TEST(affine_rsqrt) { expectRewrite("rsqrt.sopt", true); }
TEST(affine_saturate_range) { expectRewrite("saturate_range.sopt", true); }
TEST(affine_factor) { expectRewrite("factor.sopt", true); }
// Not reachable without --affine: K * rcp(t + c) + K2 with fitted K, K2.
TEST(affine_depth_reversed) { expectRewrite("depth_reversed.sopt", true, 300'000); }
// rdna3 objective with generic enumeration order: reaches rcp(t + c) (5 VALU-equivalents).
TEST(order_generic_depth_rdna3) {
  expectRewrite("depth_reversed.sopt", true, 300'000, &costRdna3(), &costGeneric());
}
// Inner constants (--inner): the pole / shift is solved, not taken from the pool.
TEST(inner_rational) { expectRewrite("rational.sopt", true, 50'000, nullptr, nullptr, true); }
TEST(inner_rsqrt_affine) { expectRewrite("rsqrt_affine.sopt", true, 50'000, nullptr, nullptr, true); }
// rdna3 without --order-model: rcp(t + c) is solved directly on the input.
TEST(inner_depth_rdna3) {
  expectRewrite("depth_reversed.sopt", true, 50'000, &costRdna3(), nullptr, true);
}
// rdna3 with its default order (search): sqrt at half order cost is reached
// after two-input products (plain rdna3 order stops before it).
TEST(order_rdna3_search_sqrt_product) {
  expectRewrite("sqrt_product.sopt", true, 2'000'000, &costRdna3(), nullptr, true);
}
// nvidia objective (default search order): the select is cheaper on NVIDIA (FSETP + FSEL
// vs FSET + FADD + FFMA).
TEST(nvidia_step_lerp) {
  expectRewrite("step_lerp.sopt", true, 2'000'000, &costNvidia(), nullptr, true);
}
// Compile-time input (far plane F): found at F = 1000, constants generalized into
// expressions of F and verified over F in [2, 1000].
TEST(specialize_depth_far) {
  expectRewrite("depth_far.sopt", true, 50'000, &costRdna3(), nullptr, true);
}

TEST(shared_leaves_reuse) {
  // pow(abs(u), 2.0) -> u * u: u appears twice, so its tree cost is out of reach in a
  // second; as a shared leaf (the target's own subexpression) it is one multiply away.
  Program p = parseProgram(
      "input c : float in [0, 16]\noutput r = pow(abs(c * 0.15 + -0.005000001), 2.0)\nbudget r : rel 1e-6\n");
  Options opt;
  opt.v1Points = 1u << 16;
  opt.search.timeLimitSec = 1.0;
  opt.search.sharedLeaves = true;
  const RunResult r = optimize(p, opt);
  CHECK(!r.accepted.empty());
  CHECK(!r.accepted.empty() && r.accepted[0].cost <= 12 && !containsOp(r.accepted[0].expr, Op::Pow));
}

TEST(subtree_search) {
  // Too large for the bottom-up search (cost 121): the subexpressions are searched on their
  // own and put back; the result is verified against the whole region's budget.
  Program p = parseProgram(
      "input c : float in [0, 16]\n"
      "output r = abs((0.005000001 - c * 0.15 - sqrt(pow(abs(c * 0.15 + -0.005000001), 2.0) - c * 0.072000004 * "
      "(c * 0.045 + 0.003 - 0.045))) / (0.3 * (c * 0.3 + 0.02 - 0.3)))\n"
      "budget r : rel 1e-6\n");
  Options opt;
  opt.v1Points = 1u << 16;
  opt.search.timeLimitSec = 1.0;
  opt.subtrees = true;
  opt.subtreeTime = 0.5;
  const RunResult r = optimize(p, opt);
  CHECK(r.subtreeSearches > 0);
  CHECK(!r.accepted.empty() && r.accepted[0].cost < r.targetCost);
  // replaceNodes / subexpr round trip: replacing a node by its own subexpression is the identity.
  const Expr& e = p.target;
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Expr s = subexpr(e, i);
    CHECK(toString(replaceNodes(e, {{i, &s}}), p.inputs) == toString(e, p.inputs));
  }
}

TEST(cut_points) {
  // v = a * b + c * 0.5 - a * c is all the rest reads of a, b, c: pow(abs(v), 4.0) is
  // top(cut) with the cut in v's sampled range. top alone is small enough to search
  // (cut * cut * (cut * cut)); the whole needs v shared, which plain leaves miss.
  Program p = parseProgram(
      "input a : float in [0, 1]\ninput b : float in [0, 1]\ninput c : float in [0, 1]\n"
      "output r = pow(abs(a * b + c * 0.5 - a * c), 4.0)\n"
      "budget r : rel 1e-5\n");
  const std::vector<Cut> cuts = findCuts(p, defaultCostModel(), 1);
  CHECK(!cuts.empty());
  bool whole = false;
  for (const Cut& c : cuts) {
    std::vector<uint32_t> map;
    const Program top = topProgram(p, c, map);
    CHECK(map.size() == top.inputs.size() && map.back() == UINT32_MAX);
    CHECK(c.lo < c.hi && c.lo >= -1.0 && c.hi <= 1.5);
    whole = whole || toString(subexpr(p.target, c.node), p.inputs) == "a * b + c * 0.5 - a * c";
    // top(sub) is the original at a few points.
    auto at = [](const Expr& e, const std::vector<float>& in) {
      PointSet ps;
      for (float x : in) ps.cols.push_back({x});
      return evalAll(e, ps, kProfileRef)[0];
    };
    for (float x : {0.1f, 0.4f, 0.9f}) {
      const std::vector<float> in = {x, 1.0f - x, 0.5f * x};
      std::vector<float> tin;
      for (uint32_t k = 0; k + 1 < map.size(); ++k) tin.push_back(in[map[k]]);
      tin.push_back(at(subexpr(p.target, c.node), in));
      CHECK(at(top.target, tin) == at(p.target, in));
    }
  }
  CHECK(whole);
  Options opt;
  opt.v1Points = 1u << 16;
  opt.search.timeLimitSec = 0.5;
  opt.search.maxBank = 20000;
  opt.search.sharedLeaves = false;
  opt.subtrees = false;
  opt.cuts = false;
  opt.search.topDown = false;
  const RunResult off = optimize(p, opt);
  opt.cuts = true;
  opt.cutTime = 2.0;  // the top search needs a second CEGIS round (counterexamples)
  const RunResult on = optimize(p, opt);
  CHECK(on.cutSearches > 0);
  CHECK(!on.accepted.empty() && on.accepted[0].cost < on.targetCost);
  CHECK(off.accepted.empty() || on.accepted[0].cost < off.accepted[0].cost);
}

TEST(simplify_identities_and_printing) {
  Program p = parseProgram("input v : float3 in [0, 1]\ninput a : float in [0, 1]\ninput b : float in [0, 1]\n"
                           "output r = a\nbudget r : exact\n");
  auto simp = [&](const char* s) { return toString(simplifyIdentities(parseExpr(s, p.inputs)), p.inputs); };
  CHECK(simp("mad(a, b, 0.0)") == "a * b");
  CHECK(simp("mad(-1.0, a, b)") == "b - a");
  CHECK(simp("mad(a, 1.0, b)") == "a + b");
  CHECK(simp("a * 1.0 + 0.0") == "a");
  CHECK(simp("a - 0.0") == "a");
  // Splat constants next to a vector print as scalars; a + -c as a - c.
  CHECK(simp("mad(v, float3(0.5, 0.5, 0.5), float3(-0.25, -0.25, -0.25))") == "mad(v, 0.5, -0.25)");
  CHECK(simp("v + float3(-0.25, -0.25, -0.25)") == "v - 0.25");
  CHECK(simp("float3(1.0, 1.0, 1.0) * a") == "float3(1.0, 1.0, 1.0) * a");  // no vector operand: keeps the type
}

TEST(disk_bank) {
  // Disk-backed bank with a tiny budget and tiny tiles: most fingerprints go to disk and
  // the enumeration runs tile by tile; the search must still find the same best rewrite.
  const Program p = loadProgram(std::string(SOPT_EXAMPLES_DIR) + "/rational.sopt");
  Options opt;
  opt.v1Points = 1u << 16;
  opt.search.timeLimitSec = 1.0;
  opt.subtrees = opt.cuts = opt.v3 = false;
  opt.search.memBudget = size_t{24} << 20;
  const RunResult ram = optimize(p, opt);
  const std::string dir = std::string(SOPT_EXAMPLES_DIR) + "/../build-disk-test";
  opt.search.diskDir = dir;
  opt.search.diskTileFloats = 1u << 16;
  opt.search.diskBlockFloats = 1u << 12;
  opt.search.memBudget = size_t{8} << 20;
  const RunResult disk = optimize(p, opt);
  CHECK(disk.search.diskEntries > 0 && disk.search.diskTilesRead > 0);
  CHECK(disk.search.diskBytes < disk.search.diskRawBytes);
  CHECK(!ram.accepted.empty() && !disk.accepted.empty() && disk.accepted[0].cost == ram.accepted[0].cost);
  // The temporary bank file is removed.
  size_t files = 0;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); ++it)
    ++files;
  CHECK(files == 0);
  std::filesystem::remove_all(dir, ec);
}
