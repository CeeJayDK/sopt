#include "search/driver.hpp"

#include "search/cuts.hpp"
#include "search/library.hpp"
#include "search/subtrees.hpp"
#include "verify/bound.hpp"
#include "verify/exact.hpp"

#include <algorithm>
#include <cmath>
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

// Whether the float32 original follows exact math: its max error against the exact
// values stays within 10% of its own range over random points. Hash noise such as
// frac(sin(dot(uv, k)) * 43758.5) does not (float32 sin of large arguments is chaotic):
// there the original's error vs exact is as large as its range, so "at least as close to
// exact" or "within F times the original's error" would accept any value, even 0.
bool followsExact(const Program& prog, uint64_t seed) {
  const PointSet ps = makeRandomPoints(prog, 4096, seed + 7, false);
  const std::vector<float> t = evalAll(prog.target, ps, kProfileRef);
  const std::vector<double> x = evalExactAll(prog.target, ps);
  double lo = INFINITY, hi = -INFINITY, err = 0.0;
  for (size_t i = 0; i < t.size() && i < x.size(); ++i) {
    if (!std::isfinite(t[i]) || !std::isfinite(x[i])) continue;
    lo = std::min(lo, double(t[i]));
    hi = std::max(hi, double(t[i]));
    err = std::max(err, std::fabs(double(t[i]) - x[i]));
  }
  return !(hi > lo) || err <= 0.1 * (hi - lo);
}

// Final pass over the accepted candidates: accuracy variants, problem inputs.
void finish(const Program& prog, const Options& opt, RunResult& res) {
  const Metrics& t = res.targetExact;
  const bool rule = accuracyRule(prog.budget) && opt.exactRule && t.exactRel > 0.0;
  std::vector<Accepted> kept;
  for (auto& a : res.accepted) {
    a.moreAccurate = opt.accuracyVariants && rule && a.klass != Klass::LessAccurate &&
                     4.0 * a.worst.exactRel <= t.exactRel && a.worst.exactAbs <= t.exactAbs;
    if (a.cost >= res.targetCost && !a.moreAccurate) continue;  // neither cheaper nor more accurate
    kept.push_back(std::move(a));
  }
  res.accepted = std::move(kept);
  for (auto& a : res.accepted) a.problems = findProblemRanges(prog, a.expr, a.klass == Klass::LessAccurate);
  // V3: a formal bound where V2 did not cover the whole domain.
  if (opt.v3 && prog.budget.kind != Budget::Kind::Exact) {
    const double t0 = nowSeconds();
    BoundOptions bo;
    bo.seconds = opt.v3Time;
    bo.maxBoxes = opt.v3MaxBoxes;
    uint32_t n = 0;
    for (auto& a : res.accepted) {
      if (n >= opt.v3Candidates) break;
      if (a.exhaustive || a.klass == Klass::LessAccurate) continue;
      ++n;
      const BoundResult b = proveBound(prog, a.expr, bo);
      a.proven = b.proven;
      a.provenFraction = b.fraction;
      a.proofBound = b.maxBound;
    }
    res.v3Sec += nowSeconds() - t0;
  }
}

}  // namespace

RunResult optimize(const Program& progIn, const Options& opt) {
  if ((!opt.exactRule && progIn.budget.vsExact) || progIn.budget.loose != opt.loose) {
    Program p = progIn;
    p.budget.vsExact = p.budget.vsExact && opt.exactRule;
    p.budget.loose = opt.loose;
    return optimize(p, opt);
  }
  if ((accuracyRule(progIn.budget) || progIn.budget.scaledRel()) && !followsExact(progIn, opt.seed)) {
    Program p = progIn;
    p.budget.vsExact = false;      // the float32 original is the only meaningful reference
    p.budget.errorScale = false;
    RunResult r = optimize(p, opt);
    r.exactOff = true;
    return r;
  }
  const Program& prog = progIn;
  if (opt.specialize)
    for (const auto& d : prog.inputs)
      if (d.compileTime) {
        RunResult r = optimizeSpecialized(prog, opt);
        finish(prog, opt, r);
        return r;
      }
  const double t0 = nowSeconds();
  RunResult res;
  res.targetCost = dagCost(prog.target, *opt.search.model, prog.inputs);
  res.targetText = toString(prog.target, prog.inputs);
  if (res.targetCost == 0) return res;

  PointSet tests = makeTestPoints(prog, opt.numTests, opt.seed);
  const PointSet stage2 = makeRandomPoints(prog, opt.stage2Points, opt.seed + 1, true);
  const PointSet v1 = makeRandomPoints(prog, opt.v1Points, opt.seed + 2, true);
  const std::vector<float> stage2Target = evalAll(prog.target, stage2, kProfileRef);
  std::vector<std::vector<float>> v1Target;
  for (const auto& prof : kAllProfiles) v1Target.push_back(evalAll(prog.target, v1, prof));
  const bool rule = accuracyRule(prog.budget);
  // Exact values (accuracy rule) and the target's error scales (Rel budgets) in one pass.
  const bool rel = prog.budget.scaledRel();
  std::vector<double> stage2Scale, v1Scale;
  const std::vector<double> stage2Exact =
      rule || rel ? evalExactAll(prog.target, stage2, rel ? &stage2Scale : nullptr) : std::vector<double>();
  const std::vector<double> v1Exact = rule || rel ? evalExactAll(prog.target, v1, rel ? &v1Scale : nullptr) : std::vector<double>();
  const std::vector<double>* s2x = rule ? &stage2Exact : nullptr;
  const std::vector<double>* v1x = rule ? &v1Exact : nullptr;
  const std::vector<double>* s2s = rel ? &stage2Scale : nullptr;
  const std::vector<double>* v1s = rel ? &v1Scale : nullptr;
  if (rule) {
    // The original's own error against the exact values (reported next to candidates').
    for (size_t p = 0; p < kAllProfiles.size(); ++p)
      res.targetExact.merge(compare(prog, prog.target, v1, kAllProfiles[p], opt.threads, &v1Target[p], v1x, v1s));
  }

  SearchConfig cfg = opt.search;
  cfg.rational = cfg.rational && opt.accuracyVariants;
  res.v2Points = opt.v2Max ? domainSize(prog, opt.v2Max) : 0;

  // Library (Options::library): rewritten forms of the target are candidates; the
  // cheapest one that passes stage 2 seeds the search (bound, shared leaves) and the
  // subtree / cut searches (its structure).
  std::vector<Candidate> libCands;
  Program seedProg;
  bool haveSeed = false;
  if (opt.library) {
    const Library& lib = opt.libraryRules ? *opt.libraryRules : defaultLibrary();
    std::vector<LibraryForm> forms =
        libraryRewrites(prog, lib, stage2, *opt.search.model, opt.librarySteps, opt.libraryForms);
    res.libraryForms = static_cast<uint32_t>(forms.size());
    for (auto& f : forms) {
      if (f.cost > res.targetCost) break;
      if (!haveSeed && compare(prog, f.expr, stage2, kProfileRef, 1, &stage2Target, s2x, s2s).pass) {
        haveSeed = true;
        res.libraryBest = f.cost;
        cfg.seeds.push_back(f.expr);
        if (f.cost < res.targetCost) cfg.seedBound = f.cost;
        seedProg = prog;
        seedProg.target = f.expr;
      }
      if (f.cost < res.targetCost) libCands.push_back({std::move(f.expr), f.cost});
    }
  }

  std::vector<Candidate> subCands;  // Options::subtrees, computed once
  bool subDone = false;
  std::vector<Candidate> cutCands;  // Options::cuts, computed once
  bool cutDone = false;
  for (uint32_t iter = 0; iter < opt.maxIterations; ++iter) {
    res.iterations = iter + 1;
    const bool lastIter = iter + 1 == opt.maxIterations;

    double ts = nowSeconds();
    // The time limit covers all CEGIS iterations (the overflow search runs to it); a
    // restart after counterexamples gets what is left, at least a tenth.
    cfg.timeLimitSec = std::max(opt.search.timeLimitSec * 0.1, opt.search.timeLimitSec - res.searchSec);
    std::vector<Candidate> cands;
    {
      // The bank is freed before the subtree / cut searches, which build their own.
      Enumerator en(prog, tests, cfg);
      cands = en.run(res.search);
    }
    res.searchSec += nowSeconds() - ts;
    if (opt.subtrees && !subDone && res.search.limitHit) {
      subDone = true;
      const double tsub = nowSeconds();
      subCands = subtreeCandidates(prog, opt, res.targetCost, &res.subtreeSearches);
      if (haveSeed) {
        std::vector<Candidate> more = subtreeCandidates(seedProg, opt, res.targetCost, &res.subtreeSearches);
        subCands.insert(subCands.end(), more.begin(), more.end());
      }
      res.subtreeSec += nowSeconds() - tsub;
    }
    if (opt.cuts && !cutDone && res.search.limitHit) {
      cutDone = true;
      const double tcut = nowSeconds();
      cutCands = cutCandidates(prog, opt, res.targetCost, &res.cutSearches);
      if (haveSeed) {
        std::vector<Candidate> more = cutCandidates(seedProg, opt, res.targetCost, &res.cutSearches);
        cutCands.insert(cutCands.end(), more.begin(), more.end());
      }
      res.cutSec += nowSeconds() - tcut;
    }
    cands.insert(cands.end(), cutCands.begin(), cutCands.end());
    cands.insert(cands.end(), subCands.begin(), subCands.end());
    cands.insert(cands.end(), libCands.begin(), libCands.end());
    for (auto& c : cands) {
      c.expr = simplifyIdentities(c.expr);
      c.cost = dagCost(c.expr, *opt.search.model, prog.inputs);
    }

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
      const Metrics m = compare(prog, c.expr, stage2, kProfileRef, 1, &stage2Target, s2x, s2s);
      if (!m.loosePass) {
        ++res.rejectedStage2;
        addCex(m.looseFailPoint);
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
    uint32_t numStrict = 0, numLoose = 0;
    for (uint64_t h : groupOrder) {
      if (numStrict >= opt.maxAlternatives) break;
      for (size_t idx : groups[h]) {
        auto& c = survivors[idx].cand;
        Metrics worst;
        bool refFailed = false;
        for (size_t p = 0; p < kAllProfiles.size(); ++p) {
          const Metrics m = compare(prog, c.expr, v1, kAllProfiles[p], opt.threads, &v1Target[p], v1x, v1s);
          worst.merge(m);
          if (!m.loosePass) {
            refFailed = p == 0;
            break;
          }
        }
        if (!worst.loosePass) {
          if (refFailed) {
            ++res.rejectedV1;
            addCex(worst.looseFailPoint);
          } else {
            ++res.rejectedProfiles;
          }
          continue;
        }
        // V2: every point of a small domain, for the cheapest few.
        bool exhaustive = false;
        if (res.v2Points && res.accepted.size() < opt.v2Candidates) {
          Metrics all;
          for (const auto& prof : kAllProfiles) {
            all.merge(compareExhaustive(prog, c.expr, prof, opt.threads));
            if (!all.loosePass) break;
          }
          if (!all.loosePass) {
            ++res.rejectedV2;
            addCex(all.looseFailPoint);
            continue;
          }
          worst = all;
          exhaustive = true;
        }
        Accepted a;
        a.exhaustive = exhaustive;
        a.text = toString(c.expr, prog.inputs);
        a.cost = c.cost;
        a.klass = classify(prog, c.expr, worst);
        a.worst = worst;
        a.expr = std::move(c.expr);
        if (a.klass == Klass::LessAccurate ? numLoose++ < opt.maxLoose : (++numStrict, true))
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
  finish(prog, opt, res);
  res.totalSec = nowSeconds() - t0;
  return res;
}

}  // namespace sopt
