#pragma once
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ir/expr.hpp"
#include "verify/points.hpp"

namespace sopt {

// Library of verified rewrites (owner, 2026-09-29: readable by the program, humans and
// AI; library/rewrites.txt). One rule per line:
//   pattern -> replacement   where cond, cond, ...   # comment
// Names in the pattern are variables standing for any subexpression (the same name twice
// is the same subexpression); numbers match exactly. Conditions: `x const` (a constant or
// compile-time constant), `x in [lo, hi]`, `x >= c` (also >, <=, <, !=) on the values of
// x (checked on samples; every rewritten form is verified like any candidate), and
// `x : float3` (x's type, for the rule check; default float). Rules are directional.
// pos(x) in the replacement (owner, 2026-10-11): x made >= 0 by what is known of its sign where the rule matches: x
// itself where it is >= 0, -x where it is <= 0, abs(x) otherwise (one rule for every sign combination, e.g. the
// zero-comparison trick `pos(a) != -pos(b)`). The rule check tries every combination.
struct RuleCond {
  enum class Kind { Const, Range, NotEq } kind = Kind::Range;
  uint32_t var = 0;
  double lo = -INFINITY, hi = INFINITY;  // Range; NotEq: lo is the excluded value
  bool loOpen = false, hiOpen = false;
};

struct RewriteRule {
  std::string text;  // the rule as written (without the comment)
  std::string source;  // file:line
  std::vector<InputDecl> vars;  // domains from the conditions (default [-100, 100])
  Expr lhs, rhs;  // pos(x) is parsed as abs(x) (see pos)
  std::vector<RuleCond> conds;
  std::vector<bool> pos;  // by variable: written pos(x) in the replacement
  std::string rhsText;    // the replacement as written, where it uses pos (abs would misprint it)
};

struct Library {
  std::string path;  // where it was loaded from ("built-in" for the embedded copy)
  std::vector<RewriteRule> rules;
  // Rule indices by the pattern's root op: a node is only matched against the rules for its
  // op (parseLibrary fills it).
  std::array<std::vector<uint32_t>, static_cast<size_t>(Op::Count)> byRoot;
};

// Throws ParseError naming the line.
Library parseLibrary(std::string_view text, const std::string& name);
Library loadLibrary(const std::string& path);
// The library used by --library without a file: $SOPT_LIBRARY, else library/rewrites.txt
// next to the executable (or one or two directories up), else the copy built in.
const Library& defaultLibrary();

// One rule checked on its own: the replacement against the pattern over the variables'
// domains, budget rel 1e-6 with the accuracy rule, every semantic profile.
struct RuleCheck {
  bool pass = false;
  double maxRel = 0.0;
  std::string note;  // why it failed (a point, or a parse/type problem)
};
RuleCheck checkRule(const RewriteRule& r, size_t points = 1u << 16, uint64_t seed = 1);

// Forms of prog.target reachable by up to maxSteps rule applications (constants folded,
// trivial identities removed), cheapest first, without the target itself. `samples`
// (points of prog's domain) decide the value conditions.
struct LibraryForm {
  Expr expr;
  uint32_t cost = 0;
  std::vector<uint32_t> rules;  // indices of the rules applied, in order
};
// The replacement with each pos(x) resolved: sign[x] > 0 -> x, < 0 -> -x, 0 (or no entry) -> abs(x).
Expr resolvePos(const RewriteRule& r, const std::vector<int8_t>& sign);
std::vector<LibraryForm> libraryRewrites(const Program& prog, const Library& lib, const PointSet& samples,
                                         const CostModel& model, unsigned maxSteps = 4, size_t maxForms = 256,
                                         bool regroup = false);

// Folder of the running executable (empty if unknown); tools shipped next to it are found there.
std::string executableDir();

}  // namespace sopt
