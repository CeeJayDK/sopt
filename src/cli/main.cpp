#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <algorithm>
#include <numeric>

#include "ir/parser.hpp"
#include "measure/isa.hpp"
#include "measure/sass.hpp"
#include "search/driver.hpp"

using namespace sopt;

namespace {

struct IsaCostRow {
  bool ok = false;
  int cost = 0;
  std::string error;
};

void usage() {
  std::puts(
      "usage: sopt <file.sopt> [options]\n"
      "  --top N           show at most N alternatives (default 20)\n"
      "  --max-cost C      search up to cost C (default: target cost - 1)\n"
      "  --tests N         fingerprint test points (default 32)\n"
      "  --v1 N            verification sample points (default 1048576)\n"
      "  --v2-max N        verify on every domain point if there are at most N (default 16777216, 0 = off)\n"
      "  --v2 N            ... for the N cheapest alternatives (default 20)\n"
      "  --max-bank N      bank entry limit (default 2000000)\n"
      "  --time S          search time limit per iteration in seconds (default 60)\n"
      "  --threads N       verification threads (default: all)\n"
      "  --seed N          random seed (default 1)\n"
      "  --no-affine       enumerate outer constants instead of solving p * v + q\n"
      "  --no-inner        don't solve inner constants (p * u(v + c) + q, u = rcp/sqrt/rsqrt)\n"
      "  --no-overflow     stop when the bank is full (default: keep combining the stored\n"
      "                    entries, checking new values as hits, until --time)\n"
      "  --no-exact-rule   candidates must stay within the budget of the float32 original\n"
      "                    (default: also accepted where at least as close to exact math)\n"
      "  --loose F         also list less accurate candidates: within F times the budget or\n"
      "                    the original's error vs exact math (default 100, 0 = off)\n"
      "  --helpers         also enumerate pure helper intrinsics (lerp, step)\n"
      "  --cost-model M    objective: rdna3 | nvidia | generic (default: rdna3)\n"
      "  --order-model M   enumeration order (default: search for rdna3/nvidia, else the model)\n"
      "  --stats           print search statistics\n"
      "  --isa             rank the shown alternatives by real GPU ISA cost (fxstat + RGA)\n"
      "  --fxstat PATH     fxstat from ReShade Testing Initiative (default: $SOPT_FXSTAT)\n"
      "  --rga PATH        AMD Radeon GPU Analyzer (default: $SOPT_RGA)\n"
      "  --asic NAME       RGA target (default: fxstat's, gfx1100 = RDNA3)\n"
      "  --isa-keep DIR    keep the generated .fx files in DIR\n"
      "  --sass            rank by NVIDIA SASS cost too (ptxas + nvdisasm from the CUDA toolkit)\n"
      "  --ptxas PATH      ptxas (default: $SOPT_PTXAS)\n"
      "  --nvdisasm PATH   nvdisasm (default: $SOPT_NVDISASM)\n"
      "  --sm N            NVIDIA target: 75 Turing, 86 Ampere, 89 Ada (default), 120 Blackwell\n"
      "  --sass-keep DIR   keep the generated .ptx/.cubin files in DIR");
}

const char* budgetText(const Budget& b, char* buf, size_t n) {
  switch (b.kind) {
    case Budget::Kind::Exact: return "exact";
    case Budget::Kind::Color8:
    case Budget::Kind::Color10:
      std::snprintf(buf, n, "color%d, max code diff %d", b.codeBits(), b.maxCodeDiff);
      return buf;
    case Budget::Kind::Texcoord:
      std::snprintf(buf, n, "texcoord, %g px at %g wide (abs %g)", b.px, b.width, b.eps);
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
  opt.loose = 100;
  size_t top = 20;
  bool stats = false;
  bool isa = false;
  IsaConfig isaCfg;
  if (const char* v = std::getenv("SOPT_FXSTAT")) isaCfg.fxstat = v;
  if (const char* v = std::getenv("SOPT_RGA")) isaCfg.rga = v;
  bool sass = false;
  SassConfig sassCfg;
  if (const char* v = std::getenv("SOPT_PTXAS")) sassCfg.ptxas = v;
  if (const char* v = std::getenv("SOPT_NVDISASM")) sassCfg.nvdisasm = v;
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
    else if (a == "--v2-max") opt.v2Max = std::strtoull(next(), nullptr, 10);
    else if (a == "--v2") opt.v2Candidates = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--max-bank") opt.search.maxBank = std::strtoull(next(), nullptr, 10);
    else if (a == "--time") opt.search.timeLimitSec = std::strtod(next(), nullptr);
    else if (a == "--threads") opt.threads = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    else if (a == "--seed") opt.seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--cost-model") {
      opt.search.model = costModelByName(next());
      if (!opt.search.model) { std::fprintf(stderr, "unknown cost model (rdna3, nvidia, generic, search)\n"); return 2; }
    }
    else if (a == "--stats") stats = true;
    else if (a == "--no-affine") opt.search.affine = false;
    else if (a == "--no-inner") opt.search.inner = false;
    else if (a == "--no-inner-prefilter") opt.search.innerPrefilter = false;
    else if (a == "--no-overflow") opt.search.overflow = false;
    else if (a == "--no-exact-rule") opt.exactRule = false;
    else if (a == "--loose") opt.loose = std::strtod(next(), nullptr);
    else if (a == "--helpers") opt.search.helpers = true;
    else if (a == "--order-model") {
      opt.search.order = costModelByName(next());
      if (!opt.search.order) { std::fprintf(stderr, "unknown cost model (rdna3, nvidia, generic, search)\n"); return 2; }
    }
    else if (a == "--isa") isa = true;
    else if (a == "--fxstat") isaCfg.fxstat = next();
    else if (a == "--rga") isaCfg.rga = next();
    else if (a == "--asic") isaCfg.asic = next();
    else if (a == "--isa-keep") isaCfg.keepDir = next();
    else if (a == "--sass") sass = true;
    else if (a == "--ptxas") sassCfg.ptxas = next();
    else if (a == "--nvdisasm") sassCfg.nvdisasm = next();
    else if (a == "--sm") sassCfg.sm = static_cast<int>(std::strtol(next(), nullptr, 10));
    else if (a == "--sass-keep") sassCfg.keepDir = next();
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
  if (sass && (sassCfg.ptxas.empty() || sassCfg.nvdisasm.empty())) {
    std::fprintf(stderr, "--sass needs ptxas and nvdisasm (--ptxas/--nvdisasm or SOPT_PTXAS/SOPT_NVDISASM)\n");
    return 2;
  }
  isaCfg.threads = opt.threads;
  sassCfg.threads = opt.threads;

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
  const CostModel& ord = opt.search.order ? *opt.search.order : defaultOrderFor(*opt.search.model);
  if (&ord != opt.search.model) models += ", order " + std::string(ord.name);
  std::printf("target:   %s = %s   (cost %u, %s)\n", prog.outputName.c_str(), r.targetText.c_str(),
              r.targetCost, models.c_str());
  std::printf("budget:   %s\n", budgetText(prog.budget, buf, sizeof(buf)));
  std::string profiles;
  for (const auto& p : kAllProfiles) profiles += (profiles.empty() ? "" : "/") + std::string(p.name);
  std::printf("verified: sampling, %zu points, profiles %s", opt.v1Points, profiles.c_str());
  if (r.v2Points)
    std::printf("; all %llu domain points for the cheapest %u (ver = all)",
                (unsigned long long)r.v2Points, opt.v2Candidates);
  std::printf("\n");
  const bool rule = accuracyRule(prog.budget) && opt.exactRule;
  if (rule)
    std::printf("accuracy: original vs exact math: max abs %.3g, rel %.3g (\"as accurate\": outside the\n"
                "          budget of the original only where at least as close to the exact value)\n",
                r.targetExact.exactAbs, r.targetExact.exactRel);

  // Real machine-code cost of the target and the shown alternatives, per vendor:
  // AMD RDNA (fxstat + RGA) and NVIDIA (ptxas + nvdisasm).
  struct Column {
    std::string name;
    IsaCostRow target;
    std::vector<IsaCostRow> rows;
  };
  const size_t shown = std::min(top, r.accepted.size());
  std::vector<const Expr*> exprs = {&prog.target};
  for (size_t i = 0; i < shown; ++i) exprs.push_back(&r.accepted[i].expr);
  std::vector<Column> cols;
  if (isa) {
    const auto m = measureIsa(exprs, prog.inputs, isaCfg);
    Column c{"amd", {}, {}};
    for (const auto& x : m) c.rows.push_back({x.ok, x.cost, x.error});
    c.target = c.rows[0];
    c.rows.erase(c.rows.begin());
    cols.push_back(std::move(c));
    if (m[0].ok)
      std::printf("amd:      COST %d (VALU %d, TRANS %d, VGPRs %d), %s via RGA\n", m[0].cost,
                  m[0].valu, m[0].trans, m[0].vgprs,
                  isaCfg.asic.empty() ? "gfx1100" : isaCfg.asic.c_str());
    else
      std::printf("amd:      target not measured: %s\n", m[0].error.c_str());
  }
  if (sass) {
    const auto m = measureSass(exprs, prog.inputs, sassCfg);
    Column c{"nv", {}, {}};
    for (const auto& x : m) c.rows.push_back({x.ok, x.cost, x.error});
    c.target = c.rows[0];
    c.rows.erase(c.rows.begin());
    cols.push_back(std::move(c));
    if (m[0].ok)
      std::printf("nv:       COST %d (ALU %d, MOV %d, MUFU %d x %d, regs %d), sm_%d via ptxas\n",
                  m[0].cost, m[0].alu, m[0].mov, m[0].mufu, mufuWeight(sassCfg.sm), m[0].regs,
                  sassCfg.sm);
    else
      std::printf("nv:       target not measured: %s\n", m[0].error.c_str());
  }
  // Measured first, by the first vendor's cost, then the next; static cost and the
  // original order break ties.
  std::vector<size_t> order(shown);
  std::iota(order.begin(), order.end(), size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
    for (const auto& c : cols) {
      const auto &a = c.rows[x], &b = c.rows[y];
      if (a.ok != b.ok) return a.ok;
      if (a.ok && a.cost != b.cost) return a.cost < b.cost;
    }
    return false;
  });
  std::printf("\n");

  if (r.accepted.empty()) {
    std::printf("no cheaper alternative found (searched up to cost %u%s)\n",
                r.search.completedCost, r.search.limitHit ? ", limit hit" : "");
  } else {
    std::printf("cost  ");
    for (const auto& c : cols) std::printf("%4s  ", c.name.c_str());
    std::printf("ver  class            max |err|  %smax code  changed  expression   (codes: %d-bit)\n",
                rule ? "vs exact  " : "", prog.budget.codeBits());
    std::vector<int> noGain(cols.size(), 0), failed(cols.size(), 0);
    for (size_t i : order) {
      const auto& a = r.accepted[i];
      std::printf("%4u  ", a.cost);
      for (size_t k = 0; k < cols.size(); ++k) {
        const auto& c = cols[k].rows[i];
        const auto& t = cols[k].target;
        if (!c.ok) {
          std::printf("%4s  ", "?");
          ++failed[k];
        } else {
          const bool gain = t.ok && c.cost < t.cost;
          noGain[k] += t.ok && !gain;
          std::printf("%3d%c  ", c.cost, t.ok && !gain ? '!' : ' ');
        }
      }
      std::printf("%-3s  %-15s  %9.3g  ", a.exhaustive ? "all" : "smp",
                  klassName(a.klass, prog.budget.codeBits()), a.worst.maxAbs);
      if (rule) std::printf("%8.3g  ", a.worst.exactAbs);
      std::printf("%8d  %6.3f%%  %s\n", a.worst.maxCodeDiff, 100.0 * a.worst.changedFraction(), a.text.c_str());
    }
    std::printf("\n%zu alternative(s) cheaper than cost %u", r.accepted.size(), r.targetCost);
    if (r.accepted.size() > top) std::printf(", showing %zu", top);
    std::printf("\n");
    for (size_t k = 0; k < cols.size(); ++k) {
      if (noGain[k])
        std::printf("%s: %d shown alternative(s) marked ! are not cheaper than the target\n",
                    cols[k].name.c_str(), noGain[k]);
      if (failed[k])
        for (size_t i : order)
          if (!cols[k].rows[i].ok) {
            std::printf("%s: %d alternative(s) not measured, e.g.: %s\n", cols[k].name.c_str(),
                        failed[k], cols[k].rows[i].error.c_str());
            break;
          }
    }
    if (r.search.limitHit)
      std::printf("note: search limit hit, levels complete up to cost %u\n", r.search.completedCost);
  }

  if (stats) {
    const auto& s = r.search;
    std::printf("\nstats: iterations %u, counterexamples %llu, rejected stage2 %llu, v1 %llu, "
                "profiles %llu, v2 %llu\n",
                r.iterations, (unsigned long long)r.counterexamples,
                (unsigned long long)r.rejectedStage2, (unsigned long long)r.rejectedV1,
                (unsigned long long)r.rejectedProfiles, (unsigned long long)r.rejectedV2);
    std::printf("final search: %.3fs, generated %llu (%.2f M/s), deduped %llu, const-skipped %llu, "
                "bank %llu, hits %llu, first hit %.3fs\n",
                s.seconds, (unsigned long long)s.generated,
                s.seconds > 0 ? s.generated / s.seconds / 1e6 : 0.0, (unsigned long long)s.deduped,
                (unsigned long long)s.constSkipped, (unsigned long long)s.bankSize,
                (unsigned long long)s.hits, s.firstHitSec);
    if (opt.search.affine)
      std::printf("affine: %llu hits via a solved outer map, %llu via an inner constant, "
                  "%llu chain entries pruned, %llu inner fits skipped (not monotonic)\n",
                  (unsigned long long)s.affineHits, (unsigned long long)s.innerHits,
                  (unsigned long long)s.affinePruned, (unsigned long long)s.innerPrefiltered);
    if (s.overflowChecked)
      std::printf("overflow: %llu values checked after the bank was full, %llu kept\n",
                  (unsigned long long)s.overflowChecked, (unsigned long long)s.overflowKept);
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
