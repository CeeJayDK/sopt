#include "search/options.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "search/library.hpp"

namespace sopt {

namespace {
const CostModel* modelArg(const char* name) {
  const CostModel* m = costModelByName(name);
  if (!m)
    std::fprintf(stderr, "unknown cost model (rdna3, amd-rdna2, amd-rdna4, amd-gcn5, amd-terascale2, nvidia, nvidia-maxwell, "
                         "nvidia-pascal, nvidia-turing, nvidia-ampere, nvidia-blackwell, intel-gen12, intel-gen9, "
                         "intel-gen7.5, generic, search)\n");
  return m;
}
}  // namespace

int parseSearchOption(const std::string& a, const std::function<const char*()>& next, Options& opt, SearchArgs& args) {
  if (a == "--time") opt.search.timeLimitSec = std::strtod(next(), nullptr);
  else if (a == "--max-bank") opt.search.maxBank = std::strtoull(next(), nullptr, 10);
  else if (a == "--v1") opt.v1Points = std::strtoull(next(), nullptr, 10);
  else if (a == "--tests") opt.numTests = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
  else if (a == "--cost-model") {
    if (!(opt.search.model = modelArg(next()))) return -1;
  } else if (a == "--order-model") {
    if (!(opt.search.order = modelArg(next()))) return -1;
  } else if (a == "--no-amd-folds") args.noAmdFolds = true;
  else if (a == "--no-affine") opt.search.affine = false;
  else if (a == "--no-inner") opt.search.inner = false;
  else if (a == "--no-inner-prefilter") opt.search.innerPrefilter = false;
  else if (a == "--no-affine-precheck") opt.search.affinePrecheck = false;
  else if (a == "--no-div-form") opt.search.divForm = false;
  else if (a == "--no-overflow") opt.search.overflow = false;
  else if (a == "--no-subtrees") opt.subtrees = false;
  else if (a == "--no-cuts") opt.cuts = false;
  else if (a == "--slack") opt.search.slack = std::atoi(next());
  else if (a == "--no-best-bound") opt.search.bestBound = false;
  else if (a == "--top-down") opt.search.topDown = true;
  else if (a == "--no-top-down") opt.search.topDown = false;
  else if (a == "--library") opt.library = true;
  else if (a == "--no-library") opt.library = false;
  else if (a == "--two-phase") opt.search.twoPhase = true;
  else if (a == "--no-two-phase") opt.search.twoPhase = false;
  else if (a == "--helpers") opt.search.helpers = true;
  else if (a == "--bits") opt.search.bits = true;
  else if (a == "--library-file") {
    static Library lib;  // alive for the whole run
    const char* f = next();
    {
      std::ifstream in(f, std::ios::binary);
      std::ostringstream text;
      text << in.rdbuf();
      args.libraryHash = std::hash<std::string>{}(text.str());
    }
    try {
      lib = loadLibrary(f);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s\n", e.what());
      return -1;
    }
    opt.library = true;
    opt.libraryRules = &lib;
  } else if (a == "--disk") opt.search.diskDir = next();
  else if (a == "--disk-max") opt.search.diskBudget = static_cast<size_t>(std::strtod(next(), nullptr) * 1073741824.0);
  else if (a == "--max-mem") opt.search.memBudget = static_cast<size_t>(std::strtod(next(), nullptr) * 1048576.0);
  else if (a == "--no-v3") opt.v3 = false;
  else if (a == "--no-schedule") opt.schedule = false;
  else if (a == "--perf-mode-first") opt.perfFirst = true;
  else if (a == "--v3-time") opt.v3Time = std::strtod(next(), nullptr);
  else if (a == "--quant-oe") opt.search.quantBits = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
  else if (a == "--cut-time") opt.cutTime = std::strtod(next(), nullptr);
  else if (a == "--no-shared-leaves") opt.search.sharedLeaves = false;
  else if (a == "--subtree-time") opt.subtreeTime = std::strtod(next(), nullptr);
  else if (a == "--subtree-max-cost") opt.subtreeMaxCost = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
  else if (a == "--no-exact-rule") opt.exactRule = false;
  else if (a == "--no-accuracy-variants") opt.accuracyVariants = false;
  else return 0;
  return 1;
}

}  // namespace sopt
