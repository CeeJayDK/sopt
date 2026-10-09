#pragma once
// GPU families (owner, 2026-10-09): a variant aimed at one card should also help other cards where it does not hurt
// the chosen one. Each family is one of SweetOpt's measured cost models; its share of Steam users (Steam Hardware &
// Software Survey, data/steam-gpu-share.txt, refreshed by the workflow steam-survey.yml) weighs a variant that is
// faster on some families and slower on others in easy mode.
#include <string>
#include <vector>

#include "ir/ops.hpp"

namespace sopt::fx {

struct Platform {
  const char* model;  // cost model name
  const char* label;  // short name for tables ("RDNA 3", "Turing")
  const char* vendor;  // "AMD", "NVIDIA", "Intel"
  const CostModel* m = nullptr;
  double share = 0.0;  // percent of Steam users (0 when unknown)
};

// The families, oldest to newest per vendor (AMD, NVIDIA, Intel), with their shares.
const std::vector<Platform>& platforms();

// The survey month the shares come from ("" when the data file has none).
const std::string& shareSurvey();

// Families searched by --all-platforms besides the chosen model: one per group whose costs differ most.
std::vector<const CostModel*> allPlatformModels();

// Share-weighted relative change of a variant against the original over the families (percent of the original's
// cost, weighted by share; negative = faster for the world). Families without a share are left out; 0 when no
// family has a share. costs / target: per platform(), as from compiledCost.
double worldChange(const std::vector<int>& costs, const std::vector<int>& target);

}  // namespace sopt::fx
