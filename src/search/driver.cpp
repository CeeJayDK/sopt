#include "search/driver.hpp"

#include <algorithm>
#include <bit>
#include <unordered_map>

namespace sopt {
namespace {

bool containsPoint(const PointSet& ps, const std::vector<float>& p) {
  for (size_t i = 0; i < ps.size(); ++i) {
    bool same = true;
    for (size_t k = 0; k < p.size() && same; ++k)
      same = std::bit_cast<uint32_t>(ps.cols[k][i]) == std::bit_cast<uint32_t>(p[k]);
    if (same) return true;
  }
  return false;
}

}  // namespace

RunResult optimize(const Program& prog, const Options& opt) {
  const double t0 = nowSeconds();
  RunResult res;
  res.targetCost = dagCost(prog.target);
  res.targetText = toString(prog.target, prog.inputs);
  if (res.targetCost == 0) return res;

  PointSet tests = makeTestPoints(prog, opt.numTests, opt.seed);
  const PointSet stage2 = makeRandomPoints(prog, opt.stage2Points, opt.seed + 1, true);
  const PointSet v1 = makeRandomPoints(prog, opt.v1Points, opt.seed + 2, true);
  const std::vector<float> stage2Target = evalAll(prog.target, stage2, kProfileRef);
  std::vector<std::vector<float>> v1Target;
  for (const auto& prof : kAllProfiles) v1Target.push_back(evalAll(prog.target, v1, prof));

  SearchConfig cfg = opt.search;
  if (cfg.maxCost == 0 || cfg.maxCost >= res.targetCost) cfg.maxCost = res.targetCost - 1;

  for (uint32_t iter = 0; iter < opt.maxIterations; ++iter) {
    res.iterations = iter + 1;
    const bool lastIter = iter + 1 == opt.maxIterations;

    double ts = nowSeconds();
    Enumerator en(prog, tests, cfg);
    std::vector<Candidate> cands = en.run(res.search);
    res.searchSec += nowSeconds() - ts;

    ts = nowSeconds();
    std::vector<std::vector<float>> cex;
    auto addCex = [&](const std::vector<float>& p) {
      if (cex.size() >= opt.maxCexPerIteration) return;
      for (const auto& q : cex)
        if (q == p) return;
      if (!containsPoint(tests, p)) cex.push_back(p);
    };

    // Stage 2: cheap CEGIS filter on a few thousand points (reference profile).
    struct Survivor {
      Candidate cand;
      uint64_t hash;
    };
    std::vector<Survivor> survivors;
    for (auto& c : cands) {
      const Metrics m = compare(prog, c.expr, stage2, kProfileRef, 1, &stage2Target);
      if (!m.pass) {
        ++res.rejectedStage2;
        addCex(m.failPoint);
      } else {
        uint64_t ops = 0;
        for (const auto& n : c.expr.nodes) ops |= uint64_t{1} << static_cast<unsigned>(n.op);
        const uint64_t key = m.valueHash ^ (ops * 0x9E3779B97F4A7C15ull);
        survivors.push_back({std::move(c), key});
      }
    }
    if (!cex.empty() && !lastIter) {
      for (const auto& p : cex) tests.add(p);
      res.counterexamples += cex.size();
      res.verifySec += nowSeconds() - ts;
      continue;
    }

    // Group survivors that use the same set of ops and are identical on the stage-2
    // points (typically operand permutations). Verify groups in cost order; within a
    // group try members until one passes V1. Stop after maxAlternatives variants so
    // trivial targets don't verify thousands of candidates.
    std::stable_sort(survivors.begin(), survivors.end(),
                     [](const Survivor& x, const Survivor& y) { return x.cand.cost < y.cand.cost; });
    std::vector<uint64_t> groupOrder;
    std::unordered_map<uint64_t, std::vector<size_t>> groups;
    for (size_t i = 0; i < survivors.size(); ++i) {
      auto& g = groups[survivors[i].hash];
      if (g.empty()) groupOrder.push_back(survivors[i].hash);
      g.push_back(i);
    }

    // V1: dense sampling under every semantic profile.
    res.accepted.clear();
    for (uint64_t h : groupOrder) {
      if (res.accepted.size() >= opt.maxAlternatives) break;
      for (size_t idx : groups[h]) {
        auto& c = survivors[idx].cand;
        Metrics worst;
        bool refFailed = false;
        for (size_t p = 0; p < kAllProfiles.size(); ++p) {
          const Metrics m = compare(prog, c.expr, v1, kAllProfiles[p], opt.threads, &v1Target[p]);
          worst.merge(m);
          if (!m.pass) {
            refFailed = p == 0;
            break;
          }
        }
        if (!worst.pass) {
          if (refFailed) {
            ++res.rejectedV1;
            addCex(worst.failPoint);
          } else {
            ++res.rejectedProfiles;
          }
          continue;
        }
        Accepted a;
        a.text = toString(c.expr, prog.inputs);
        a.cost = c.cost;
        a.klass = classify(prog, c.expr, worst);
        a.worst = worst;
        a.expr = std::move(c.expr);
        res.accepted.push_back(std::move(a));
        break;  // one verified member per stage-2 group
      }
    }
    res.verifySec += nowSeconds() - ts;
    if (!cex.empty() && !lastIter) {
      for (const auto& p : cex) tests.add(p);
      res.counterexamples += cex.size();
      continue;
    }
    break;
  }

  std::sort(res.accepted.begin(), res.accepted.end(), [](const Accepted& x, const Accepted& y) {
    if (x.cost != y.cost) return x.cost < y.cost;
    if (x.klass != y.klass) return x.klass < y.klass;
    if (x.worst.maxAbs != y.worst.maxAbs) return x.worst.maxAbs < y.worst.maxAbs;
    return x.text < y.text;
  });

  // Alternatives that produce identical values on every verification point under every
  // profile are the same variant for the user (e.g. x < 0.5 ? a : b vs x >= 0.5 ? b : a):
  // keep only the first (cheapest) of each group.
  std::vector<Accepted> grouped;
  for (auto& a : res.accepted) {
    bool dup = false;
    for (const auto& g : grouped) dup = dup || g.worst.valueHash == a.worst.valueHash;
    if (!dup) grouped.push_back(std::move(a));
  }
  res.accepted = std::move(grouped);
  res.totalSec = nowSeconds() - t0;
  return res;
}

}  // namespace sopt
