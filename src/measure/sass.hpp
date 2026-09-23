#pragma once
#include <string>
#include <vector>

#include "ir/expr.hpp"

namespace sopt {

// NVIDIA cost of a candidate: it is emitted as a PTX kernel (inputs loaded from memory,
// result stored), compiled by ptxas for one GPU generation and disassembled with
// nvdisasm; the SASS instructions of the arithmetic are counted by class. Both tools
// come with the CUDA toolkit (pip: nvidia-cuda-nvcc-cu12 for ptxas,
// nvidia-cuda-nvdisasm). This is the CUDA compiler, not the graphics driver's, which
// is expected to share its backend; check against Nsight Graphics when it matters.
struct SassConfig {
  std::string ptxas;     // path to ptxas
  std::string nvdisasm;  // path to nvdisasm
  int sm = 89;           // 75 Turing, 86 Ampere, 89 Ada, 120 Blackwell (GeForce)
  std::string keepDir;   // write the generated .ptx/.cubin files here and keep them
  unsigned threads = 0;  // 0 = hardware concurrency
};

struct SassCost {
  bool ok = false;
  std::string error;
  int alu = 0;   // FP32/int ALU instructions (FADD, FFMA, FMNMX, FSEL, FSETP, ...)
  int mov = 0;   // constant materialization (MOV of an immediate)
  int mufu = 0;  // MUFU: rcp, rsqrt, sqrt, ex2, lg2, sin, cos
  int regs = 0;  // registers per thread (from ptxas -v), 0 if unknown
  int cost = 0;  // alu + mov + mufuWeight(sm) * mufu, see below
  std::string sass;  // the counted instructions, one per line
};

// Throughput ratio FP32 : MUFU per SM from the CUDA C++ Programming Guide's arithmetic
// instruction table (per clock per SM): 128 : 16 on sm_86/89/90/120, 64 : 16 on
// sm_75/80. Provisional: taken from memory, verify against the guide.
int mufuWeight(int sm);

std::string emitPtx(const Expr& e, const std::vector<InputDecl>& inputs, int sm);

// Counts the arithmetic instructions in nvdisasm output (loads, stores, parameter
// moves and control flow are skipped).
SassCost parseSass(const std::string& disasm, int sm);

std::vector<SassCost> measureSass(const std::vector<const Expr*>& exprs,
                                  const std::vector<InputDecl>& inputs, const SassConfig& cfg);

}  // namespace sopt
