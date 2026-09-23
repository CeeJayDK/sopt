#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fx/codegen.hpp"
#include "ir/expr.hpp"

namespace sopt::fx {

struct LoadOptions {
  std::vector<std::filesystem::path> includePaths;
  std::vector<std::pair<std::string, std::string>> macros;  // extra definitions
  unsigned width = 1920, height = 1080;                      // BUFFER_WIDTH / BUFFER_HEIGHT
};

// One parse of an effect: the recorded dataflow graph plus the preprocessed text of
// every source line (to see which lines contain macro expansions).
struct Effect {
  std::filesystem::path path;
  std::unique_ptr<Codegen> cg;
  std::map<std::pair<std::string, uint32_t>, std::string> ppLines;  // (file, line) -> text
  std::vector<std::string> sourceFiles;  // the effect and its includes
};

// Preprocesses and parses one .fx file. Returns null and sets errors on failure.
std::unique_ptr<Effect> loadEffect(const std::filesystem::path& path, const LoadOptions& opt,
                                   std::string& errors);

// Where a region's input range comes from.
struct Fact {
  std::string input;
  std::string source;  // "ui_min/ui_max", "TEXCOORD", "BackBuffer (8-bit)", "assumed", ...
  bool assumed = false;
};

// One statement of a pixel shader whose value is pure arithmetic over variables:
//   Init:   float3 x = <rhs>;
//   Store:  x.rgb = <rhs>;   (also x *= <rhs>, then the region is x * (<rhs>))
//   Return: return <rhs>;
// The program's inputs are the variables it reads (named by their FX text, e.g.
// "color.rgb"), so a candidate printed with toString() is valid FX in place.
struct Region {
  enum class Kind { Init, Store, Return } kind = Kind::Store;
  std::string file;          // source file path as the preprocessor names it
  uint32_t line = 0;         // first line of the statement (1-based)
  uint32_t lastLine = 0;     // line with the terminating ';'
  std::string function;      // enclosing function
  std::string lhs;           // statement text before the value: "float3 x =", "x.rgb =", "return"
  std::string original;      // original statement text (joined lines)
  Program prog;
  std::vector<Fact> facts;   // one per input
  std::string budgetReason;  // how the budget was derived
};

struct RegionOptions {
  uint32_t minOps = 2;        // skip statements with fewer IR operations
  uint32_t maxInputs = 4;     // skip statements reading more distinct variables
  uint32_t maxSlots = 8;      // ... or more input components
  double defaultLo = -1000.0, defaultHi = 1000.0;  // range when nothing is known
  double relEps = 1e-6;       // budget for values with a general use
  double texcoordPx = 0.01;   // budget for values only used as texture coordinates
};

struct SkipCount {
  std::map<std::string, uint32_t> reasons;  // reason -> statements
  std::vector<std::string> details;         // "file:line: reason" when keepDetails
  bool keepDetails = false;
  void add(const std::string& r) { ++reasons[r]; }
  void add(const std::string& r, const std::string& file, uint32_t line) {
    add(r);
    if (keepDetails) details.push_back(file + ":" + std::to_string(line) + ": " + r);
  }
};

// Regions of all functions reachable from pixel shader entry points. `alt` is the same
// effect parsed at another resolution: statements whose value differs between the two
// depend on BUFFER_WIDTH/HEIGHT (through a static const) and are skipped.
std::vector<Region> extractRegions(const Effect& fx, const Effect* alt, const RegionOptions& opt,
                                   SkipCount& skipped);

// UTF-8 path strings, as the preprocessor writes them into #line directives.
std::string pathString(const std::filesystem::path& p);
std::filesystem::path pathFrom(const std::string& s);

// Source text cache (lines without terminators).
const std::vector<std::string>* sourceLines(const std::string& file);

}  // namespace sopt::fx
