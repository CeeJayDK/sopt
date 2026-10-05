#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "ir/expr.hpp"

namespace sopt {

// Driver statistics (owner, 2026-10-04: the Intel driver's own instruction counts for sopt's variants).
// A round trip, since the drivers are on the user's PC and sopt usually is not:
//   1. sopt-fx --export-spirv DIR: each region's original and variants as the SPIR-V ReShade hands the
//      driver (fxstat --no-optimize: NAME.ps.spv + NAME.vs.spv), names in DIR/manifest.txt (name, tab, key).
//   2. ShaderInfo --batch DIR on the PC: every driver's statistics -> DIR/shaderinfo-batch-<gpu>.csv.
//   3. sopt-fx --driver-stats DIR/shaderinfo-batch-<gpu>.csv: the counts matched back by region and
//      expression text (searches are time-limited, so a rerun may find other variants).

// Writes the expressions (index 0 = the original) as NAME_k.ps.spv / .vs.spv into dir. Returns the
// shader names (empty where fxstat failed; error has the first failure).
std::vector<std::string> exportDriverShaders(const std::vector<const Expr*>& exprs,
                                             const std::vector<InputDecl>& inputs, const std::string& fxstat,
                                             const std::filesystem::path& dir, const std::string& name,
                                             std::string& error);

// One statistics file from ShaderInfo --batch.
struct DriverStats {
  unsigned vendor = 0;  // PCI vendor (0x8086 Intel, 0x10DE NVIDIA, 0x1002 AMD)
  std::string gpu;
  // shader -> executable -> statistic -> value, executables in file order per shader
  std::map<std::string, std::vector<std::pair<std::string, std::map<std::string, double>>>> shaders;
  std::map<std::string, bool> compiled;
  // manifest: "file:line\texpression" -> shader name
  std::map<std::string, std::string> byKey;
};

// Reads the statistics CSV and manifest.txt from the same folder; false (error set) when unreadable.
bool loadDriverStats(const std::filesystem::path& csv, DriverStats& out, std::string& error);

// The manifest key of a region's expression (text as sopt-fx writes it, "orig" for the original).
std::string driverKey(const std::string& file, unsigned line, const std::string& expr);

// The count used as the vendor's cost: Intel "Instruction Count" of the pixel shader executable
// (the first fragment executable: Intel compiles several SIMD widths). -1 when not available.
int driverCost(const DriverStats& stats, const std::string& shader);

}  // namespace sopt
