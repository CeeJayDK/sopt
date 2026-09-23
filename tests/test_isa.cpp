#include <cstdlib>
#include <string>

#include "ir/parser.hpp"
#include "measure/isa.hpp"
#include "test.hpp"

using namespace sopt;

namespace {

std::vector<InputDecl> inputs(int n) {
  std::vector<InputDecl> v;
  const char* names[] = {"a", "b", "c", "d", "e"};
  for (int i = 0; i < n; ++i) v.push_back({names[i], 0.0, 1.0, 0});
  return v;
}

bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

}  // namespace

TEST(isa_emit_effect) {
  const auto in = inputs(5);  // five inputs need two textures
  const std::string fx = emitEffect(parseExpr("mad(a, b, c) - d * e", in), in);
  CHECK(has(fx, "float sopt_region(float a, float b, float c, float d, float e)"));
  CHECK(has(fx, "return mad(a, b, c) - d * e;"));
  CHECK(has(fx, "tex2Dlod(SoptSamp1, float4(texcoord, 0.0, 0.0))"));
  CHECK(has(fx, "sopt_region(in0.x, in0.y, in0.z, in0.w, in1.x)"));
  CHECK(has(fx, "PixelShader = SoptPS;"));
}

TEST(isa_parse_fxstat_json) {
  const std::string json =
      "{\n  \"entry_points\": [\n    {\n      \"name\": \"E__SoptVS\",\n"
      "      \"stage\": \"vertex\",\n"
      "      \"isa\": { \"valu\": 14, \"trans\": 0, \"vgprs\": 9, \"cost\": 14 }\n    },\n"
      "    {\n      \"name\": \"E__SoptPS\",\n      \"stage\": \"pixel\",\n"
      "      \"isa\": { \"valu\": 4, \"trans\": 1, \"salu\": 8, \"vmem\": 1, \"vgprs\": 5, "
      "\"cost\": 7 }\n    }\n  ]\n}\n";
  const IsaCost c = parseFxstatJson(json);
  CHECK(c.ok);
  CHECK(c.valu == 4 && c.trans == 1 && c.salu == 8 && c.vmem == 1 && c.vgprs == 5 && c.cost == 7);
  const IsaCost bad = parseFxstatJson("t.fx(3, 1): error: syntax error\n");
  CHECK(!bad.ok && has(bad.error, "syntax error"));
}

// Runs only when the tools are available (SOPT_FXSTAT and SOPT_RGA).
TEST(isa_measure_with_rga) {
  const char* fxstat = std::getenv("SOPT_FXSTAT");
  const char* rga = std::getenv("SOPT_RGA");
  if (!fxstat || !rga) {
    std::printf("  skipped: set SOPT_FXSTAT and SOPT_RGA\n");
    return;
  }
  IsaConfig cfg;
  cfg.fxstat = fxstat;
  cfg.rga = rga;
  const auto in = inputs(2);
  const Expr mul = parseExpr("a * b", in);
  const Expr fma = parseExpr("a * b + a", in);  // contracted: still one VALU op
  const Expr rcp = parseExpr("rcp(a)", in);
  const Expr id = parseExpr("a", in);  // scaffolding alone costs no VALU
  auto r = measureIsa({&mul, &fma, &rcp, &id}, in, cfg);
  CHECK(r[0].ok && r[1].ok && r[2].ok && r[3].ok);
  CHECK(r[3].valu == 0);
  CHECK(r[0].valu == 1 && r[0].trans == 0);
  CHECK(r[1].valu == 1);
  CHECK(r[2].trans == 1 && r[2].cost == 4);
  auto in2 = in;
  in2[0].name = "float";  // not a valid parameter name: compile error is reported
  const auto e = measureIsa({&id}, in2, cfg);
  CHECK(!e[0].ok && !e[0].error.empty());
}
