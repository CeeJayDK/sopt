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
  const fx::Region* ret = at(l, 32);
  CHECK(ret != nullptr);
  if (ret) {
    CHECK(ret->kind == fx::Region::Kind::Return);
    CHECK(ret->prog.budget.kind == Budget::Kind::Color8);
    CHECK(ret->prog.budget.maxCodeDiff == 0);
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
  CHECK(text.find("\tfloat3 r = c; // sopt: bit-exact") != std::string::npos);
  CHECK(text.find("#else\n\tfloat3 r = c * 0.5 + c * 0.5;\n#endif") != std::string::npos);
  // The effect, copied next to the changed header, parses with either switch value.
  fs::copy_file(kEffect, out / "sopt_test.fx", fs::copy_options::overwrite_existing, ec);
  for (const char* all : {"0", "1"}) {
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
