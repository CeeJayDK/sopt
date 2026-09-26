#pragma once
#include <string>
#include <vector>

#include "ir/expr.hpp"

namespace sopt {

// Backend normalization (M4): original and candidates are emitted as the same tiny
// ReShade FX effect (emitEffect) and compiled the way ReShade compiles them; a candidate
// whose optimized code is identical to the original's is something the compiler already
// does on that backend.
//   SPIR-V (Vulkan, OpenGL via SPIR-V): fxstat's optimized SPIR-V (spirv-opt passes).
//   DXBC (DX9-DX11): ReShade's HLSL compiled by Microsoft's D3DCompile at -O3 (sopt-fxc,
//   tools/fxc; under Wine with Microsoft's d3dcompiler_47.dll off Windows).
struct BackendConfig {
  std::string fxstat;     // path to fxstat (ReShade Testing Initiative)
  std::string spirvDis;   // path to spirv-dis (SPIRV-Tools), for the identity check
  std::string fxc;        // path to sopt-fxc.exe; empty = no DXBC
  std::string wine;       // prefix for running sopt-fxc off Windows (e.g. "wine"); empty = direct
  std::string keepDir;    // keep the generated files here (empty = temp)
  unsigned threads = 0;
};

struct BackendCode {
  std::string error;       // first failure, if any
  int spirv = -1;          // SPIR-V arithmetic instructions of the pixel shader (fxstat "alu"), -1 = n/a
  int dxbc = -1;           // DXBC instructions after fxc -O3 (not dcl_ / ret), -1 = n/a
  std::string spirvCode;   // canonical optimized SPIR-V of the pixel shader (ids renumbered)
  std::string dxbcCode;    // fxc disassembly of the pixel shader (instructions only)
};

// Compiles each expression (index 0 is usually the original).
std::vector<BackendCode> measureBackends(const std::vector<const Expr*>& exprs,
                                         const std::vector<InputDecl>& inputs, const BackendConfig& cfg);

// fxc disassembly -> the instruction lines (no comments, declarations or ret).
std::string dxbcInstructions(const std::string& disassembly, int& count);
// spirv-dis text -> ids renumbered in order of first use, debug names dropped.
std::string canonicalSpirv(const std::string& text);

}  // namespace sopt
