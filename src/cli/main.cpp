#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <algorithm>
#include <numeric>

#include "ir/parser.hpp"
#include "measure/isa.hpp"
#include "search/driver.hpp"

using namespace sopt;

namespace {

void usage() {
  std::puts(
      "usage: sopt <file.sopt> [options]\n"
      "  --top N           show at most N alternatives (default 20)\n"
      "  --max-cost C      search up to cost C (default: target cost - 1)\n"
      "  --tests N         fingerprint test points (default 32)\n"
      "  --v1 N            verification sample points (default 1048576)\n"
      "  --max-bank N      bank entry limit (default 2000000)\n"
      "  --time S          search time limit per iteration in seconds (default 60)\n"
      "  --threads N       verification threads (default: all)\n"
      "  --seed N          random seed (default 1)\n"
      "  --no-affine       enumerate outer constants instead of solving p * v + q\n"
      "  --no-inner        don't solve inner constants (p * u(v + c) + q, u = rcp/sqrt/rsqrt)\n"
      "  --helpers         also enumerate pure helper intrinsics (lerp, step)\n"
      "  --cost-model M    objective: generic | rdna3 (default: generic)\n"
      "  --order-model M   enumeration order, e.g. generic with --cost-model rdna3\n"
      "  --stats           print search statistics\n"
      "  --isa             rank the shown alternatives by real GPU ISA cost (fxstat + RGA)\n"
      "  --fxstat PATH     fxstat from ReShade Testing Initiative (default: $SOPT_FXSTAT)\n"
      "  --rga PATH        AMD Radeon GPU Analyzer (default: $SOPT_RGA)\n"
      "  --asic NAME       RGA target (default: fxstat's, gfx1100 = RDNA3)\n"
      "  --isa-keep DIR    keep the generated .fx files in DIR");
}

const char* budgetText(const Budget& b, char* buf, size_t n) {
  switch (b.kind) {
    case Budget::Kind::Exact: return "exact";
    case Budget::Kind::Color8:
      std::snprintf(buf, n, "color8, max code diff %d", b.maxCodeDiff);
      return buf;
    case Budget::Kind::Abs: std::snprintf(buf, n, "abs %g", b.eps); return buf;
    case Budget::Kind::Rel: std::snprintf(buf, n, "rel %g", b.eps); return buf;
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  std::string path;
  Options opt;
  size_t top = 20;
  bool stats = false;
  bool isa = false;
  IsaConfig isaCfg;
  if (const char* v = std::getenv("SOPT_FXSTAT")) isaCfg.fxstat = v;
  if (const char* v = std::getenv("SOPT_RGA")) isaCfg.rga = v;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--top") top = std::strtoull(next(), nullptr, 10);
    else if (a == "--max-cost") opt.search.maxCost = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--tests") opt.numTests = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--v1") opt.v1Points = std::strtoull(next(), nullptr, 10);
    else if (a == "--max-bank") opt.search.maxBank = std::strtoull(next(), nullptr, 10);
    else if (a == "--time") opt.search.timeLimitSec = std::strtod(next(), nullptr);
    else if (a == "--threads") opt.threads = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    else if (a == "--seed") opt.seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--cost-model") {
      opt.search.model = costModelByName(next());
      if (!opt.search.model) { std::fprintf(stderr, "unknown cost model (generic, rdna3)\n"); return 2; }
    }
    else if (a == "--stats") stats = true;
    else if (a == "--no-affine") opt.search.affine = false;
    else if (a == "--no-inner") opt.search.inner = false;
    else if (a == "--helpers") opt.search.helpers = true;
    else if (a == "--order-model") {
      opt.search.order = costModelByName(next());
      if (!opt.search.order) { std::fprintf(stderr, "unknown cost model (generic, rdna3)\n"); return 2; }
    }
    else if (a == "--isa") isa = true;
    else if (a == "--fxstat") isaCfg.fxstat = next();
    else if (a == "--rga") isaCfg.rga = next();
    else if (a == "--asic") isaCfg.asic = next();
    else if (a == "--isa-keep") isaCfg.keepDir = next();
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    else path = a;
  }
  if (path.empty()) {
    usage();
    return 2;
  }

  if (isa && (isaCfg.fxstat.empty() || isaCfg.rga.empty())) {
    std::fprintf(stderr, "--isa needs fxstat and rga (--fxstat/--rga or SOPT_FXSTAT/SOPT_RGA)\n");
    return 2;
  }
  isaCfg.threads = opt.threads;

  Program prog;
  try {
    prog = loadProgram(path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", path.c_str(), e.what());
    return 1;
  }

  const RunResult r = optimize(prog, opt);
  char buf[128];
  std::string models(opt.search.model->name);
  if (opt.search.order && opt.search.order != opt.search.model)
    models += ", order " + std::string(opt.search.order->name);
  std::printf("target:   %s = %s   (cost %u, %s)\n", prog.outputName.c_str(), r.targetText.c_str(),
              r.targetCost, models.c_str());
  std::printf("budget:   %s\n", budgetText(prog.budget, buf, sizeof(buf)));
  std::string profiles;
  for (const auto& p : kAllProfiles) profiles += (profiles.empty() ? "" : "/") + std::string(p.name);
  std::printf("verified: sampling, %zu points, profiles %s\n", opt.v1Points, profiles.c_str());

  // Real ISA cost of the target and the shown alternatives.
  const size_t shown = std::min(top, r.accepted.size());
  std::vector<size_t> order(shown);
  std::iota(order.begin(), order.end(), size_t{0});
  std::vector<IsaCost> isaCost;
  IsaCost targetIsa;
  if (isa) {
    std::vector<const Expr*> exprs = {&prog.target};
    for (size_t i = 0; i < shown; ++i) exprs.push_back(&r.accepted[i].expr);
    isaCost = measureIsa(exprs, prog.inputs, isaCfg);
    targetIsa = isaCost[0];
    isaCost.erase(isaCost.begin());
    if (targetIsa.ok)
      std::printf("isa:      COST %d (VALU %d, TRANS %d, VGPRs %d), %s via RGA\n", targetIsa.cost,
                  targetIsa.valu, targetIsa.trans, targetIsa.vgprs,
                  isaCfg.asic.empty() ? "gfx1100" : isaCfg.asic.c_str());
    else
      std::printf("isa:      target not measured: %s\n", targetIsa.error.c_str());
    // Measured first, by ISA cost; static cost and the original order break ties.
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
      const auto &a = isaCost[x], &b = isaCost[y];
      if (a.ok != b.ok) return a.ok;
      return a.ok && a.cost != b.cost ? a.cost < b.cost : false;
    });
  }
  std::printf("\n");

  if (r.accepted.empty()) {
    std::printf("no cheaper alternative found (searched up to cost %u%s)\n",
                r.search.completedCost, r.search.limitHit ? ", limit hit" : "");
  } else {
    std::printf(isa ? "cost   isa  class            max |err|  max code  changed  expression\n"
                    : "cost  class            max |err|  max code  changed  expression\n");
    int noGain = 0, failed = 0;
    for (size_t i : order) {
      const auto& a = r.accepted[i];
      std::printf("%4u  ", a.cost);
      if (isa) {
        const auto& c = isaCost[i];
        if (!c.ok) {
          std::printf("%4s  ", "?");
          ++failed;
        } else {
          const bool gain = targetIsa.ok && c.cost < targetIsa.cost;
          noGain += targetIsa.ok && !gain;
          std::printf("%3d%c  ", c.cost, targetIsa.ok && !gain ? '!' : ' ');
        }
      }
      std::printf("%-15s  %9.3g  %8d  %6.3f%%  %s\n", klassName(a.klass), a.worst.maxAbs,
                  a.worst.maxCodeDiff, 100.0 * a.worst.changedFraction(), a.text.c_str());
    }
    std::printf("\n%zu alternative(s) cheaper than cost %u", r.accepted.size(), r.targetCost);
    if (r.accepted.size() > top) std::printf(", showing %zu", top);
    std::printf("\n");
    if (noGain) std::printf("isa: %d shown alternative(s) marked ! are not cheaper in ISA\n", noGain);
    if (failed) {
      for (size_t i : order)
        if (!isaCost[i].ok) {
          std::printf("isa: %d alternative(s) not measured, e.g.: %s\n", failed,
                      isaCost[i].error.c_str());
          break;
        }
    }
    if (r.search.limitHit)
      std::printf("note: search limit hit, levels complete up to cost %u\n", r.search.completedCost);
  }

  if (stats) {
    const auto& s = r.search;
    std::printf("\nstats: iterations %u, counterexamples %llu, rejected stage2 %llu, v1 %llu, "
                "profiles %llu\n",
                r.iterations, (unsigned long long)r.counterexamples,
                (unsigned long long)r.rejectedStage2, (unsigned long long)r.rejectedV1,
                (unsigned long long)r.rejectedProfiles);
    std::printf("final search: %.3fs, generated %llu (%.2f M/s), deduped %llu, const-skipped %llu, "
                "bank %llu, hits %llu, first hit %.3fs\n",
                s.seconds, (unsigned long long)s.generated,
                s.seconds > 0 ? s.generated / s.seconds / 1e6 : 0.0, (unsigned long long)s.deduped,
                (unsigned long long)s.constSkipped, (unsigned long long)s.bankSize,
                (unsigned long long)s.hits, s.firstHitSec);
    if (opt.search.affine)
      std::printf("affine: %llu hits via a solved outer map, %llu via an inner constant, "
                  "%llu chain entries pruned\n",
                  (unsigned long long)s.affineHits, (unsigned long long)s.innerHits,
                  (unsigned long long)s.affinePruned);
    if (s.objPruned)
      std::printf("objective: %llu entries pruned (cost >= target)\n", (unsigned long long)s.objPruned);
    std::printf("time: search %.3fs, verify %.3fs, total %.3fs\n", r.searchSec, r.verifySec,
                r.totalSec);
    std::printf("level  generated      added\n");
    for (const auto& l : s.levels)
      std::printf("%5u  %9llu  %9llu\n", l.cost, (unsigned long long)l.generated,
                  (unsigned long long)l.added);
  }
  return 0;
}
