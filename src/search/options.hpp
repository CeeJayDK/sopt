#pragma once
#include <cstddef>
#include <functional>
#include <string>

#include "search/driver.hpp"

namespace sopt {

// What a search option sets outside Options.
struct SearchArgs {
  bool noAmdFolds = false;  // applied after parsing (--cost-model may come later): withoutAmdFolds
  size_t libraryHash = 0;   // --library-file contents (sopt-fx cache key)
};

// The search options sopt, sopt-fx and sopt-bench share. `next` returns the option's value.
// Returns 1 when `a` was one of them, 0 when not, -1 on a bad value (message printed).
int parseSearchOption(const std::string& a, const std::function<const char*()>& next, Options& opt, SearchArgs& args);

}  // namespace sopt
