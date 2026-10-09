#include "fx/platforms.hpp"

#include <sstream>

namespace sopt {
extern const char* const kSteamGpuShare;  // data/steam-gpu-share.txt, built in (cmake/embed_library.cmake)
}

namespace sopt::fx {

namespace {

struct Data {
  std::vector<Platform> list;
  std::string survey;
};

const Data& data() {
  static const Data d = [] {
    Data r;
    r.list = {
        {"amd-gcn5", "GCN", "AMD"},           {"amd-rdna2", "RDNA 2", "AMD"},
        {"rdna3", "RDNA 3", "AMD"},           {"amd-rdna4", "RDNA 4", "AMD"},
        {"nvidia-maxwell", "Maxwell", "NVIDIA"}, {"nvidia-pascal", "Pascal", "NVIDIA"},
        {"nvidia-turing", "Turing", "NVIDIA"}, {"nvidia-ampere", "Ampere/Ada", "NVIDIA"},
        {"nvidia-blackwell", "Blackwell", "NVIDIA"}, {"intel-gen7.5", "Gen7.5", "Intel"},
        {"intel-gen9", "Gen9", "Intel"},      {"intel-gen12", "Gen12", "Intel"},
    };
    for (auto& p : r.list) p.m = costModelByName(p.model);
    std::istringstream in(kSteamGpuShare);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("# survey: ", 0) == 0) {
        r.survey = line.substr(10);
        if (r.survey == "none" || r.survey == "unknown") r.survey.clear();
        continue;
      }
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ls(line);
      std::string name;
      double pct = 0;
      if (!(ls >> name >> pct)) continue;
      for (auto& p : r.list)
        if (name == p.model) p.share = pct;
    }
    return r;
  }();
  return d;
}

}  // namespace

const std::vector<Platform>& platforms() { return data().list; }

const std::string& shareSurvey() { return data().survey; }

std::vector<const CostModel*> allPlatformModels() {
  std::vector<const CostModel*> r;
  for (const char* n : {"rdna3", "nvidia-turing", "nvidia-ampere", "intel-gen9"}) r.push_back(costModelByName(n));
  return r;
}

double worldChange(const std::vector<int>& costs, const std::vector<int>& target) {
  const auto& ps = platforms();
  double sum = 0, weight = 0;
  for (size_t k = 0; k < ps.size() && k < costs.size() && k < target.size(); ++k) {
    if (ps[k].share <= 0 || target[k] <= 0) continue;
    sum += ps[k].share * 100.0 * (costs[k] - target[k]) / target[k];
    weight += ps[k].share;
  }
  return weight > 0 ? sum / weight : 0.0;
}

}  // namespace sopt::fx
