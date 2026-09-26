#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "fx/frontend.hpp"
#include "verify/verify.hpp"

namespace sopt::fx {

struct Variant {
  Expr expr;
  std::string text;  // FX expression over the region's inputs
  uint32_t cost = 0;
  Klass klass = Klass::Within;
  Metrics worst;
  bool exhaustive = false;
  int amd = -1, nv = -1;  // measured ISA cost (fxstat + RGA, ptxas + nvdisasm), -1 = not measured
  // Input values where it fails (describeProblems), e.g. "fails at F = 200 (NaN/inf at
  // some), fine on ..."; owner: kept and marked, the user decides; never picked by SOPT_AUTO.
  std::string problems;
  // Clearly more accurate than the original against exact math (accuracy variants, owner
  // 2026-09-26); accuracyOnly: not faster, kept at most one instruction slower per
  // measured vendor. Never picked by SOPT_AUTO.
  bool moreAccurate = false;
  bool accuracyOnly = false;
  // Backend normalization (--backends): instruction counts after the compilers' optimizers
  // (-1 = not measured) and whether the code is identical to the original's there.
  int spirv = -1, dxbc = -1;
  bool spirvSame = false, dxbcSame = false;
};

struct RegionResult {
  Region region;
  std::string effect;  // the .fx it was found in
  uint32_t targetCost = 0;
  std::vector<Variant> variants;  // cheapest first; less accurate ones after the others
  // Found, but some input range is assumed (not a fact): only in the report, unless
  // sopt-fx --assumed.
  std::vector<Variant> unwritten;
  bool limitHit = false;
  uint32_t onlyContraction = 0;  // cheaper only by explicit fma or free swizzles (dropped)
  int targetAmd = -1, targetNv = -1;
  int targetSpirv = -1, targetDxbc = -1;  // backend normalization of the original
  double targetExactAbs = -1;    // the original's max error vs exact math (-1: not measured)
  uint32_t measuredNoGain = 0;   // dropped: not cheaper in the measured ISA
  uint32_t completedCost = 0;
  double sec = 0.0;
};

// Cost as the GPU compiler sees it: an add/sub over a single-use mul is contracted into
// an fma anyway (free here instead of the model's fusedAdd), and swizzles and
// constructors are free (register moves the compiler removes). Variants are kept only
// if this is lower than the original's, so that explicit mad() alone is no gain.
// Nodes computed only from constants and compile-time inputs are free too.
// "color8, max code diff 0", "rel 1e-06", ...
std::string budgetString(const Budget& b);

uint32_t compiledCost(const Expr& e, const CostModel& m, const std::vector<InputDecl>& inputs = {});

// Name of the preprocessor switch of a region: SOPT_<file stem>_<line>.
std::string switchName(const Region& r);

// SOPT_AUTO: the variant (1-based) with the lowest measured cost on AMD (amd) or NVIDIA,
// if lower than the original's; less accurate variants are never picked. 0 = none.
int vendorPick(const RegionResult& rr, bool amd);

// The statement text that replaces the region's lines for one variant.
std::string variantStatement(const Region& r, const std::string& expr);

// Writes a copy of every source file that has regions with variants into outDir (by
// file name) with a switch per region:
//   #ifndef SOPT_File_12
//   #define SOPT_File_12 SOPT_ALL   // 0 = original, 1..n = variants
//   #endif
//   #if SOPT_File_12 == 1 ... #else <original> #endif
// SOPT_ALL (default 0) selects the first variant of every region at once. Returns the
// written paths.
std::vector<std::filesystem::path> writeVariants(const std::vector<RegionResult>& results,
                                                 const std::filesystem::path& outDir,
                                                 std::string& errors);

struct ReportInfo {
  std::vector<std::string> effects;              // processed .fx files
  std::vector<std::pair<std::string, std::string>> failed;  // file, errors
  SkipCount skipped;
  std::string costModel;
  double seconds = 0.0;
  size_t checks = 0, checkFailures = 0;  // re-parses of the variant effects
  bool amd = false, nv = false;           // ISA measurements ran
  bool spirv = false, dxbc = false;       // backend normalization ran
};

std::string markdownReport(const std::vector<RegionResult>& results, const ReportInfo& info);

}  // namespace sopt::fx
