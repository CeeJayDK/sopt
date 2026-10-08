#pragma once
#include <algorithm>
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
  std::string extra;               // more preprocessor condition for this edit ("&& !__X__"), else the switch alone
};

struct SourceRewrite {
  std::string file;         // source file path as the preprocessor names it
  uint32_t line = 0;        // where it is reported (e.g. the array's declaration)
  std::string function;
  std::string kind;         // "static const table", "to the vertex shader"
  std::string tag = "T";    // switch letter: SOPT_<file>_<tag><line>
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

// Source text helpers (shared by the rewrites).
namespace source {
bool identChar(char c);
std::string trim(const std::string& s);
// Skips whitespace and comments from pos; returns the next position.
size_t skipSpace(const std::string& s, size_t p);
// From an opening bracket at p: the position of its match (comments skipped), npos if none.
size_t matching(const std::string& s, size_t p);
// Source lines as one text with the offset of each line's start.
struct Text {
  std::string s;
  std::vector<size_t> starts;  // starts[i] = offset of line i + 1
  uint32_t lineOf(size_t pos) const {
    return static_cast<uint32_t>(std::upper_bound(starts.begin(), starts.end(), pos) - starts.begin());
  }
};
Text joinLines(const std::vector<std::string>& lines);
// Positions of `word` as a whole identifier outside comments in [a, b).
std::vector<size_t> findWord(const std::string& s, const std::string& word, size_t a, size_t b);
// A qualified name (A::B::c) starting at p; returns its end.
size_t nameEnd(const std::string& s, size_t p);
}  // namespace source

// The switch of a rewrite: SOPT_<file stem>_<tag><line>.
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
