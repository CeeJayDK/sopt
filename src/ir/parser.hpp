#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ir/expr.hpp"

namespace sopt {

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Text format (one statement per line, '#' starts a comment):
//   input a : float in [0, 1] grid 255
//   output r = 1.0 - (1.0 - a) * (1.0 - b)
//   budget r : color8 [maxdiff 1] | exact | abs 1e-5 | rel 1e-5
Program parseProgram(std::string_view text);
Program loadProgram(const std::string& path);

// Returns the text after a "# expect:" comment line, or an empty string.
// Used by tests and the benchmark to state a known cheaper rewrite.
std::string readExpect(const std::string& path);

// Parse an expression over the given inputs (used by tests and the benchmark).
Expr parseExpr(std::string_view text, const std::vector<InputDecl>& inputs);

}  // namespace sopt
