#include <cmath>
#include <string>

#include "ir/eval.hpp"
#include "ir/parser.hpp"
#include "measure/isa.hpp"
#include "measure/sass.hpp"
#include "search/driver.hpp"
#include "test.hpp"
#include "verify/points.hpp"
#include "verify/verify.hpp"

using namespace sopt;

namespace {

std::vector<InputDecl> vin() {
  InputDecl v{"v", -1.0, 1.0, 0};
  v.type = Type::Float3;
  InputDecl c{"c", 0.0, 1.0, 255};
  c.type = Type::Float3;
  InputDecl s{"s", 0.0, 1.0, 0};
  return {v, c, s};
}

bool throws(const char* text) {
  try {
    parseExpr(text, vin());
  } catch (const ParseError&) {
    return true;
  }
  return false;
}

bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

}  // namespace

TEST(vector_parse_types) {
  const auto in = vin();
  auto type = [&](const char* t) { const Expr e = parseExpr(t, in); return e.nodes[e.root].type; };
  CHECK(type("v") == Type::Float3);
  CHECK(type("v * s") == Type::Float3);          // scalar broadcast
  CHECK(type("dot(v, c)") == Type::Float);
  CHECK(type("length(v - c)") == Type::Float);
  CHECK(type("normalize(v)") == Type::Float3);
  CHECK(type("v.xy") == Type::Float2);
  CHECK(type("c.rgb") == Type::Float3);
  CHECK(type("float4(v, s)") == Type::Float4);
  CHECK(type("float3(s)") == Type::Float3);      // broadcast constructor
  CHECK(type("v.x < s ? v : c") == Type::Float3);
  CHECK(throws("v + v.xy"));                     // float3 + float2
  CHECK(throws("v < c"));                        // comparisons are scalar
  CHECK(throws("v.q"));
  CHECK(throws("s.y"));                          // out of range
  CHECK(throws("float3(v, s)"));                 // 4 components for float3
}

TEST(vector_print_roundtrip) {
  const auto in = vin();
  for (const char* t : {"dot(v, float3(0.2126, 0.7152, 0.0722))", "normalize(v) * length(v)",
                        "v.x * c.z + s", "saturate(v * 2.0 - 1.0)", "(v + c).y", "float4(v, s)"}) {
    const std::string printed = toString(parseExpr(t, in), in);
    CHECK(toString(parseExpr(printed, in), in) == printed);
  }
  CHECK(toString(parseExpr("c.rgb", in), in) == "c");  // identity swizzle
  // Constant folding works per component.
  CHECK(toString(parseExpr("float3(1.0, 2.0, 3.0) * 2.0", in), in) == "float3(2.0, 4.0, 6.0)");
  CHECK(toString(parseExpr("dot(float2(3.0, 4.0), float2(3.0, 4.0))", in), in) == "25.0");
}

TEST(vector_eval_helpers) {
  const auto in = vin();
  Program prog;
  prog.inputs = in;
  prog.target = parseExpr("v", in);
  PointSet ps = makeTestPoints(prog, 64, 3);
  auto vals = [&](const char* t, const Profile& p) { return evalAll(parseExpr(t, in), ps, p); };
  const auto dot = vals("dot(v, c)", kProfileRef);
  const auto len = vals("length(v)", kProfileRef);
  const auto nrm = vals("normalize(v)", kProfileRef);
  const auto dst = vals("distance(v, c)", kProfileRef);
  const auto fused = vals("dot(v, c)", kProfileFma);
  const size_t n = ps.size();
  const uint32_t sv = ps.slotOf(0), sc = ps.slotOf(1);
  for (size_t i = 0; i < n; ++i) {
    const float v[3] = {ps.cols[sv][i], ps.cols[sv + 1][i], ps.cols[sv + 2][i]};
    const float c[3] = {ps.cols[sc][i], ps.cols[sc + 1][i], ps.cols[sc + 2][i]};
    const float d = v[0] * c[0] + v[1] * c[1] + v[2] * c[2];  // left to right, unfused
    CHECK(dot[i] == d);
    CHECK(fused[i] == std::fma(v[2], c[2], std::fma(v[1], c[1], v[0] * c[0])));
    const float vv = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    CHECK(len[i] == std::sqrt(vv));
    const float r = 1.0f / std::sqrt(vv);
    if (std::isfinite(r))  // v = 0 gives NaN in both
      for (int k = 0; k < 3; ++k) CHECK(nrm[k * n + i] == v[k] * r);  // component-major
    const float e[3] = {v[0] - c[0], v[1] - c[1], v[2] - c[2]};
    CHECK(dst[i] == std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]));
  }
}

TEST(vector_budgets_parse) {
  const Program a = parseProgram("input c : float3 in [0, 1] grid 1023\noutput r = c * 0.5\nbudget r : color10\n");
  CHECK(a.budget.kind == Budget::Kind::Color10 && a.budget.codeBits() == 10);
  CHECK(a.inputs[0].type == Type::Float3);
  const Program b = parseProgram("input t : float2 in [0, 1]\noutput r = t * 2.0\nbudget r : texcoord 0.5\n");
  CHECK(b.budget.kind == Budget::Kind::Texcoord && std::fabs(b.budget.eps - 0.5 / 3840.0) < 1e-12);
  const Program d = parseProgram("input t : float in [0, 1]\noutput r = t * 2.0\nbudget r : depth\n");
  CHECK(d.budget.kind == Budget::Kind::Exact);
}

TEST(v2_domain) {
  InputDecl g{"g", 0.0, 1.0, 255};
  CHECK(domainCount(g) == 256);
  CHECK(domainValue(g, 0) == 0.0f && domainValue(g, 255) == 1.0f);
  InputDecl f{"f", 1.0, 2.0, 0};  // every float in [1, 2]: 2^23 + 1 values
  CHECK(domainCount(f) == (1u << 23) + 1);
  CHECK(domainValue(f, 1) == std::nextafter(1.0f, 2.0f));
  Program p = parseProgram("input c : float3 in [0, 1] grid 255\noutput r = c.x\nbudget r : color8\n");
  CHECK(domainSize(p, 1ull << 24) == 256ull * 256 * 256);
  CHECK(domainSize(p, 1ull << 20) == 0);  // too big for the cap
}

TEST(v2_exhaustive_checks_every_point) {
  const Program p = parseProgram(
      "input a : float in [0, 1] grid 255\ninput b : float in [0, 1] grid 255\n"
      "output r = a * b\nbudget r : exact\n");
  const Expr good = parseExpr("b * a", p.inputs);
  CHECK(compareExhaustive(p, good, kProfileRef).pass);
  CHECK(compareExhaustive(p, good, kProfileRef).checked == 65536);
  const Expr off = parseExpr("a == 1.0 ? 0.0 : a * b", p.inputs);  // wrong only where a = 1
  const Metrics m = compareExhaustive(p, off, kProfileRef);
  CHECK(!m.pass && m.failPoint[0] == 1.0f);
}

TEST(vector_rewrite_normalize_length) {
  const Program prog = loadProgram(std::string(SOPT_EXAMPLES_DIR) + "/normalize_length.sopt");
  Options opt;
  opt.v1Points = 1u << 16;
  opt.search.maxBank = 200'000;
  opt.search.overflow = false;
  const RunResult r = optimize(prog, opt);
  CHECK(!r.accepted.empty());
  if (!r.accepted.empty()) CHECK(r.accepted[0].text == "v" && r.accepted[0].cost == 0);
}

TEST(vector_emitters) {
  const auto in = vin();
  const Expr e = parseExpr("normalize(v) * s + c.x", in);
  const std::string fx = emitEffect(e, in);
  CHECK(has(fx, "float3 sopt_region(float3 v, float3 c, float s)"));
  CHECK(has(fx, "float3(in0.x, in0.y, in0.z)"));  // v: slots 0..2
  CHECK(has(fx, "float3(in0.w, in1.x, in1.y)"));  // c: slots 3..5
  CHECK(has(fx, "return float4(r, 0.0);"));
  const std::string ptx = emitPtx(e, in, 89);
  CHECK(has(ptx, "rsqrt.approx.ftz.f32"));
  CHECK(has(ptx, "[%a1+24]"));  // s is slot 6
}
