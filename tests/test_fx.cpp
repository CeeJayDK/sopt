#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "fx/frontend.hpp"
#include "fx/variants.hpp"
#include "search/driver.hpp"
#include "test.hpp"

using namespace sopt;
namespace fs = std::filesystem;

namespace {

const fs::path kEffect = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_test.fx";

struct Loaded {
  std::vector<fx::Region> regions;
  fx::SkipCount skipped;
};

Loaded load() {
  Loaded l;
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(kEffect, lo, err);
  CHECK(e != nullptr);
  if (!e) {
    std::printf("  %s\n", err.c_str());
    return l;
  }
  lo.width = 2560;
  lo.height = 1440;
  auto alt = fx::loadEffect(kEffect, lo, err);
  l.skipped.keepDetails = true;
  l.regions = fx::extractRegions(*e, alt.get(), fx::RegionOptions(), l.skipped);
  return l;
}

const fx::Region* at(const Loaded& l, uint32_t line) {
  for (const auto& r : l.regions)
    if (r.line == line && fs::path(r.file).filename() == "sopt_test.fx") return &r;
  return nullptr;
}

bool skippedAt(const Loaded& l, uint32_t line, const std::string& reason) {
  const std::string key = "sopt_test.fx:" + std::to_string(line) + ": " + reason;
  for (const auto& d : l.skipped.details)
    if (d.size() >= key.size() && d.compare(d.size() - key.size(), key.size(), key) == 0) return true;
  return false;
}

}  // namespace

TEST(fx_regions_and_facts) {
  const Loaded l = load();
  // float luma = color.r * 0.25 + ...: color comes from the back buffer.
  const fx::Region* luma = at(l, 23);
  CHECK(luma != nullptr);
  if (luma) {
    CHECK(luma->kind == fx::Region::Kind::Init);
    CHECK(luma->lhs == "float luma =");
    CHECK(luma->prog.inputs.size() == 1);
    CHECK(luma->prog.inputs[0].name == "color");
    CHECK(luma->prog.inputs[0].type == Type::Float3);
    CHECK(luma->prog.inputs[0].lo == 0.0 && luma->prog.inputs[0].hi == 1.0);
    CHECK(luma->prog.inputs[0].grid == 255);
    CHECK(!luma->facts[0].assumed);
  }
  // float gate = luma * Strength + Plain: ui range, derived range, assumed default.
  const fx::Region* gate = at(l, 26);
  CHECK(gate != nullptr);
  if (gate) {
    CHECK(gate->prog.budget.kind == Budget::Kind::Exact);  // compared with 0.5
    bool strength = false, plain = false, lumaIn = false;
    for (size_t k = 0; k < gate->prog.inputs.size(); ++k) {
      const auto& d = gate->prog.inputs[k];
      if (d.name == "Strength") strength = d.lo == 0.0 && d.hi == 2.0 && gate->facts[k].source == "ui_min/ui_max";
      if (d.name == "Plain") plain = gate->facts[k].assumed;
      if (d.name == "luma") lumaIn = d.lo >= 0.0 && d.hi <= 1.0 && !gate->facts[k].assumed;
    }
    CHECK(strength);
    CHECK(plain);
    CHECK(lumaIn);
  }
  // uv is only used as a texture coordinate.
  const fx::Region* uv = at(l, 24);
  CHECK(uv != nullptr);
  if (uv) {
    CHECK(uv->prog.budget.kind == Budget::Kind::Texcoord);
    CHECK(uv->prog.inputs.size() == 1 && uv->prog.inputs[0].name == "texcoord");
    CHECK(uv->facts[0].source.rfind("TEXCOORD", 0) == 0);
  }
  // The pixel shader's result goes to the 8-bit back buffer without blending.
  const fx::Region* ret = at(l, 33);
  CHECK(ret != nullptr);
  if (ret) {
    CHECK(ret->kind == fx::Region::Kind::Return);
    CHECK(ret->prog.budget.kind == Budget::Kind::Color8);
    CHECK(ret->prog.budget.maxCodeDiff == 0);
  }
  // A texture fetch is an input named by its call text (macros inside it are fine).
  const fx::Region* fetch = at(l, 32);
  CHECK(fetch != nullptr);
  if (fetch) {
    bool found = false;
    for (size_t k = 0; k < fetch->prog.inputs.size(); ++k)
      if (fetch->facts[k].fetch)
        found = fetch->prog.inputs[k].name == "tex2D(BackBuffer, texcoord + BUFFER_RCP_WIDTH).xyz" &&
                fetch->prog.inputs[k].grid == 255;
    CHECK(found);
  }
  // Statements that cannot be regions, and why.
  CHECK(skippedAt(l, 29, "uses a macro"));
  CHECK(skippedAt(l, 30, "depends on BUFFER_WIDTH/HEIGHT"));
  CHECK(skippedAt(l, 28, "function call"));
  // Header function reached from the pixel shader, parameter range from its call site.
  bool helper = false;
  for (const auto& r : l.regions)
    if (fs::path(r.file).filename() == "sopt_test.fxh" && r.line == 4)
      helper = r.prog.inputs.size() == 1 && r.prog.inputs[0].hi == 1.0 && !r.facts[0].assumed;
  CHECK(helper);
}

TEST(fx_search_and_variants) {
  const Loaded l = load();
  const fx::Region* helper = nullptr;
  for (const auto& r : l.regions)
    if (fs::path(r.file).filename() == "sopt_test.fxh" && r.line == 4) helper = &r;
  CHECK(helper != nullptr);
  if (!helper) return;
  // c * 0.5 + c * 0.5 == c exactly.
  Options opt;
  opt.search.timeLimitSec = 5;
  opt.v1Points = 1u << 14;
  const RunResult res = optimize(helper->prog, opt);
  fx::RegionResult rr;
  rr.region = *helper;
  rr.targetCost = res.targetCost;
  for (const auto& a : res.accepted) {
    if (fx::compiledCost(a.expr, *opt.search.model) >= fx::compiledCost(helper->prog.target, *opt.search.model))
      continue;
    fx::Variant v;
    v.expr = a.expr;
    v.text = a.text;
    v.cost = a.cost;
    v.klass = a.klass;
    rr.variants.push_back(v);
  }
  CHECK(!rr.variants.empty());
  if (rr.variants.empty()) return;
  CHECK(rr.variants[0].text == "c");
  CHECK(rr.variants[0].klass == Klass::BitExact);

  const fs::path out = fs::temp_directory_path() / "sopt_test_fx_out";
  std::error_code ec;
  fs::remove_all(out, ec);
  std::string errors;
  const auto files = fx::writeVariants({rr}, out, errors);
  CHECK(errors.empty());
  CHECK(files.size() == 1);
  std::ifstream f(out / "sopt_test.fxh");
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  CHECK(text.find("#ifndef SOPT_sopt_test_4\n#define SOPT_sopt_test_4 SOPT_ALL") != std::string::npos);
  CHECK(text.find("#if SOPT_sopt_test_4 >= 1\n\tfloat3 r = c; // sopt: bit-exact") != std::string::npos);
  CHECK(text.find("#else\n\tfloat3 r = c * 0.5 + c * 0.5;\n#endif") != std::string::npos);
  // The effect, copied next to the changed header, parses with either switch value.
  fs::copy_file(kEffect, out / "sopt_test.fx", fs::copy_options::overwrite_existing, ec);
  for (const char* all : {"0", "1", "3"}) {
    fx::LoadOptions lo;
    lo.macros.emplace_back("SOPT_ALL", all);
    std::string err;
    auto e = fx::loadEffect(out / "sopt_test.fx", lo, err);
    CHECK(e != nullptr);
    bool usesCopy = false;
    if (e)
      for (const auto& s : e->sourceFiles) usesCopy = usesCopy || fs::path(s).parent_path() == out;
    CHECK(usesCopy);
  }
  fs::remove_all(out, ec);
}

TEST(fx_compiled_cost) {
  // Explicit mad is no gain over a * b + c (the compiler contracts it), nor are
  // modifiers and constructors.
  auto cost = [](const char* src) {
    ExprBuilder b;
    const uint32_t x = b.input(0), y = b.input(1), z = b.input(2);
    uint32_t r = 0;
    if (!std::strcmp(src, "x*y+z")) r = b.op(Op::Add, b.op(Op::Mul, x, y), z);
    if (!std::strcmp(src, "mad")) r = b.op(Op::Mad, x, y, z);
    if (!std::strcmp(src, "-x*y+z")) r = b.op(Op::Add, b.op(Op::Mul, b.op(Op::Neg, x), y), z);
    if (!std::strcmp(src, "clamp01")) r = b.op(Op::Clamp, b.op(Op::Mul, x, y), b.constant(0.0f), b.constant(1.0f));
    if (!std::strcmp(src, "mul")) r = b.op(Op::Mul, x, y);
    return fx::compiledCost(b.finish(r), defaultCostModel());
  };
  CHECK(cost("x*y+z") == cost("mad"));
  CHECK(cost("-x*y+z") == cost("mad"));
  CHECK(cost("clamp01") == cost("mul"));
}

TEST(fx_user_ranges) {
  double lo = 0, hi = 0;
  CHECK(fx::parseRange("[0, 0.5]", lo, hi) && lo == 0.0 && hi == 0.5);
  CHECK(fx::parseRange("2 -1", lo, hi) && lo == -1.0 && hi == 2.0);
  CHECK(!fx::parseRange("[0, x]", lo, hi));

  const fs::path file = fs::temp_directory_path() / "sopt_test_facts.txt";
  {
    std::ofstream f(file);
    f << "# comment\nsopt_test.fx global Plain = [0.25, 0.75]  # user\n\n";
  }
  fx::UserRanges user;
  std::string err;
  CHECK(fx::readUserRanges(file, user, err));
  CHECK(user.count("sopt_test.fx global Plain") == 1);
  fs::remove(file);

  // gate = luma * Strength + Plain: Plain had no fact, now the user's range.
  fx::LoadOptions lo2;
  auto e = fx::loadEffect(kEffect, lo2, err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::RegionOptions ro;
  ro.userRanges = &user;
  fx::SkipCount sk;
  bool found = false;
  for (const auto& r : fx::extractRegions(*e, nullptr, ro, sk))
    if (r.line == 26 && fs::path(r.file).filename() == "sopt_test.fx")
      for (size_t k = 0; k < r.prog.inputs.size(); ++k)
        if (r.prog.inputs[k].name == "Plain")
          found = r.prog.inputs[k].lo == 0.25 && r.prog.inputs[k].hi == 0.75 && !r.facts[k].assumed &&
                  r.facts[k].key == "sopt_test.fx global Plain";
  CHECK(found);
}

TEST(fx_semantic_ranges) {
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_semantics.fx", lo, err);
  CHECK(e != nullptr);
  if (!e) {
    std::printf("  %s\n", err.c_str());
    return;
  }
  fx::SkipCount sk;
  auto input = [&](uint32_t line, const std::string& name) -> std::pair<InputDecl, fx::Fact> {
    for (const auto& r : fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk))
      if (r.line == line)
        for (size_t k = 0; k < r.prog.inputs.size(); ++k)
          if (r.prog.inputs[k].name == name) return {r.prog.inputs[k], r.facts[k]};
    return {};
  };
  // Struct member with TEXCOORD0: the convention, [0, 1].
  auto [a, fa] = input(23, "i.uv");
  CHECK(a.lo == 0.0 && a.hi == 1.0 && !fa.assumed && fa.source == "TEXCOORD semantic (convention)");
  // COLOR0: no fact, but [0, 1] is suggested.
  auto [t, ft] = input(24, "i.tint.xyz");
  CHECK(ft.assumed && ft.suggestLo == 0.0 && ft.suggestHi == 1.0);
  CHECK(ft.suggestWhy.find("COLOR") != std::string::npos);
  // The vertex shader adds a uniform without a range: its output is unknown, so the
  // TEXCOORD convention applies; SV_Position is in pixels.
  auto [b, fb] = input(36, "uv");
  CHECK(b.lo == 0.0 && b.hi == 1.0 && fb.source == "TEXCOORD semantic (convention)");
  auto [p, fp] = input(37, "vpos.x");
  CHECK(p.lo == 0.0 && p.hi == 7680.0 && !fp.assumed);
}

TEST(fx_macro_inputs) {
  const fs::path path = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_macros.fx";
  fx::LoadOptions lo;
  std::string err;
  auto plain = fx::loadEffect(path, lo, err);
  CHECK(plain != nullptr);
  if (!plain) return;
  // Both definitions are user-changeable (#ifndef) and numeric; FIXED is not.
  const auto sym = fx::symbolicMacros(path, lo, *plain);
  CHECK(sym.count("TEST_FAR") == 1 && sym.count("TEST_MODE") == 1 && sym.count("FIXED") == 0);
  lo.symbolic = sym;
  auto e = fx::loadEffect(path, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::SkipCount sk;
  sk.keepDetails = true;
  bool found = false;
  for (const auto& r : fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk)) {
    if (r.line != 23) continue;
    for (size_t k = 0; k < r.prog.inputs.size(); ++k)
      if (r.prog.inputs[k].name == "TEST_FAR")
        found = r.prog.inputs[k].compileTime && r.facts[k].key == "macro global TEST_FAR" &&
                r.facts[k].suggestHi == 2000.0;
    // TEST_FAR - 1.0 is folded by the compiler: only the fma (mul + sub) and div cost.
    const CostModel& m = defaultCostModel();
    CHECK(dagCost(r.prog.target, m, r.prog.inputs) == uint32_t(m[Op::Mul] + m.fusedAdd + m[Op::Div]));
  }
  CHECK(found);
  // #if still uses the value (the region exists), FIXED is still a plain macro.
  bool fixedSkipped = false;
  for (const auto& d : sk.details) fixedSkipped = fixedSkipped || d.find("sopt_macros.fx:25: uses a macro") != std::string::npos;
  CHECK(fixedSkipped);
}

TEST(fx_chain_windows) {
  const fs::path path = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_chain.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(path, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::SkipCount sk;
  auto regions = fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk);
  auto window = [&](uint32_t line, std::vector<uint32_t> removed) -> const fx::Region* {
    for (const auto& r : regions) {
      if (r.line != line || r.removed.size() != removed.size()) continue;
      bool same = true;
      for (size_t k = 0; k < removed.size(); ++k) same = same && r.removed[k].first == removed[k];
      if (same) return &r;
    }
    return nullptr;
  };
  // d = d * 2.0 + 1.0 with d = 1.0 - d (inside #if TEST_REV) inlined.
  const fx::Region* rev = window(27, {25});
  CHECK(rev != nullptr);
  if (rev) {
    CHECK(rev->guard == "(TEST_REV)");
    CHECK(toString(rev->prog.target, rev->prog.inputs) == "1.0 + (1.0 - d) * 2.0");
  }
  // ... and the definition from the fetch: the untaken #if TEST_LOG is in between.
  const fx::Region* full = window(27, {20, 25});
  CHECK(full != nullptr);
  if (full) CHECK(full->guard == "!(TEST_LOG) && (TEST_REV)");
  // y reads the intermediate d: no chain into d = d * d.
  CHECK(window(29, {27}) == nullptr);
  // Plain chain without directives, up to 3 statements before the root.
  const fx::Region* e3 = window(33, {30, 31, 32});
  CHECK(e3 != nullptr);
  if (e3) {
    CHECK(e3->guard.empty());
    CHECK(toString(e3->prog.target, e3->prog.inputs) == "(uv.y * 0.5 + 0.25) * (uv.y * 0.5 + 0.25)");
  }
  if (!rev) return;

  // Variants apply only under the guard; the effect parses with either TEST_REV.
  fx::RegionResult rr;
  rr.region = *rev;
  rr.targetCost = 10;
  fx::Variant v;
  v.text = "3.0 - 2.0 * d";
  v.cost = 5;
  rr.variants.push_back(v);
  const fs::path out = fs::temp_directory_path() / "sopt_test_fx_chain";
  std::error_code ec;
  fs::remove_all(out, ec);
  std::string errors;
  const auto files = fx::writeVariants({rr}, out, errors);
  CHECK(errors.empty() && files.size() == 1);
  std::ifstream f(out / "sopt_chain.fx");
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  CHECK(text.find("#if SOPT_sopt_chain_25_27 < 1 || !((TEST_REV))\n\td = 1.0 - d;\n#endif") != std::string::npos);
  CHECK(text.find("#if SOPT_sopt_chain_25_27 >= 1 && (TEST_REV)\n\td = 3.0 - 2.0 * d;") != std::string::npos);
  for (const char* rev : {"0", "1"}) {
    fx::LoadOptions o;
    o.macros = {{"SOPT_ALL", "1"}, {"TEST_REV", rev}};
    std::string err2;
    CHECK(fx::loadEffect(out / "sopt_chain.fx", o, err2) != nullptr);
  }
  fs::remove_all(out, ec);
}
