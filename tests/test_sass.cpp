#include <cstdlib>
#include <string>

#include "ir/parser.hpp"
#include "measure/sass.hpp"
#include "test.hpp"

using namespace sopt;

namespace {

std::vector<InputDecl> ab() { return {{"a", 0.0, 1.0, 0}, {"b", 0.0, 1.0, 0}}; }
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

}  // namespace

TEST(sass_emit_ptx) {
  const auto in = ab();
  const std::string ptx = emitPtx(parseExpr("mad(rcp(a + 0.5), b, 2.0) / b", in), in, 89);
  CHECK(has(ptx, ".target sm_89"));
  CHECK(has(ptx, "[%a1+4];"));  // b is the second input
  CHECK(has(ptx, "rcp.approx.ftz.f32"));
  CHECK(has(ptx, "fma.rn.f32"));
  CHECK(has(ptx, "0f3F000000"));  // 0.5 as an exact hex immediate
  CHECK(has(ptx, "st.global.f32 [%a2]"));
}

TEST(sass_parse) {
  const std::string disasm =
      "        /*0000*/                   MOV R1, c[0x0][0x28] ;\n"
      "        /*0010*/                   LDG.E R2, desc[UR4][R2.64] ;\n"
      "        /*0020*/                   MOV R5, 0x4479c000 ;\n"
      "        /*0030*/                   FADD R0, -R2, 1 ;\n"
      "        /*0040*/                   FFMA R6, R0, -R5, 1000 ;\n"
      "        /*0050*/                   MUFU.RCP R7, R6 ;\n"
      "        /*0060*/               @P0 FMUL R7, R0, R7 ;\n"
      "        /*0070*/                   STG.E desc[UR4][R4.64], R7 ;\n"
      "        /*0080*/                   EXIT ;\n";
  const SassCost c = parseSass(disasm, 89);
  CHECK(c.ok);
  CHECK(c.alu == 3 && c.mov == 1 && c.mufu == 1);
  CHECK(c.cost == 3 + 1 + mufuWeight(89));
}

// Runs only when the tools are available (SOPT_PTXAS and SOPT_NVDISASM).
TEST(sass_measure_with_ptxas) {
  const char* ptxas = std::getenv("SOPT_PTXAS");
  const char* nvdisasm = std::getenv("SOPT_NVDISASM");
  if (!ptxas || !nvdisasm) {
    std::printf("  skipped: set SOPT_PTXAS and SOPT_NVDISASM\n");
    return;
  }
  SassConfig cfg;
  cfg.ptxas = ptxas;
  cfg.nvdisasm = nvdisasm;
  const auto in = ab();
  const Expr fma = parseExpr("a * b + a", in);  // contracted to one FFMA
  const Expr rcp = parseExpr("rcp(a)", in);
  const Expr id = parseExpr("a", in);  // scaffolding alone: nothing counted
  const auto r = measureSass({&fma, &rcp, &id}, in, cfg);
  CHECK(r[0].ok && r[1].ok && r[2].ok);
  CHECK(r[0].alu == 1 && r[0].mufu == 0);
  CHECK(r[1].mufu == 1);
  CHECK(r[2].alu == 0 && r[2].mufu == 0 && r[2].mov == 0);
}
