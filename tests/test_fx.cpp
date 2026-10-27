#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ir/parser.hpp"
#include "fx/blend.hpp"
#include "fx/frontend.hpp"
#include "fx/hoist.hpp"
#include "fx/variants.hpp"
#include "fx/platforms.hpp"
#include "search/driver.hpp"
#include "test.hpp"

using namespace sopt;
namespace fs = std::filesystem;

namespace {

const fs::path kEffect = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_test.fx";

// Facts and suggestions as they are without likely ranges (fx_likely_ranges tests those).
fx::RegionOptions noLikely() {
  fx::RegionOptions r;
  r.likelyRanges = false;
  return r;
}

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
  lo.width = fx::kAltWidth;
  lo.height = fx::kAltHeight;
  auto alt = fx::loadEffect(kEffect, lo, err);
  l.skipped.keepDetails = true;
  l.regions = fx::extractRegions(*e, alt.get(), noLikely(), l.skipped);
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
  CHECK(at(l, 29) != nullptr);  // SCALE: one value for every user, spelled out (2026-10-26; was "uses a macro")
  CHECK(skippedAt(l, 30, "depends on BUFFER_WIDTH/HEIGHT"));
  // A call of a small helper is seen through (2026-10-26; was "function call").
  bool call = false;
  for (const auto& r : l.regions)
    call = call || (r.line == 28 && toString(r.prog.target, r.prog.inputs) == "color * 0.5 + color * 0.5");
  CHECK(call);
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
  CHECK(text.find("#ifndef SOPT_sopt_test_4\n#define SOPT_sopt_test_4 SOPT_GROUP") != std::string::npos);
  CHECK(text.find("#if SOPT_sopt_test_4 >= 1\n\tfloat3 r = c; // sopt: bit-exact") != std::string::npos);
  CHECK(text.find("#else\n\tfloat3 r = c * 0.5 + c * 0.5;\n#endif") != std::string::npos);
  // The effect, copied next to the changed header, parses with either switch value.
  fs::copy_file(kEffect, out / "sopt_test.fx", fs::copy_options::overwrite_existing, ec);
  for (const char* all : {"0", "1", "3"}) {
    fx::LoadOptions lo;
    lo.macros.emplace_back("SOPT_GROUP", all);
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
    for (const auto& r : fx::extractRegions(*e, nullptr, noLikely(), sk))
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
  // #if still uses the value (the region exists); FIXED has one value for every user: spelled out (2026-10-26).
  bool fixed = false;
  for (const auto& r : fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk))
    fixed = fixed || (r.line == 25 && toString(r.prog.target, r.prog.inputs) == "d * 3.0 + 1.0");
  CHECK(fixed);
  // A macro defined inside #if depends on a setting: spelled out under the definition's condition; the include guard
  // does not count. Function-like macros are expanded, one that is a texture fetch is an input (2026-10-26).
  auto f = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_fixedmacro.fx", fx::LoadOptions(), err);
  CHECK(f != nullptr);
  if (!f) return;
  fx::SkipCount sf;
  sf.keepDetails = true;
  bool lit = false, cond = false, fn = false, condFn = false, fetch = false, win = false;
  for (const auto& r : fx::extractRegions(*f, nullptr, fx::RegionOptions(), sf)) {
    win = win || (r.line == 22 && !r.removed.empty() && r.guard == "(SETTING)");  // b inlined: COND's condition
    if (!r.removed.empty()) continue;
    const std::string t = toString(r.prog.target, r.prog.inputs);
    lit = lit || (r.line == 20 && t.find("0.75") != std::string::npos && r.guard.empty());
    cond = cond || (r.line == 21 && t.find("0.5") != std::string::npos && r.guard == "(SETTING)");
    fn = fn || (r.line == 34 && t.find("max(") != std::string::npos && r.guard.empty());
    condFn = condFn || (r.line == 35 && t.find("0.5") != std::string::npos && r.guard == "(SETTING)");
    for (const auto& d : r.prog.inputs) fetch = fetch || (r.line == 36 && d.name == "getColor(p.yx).y");
  }
  CHECK(lit && cond && fn && condFn && fetch);
  CHECK(win);
  for (const auto& d : sf.details)
    if (d.find("uses a macro") != std::string::npos) std::printf("  %s\n", d.c_str());
}

TEST(fx_buffer_inputs) {
  const fs::path path = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_buffer.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffectBufferSymbolic(path, lo, err);
  CHECK(e != nullptr && e->bufferSymbolic);
  if (!e) {
    std::printf("  %s\n", err.c_str());
    return;
  }
  // The texture size line needs constants: it keeps the numbers. The static consts become
  // named expressions (each use parses the initializer again).
  CHECK(lo.symbolicExclude.size() == 1 && lo.namedExpressions);
  lo.width = fx::kAltWidth;
  lo.height = fx::kAltHeight;
  auto alt = fx::loadEffect(path, lo, err);
  CHECK(alt != nullptr);
  fx::SkipCount sk;
  sk.keepDetails = true;
  const auto regions = fx::extractRegions(*e, alt.get(), fx::RegionOptions(), sk);
  auto region = [&](uint32_t line) -> const fx::Region* {
    for (const auto& r : regions)
      if (r.line == line && r.removed.empty()) return &r;
    return nullptr;
  };
  auto skipped = [&](uint32_t line, const std::string& why) {
    const std::string key = "sopt_buffer.fx:" + std::to_string(line) + ": " + why;
    for (const auto& d : sk.details)
      if (d.find(key) != std::string::npos) return true;
    return false;
  };
  // SCREEN_SIZE and PIXEL_SIZE expand to the symbolic sizes, kPixel and kAspect are named
  // expressions of them: compile-time inputs with a fact.
  for (uint32_t line : {17u, 18u, 20u, 21u}) {
    const fx::Region* r = region(line);
    CHECK(r != nullptr);
    if (!r) continue;
    int sizes = 0;
    for (size_t k = 0; k < r->prog.inputs.size(); ++k) {
      const auto& d = r->prog.inputs[k];
      if (d.name != "BUFFER_WIDTH" && d.name != "BUFFER_HEIGHT") continue;
      ++sizes;
      CHECK(d.compileTime && d.lo == 1.0 && d.hi == 7680.0 && !r->facts[k].assumed);
      CHECK(d.value == (d.name == "BUFFER_WIDTH" ? 1920.0 : 1080.0));
    }
    CHECK(sizes == 2);
  }
  // BUFFER_WIDTH / 3 is an integer division: not float arithmetic, skipped.
  CHECK(region(19) == nullptr && skipped(19, "non-float arithmetic"));
  // Baked sizes: kAspect = H / W is 0.5625 at 1920x1080 and at 2560x1440; the 32:9 second
  // parse sees that the region depends on the aspect ratio.
  fx::LoadOptions plain;
  auto p1 = fx::loadEffect(path, plain, err);
  plain.width = fx::kAltWidth;
  plain.height = fx::kAltHeight;
  auto p2 = fx::loadEffect(path, plain, err);
  CHECK(p1 != nullptr && p2 != nullptr);
  if (!p1 || !p2) return;
  fx::SkipCount sk2;
  sk2.keepDetails = true;
  bool aspectRegion = false;
  for (const auto& r : fx::extractRegions(*p1, p2.get(), fx::RegionOptions(), sk2))
    aspectRegion = aspectRegion || (r.line == 21 && r.removed.empty());
  bool aspectSkipped = false;
  for (const auto& d : sk2.details)
    aspectSkipped = aspectSkipped || d.find("sopt_buffer.fx:21: depends on BUFFER_WIDTH/HEIGHT") != std::string::npos;
  CHECK(!aspectRegion && aspectSkipped);
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
  if (full) {
    CHECK(full->guard == "!(TEST_LOG) && (TEST_REV)");
    CHECK(full->lhs == "float d =");  // from the declaration: the variant declares d
  }
  if (rev) CHECK(rev->lhs == "d =");
  // y reads the intermediate d: no chain into d = d * d.
  CHECK(window(29, {27}) == nullptr);
  // Plain chain without directives, up to 3 statements before the root.
  const fx::Region* e3 = window(33, {30, 31, 32});
  CHECK(e3 != nullptr);
  if (e3) {
    CHECK(e3->guard.empty());
    CHECK(e3->lhs == "float e =");
    CHECK(toString(e3->prog.target, e3->prog.inputs) == "(uv.y * 0.5 + 0.25) * (uv.y * 0.5 + 0.25)");
  }
  if (!rev || !e3) return;

  // Variants apply only under the guard; the effect parses with either TEST_REV.
  fx::RegionResult rr;
  rr.region = *rev;
  rr.targetCost = 10;
  fx::Variant v;
  v.text = "3.0 - 2.0 * d";
  v.cost = 5;
  rr.variants.push_back(v);
  fx::RegionResult re;  // chain from a declaration
  re.region = *e3;
  re.targetCost = 10;
  v.text = "uv.y * uv.y";
  re.variants.push_back(v);
  const fs::path out = fs::temp_directory_path() / "sopt_test_fx_chain";
  std::error_code ec;
  fs::remove_all(out, ec);
  std::string errors;
  const auto files = fx::writeVariants({rr, re}, out, errors);
  CHECK(errors.empty() && files.size() == 1);
  std::ifstream f(out / "sopt_chain.fx");
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  CHECK(text.find("#if SOPT_sopt_chain_25_27 < 1 || !((TEST_REV))\n\td = 1.0 - d;\n#endif") != std::string::npos);
  CHECK(text.find("#if SOPT_sopt_chain_25_27 >= 1 && (TEST_REV)\n\td = 3.0 - 2.0 * d;") != std::string::npos);
  CHECK(text.find("\tfloat e = uv.y * uv.y;") != std::string::npos);
  for (const char* rev : {"0", "1"}) {
    fx::LoadOptions o;
    o.macros = {{"SOPT_GROUP", "1"}, {"TEST_REV", rev}};
    std::string err2;
    CHECK(fx::loadEffect(out / "sopt_chain.fx", o, err2) != nullptr);
  }
  fs::remove_all(out, ec);
}

TEST(fx_vendor_auto) {
  const fs::path path = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_chain.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(path, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::SkipCount sk;
  const fx::Region* reg = nullptr;
  auto regions = fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk);
  for (const auto& r : regions)
    if (r.line == 27 && r.removed.empty()) reg = &r;
  CHECK(reg != nullptr);
  if (!reg) return;
  // Variant 1 is fastest on NVIDIA, variant 2 on AMD, variant 3 is less accurate.
  fx::RegionResult rr;
  rr.region = *reg;
  rr.targetCost = 10;
  rr.targetAmd = 3;
  rr.targetNv = 4;
  const char* texts[] = {"mad(d, 2.0, 1.0)", "d + d + 1.0", "d * 2.0"};
  const int amd[] = {3, 2, 1}, nv[] = {2, 4, 1};
  for (int k = 0; k < 3; ++k) {
    fx::Variant v;
    v.text = texts[k];
    v.cost = 5;
    v.amd = amd[k];
    v.nv = nv[k];
    v.klass = k == 2 ? Klass::LessAccurate : Klass::Within;
    rr.variants.push_back(v);
  }
  CHECK(fx::vendorPick(rr, fx::Vendor::Amd) == 2 && fx::vendorPick(rr, fx::Vendor::Nv) == 1);
  {
    // A variant with problem inputs is never picked.
    fx::RegionResult marked = rr;
    marked.variants[1].problems = "fails at d = 0.5 (NaN/inf at some)";
    CHECK(fx::vendorPick(marked, fx::Vendor::Amd) == 0 && fx::vendorPick(marked, fx::Vendor::Nv) == 1);
  }
  const fs::path out = fs::temp_directory_path() / "sopt_test_fx_vendor";
  std::error_code ec;
  fs::remove_all(out, ec);
  std::string errors;
  CHECK(fx::writeVariants({rr}, out, errors).size() == 1);
  std::ifstream f(out / "sopt_chain.fx");
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  CHECK(text.find("#if SOPT_AUTO && __VENDOR__ == 0x1002\n#define SOPT_sopt_chain_27 2\n"
                  "#elif SOPT_AUTO && __VENDOR__ == 0x10DE\n#define SOPT_sopt_chain_27 1\n#else\n"
                  "#define SOPT_sopt_chain_27 SOPT_GROUP") != std::string::npos);
  for (const char* vendor : {"0x1002", "0x10DE", "0x8086"})
    for (const char* autoOn : {"0", "1"}) {
      fx::LoadOptions o;
      o.macros = {{"__VENDOR__", vendor}, {"SOPT_AUTO", autoOn}};
      std::string err2;
      CHECK(fx::loadEffect(out / "sopt_chain.fx", o, err2) != nullptr);
    }
  fs::remove_all(out, ec);

  // Per API: fxc already compiles variant 2 to the original's code, so on DX9-DX12 AMD
  // gets the original, on Vulkan/OpenGL variant 2.
  rr.targetDxbc = 5;
  rr.variants[1].dxbc = 5;
  rr.variants[1].dxbcSame = true;
  CHECK(fx::vendorPick(rr, fx::Vendor::Amd, true) == 0 && fx::vendorPick(rr, fx::Vendor::Amd, false) == 2);
  CHECK(fx::writeVariants({rr}, out, errors).size() == 1);
  std::ifstream f2(out / "sopt_chain.fx");
  std::stringstream ss2;
  ss2 << f2.rdbuf();
  CHECK(ss2.str().find("#if SOPT_AUTO && __VENDOR__ == 0x1002 && __RENDERER__ < 0x10000\n#define SOPT_sopt_chain_27 0\n"
                       "#elif SOPT_AUTO && __VENDOR__ == 0x1002\n#define SOPT_sopt_chain_27 2\n") != std::string::npos);
  for (const char* renderer : {"0xb000", "0x20000"}) {
    fx::LoadOptions o;
    o.macros = {{"__VENDOR__", "0x1002"}, {"__RENDERER__", renderer}, {"SOPT_AUTO", "1"}};
    std::string err2;
    CHECK(fx::loadEffect(out / "sopt_chain.fx", o, err2) != nullptr);
  }
  fs::remove_all(out, ec);
}

TEST(fx_constant_array_range) {
  const fs::path path = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_array.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(path, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::SkipCount sk;
  bool found = false;
  for (const auto& r : fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk)) {
    if (r.line != 12) continue;
    for (const auto& d : r.prog.inputs)
      if (d.name == "uv.x") {
        found = true;
        CHECK(d.lo <= 0.0 && d.hi >= 1.0);  // all four corners, not only corners[0]
      }
  }
  CHECK(found);
}

TEST(fx_modern_fetch_syntax) {
  const fs::path path = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_fetch.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(path, lo, err);
  CHECK(e != nullptr);
  if (!e) {
    std::printf("  %s\n", err.c_str());
    return;
  }
  fx::SkipCount sk;
  std::vector<std::string> names;
  for (const auto& r : fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk))
    for (size_t k = 0; k < r.prog.inputs.size(); ++k)
      if (r.facts[k].fetch) names.push_back(r.prog.inputs[k].name);
  auto has = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) != names.end(); };
  CHECK(has("tex2D(BackBuffer, uv, int2(1, 0)).x"));
  CHECK(has("tex2Dlod(BackBuffer, float4(uv, 0, 0), int2(0, 1)).x"));
  CHECK(has("tex2DgatherG(BackBuffer, uv).xy"));
  for (const auto& n : names) CHECK(n.find("offset") == std::string::npos && n.find("gather(") == std::string::npos);
}

TEST(fx_windows_include_names) {
  const fs::path path = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_backslash.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(path, lo, err);
  CHECK(e != nullptr);
  if (!e) std::printf("  %s\n", err.c_str());
}

// Back buffer formats (owner, 2026-09-30): an scRGB extraction gives back buffer inputs
// [-0.5, 125] (derived values follow), and a variant with a format guard applies only
// under it, the statements it inlines coming back where the original is used.
TEST(fx_back_buffer_formats) {
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(kEffect, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::SkipCount sk;
  fx::RegionOptions hdr;
  hdr.hdrBackBuffer = true;
  const auto regions = fx::extractRegions(*e, nullptr, hdr, sk);
  bool fetchHdr = false, derivedHdr = false, ret = false;
  for (const auto& r : regions) {
    if (fs::path(r.file).filename() != "sopt_test.fx") continue;
    for (size_t k = 0; k < r.prog.inputs.size(); ++k) {
      const auto& d = r.prog.inputs[k];
      if (r.line == 32 && r.facts[k].fetch) fetchHdr = d.lo == fx::kScRgbLo && d.hi == fx::kScRgbHi && d.grid == 0;
      if (r.line == 23 && d.name == "color") derivedHdr = d.lo == fx::kScRgbLo && d.hi == fx::kScRgbHi;
    }
    if (r.kind == fx::Region::Kind::Return) ret = r.budgetReason.find("back buffer") != std::string::npos;
  }
  CHECK(fetchHdr);
  CHECK(derivedHdr);
  CHECK(ret);  // the pixel shader writes the back buffer

  const fs::path path = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_chain.fx";
  auto c = fx::loadEffect(path, lo, err);
  CHECK(c != nullptr);
  if (!c) return;
  const auto chain = fx::extractRegions(*c, nullptr, fx::RegionOptions(), sk);
  const fx::Region* e3 = nullptr;
  for (const auto& r : chain)
    if (r.line == 33 && r.removed.size() == 3) e3 = &r;
  CHECK(e3 != nullptr);
  if (!e3) return;
  fx::RegionResult rr;
  rr.region = *e3;
  rr.targetCost = 10;
  fx::Variant v;
  v.text = "uv.y * uv.y";
  v.cost = 5;
  v.formatGuard = "BUFFER_COLOR_SPACE <= 1";
  rr.variants.push_back(v);
  const fs::path out = fs::temp_directory_path() / "sopt_test_fx_formats";
  std::error_code ec;
  fs::remove_all(out, ec);
  std::string errors;
  CHECK(fx::writeVariants({rr}, out, errors).size() == 1);
  std::ifstream f(out / "sopt_chain.fx");
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  CHECK(text.find("#if SOPT_sopt_chain_30_33 >= 1 && (BUFFER_COLOR_SPACE <= 1)\n") != std::string::npos);
  CHECK(text.find("#if !((SOPT_sopt_chain_30_33 >= 1 && (BUFFER_COLOR_SPACE <= 1)))\n") != std::string::npos);
  for (const char* space : {"1", "2"}) {
    fx::LoadOptions o;
    o.macros = {{"SOPT_GROUP", "1"}, {"BUFFER_COLOR_SPACE", space}};
    std::string err2;
    CHECK(fx::loadEffect(out / "sopt_chain.fx", o, err2) != nullptr);
  }
  fs::remove_all(out, ec);
}

TEST(fx_hlsl) {
  // Plain HLSL: cbuffer members are uniforms, texture methods are fetches (leaves in
  // their own syntax), facts for a texture apply to every read of it.
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_hlsl.hlsl";
  fx::LoadOptions lo;
  lo.hlsl = true;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) {
    std::printf("  %s\n", err.c_str());
    return;
  }
  CHECK(e->hlsl);
  CHECK((fx::unrangedTextures(*e) == std::vector<std::string>{"gColor", "gDepth"}));
  auto regionAt = [](const std::vector<fx::Region>& rs, uint32_t line) -> const fx::Region* {
    for (const auto& r : rs)
      if (r.line == line && r.removed.empty()) return &r;
    return nullptr;
  };
  auto inputIndex = [](const fx::Region& r, const std::string& name) {
    for (size_t k = 0; k < r.prog.inputs.size(); ++k)
      if (r.prog.inputs[k].name == name) return int(k);
    return -1;
  };
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, noLikely(), sk);
  const fx::Region* c = regionAt(regions, 15);
  CHECK(c != nullptr);
  if (c) {
    CHECK(c->hlsl);
    const int k = inputIndex(*c, "gColor.Sample(gLinear, uv).xyz");
    CHECK(k >= 0);
    if (k >= 0) {
      CHECK(c->facts[k].fetch && c->facts[k].assumed);
      CHECK(c->facts[k].key == "sopt_hlsl.hlsl texture gColor");
    }
  }
  const fx::Region* w = regionAt(regions, 19);
  CHECK(w != nullptr);
  if (w) {
    const int k = inputIndex(*w, "gContrast");
    CHECK(k >= 0 && w->facts[k].key == "sopt_hlsl.hlsl global gContrast");
  }

  // The texture's range reaches s = gColor.Sample(...).rgb.
  fx::UserRanges user;
  user["sopt_hlsl.hlsl texture gColor"] = {0.0, 1.0};
  fx::RegionOptions ro;
  ro.userRanges = &user;
  fx::SkipCount sk2;
  const auto ranged = fx::extractRegions(*e, nullptr, ro, sk2);
  const fx::Region* w2 = regionAt(ranged, 19);
  CHECK(w2 != nullptr);
  if (w2) {
    const int k = inputIndex(*w2, "s");
    CHECK(k >= 0 && w2->prog.inputs[k].lo == 0.0 && w2->prog.inputs[k].hi == 1.0 && !w2->facts[k].assumed);
  }

  // A variant file of the HLSL source parses as HLSL with either switch value.
  const fx::Region* r = regionAt(ranged, 20);
  CHECK(r != nullptr);
  if (!r) return;
  fx::RegionResult rr;
  rr.region = *r;
  ExprBuilder b;
  fx::Variant v;
  v.expr = b.finish(b.input(0));
  v.text = "n";
  v.cost = 0;
  v.klass = Klass::BitExact;
  rr.variants.push_back(v);
  const fs::path out = fs::temp_directory_path() / "sopt_test_hlsl_out";
  std::error_code ec;
  fs::remove_all(out, ec);
  std::string errors;
  const auto files = fx::writeVariants({rr}, out, errors);
  CHECK(errors.empty() && files.size() == 1);
  std::ifstream f(out / "sopt_hlsl.hlsl");
  std::stringstream ss;
  ss << f.rdbuf();
  CHECK(ss.str().find("float3 r = n; // sopt: bit-exact") != std::string::npos);
  CHECK(ss.str().find("__VENDOR__") == std::string::npos);
  for (const char* all : {"0", "1"}) {
    fx::LoadOptions lv = lo;
    lv.macros.emplace_back("SOPT_GROUP", all);
    CHECK(fx::loadEffect(out / "sopt_hlsl.hlsl", lv, err) != nullptr);
  }
  fs::remove_all(out, ec);
}

namespace {

const fx::Region* regionOn(const std::vector<fx::Region>& rs, uint32_t line, bool window = false) {
  for (const auto& r : rs)
    if (r.line == line && r.removed.empty() != window) return &r;
  return nullptr;
}

int inputNamed(const fx::Region& r, const std::string& name) {
  for (size_t k = 0; k < r.prog.inputs.size(); ++k)
    if (r.prog.inputs[k].name == name) return int(k);
  return -1;
}

// Writes one variant (the region's own value) for each region and checks that the
// variant file loads with SOPT_GROUP 0 and 1.
void checkVariantFile(const std::vector<const fx::Region*>& regions, const fs::path& file, const fx::LoadOptions& lo,
                      const std::string& expect) {
  std::vector<fx::RegionResult> results;
  for (const fx::Region* r : regions) {
    fx::RegionResult rr;
    rr.region = *r;
    fx::Variant v;
    v.expr = r->prog.target;
    v.text = toString(r->prog.target, r->prog.inputs);
    v.cost = 1;
    v.klass = Klass::BitExact;
    rr.variants.push_back(v);
    results.push_back(rr);
  }
  const fs::path out = fs::temp_directory_path() / "sopt_test_compute_out";
  std::error_code ec;
  fs::remove_all(out, ec);
  std::string errors;
  const auto files = fx::writeVariants(results, out, errors);
  CHECK(errors.empty() && files.size() == 1);
  std::ifstream f(out / file.filename());
  std::stringstream ss;
  ss << f.rdbuf();
  CHECK(ss.str().find(expect) != std::string::npos);
  for (const char* all : {"0", "1"}) {
    fx::LoadOptions lv = lo;
    lv.macros.emplace_back("SOPT_GROUP", all);
    std::string err;
    CHECK(fx::loadEffect(out / file.filename(), lv, err) != nullptr);
  }
  fs::remove_all(out, ec);
}

}  // namespace

TEST(fx_compute) {
  // ReShade FX compute shaders: thread IDs are float inputs (converted from uint) with ranges
  // from their semantic and the pass's group size, tex2Dstore's value is a region with the
  // storage texture's budget, groupshared reads take a user range by variable.
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_compute.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) {
    std::printf("  %s\n", err.c_str());
    return;
  }
  CHECK((fx::groupsharedVariables(*e) == std::vector<std::string>{"tile"}));
  fx::UserRanges user;
  user["sopt_compute.fx groupshared tile"] = {0.0, 1.0};
  fx::RegionOptions ro;
  ro.userRanges = &user;
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, ro, sk);
  const fx::Region* uv = regionOn(regions, 16);
  CHECK(uv != nullptr);
  if (uv) {
    const int k = inputNamed(*uv, "float2(id.xy)");
    CHECK(k >= 0 && uv->prog.inputs[k].lo == 0.0 && uv->prog.inputs[k].hi == 7680.0 && uv->prog.inputs[k].grid == 1);
  }
  const fx::Region* g = regionOn(regions, 17);
  CHECK(g != nullptr);
  if (g) {
    const int k = inputNamed(*g, "float2(tid.xy)");
    CHECK(k >= 0 && g->prog.inputs[k].hi == 7.0);  // CSMain<8, 8>
  }
  const fx::Region* w = regionOn(regions, 23);
  CHECK(w != nullptr);
  if (w) {
    CHECK(w->kind == fx::Region::Kind::Write && w->lhs == "tex2Dstore(StOut, id.xy," && w->rhs == ")");
    CHECK(w->prog.budget.kind == Budget::Kind::Color8);  // RGBA8 storage
  }
  const fx::Region* f = regionOn(regions, 24);
  CHECK(f != nullptr);
  if (f) {
    CHECK(f->prog.budget.kind == Budget::Kind::Rel);  // RGBA16F storage
    const int k = inputNamed(*f, "n");
    CHECK(k >= 0 && f->prog.inputs[k].lo == 0.0 && f->prog.inputs[k].hi == 1.0 && !f->facts[k].assumed);
  }
  if (uv && w) checkVariantFile({uv, w}, file, lo, "tex2Dstore(StOut, id.xy, float4(");
}

TEST(fx_hlsl_compute) {
  // Plain HLSL compute shader: [numthreads] makes the entry a compute shader; RW texture
  // writes are regions (a float2 element's value without the parser's widening), compound
  // writes read the element first; Name[index] reads are fetch inputs keyed by the resource.
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_compute.hlsl";
  fx::LoadOptions lo;
  lo.hlsl = true;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) {
    std::printf("  %s\n", err.c_str());
    return;
  }
  CHECK((fx::unrangedTextures(*e) ==
         std::vector<std::string>{"Depth", "Input", "Motion", "Offsets", "Output", "Sums", "Weights"}));
  CHECK(fx::textureFactKey(*e, "Weights") == "sopt_compute.hlsl buffer Weights");
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk);
  const fx::Region* in = regionOn(regions, 40);
  CHECK(in != nullptr);
  if (in) {
    const int k = inputNamed(*in, "Input[id.xy]");
    CHECK(k >= 0 && in->facts[k].fetch && in->facts[k].key == "sopt_compute.hlsl texture Input");
  }
  const fx::Region* m = regionOn(regions, 41);
  CHECK(m != nullptr);
  if (m) {
    const int k = inputNamed(*m, "Weights[gi].xy");
    CHECK(k >= 0 && m->facts[k].key == "sopt_compute.hlsl buffer Weights");
  }
  const fx::Region* o = regionOn(regions, 51);
  CHECK(o != nullptr && o->kind == fx::Region::Kind::Write && o->lhs == "Output[id.xy] =" && o->rhs.empty());
  const fx::Region* mo = regionOn(regions, 52);
  CHECK(mo != nullptr);
  if (mo) CHECK(toString(mo->prog.target, mo->prog.inputs) == "off * 2.0 + off * d");
  const fx::Region* c = regionOn(regions, 56, true);
  CHECK(c != nullptr);
  if (c) CHECK(inputNamed(*c, "Output[id.xy]") >= 0 && inputNamed(*c, "float(gid.x)") >= 0);
  // old = Motion[id.xy] is not inlined past the write to Motion.
  CHECK(regionOn(regions, 59) != nullptr && regionOn(regions, 59, true) == nullptr);
  if (o && mo && c) checkVariantFile({o, mo, c}, file, lo, "Motion[id.xy] = off * 2.0 + off * d; // sopt");
}

TEST(fx_register_counts) {
  // Register counts go into the report (columns with the change) and, where they change,
  // the variant comment.
  const Loaded l = load();
  const fx::Region* r = at(l, 23);
  CHECK(r != nullptr);
  if (!r) return;
  fx::RegionResult rr;
  rr.region = *r;
  rr.targetCost = 20;
  rr.targetAmd = 7;
  rr.targetAmdVgprs = 10;
  rr.targetAmdSgprs = 6;
  rr.targetNv = 9;
  rr.targetNvRegs = 12;
  fx::Variant v;
  v.expr = r->prog.target;
  v.text = "color.r";
  v.cost = 10;
  v.klass = Klass::Within;
  v.amd = 5;
  v.amdVgprs = 12;
  v.amdSgprs = 6;
  v.nv = 8;
  v.nvRegs = 12;
  rr.variants.push_back(v);
  fx::ReportInfo info;
  info.amd = info.nv = true;
  const std::string md = fx::markdownReport({rr}, info);
  CHECK(md.find("amd 7 (10 vgpr, 6 sgpr), nv 9 (12 regs)") != std::string::npos);
  CHECK(md.find("| amd | amd vgpr | nv | nv regs |") != std::string::npos);
  CHECK(md.find("| 5 (-29%) | 12 (+2) | 8 (-11%) | 12 |") != std::string::npos);

  const fs::path out = fs::temp_directory_path() / "sopt_test_regs_out";
  std::error_code ec;
  fs::remove_all(out, ec);
  std::string errors;
  fx::writeVariants({rr}, out, errors);
  std::ifstream f(out / "sopt_test.fx");
  std::stringstream ss;
  ss << f.rdbuf();
  CHECK(ss.str().find("amd 7 -> 5, nv 9 -> 8, vgpr 10 -> 12") != std::string::npos);
  CHECK(ss.str().find("nv regs") == std::string::npos);  // unchanged: not mentioned
  fs::remove_all(out, ec);

  // A variant kept only for fewer registers (owner, 2026-10-04): labeled, never SOPT_AUTO.
  fx::RegionResult rg = rr;
  fx::Variant& w = rg.variants[0];
  w.amd = 7;
  w.amdVgprs = 8;
  w.nv = 9;
  w.nvRegs = 12;
  w.fewerRegisters = w.notFaster = true;
  CHECK(fx::vendorPick(rg, fx::Vendor::Amd) == 0 && fx::vendorPick(rg, fx::Vendor::Nv) == 0);
  fs::remove_all(out, ec);
  fx::writeVariants({rg}, out, errors);
  std::ifstream g(out / "sopt_test.fx");
  std::stringstream gs;
  gs << g.rdbuf();
  CHECK(gs.str().find("within budget, fewer registers (not faster)") != std::string::npos);
  CHECK(gs.str().find("vgpr 10 -> 8") != std::string::npos);
  fs::remove_all(out, ec);
}

TEST(fx_precise_and_too_exact) {
  // A region that writes or feeds a precise variable is judged against float math only (no
  // exact rule): the add-round (x + C) - C must not become x there.
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_precise.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk);
  auto at = [&](uint32_t line) -> const fx::Region* {
    for (const auto& r : regions)
      if (r.line == line && r.removed.empty()) return &r;
    return nullptr;
  };
  const fx::Region* r = at(12);  // precise float r = ...
  const fx::Region* t = at(13);  // float t = ..., feeds precise s
  const fx::Region* q = at(15);  // not precise
  CHECK(r && t && q);
  if (!r || !t || !q) return;
  CHECK(!r->prog.budget.vsExact && r->budgetReason.find("precise") != std::string::npos);
  CHECK(!t->prog.budget.vsExact && !r->prog.budget.errorScale && !t->prog.budget.errorScale);
  CHECK(q->prog.budget.vsExact && q->prog.budget.errorScale);

  // A too-exact variant is switched by SOPT_TOO_EXACT and never picked by SOPT_AUTO.
  fx::RegionResult rr;
  rr.region = *q;
  rr.targetCost = 20;
  rr.targetAmd = 5;
  fx::Variant v;
  v.expr = q->prog.target;
  v.text = "uv.x * Amount";
  v.cost = 4;
  v.klass = Klass::Accurate;
  v.amd = 1;
  rr.variants.push_back(v);
  CHECK(fx::vendorPick(rr, fx::Vendor::Amd) == 0);
  const fs::path out = fs::temp_directory_path() / "sopt_test_tooexact_out";
  std::error_code ec;
  fs::remove_all(out, ec);
  std::string errors;
  fx::writeVariants({rr}, out, errors);
  std::ifstream f(out / "sopt_precise.fx");
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  CHECK(text.find("#define SOPT_TOO_EXACT 1") != std::string::npos);
  CHECK(text.find(">= 1 && SOPT_TOO_EXACT") != std::string::npos);
  CHECK(text.find("// sopt: too exact") != std::string::npos);
  for (const char* te : {"0", "1"}) {
    fx::LoadOptions lv;
    lv.macros.emplace_back("SOPT_GROUP", "1");
    lv.macros.emplace_back("SOPT_TOO_EXACT", te);
    CHECK(fx::loadEffect(out / "sopt_precise.fx", lv, err) != nullptr);
  }
  fs::remove_all(out, ec);
}

TEST(fx_namespace) {
  // Ns::Gain is read inside namespace Ns: its leaf (and so the variant text) is the plain name, as
  // the source writes it; Other::Far keeps its namespace. Fact keys stay fully qualified.
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_namespace.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) {
    std::printf("  %s\n", err.c_str());
    return;
  }
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk);
  const fx::Region* r = regionOn(regions, 22);
  CHECK(r != nullptr);
  if (r) {
    CHECK(inputNamed(*r, "Gain") >= 0);
    CHECK(inputNamed(*r, "Other::Far") >= 0);
    CHECK(inputNamed(*r, "Ns::Gain") < 0);
    bool qualifiedKey = false;
    for (const auto& f : r->facts) qualifiedKey = qualifiedKey || f.key == "sopt_namespace.fx global Ns::Gain";
    CHECK(qualifiedKey);
  }
}


TEST(fx_table_rewrite) {
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_table.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  const std::vector<fx::SourceRewrite> rw = fx::tableRewrites(*e);
  CHECK(rw.size() == 1);  // Weights is written after its initializer, Gains / Tints entries are not stable
  if (rw.size() != 1) return;
  CHECK(rw[0].line == 19 && rw[0].function == "TablePS");
  CHECK(rw[0].edits.size() == 3);  // the table before the function, the declaration, the use
  bool use = false;
  for (const auto& ed : rw[0].edits)
    for (const auto& l : ed.lines)
      use = use || l.find("(Preset == 0 ? Custom : sopt_TablePS_Coefficients_L19_at(Preset))") != std::string::npos;
  CHECK(use);
  // Per vendor (owner, 2026-10-17): a select tree for NVIDIA, the table for the rest, Intel keeps the array.
  bool tree = false, intelOff = true;
  for (const auto& ed : rw[0].edits) {
    intelOff = intelOff && ed.extra.find("__VENDOR__ != 0x8086") != std::string::npos;
    for (const auto& l : ed.lines) tree = tree || l.find("(i & 2) != 0 ?") != std::string::npos;
  }
  CHECK(tree && intelOff);
  // One version for all GPUs (owner, 2026-10-25): no vendor in any edit, the tree from D3D10 on.
  std::vector<fx::SourceRewrite> one = rw;
  fx::oneVersionForAllGpus(one);
  bool anyVendor = false, d3d10 = false;
  for (const auto& ed : one[0].edits) {
    anyVendor = anyVendor || ed.extra.find("__VENDOR__") != std::string::npos;
    for (const auto& l : ed.lines) {
      anyVendor = anyVendor || l.find("__VENDOR__") != std::string::npos;
      d3d10 = d3d10 || l == "#if __RENDERER__ >= 0xa000";
    }
  }
  CHECK(!anyVendor && d3d10 && one[0].description.find("every GPU") != std::string::npos);
  // Written and parsed with the switch off and on, as each vendor (and D3D9 for NVIDIA) compiles it.
  for (const std::vector<fx::SourceRewrite>* set : {&rw, static_cast<const std::vector<fx::SourceRewrite>*>(&one)}) {
    const fs::path out = fs::temp_directory_path() / "sopt-test-table";
    std::string errors;
    const auto files = fx::writeVariants({}, out, errors, *set);
    CHECK(files.size() == 1 && errors.empty());
    for (const char* on : {"0", "1"})
      for (const char* vendor : {"0x10DE", "0x1002", "0x8086"})
        for (const char* renderer : {"0xb000", "0x9000"}) {
          fx::LoadOptions l2;
          l2.macros.emplace_back(fx::rewriteSwitch((*set)[0]), on);
          l2.macros.emplace_back("__VENDOR__", vendor);
          l2.macros.emplace_back("__RENDERER__", renderer);
          std::string err2;
          CHECK(fx::loadEffect(out / "sopt_table.fx", l2, err2) != nullptr);
          if (!err2.empty()) std::printf("  %s %s %s: %s\n", on, vendor, renderer, err2.c_str());
        }
    fs::remove_all(out);
  }
}

TEST(fx_int_conversions) {
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_int.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk);
  // p is read as int2: 1.9999999 and 2.0 pick different texels, so it must be exact.
  const fx::Region* p = regionOn(regions, 16);
  CHECK(p != nullptr && p->prog.budget.kind == Budget::Kind::Exact);
  // n = int(Scale), Scale in [1.5, 2.5]: n is 1 or 2, not 1.5 .. 2.5.
  const fx::Region* s = regionOn(regions, 18);
  CHECK(s != nullptr);
  if (s) {
    bool n = false;
    for (const auto& d : s->prog.inputs) n = n || (d.name == "n" && d.lo <= 1.0 && d.hi >= 2.0);
    CHECK(n);
  }
}

TEST(fx_sugar_intrinsics) {
  // float %, %=, all (a float vector the parser casts to bool2), any (a vector comparison), asin, acos, atan2 + atan.
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_intrinsics.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk);
  for (uint32_t line : {16u, 18u, 19u, 20u, 21u, 22u, 23u}) {
    const fx::Region* r = regionOn(regions, line);
    CHECK(r != nullptr);
    if (!r) std::printf("  no region on line %u\n", line);
  }
  // atan2 written out is ~30 operations: a region only above the default limit of 24.
  CHECK(regionOn(regions, 24) == nullptr);
  fx::RegionOptions big;
  big.maxOps = 40;
  fx::SkipCount sk2;
  CHECK(regionOn(fx::extractRegions(*e, nullptr, big, sk2), 24) != nullptr);
}

TEST(fx_include_from_folder_above) {
  // No include paths: sopt_test.fxh is found in the folder above the effect.
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sub" / "sopt_up.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) std::printf("  %s\n", err.c_str());
}

TEST(fx_include_from_subfolder) {
  // As ReShade's Shaders\** search path: Pack\a.fx includes "b.fxh" from Pack\inc (BFBFX's bfb_inc).
  const fs::path root = fs::temp_directory_path() / "sopt-test-sub" / "reshade-shaders" / "Shaders";
  fs::create_directories(root / "Pack" / "inc");
  std::ofstream(root / "Pack" / "inc" / "b.fxh") << "float4 VS(uint id : SV_VertexID) : SV_Position { return 0; }\n"
                                                    "float4 PS(float4 p : SV_Position) : SV_Target { return p; }\n";
  std::ofstream(root / "Pack" / "a.fx") << "#include \"b.fxh\"\n"
                                           "technique T { pass { VertexShader = VS; PixelShader = PS; } }\n";
  std::string err;
  auto e = fx::loadEffect(root / "Pack" / "a.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) std::printf("  %s\n", err.c_str());
  std::error_code ec;
  fs::remove_all(root.parent_path().parent_path(), ec);
}

TEST(fx_pragma_once_one_spelling) {
  // a.fx includes h.fxh, and inc/b.fxh includes it through an include path: on Windows "Pack/h.fxh" and "Pack\h.fxh"
  // were two files to #pragma once, so h.fxh came twice ("redefinition", BFBFX's OpenRSF.fxh, 2026-10-25).
  const fs::path root = fs::temp_directory_path() / "sopt-test-once" / "reshade-shaders" / "Shaders";
  fs::create_directories(root / "Pack" / "inc");
  std::ofstream(root / "Pack" / "h.fxh") << "#pragma once\ntexture TexH { Width = 1; Height = 1; };\nfloat K() { return 0.5; }\n";
  std::ofstream(root / "Pack" / "inc" / "b.fxh") << "#include \"h.fxh\"\n";
  std::ofstream(root / "Pack" / "a.fx") << "#include \"h.fxh\"\n#include \"inc/b.fxh\"\n"
                                           "float4 VS(uint id : SV_VertexID) : SV_Position { return K(); }\n"
                                           "float4 PS(float4 p : SV_Position) : SV_Target { return p * K(); }\n"
                                           "technique T { pass { VertexShader = VS; PixelShader = PS; } }\n";
  std::string err;
  auto e = fx::loadEffect((root / "Pack").generic_string() + "/a.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) std::printf("  %s\n", err.c_str());
  std::error_code ec;
  fs::remove_all(root.parent_path().parent_path(), ec);
}

TEST(fx_const_guard) {
  // Line 20 names Dark, a static const here but a slider when SLIDERS is 1: its variants only for this build (a guard,
  // which easy mode leaves alone). Line 21 names no such constant: no guard. A #define over two lines (11) must not
  // make the file's regions give up (SuperDepth3D, 2026-10-25).
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_constguard.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::RegionOptions ro;
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, ro, sk);
  const fx::Region* t = regionOn(regions, 20);
  const fx::Region* u = regionOn(regions, 21);
  CHECK(t != nullptr && u != nullptr);
  if (t) CHECK(t->guard.find("SLIDERS") != std::string::npos);
  if (u) CHECK(u->guard.empty());
}

TEST(fx_likely_ranges) {
  // A value read from a texture in plain HLSL (format unknown: c = gColor.Sample(...)) gets the likely range [0, 1],
  // also where it is used later (c.x); --no-likely: the assumed default.
  fx::LoadOptions lo;
  lo.hlsl = true;
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_extras.hlsl", lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  auto factOf = [&](bool likely) {
    fx::RegionOptions ro;
    ro.likelyRanges = likely;
    fx::SkipCount sk;
    const auto regions = fx::extractRegions(*e, nullptr, ro, sk);
    const fx::Region* r = regionOn(regions, 13);
    CHECK(r != nullptr);
    for (size_t k = 0; r && k < r->facts.size(); ++k)
      if (r->prog.inputs[k].name == "c.x") return std::make_pair(r->facts[k], r->prog.inputs[k]);
    return std::make_pair(fx::Fact(), InputDecl());
  };
  const auto [on, onIn] = factOf(true);
  CHECK(on.likely && onIn.lo == 0.0 && onIn.hi == 1.0);
  const auto [off, offIn] = factOf(false);
  CHECK(!off.likely && off.assumed);
}

TEST(fx_hlsl_glsl_extras) {
  // Intrinsics HLSL / GLSL have and ReShade FX lacks: the file loads and the statements are regions.
  auto regionsOf = [](const char* name, bool glsl) {
    fx::LoadOptions lo;
    lo.glsl = glsl;
    lo.hlsl = !glsl;
    std::string err;
    auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / name, lo, err);
    CHECK(e != nullptr);
    if (!e) std::printf("  %s\n", err.c_str());
    fx::SkipCount sk;
    return e ? fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk) : std::vector<fx::Region>{};
  };
  const auto h = regionsOf("sopt_extras.hlsl", false);
  CHECK(regionOn(h, 13) != nullptr);  // fmod
  const auto obs = regionsOf("sopt_obs.hlsl", false);
  CHECK(regionOn(obs, 15) != nullptr);  // OBS Studio: brace initializers, -NEG(-1)
  const fx::Region* inv = regionOn(obs, 17);  // a likely range from the name: an inverse size
  CHECK(inv != nullptr);
  for (size_t k = 0; inv && k < inv->facts.size(); ++k)
    if (inv->prog.inputs[k].name == "base_dimension_i.x")
      CHECK(inv->facts[k].likely && inv->prog.inputs[k].lo > 0.0 && inv->prog.inputs[k].hi == 1.0);
  const auto g = regionsOf("sopt_extras.frag", true);
  for (uint32_t line : {13u, 14u, 15u, 16u}) CHECK(regionOn(g, line) != nullptr);  // asinh, acosh, atanh, atan(x)
  // A Vulkan GLSL compute shader as FFmpeg writes them: sampler / image arrays, push constants, buffers, shared,
  // specialization constants, matrix products, vector ==, length(), imageStore, gl_GlobalInvocationID.
  CHECK(regionOn(regionsOf("sopt_compute.glsl", true), 40) != nullptr);
  // GLSL as WebRender writes it: array types, a prototype (main moves after the definition, its lines kept), precision
  // qualifiers, gl_FragCoord in a helper, a last case without break.
  const auto w = regionsOf("sopt_glsl_more.frag", true);
  for (uint32_t line : {22u, 30u, 43u}) CHECK(regionOn(w, line) != nullptr);
}

TEST(fx_window_same_block) {
  // dh_uber_rt.fx (2026-10-20): "float2 w = uv * 0.5; { ... w *= ...; }": a window from the declaration
  // would declare w inside the inner block. Windows with the inner temporary d stay.
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_block.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::RegionOptions ro;
  fx::SkipCount sk;
  bool inner = false;
  for (const auto& r : fx::extractRegions(*e, nullptr, ro, sk))
    if (r.line == 9) {
      CHECK(r.kind != fx::Region::Kind::Init);
      inner = inner || !r.removed.empty();
    }
  CHECK(inner);
}

TEST(fx_per_draw_locals) {
  // A local computed from uniforms only (also under an if on a uniform) is a constant in performance mode, one set
  // under a per-pixel condition is not (LumaSharpen's sharp_strength_luma, owner 2026-10-25).
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_perdraw.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::RegionOptions ro;
  fx::SkipCount sk;
  int w = -1, k = -1;
  for (const auto& r : fx::extractRegions(*e, nullptr, ro, sk))
    for (const auto& d : r.prog.inputs) {
      if (r.line == 15 && d.name == "w") w = d.rate == InputDecl::Rate::Uniform;
      if (r.line == 16 && d.name == "k") k = d.rate == InputDecl::Rate::Uniform;
    }
  CHECK(w == 1 && k == 0);
}

TEST(fx_inline_calls) {
  // Calls of small pure helpers are seen through (owner, 2026-10-26): parameters bound per call (Luma twice with
  // different arguments), variables assigned again, nested calls, ifs and early returns (selects), components
  // written (c.rg *= 0.5).
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_inline.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::RegionOptions ro;
  fx::SkipCount sk;
  sk.keepDetails = true;
  const auto regions = fx::extractRegions(*e, nullptr, ro, sk);
  const fx::Region* a = regionOn(regions, 12);
  const fx::Region* b = regionOn(regions, 13);
  const fx::Region* d = regionOn(regions, 14);
  CHECK(a && b && d);
  if (d) {
    const std::string td = toString(d->prog.target, d->prog.inputs);
    std::printf("  %s\n", td.c_str());
    CHECK(td.find("(c.x > 0.5 ? 1.0 : c.x) * c.y") != std::string::npos);  // Branchy: the early return
    CHECK(td.find("!(c.y > 0.5) ? c.z * 3.0 : (c.y > 0.5 ? c.y * 2.0 : c.z)") != std::string::npos);  // SideB
    CHECK(td.find("(0.5 * c.xy).y") != std::string::npos);  // Tint: v.rg *= 0.5
  }
  // One helper of several instructions is searched alone, a macro left of the = does not matter (2026-10-26); one
  // instruction is not, with how often its value is read.
  CHECK(regionOn(regions, 15) != nullptr && !regionOn(regions, 16));
  bool used = false;
  for (const auto& d : sk.details) used = used || d.find(":16: one instruction (value used several times)") != std::string::npos;
  CHECK(used);
  if (!a || !b) return;
  const std::string ta = toString(a->prog.target, a->prog.inputs), tb = toString(b->prog.target, b->prog.inputs);
  std::printf("  %s\n  %s\n", ta.c_str(), tb.c_str());
  CHECK(ta.find("Strength") != std::string::npos && ta.find(".zyx") != std::string::npos);
  CHECK(tb.find("dot(") != std::string::npos);
  CHECK(tb.find("c.z * 2.0 * (c.z * 2.0 + 1.0)") != std::string::npos);  // Twice: x and y assigned again
}

TEST(fx_select_conditions) {
  // A scalar condition HLSL widens to vector branches, a bool / int slider or a float as the condition itself
  // (2026-10-26: "vector select" was mostly these): all regions, the non-bool ones tested against 0.
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_select.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::RegionOptions ro;
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, ro, sk);
  for (uint32_t line = 9; line <= 15; ++line) {
    const fx::Region* r = regionOn(regions, line);
    CHECK(r != nullptr);
    if (r && line == 10) CHECK(toString(r->prog.target, r->prog.inputs).rfind("float(Flag) != 0.0 ?", 0) == 0);
  }
}

TEST(fx_oneline_if) {
  // A one-line if / else in front of an assignment stays as written, the value is the region (FXAA, 2026-10-26).
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_oneline_if.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::RegionOptions ro;
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, ro, sk);
  const fx::Region* r9 = regionOn(regions, 9);
  const fx::Region* r10 = regionOn(regions, 10);
  CHECK(r9 && r10 && regionOn(regions, 11));
  if (r9) CHECK(r9->prefix == "if (c.z > 0.5) " && r9->lhs == "pos.x =" && r9->tail == ";");
  if (r10) CHECK(r10->prefix + r10->lhs == "else pos.y =");  // the location is at "else": the older prefix rule
  // Other code on the same line (2026-10-26): kept before / after the statement, never in a window.
  auto has = [&](uint32_t line, const char* prefix, const char* lhs, const char* tail) {
    for (const auto& r : regions)
      if (r.line == line && r.removed.empty() && r.prefix == prefix && r.lhs == lhs && r.tail == tail) return true;
    return false;
  };
  CHECK(has(14, "float3 OMul(float3 a, float3 b) { ", "return", "; }"));
  CHECK(has(18, "", "float a =", "; float b = c.y * 3.0 + c.y * 0.5;"));
  CHECK(has(18, "float a = c.x * c.y + c.z; ", "float b =", ";"));
  CHECK(has(19, "", "float s =", ", t = c.z * c.z + c.y * c.z;"));
  CHECK(has(19, "float s = c.x * 2.0 + c.y, ", "t =", ";"));
  for (const auto& r : regions) {
    for (const auto& [a, b] : r.removed) CHECK(b < 18 || a > 19);
    if (r.line < 18 || r.line > 19) continue;
    CHECK(!r.sharesLines() || r.removed.empty());
    if (r.line == 19 && r.lhs == "t =")
      CHECK(fx::variantStatement(r, "x") == "float s = c.x * 2.0 + c.y, t = x;");
  }
}

TEST(fx_setting_paths) {
  // Code only other values of a setting compile (owner, 2026-10-26): the values the #if lines test, and the regions
  // of such a parse on lines the default leaves out, under their #if.
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_settings.fx";
  std::string err;
  auto def = fx::loadEffect(file, fx::LoadOptions(), err);
  CHECK(def != nullptr);
  if (!def) return;
  const auto values = fx::settingValues(*def);
  CHECK(values.size() == 2 && values[0] == std::make_pair(std::string("SETTING_MODE"), std::string("2")) &&
        values[1] == std::make_pair(std::string("SETTING_MODE"), std::string("4")));
  fx::LoadOptions lo;
  lo.macros.emplace_back("SETTING_MODE", "4");
  auto cfg = fx::loadEffect(file, lo, err);
  CHECK(cfg != nullptr);
  if (!cfg) return;
  const auto regions = fx::settingRegions(*def, *cfg, nullptr, fx::RegionOptions());
  CHECK(regions.size() == 1 && regions[0].line == 15 &&
        regions[0].guard.find("(SETTING_MODE > 3)") != std::string::npos);
}

TEST(fx_int_inputs) {
  // Integer variables read as float (owner, 2026-10-26): inputs written float(x), a loop counter in [A, B] from its
  // for header (B: its value after the loop), an int slider on whole numbers.
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_intinputs.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::SkipCount sk;
  bool loop = false, slider = false, arith = false;
  for (const auto& r : fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk)) {
    if (!r.removed.empty()) continue;
    // Integer + - * converted to float: float math on float(Steps) (whole numbers far below 2^24).
    arith = arith || (r.line == 14 && r.prog.inputs.size() == 2 &&
                      toString(r.prog.target, r.prog.inputs).find("float(Steps) * 2.0 + 1.0") != std::string::npos);
    for (size_t k = 0; k < r.prog.inputs.size(); ++k) {
      const auto& d = r.prog.inputs[k];
      loop = loop || (r.line == 12 && d.name == "float(i)" && d.lo == 0 && d.hi == 4 && !r.facts[k].assumed);
      slider = slider || (r.line == 13 && d.name == "float(Steps)" && d.lo == 1 && d.hi == 8 && d.grid == 7);
    }
  }
  CHECK(loop && slider && arith);
}

TEST(fx_call_inputs) {
  // A pure helper sopt cannot inline (a texture read, a loop) is an input named by its call text, its range from the
  // helper's returns (2026-10-26).
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_calls.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::RegionOptions ro;
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, ro, sk);
  const fx::Region* a = regionOn(regions, 11);
  const fx::Region* b = regionOn(regions, 12);
  CHECK(a && b);
  if (!a || !b) return;
  CHECK(a->prog.inputs.size() == 1 && a->prog.inputs[0].name == "GetLuma(p.xy)" && !a->facts[0].assumed &&
        a->prog.inputs[0].lo == 0.0 && a->prog.inputs[0].hi >= 1.0);
  bool sum = false;
  for (size_t k = 0; k < b->prog.inputs.size(); ++k) sum = sum || b->prog.inputs[k].name == "Sum(c.r)";
  CHECK(sum);
}

TEST(fx_window_temp_other_settings) {
  // e is read once with the default settings and once more with TEMP_FIX: no window removes its declaration
  // (ArcaneBloom.fx, 2026-10-26).
  std::string err;
  auto e = fx::loadEffect(fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_settingtemp.fx", fx::LoadOptions(), err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::RegionOptions ro;
  fx::SkipCount sk;
  sk.keepDetails = true;
  bool removes = false;
  for (const auto& r : fx::extractRegions(*e, nullptr, ro, sk))
    for (const auto& [a, b] : r.removed) removes = removes || (a <= 13 && 13 <= b);
  bool reason = false;
  for (const auto& d : sk.details) reason = reason || d.find(":14: window: a temporary is used under other settings") != std::string::npos;
  CHECK(!removes && reason);
}

TEST(fx_output_keeps_folders) {
  // sopt-out keeps the folders below the input folder, so it can be copied over the Shaders folder (2026-10-21);
  // files outside every input folder (ReShade.fxh above a pack's folder) keep their name only.
  const fs::path root = fs::path("C:/x/reshade-shaders/Shaders");
  CHECK(fx::outputName(root / "BFBFX" / "bfb_inc" / "random.fxh", {root}).generic_string() == "BFBFX/bfb_inc/random.fxh");
  CHECK(fx::outputName(root / "Vibrance.fx", {root / "BFBFX", root}).generic_string() == "Vibrance.fx");
  CHECK(fx::outputName(root / "ReShade.fxh", {root / "BFBFX"}).generic_string() == "ReShade.fxh");
  CHECK(fx::outputName(root / "BFBFX" / "AO.fx", {}).generic_string() == "AO.fx");
}

TEST(fx_hoist_rewrite) {
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_hoist.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::RegionOptions ro;
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, ro, sk);
  const std::vector<fx::SourceRewrite> rw = fx::hoistRewrites(*e, regions, *costModelByName("rdna3"));
  CHECK(rw.size() == 1);
  if (rw.size() != 1) return;
  CHECK(rw[0].function == "HoistPS" && rw[0].tag == "V");
  bool wrapper = false, pass = false, flat = false, cheap = false;
  for (const auto& ed : rw[0].edits) {
    for (const auto& l : ed.lines) {
      wrapper = wrapper || l.find("void sopt_VS_HoistPS(in uint id : SV_VertexID, out float4 vpos : SV_Position, "
                                  "out float2 texcoord : TEXCOORD0") != std::string::npos;
      pass = pass || l.find("VertexShader = sopt_VS_HoistPS;") != std::string::npos;
      flat = flat || (l.find("color * sopt_f0.x") != std::string::npos &&  // k = Strength * ... inlined
                      ed.extra.find("!__RESHADE_PERFORMANCE_MODE__") != std::string::npos);
    }
    cheap = cheap || (ed.first <= 24 && ed.last >= 24);  // float2 o = texcoord + Strength * 0.01;
  }
  CHECK(wrapper && pass && flat && !cheap);
  // Written and parsed with the switch off and on, with and without performance mode.
  const fs::path out = fs::temp_directory_path() / "sopt-test-hoist";
  std::string errors;
  const auto files = fx::writeVariants({}, out, errors, rw);
  CHECK(files.size() == 1 && errors.empty());
  for (const char* on : {"0", "1"})
    for (const char* perf : {"0", "1"}) {
      fx::LoadOptions l2;
      l2.macros.emplace_back(fx::rewriteSwitch(rw[0]), on);
      l2.macros.emplace_back("__RESHADE_PERFORMANCE_MODE__", perf);
      std::string err2;
      CHECK(fx::loadEffect(out / "sopt_hoist.fx", l2, err2) != nullptr);
      if (!err2.empty()) std::printf("  %s\n", err2.c_str());
    }
  fs::remove_all(out);
}

TEST(fx_blend_rewrite) {
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_blend.fx";
  fx::LoadOptions lo;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  const std::vector<fx::SourceRewrite> rw = fx::blendRewrites(*e, fx::RegionOptions(), 1);
  // lerp, multiply, min, screen (color.rgb = ...; return color;), add (out parameter); not the sharpen
  // (B = 1 + Strength)
  CHECK(rw.size() == 5);
  if (rw.size() != 5) return;
  auto has = [&](const fx::SourceRewrite& r, const char* text) {
    for (const auto& ed : r.edits)
      for (const auto& l : ed.lines)
        if (l.find(text) != std::string::npos) return true;
    return false;
  };
  CHECK(rw[0].function == "LerpPS" && has(rw[0], "SrcBlend = ONE; DestBlend = SRCALPHA;") &&
        has(rw[0], "float4 LerpPS(") && has(rw[0], "return float4("));
  CHECK(rw[1].function == "VignettePS" && has(rw[1], "SrcBlend = DESTCOLOR; DestBlend = ZERO;"));
  CHECK(rw[2].function == "DarkenPS" && has(rw[2], "BlendOp = MIN;"));
  CHECK(rw[3].function == "ScreenPS" && has(rw[3], "SrcBlend = ONE; DestBlend = INVSRCCOLOR;") && has(rw[3], "return float4("));
  CHECK(rw[4].function == "AddPS" && has(rw[4], "SrcBlend = ONE; DestBlend = ONE;") && has(rw[4], "result = float4("));
  const fs::path out = fs::temp_directory_path() / "sopt-test-blend";
  std::string errors;
  const auto files = fx::writeVariants({}, out, errors, rw);
  CHECK(files.size() == 1 && errors.empty());
  for (const char* on : {"0", "1"}) {
    fx::LoadOptions l2;
    l2.macros.emplace_back("SOPT_GROUP", on);
    std::string err2;
    auto e2 = fx::loadEffect(out / "sopt_blend.fx", l2, err2);
    CHECK(e2 != nullptr);
    if (!err2.empty()) std::printf("  %s\n", err2.c_str());
    // With the switch on, three passes blend.
    if (e2) {
      int blends = 0;
      for (const auto& t : e2->cg->mod().techniques)
        for (const auto& p : t.passes) blends += p.blend_enable[0];
      CHECK(blends == (on[0] == '1' ? 5 : 0));
    }
  }
  fs::remove_all(out);
}

TEST(fx_glsl) {
  // GLSL fragment shaders: in / out globals are the entry point's inputs and outputs, sampler2D
  // reads are fetches in their GLSL text keyed by the sampler, variants print as GLSL and the
  // header goes after #version.
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_glsl.frag";
  fx::LoadOptions lo;
  lo.glsl = true;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) {
    std::printf("  %s\n", err.c_str());
    return;
  }
  CHECK(e->glsl && e->hlsl);
  CHECK(fx::textureFactKey(*e, "__sopt_tex_uScene") == "sopt_glsl.frag texture uScene");
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk);
  const fx::Region* w = regionOn(regions, 21, true);
  CHECK(w != nullptr);
  if (w) {
    CHECK(w->glsl);
    const int k = inputNamed(*w, "texture(uScene, vUv + vec2(uTexel.x, 0.0)).xyz");
    CHECK(k >= 0 && w->facts[k].fetch && w->facts[k].key == "sopt_glsl.frag texture uScene");
  }
  const fx::Region* v = regionOn(regions, 22);
  CHECK(v != nullptr);
  if (!v) return;
  const int k = inputNamed(*v, "vUv.x");
  CHECK(k >= 0 && !v->facts[k].assumed && v->prog.inputs[k].lo == 0.0 && v->prog.inputs[k].hi == 1.0);
  CHECK(toGlsl(v->prog.target, v->prog.inputs) == "fract(vUv.x * 0.5) * 2.0 + 1.0 - fract(vUv.x * 0.5) * 2.0");

  // GLSL printing: names, mad / rcp / saturate written out, scalars GLSL does not broadcast.
  const std::vector<InputDecl> in = {{"a", -4, 4, 0, Type::Float3}, {"s", -4, 4, 0, Type::Float}};
  auto glsl = [&](const char* t) { return toGlsl(parseExpr(t, in), in); };
  CHECK(glsl("lerp(a, frac(a), s)") == "mix(a, fract(a), s)");
  CHECK(glsl("pow(a, s)") == "pow(a, vec3(s))");
  CHECK(glsl("mad(a, s, 0.5) * 2.0") == "(a * s + 0.5) * 2.0");
  CHECK(glsl("saturate(rsqrt(a)) + rcp(s + 1.0)") == "clamp(inversesqrt(a), 0.0, 1.0) + 1.0 / (s + 1.0)");
  CHECK(glsl("float3(round(s), 1.0, 2.0)") == "vec3(roundEven(s), 1.0, 2.0)");
  CHECK(glsl("s < 0.5 ? a : s") == "s < 0.5 ? a : vec3(s)");

  fx::RegionResult rr;
  rr.region = *v;
  fx::Variant var;
  var.expr = parseExpr("1.0", {});
  var.text = "1.0";
  var.cost = 0;
  var.klass = Klass::BitExact;
  rr.variants.push_back(var);
  const fs::path out = fs::temp_directory_path() / "sopt_test_glsl_out";
  std::error_code ec;
  fs::remove_all(out, ec);
  std::string errors;
  const auto files = fx::writeVariants({rr}, out, errors);
  CHECK(errors.empty() && files.size() == 1);
  std::ifstream f(out / file.filename());
  std::stringstream ss;
  ss << f.rdbuf();
  CHECK(ss.str().rfind("#version 410 core\n// Variants generated by sopt", 0) == 0);
  CHECK(ss.str().find("    float v = 1.0;") != std::string::npos);
  for (const char* all : {"0", "1"}) {
    fx::LoadOptions lv = lo;
    lv.macros.emplace_back("SOPT_GROUP", all);
    CHECK(fx::loadEffect(out / file.filename(), lv, err) != nullptr);
  }
  fs::remove_all(out, ec);
}

TEST(fx_easy_mode) {
  // Easy mode (owner, 2026-10-08): one recommended variant per region (safe picks), written in directly
  // (clean) or behind switches that default to it.
  const fs::path file = fs::path(SOPT_TESTS_DIR) / "fx" / "sopt_glsl.frag";
  fx::LoadOptions lo;
  lo.glsl = true;
  std::string err;
  auto e = fx::loadEffect(file, lo, err);
  CHECK(e != nullptr);
  if (!e) return;
  fx::SkipCount sk;
  const auto regions = fx::extractRegions(*e, nullptr, fx::RegionOptions(), sk);
  const fx::Region* v = regionOn(regions, 22);
  CHECK(v != nullptr);
  if (!v) return;
  auto variant = [](const char* text, Klass k, int amd) {
    fx::Variant var;
    var.expr = parseExpr(text, {});
    var.text = text;
    var.klass = k;
    var.amd = amd;
    return var;
  };
  fx::RegionResult rr;
  rr.region = *v;
  rr.targetAmd = 5;
  rr.variants = {variant("2.0", Klass::LessAccurate, 1), variant("3.0", Klass::Within, 6),
                 variant("1.0", Klass::BitExact, 2), variant("4.0", Klass::Accurate, 1)};
  std::vector<fx::RegionResult> results = {rr};
  std::vector<fx::SourceRewrite> rewrites;
  fx::easyPicks(results, rewrites, fx::EasyOptions{});
  CHECK(results[0].variants.size() == 1 && results[0].variants[0].text == "1.0");  // not less accurate / slower / too exact
  std::vector<fx::RegionResult> tooExact = {rr};
  fx::EasyOptions eo;
  eo.tooExact = true;
  fx::easyPicks(tooExact, rewrites, eo);
  CHECK(tooExact[0].variants.size() == 1 && tooExact[0].variants[0].text == "4.0");
  // Identical results only (owner, 2026-10-25): a faster "within budget" variant gives way to the bit-exact one; and a
  // pick fxc compiles to the original's code is a change of style only, never picked.
  fx::RegionResult rw = rr;
  rw.variants = {variant("3.0", Klass::Within, 1), variant("1.0", Klass::BitExact, 2)};
  std::vector<fx::RegionResult> within = {rw}, identical = {rw}, same = {rw};
  fx::easyPicks(within, rewrites, fx::EasyOptions{});
  CHECK(within[0].variants.size() == 1 && within[0].variants[0].text == "3.0");
  fx::EasyOptions io;
  io.identicalOnly = true;
  fx::easyPicks(identical, rewrites, io);
  CHECK(identical[0].variants.size() == 1 && identical[0].variants[0].text == "1.0");
  same[0].variants[0].dxbcSame = true;
  fx::easyPicks(same, rewrites, fx::EasyOptions{});
  CHECK(same[0].variants.size() == 1 && same[0].variants[0].text == "1.0");
  // A guessed (likely) range: nothing in easy mode, not every variant one after another (SuperDepth3D.fx, 2026-10-24).
  std::vector<fx::RegionResult> guessed = {rr};
  if (guessed[0].region.facts.empty()) guessed[0].region.facts.resize(1);
  guessed[0].region.facts[0].likely = true;
  fx::easyPicks(guessed, rewrites, fx::EasyOptions{});
  CHECK(guessed[0].variants.empty());
  // Same on the chosen card, faster on some GPU families (owner, 2026-10-09): taken only when the Steam-share
  // weighted change is negative; among equals the one that helps the world most.
  {
    const size_t n = fx::platforms().size();
    fx::RegionResult other = rr;
    other.targetAmd = -1;
    other.targetPlatform.assign(n, 10);
    auto cross = [&](const char* text, double world, int slowerAt) {
      fx::Variant var = variant(text, Klass::BitExact, -1);
      var.otherGpus = true;
      var.platform.assign(n, 8);
      if (slowerAt >= 0) var.platform[static_cast<size_t>(slowerAt)] = 12;
      var.world = world;
      return var;
    };
    other.variants = {cross("5.0", 2.0, 0), cross("6.0", -1.0, 1), cross("7.0", -3.0, 2)};
    std::vector<fx::RegionResult> picks = {other};
    fx::easyPicks(picks, rewrites, fx::EasyOptions{});
    if (!fx::shareSurvey().empty())
      CHECK(picks[0].variants.size() == 1 && picks[0].variants[0].text == "7.0");
    else
      CHECK(picks[0].variants.empty());  // without survey data, no family may get slower
    other.variants = {cross("8.0", 1.0, -1)};  // slower nowhere: always fine
    picks = {other};
    picks[0].variants[0].world = -1.0;
    fx::easyPicks(picks, rewrites, fx::EasyOptions{});
    CHECK(picks[0].variants.size() == 1);
  }
  const fs::path out = fs::temp_directory_path() / "sopt_test_easy_out";
  std::error_code ec;
  for (const bool clean : {true, false}) {
    fs::remove_all(out, ec);
    fx::WriteOptions wo;
    wo.clean = clean;
    wo.allOn = !clean;
    std::string errors;
    CHECK(fx::writeVariants(results, out, errors, {}, wo).size() == 1 && errors.empty());
    std::ifstream f(out / file.filename());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    CHECK(s.rfind("#version 410 core\n// Optimized by SweetOpt", 0) == 0);
    CHECK(s.find("    float v = 1.0;") != std::string::npos);
    CHECK((s.find("SOPT_") == std::string::npos) == clean);
    CHECK(clean || s.find("#define SOPT_GROUP 1") != std::string::npos);
    CHECK(fx::loadEffect(out / file.filename(), lo, err) != nullptr);
  }
  fs::remove_all(out, ec);
}
