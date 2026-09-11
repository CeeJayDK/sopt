#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ir/parser.hpp"
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
      "  --stats           print search statistics");
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
    else if (a == "--stats") stats = true;
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    else path = a;
  }
  if (path.empty()) {
    usage();
    return 2;
  }

  Program prog;
  try {
    prog = loadProgram(path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", path.c_str(), e.what());
    return 1;
  }

  const RunResult r = optimize(prog, opt);
  char buf[128];
  std::printf("target:   %s = %s   (cost %u)\n", prog.outputName.c_str(), r.targetText.c_str(),
              r.targetCost);
  std::printf("budget:   %s\n", budgetText(prog.budget, buf, sizeof(buf)));
  std::printf("verified: sampling, %zu points, profiles ref/mix/fma\n\n", opt.v1Points);

  if (r.accepted.empty()) {
    std::printf("no cheaper alternative found (searched up to cost %u%s)\n",
                r.search.completedCost, r.search.limitHit ? ", limit hit" : "");
  } else {
    std::printf("cost  class            max |err|  max code  changed  expression\n");
    for (size_t i = 0; i < r.accepted.size() && i < top; ++i) {
      const auto& a = r.accepted[i];
      std::printf("%4u  %-15s  %9.3g  %8d  %6.3f%%  %s\n", a.cost, klassName(a.klass),
                  a.worst.maxAbs, a.worst.maxCodeDiff, 100.0 * a.worst.changedFraction(),
                  a.text.c_str());
    }
    std::printf("\n%zu alternative(s) cheaper than cost %u", r.accepted.size(), r.targetCost);
    if (r.accepted.size() > top) std::printf(", showing %zu", top);
    std::printf("\n");
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
    std::printf("time: search %.3fs, verify %.3fs, total %.3fs\n", r.searchSec, r.verifySec,
                r.totalSec);
    std::printf("level  generated      added\n");
    for (const auto& l : s.levels)
      std::printf("%5u  %9llu  %9llu\n", l.cost, (unsigned long long)l.generated,
                  (unsigned long long)l.added);
  }
  return 0;
}
