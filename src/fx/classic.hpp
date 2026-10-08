#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "fx/frontend.hpp"

namespace sopt::fx {

// Classical source transforms (owner, 2026-10-08: "the regular optimizer tricks before we start
// superoptimizing"). Each one rewrites several source lines at once; the variant files switch it
// with SOPT_<file>_T<line> (0 = original, 1 = rewritten; default SOPT_ALL like the variants).
struct LineEdit {
  uint32_t first = 0, last = 0;    // 1-based source lines replaced (inclusive)
  std::vector<std::string> lines;  // their replacement (empty: removed)
};

struct SourceRewrite {
  std::string file;         // source file path as the preprocessor names it
  uint32_t line = 0;        // where it is reported (e.g. the array's declaration)
  std::string function;
  std::string kind;         // "static const table"
  std::string description;  // one line for the report and the variant comment
  // Table rewrites: entries that are not constants (a uniform, ...). All-constant local arrays are
  // made constant data by the compilers already (measured: fxc and AMD's Vulkan compiler unroll and
  // fold them); a non-constant entry keeps the whole array per pixel (Monochrome: in scratch memory).
  uint32_t otherEntries = 0;
  std::vector<LineEdit> edits;  // sorted, disjoint
  // Measured (--isa: fxstat + RGA, the pixel / compute shader of the effect it was found in, with the
  // switch off and on), without and with performance mode; -1 = not measured.
  int amdBefore = -1, amdAfter = -1, amdPerfBefore = -1, amdPerfAfter = -1;
  int scratchBefore = -1, scratchAfter = -1;  // AMD scratch (spilled) registers
  int vgprBefore = -1, vgprAfter = -1;
};

// The switch of a rewrite: SOPT_<file stem>_T<line>.
std::string rewriteSwitch(const SourceRewrite& r);

// Local arrays initialized with constants (at most two other entries, e.g. a uniform for a
// "custom" preset) and indexed at run time are built in every pixel, often in scratch memory
// (SweetFX Monochrome, RDNA 3 without performance mode: 58 VALU and spills vs 15 VALU). sopt-fx
// keeps all-constant ones only where --isa measures a gain (SourceRewrite::otherEntries). As a
// static const table the compiler keeps them in its constant data: the declaration goes away, the
// table is defined before the function, and every name[i] becomes table[i], with the other entries
// picked as (i == k ? value : table[i]). Only arrays never written after their initializer, read
// only by index, with all text on plain source lines (no macros around them).
std::vector<SourceRewrite> tableRewrites(const Effect& fx);

}  // namespace sopt::fx
