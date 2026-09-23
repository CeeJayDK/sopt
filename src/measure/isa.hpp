#pragma once
#include <string>
#include <vector>

#include "ir/expr.hpp"

namespace sopt {

// Real GPU cost of a candidate: it is emitted as a tiny ReShade FX effect, compiled
// by fxstat (ReShade Testing Initiative) and AMD's Radeon GPU Analyzer, and the
// pixel shader's ISA statistics are read back.
struct IsaConfig {
  std::string fxstat;   // path to fxstat
  std::string rga;      // path to rga
  std::string asic;     // empty = fxstat's default (gfx1100, RDNA3)
  std::string keepDir;  // write the generated .fx files here and keep them (empty = temp)
  unsigned threads = 0; // 0 = hardware concurrency
};

struct IsaCost {
  bool ok = false;
  std::string error;
  int valu = 0;   // includes transcendentals
  int trans = 0;
  int salu = 0;
  int vmem = 0;
  int vgprs = 0;
  int cost = 0;   // fxstat COST = VALU + 3 * TRANS
};

// FX effect whose pixel shader reads the inputs from a texture (so the compiler
// cannot constant-fold them) and returns the expression. The expression is a
// function sopt_region(<inputs>), ready to paste into a shader.
std::string emitEffect(const Expr& e, const std::vector<InputDecl>& inputs);

// Measures each expression. Runs fxstat processes in parallel.
std::vector<IsaCost> measureIsa(const std::vector<const Expr*>& exprs,
                                const std::vector<InputDecl>& inputs, const IsaConfig& cfg);

// Reads the pixel-stage "isa" object from fxstat --json output.
IsaCost parseFxstatJson(const std::string& json);

}  // namespace sopt
