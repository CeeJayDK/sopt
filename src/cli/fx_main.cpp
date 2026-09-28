#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>

#include "fx/frontend.hpp"
#include "fx/variants.hpp"
#include "measure/isa.hpp"
#include "measure/backends.hpp"
#include "measure/sass.hpp"
#include "measure/tools.hpp"
#include "search/driver.hpp"
#include "verify/bound.hpp"

using namespace sopt;
namespace fs = std::filesystem;

namespace {

// --region F[:L]: file name F (case-insensitive, any folder), region ending at or spanning L.
bool regionMatches(const fx::Region& r, const std::string& spec) {
  const size_t colon = spec.rfind(':');
  const bool hasLine = colon != std::string::npos && colon + 1 < spec.size() &&
                       spec.find_first_not_of("0123456789", colon + 1) == std::string::npos;
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string file = lower(hasLine ? spec.substr(0, colon) : spec);
  if (lower(fs::path(r.file).filename().string()) != file) return false;
  if (!hasLine) return true;
  const auto line = static_cast<uint32_t>(std::strtoul(spec.c_str() + colon + 1, nullptr, 10));
  const uint32_t first = r.removed.empty() ? r.line : std::min(r.line, r.removed.front().first);
  return line >= first && line <= r.line;
}

void usage() {
  std::puts(
      "usage: sopt-fx [options] <file.fx | directory>...\n"
      "Finds cheaper verified alternatives to arithmetic statements of pixel shaders and\n"
      "writes variant .fx files with a preprocessor switch per statement plus a report.\n"
      "  -I DIR            include directory (ReShade.fxh etc.), repeatable\n"
      "  -D NAME[=VALUE]   preprocessor definition, repeatable\n"
      "  -o DIR            output directory (default sopt-out)\n"
      "  --list            only list the regions and their facts, no search\n"
      "  --region F[:L]    only regions of file F (name, any folder) ending at or spanning\n"
      "                    line L, repeatable; for long runs of single regions (--time)\n"
      "  --skips           list skipped statements with the reason\n"
      "  --variants N      alternatives per region in the variant files (default 3)\n"
      "  --time S          search time limit per region and iteration (default 5)\n"
      "  --max-bank N      bank entry limit per region (default 500000)\n"
      "  --v1 N            verification sample points (default 262144)\n"
      "  --jobs N          regions searched in parallel (default: all cores)\n"
      "  --max-inputs N    skip statements reading more variables (default 4)\n"
      "  --max-ops N       skip regions with more operations (default 24)\n"
      "  --max-statements N  statements per window: a statement with the single-use\n"
      "                    temporaries it reads or the statements before it that compute\n"
      "                    its variable (default 4; 1 = single statements only)\n"
      "  --cost-model M    rdna3 | nvidia | generic (default rdna3)\n"
      "  --isa             measure original and variants with fxstat + RGA (AMD); variants\n"
      "                    must be cheaper for some measured vendor ($SOPT_FXSTAT, $SOPT_RGA)\n"
      "  --sass            same with ptxas + nvdisasm (NVIDIA; $SOPT_PTXAS, $SOPT_NVDISASM)\n"
      "  --sm N            NVIDIA target for --sass (default 89)\n"
      "  --backends        compile original and variants per backend and compare after the\n"
      "                    compilers' optimizers: SPIR-V (fxstat, $SOPT_FXSTAT; spirv-dis,\n"
      "                    $SOPT_SPIRV_DIS) and DXBC via Microsoft's fxc ($SOPT_FXC =\n"
      "                    sopt-fxc.exe, run with $SOPT_WINE, default wine, off Windows)\n"
      "  --assumed         also write variants of regions whose input ranges are assumed\n"
      "  --no-accuracy-variants  do not keep candidates that are only more accurate (not cheaper)\n"
      "  --no-exact-rule   variants must stay within the budget of the original (default:\n"
      "                    also where at least as close to exact math as the original)\n"
      "  --no-shared-leaves  the target's own subexpressions are not free leaves of the\n"
      "                    search (default: they are, for rewrites that reuse a value, u * u)\n"
      "  --no-subtrees     no subtree search (default: when the search hits a limit, also\n"
      "                    search subexpressions of cost <= --subtree-max-cost, default 64,\n"
      "                    --subtree-time S each, default 1, and put the cheaper forms back)\n"
      "  --max-mem MB      memory for the search bank (default: the RAM available at start\n"
      "                    minus a little for the system, shared by regions searched in\n"
      "                    parallel); --max-bank N also caps entries\n"
      "  --disk DIR        disk-backed bank for long runs on single regions: when half the\n"
      "                    memory budget holds fingerprints, the rest go to zstd-compressed\n"
      "                    tiles in DIR (a temporary file); --disk-max GB caps it (default:\n"
      "                    the free space minus a reserve)\n"
      "  --top-down        top-down split: after each level, look up the missing operand b of\n"
      "                    op(a, b) = target for each new entry a (add, sub, mul, div)\n"
      "  --slack N         best-so-far bound: keep hits and parts of hits up to N above the\n"
      "                    cheapest hit found so far (default 1; 0 when the bank is full, -1 when\n"
      "                    it is also past half the time); --no-best-bound: bound = the original\n"
      "  --no-cuts         no cut points (default: when the search hits a limit, split the\n"
      "                    target at values the rest depends on for all inputs below them\n"
      "                    and search both parts on their own, --cut-time S per part, default 1)\n"
      "  --quant-oe N      quantized dedup: values equal after rounding away the low N\n"
      "                    mantissa bits on the test points count as one (default 0 = bit-exact)\n"
      "  --no-v3           no V3 (default: prove a formal error bound by interval subdivision\n"
      "                    for the cheapest 3 alternatives where V2 does not apply, --v3-time S\n"
      "                    each, default 2; sopt-fx: for the written variants)\n"
      "  --no-overflow     stop a region's search when the bank is full (default: keep\n"
      "                    combining the stored entries until --time)\n"
      "  --loose F         also list less accurate variants: within F times the budget or\n"
      "                    the original's error vs exact math (color: one more code), if\n"
      "                    cheaper than every accurate one; after them (default 100, 0 = off)\n"
      "                    defaults (no fact); otherwise they are only in the report\n"
      "  --facts FILE      ranges for inputs without facts (format: see sopt-facts.txt,\n"
      "                    which every run writes to the output directory)\n"
      "  --ask             ask for the missing ranges in the terminal (Enter = suggestion)\n"
      "  --no-macro-inputs bake preprocessor definitions in instead of keeping the ones\n"
      "                    users can change as compile-time inputs\n"
      "  --max-width N     largest render target width: SV_Position in [0, N], texture\n"
      "                    coordinates within 0.01 px at N (default 7680 = 8K; the\n"
      "                    hardware limit is 16384)");
}

void collect(const fs::path& p, std::vector<fs::path>& out) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) {
    std::vector<fs::path> sub;
    for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); ++it)
      if (it->is_regular_file() && it->path().extension() == ".fx") sub.push_back(it->path());
    std::sort(sub.begin(), sub.end());
    out.insert(out.end(), sub.begin(), sub.end());
  } else {
    out.push_back(p);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<fs::path> inputs;
  fx::LoadOptions load;
  fx::RegionOptions ropt;
  fs::path outDir = "sopt-out";
  bool list = false, skips = false;
  std::vector<std::string> regionFilter;
  size_t numVariants = 3;
  unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
  Options opt;
  opt.search.timeLimitSec = 5.0;
  opt.v1Points = 1u << 18;
  opt.maxIterations = 4;
  opt.loose = 100;
  // Only the cheapest few variants per region are written: verifying 50 wastes time.
  opt.maxAlternatives = 20;
  bool isa = false, sass = false, backends = false, allowAssumed = false, ask = false, symbolic = true;
  fs::path factsFile;
  IsaConfig isaCfg;
  if (const char* v = std::getenv("SOPT_FXSTAT")) isaCfg.fxstat = v;
  if (const char* v = std::getenv("SOPT_RGA")) isaCfg.rga = v;
  SassConfig sassCfg;
  if (const char* v = std::getenv("SOPT_PTXAS")) sassCfg.ptxas = v;
  if (const char* v = std::getenv("SOPT_NVDISASM")) sassCfg.nvdisasm = v;
  BackendConfig backCfg;
  backCfg.fxstat = isaCfg.fxstat;
  backCfg.spirvDis = "spirv-dis";
  if (const char* v = std::getenv("SOPT_SPIRV_DIS")) backCfg.spirvDis = v;
  if (const char* v = std::getenv("SOPT_FXC")) backCfg.fxc = v;
#ifndef _WIN32
  backCfg.wine = "wine";
#endif
  if (const char* v = std::getenv("SOPT_WINE")) backCfg.wine = v;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-I") load.includePaths.push_back(next());
    else if (a.rfind("-I", 0) == 0 && a.size() > 2) load.includePaths.push_back(a.substr(2));
    else if (a == "-D" || (a.rfind("-D", 0) == 0 && a.size() > 2)) {
      const std::string d = a == "-D" ? next() : a.substr(2);
      const size_t eq = d.find('=');
      load.macros.emplace_back(d.substr(0, eq), eq == std::string::npos ? "1" : d.substr(eq + 1));
    } else if (a == "-o") outDir = next();
    else if (a == "--list") list = true;
    else if (a == "--region") regionFilter.push_back(next());
    else if (a == "--skips") skips = true;
    else if (a == "--variants") numVariants = std::strtoul(next(), nullptr, 10);
    else if (a == "--time") opt.search.timeLimitSec = std::strtod(next(), nullptr);
    else if (a == "--max-bank") opt.search.maxBank = std::strtoull(next(), nullptr, 10);
    else if (a == "--v1") opt.v1Points = std::strtoull(next(), nullptr, 10);
    else if (a == "--jobs") jobs = std::max(1ul, std::strtoul(next(), nullptr, 10));
    else if (a == "--max-inputs") ropt.maxInputs = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--max-ops") ropt.maxOps = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--max-statements")
      ropt.maxStatements = std::max<uint32_t>(1, static_cast<uint32_t>(std::strtoul(next(), nullptr, 10)));
    else if (a == "--cost-model") {
      opt.search.model = costModelByName(next());
      if (!opt.search.model) { std::fprintf(stderr, "unknown cost model\n"); return 2; }
    } else if (a == "--isa") isa = true;
    else if (a == "--assumed") allowAssumed = true;
    else if (a == "--no-exact-rule") opt.exactRule = false;
    else if (a == "--no-accuracy-variants") opt.accuracyVariants = false;
    else if (a == "--loose") opt.loose = std::strtod(next(), nullptr);
    else if (a == "--no-overflow") opt.search.overflow = false;
    else if (a == "--no-subtrees") opt.subtrees = false;
    else if (a == "--no-cuts") opt.cuts = false;
    else if (a == "--slack") opt.search.slack = std::atoi(next());
    else if (a == "--no-best-bound") opt.search.bestBound = false;
    else if (a == "--top-down") opt.search.topDown = true;
    else if (a == "--disk") opt.search.diskDir = next();
    else if (a == "--disk-max") opt.search.diskBudget = static_cast<size_t>(std::strtod(next(), nullptr) * 1073741824.0);
    else if (a == "--max-mem") opt.search.memBudget = static_cast<size_t>(std::strtod(next(), nullptr) * 1048576.0);
    else if (a == "--tests") opt.numTests = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--no-v3") opt.v3 = false;
    else if (a == "--v3-time") opt.v3Time = std::strtod(next(), nullptr);
    else if (a == "--quant-oe") opt.search.quantBits = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--cut-time") opt.cutTime = std::strtod(next(), nullptr);
    else if (a == "--no-shared-leaves") opt.search.sharedLeaves = false;
    else if (a == "--subtree-time") opt.subtreeTime = std::strtod(next(), nullptr);
    else if (a == "--subtree-max-cost") opt.subtreeMaxCost = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--facts") factsFile = next();
    else if (a == "--ask") ask = true;
    else if (a == "--no-macro-inputs") symbolic = false;
    else if (a == "--max-width") ropt.maxWidth = std::strtod(next(), nullptr);
    else if (a == "--sass") sass = true;
    else if (a == "--backends") backends = true;
    else if (a == "--sm") sassCfg.sm = std::atoi(next());
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    else collect(a, inputs);
  }
  if (inputs.empty()) {
    usage();
    return 2;
  }
  const auto t0 = std::chrono::steady_clock::now();

  // User-supplied ranges for inputs without facts.
  fx::UserRanges userRanges;
  if (!factsFile.empty()) {
    std::string err;
    if (!fx::readUserRanges(factsFile, userRanges, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 2;
    }
  }
  ropt.userRanges = &userRanges;

  // Front end: every effect twice (two resolutions, see extractRegions).
  fx::ReportInfo info;
  info.costModel = std::string(opt.search.model->name);
  std::vector<fx::RegionResult> results;
  std::vector<std::pair<fs::path, std::vector<std::string>>> effectFiles;  // effect, its sources
  auto extractAll = [&]() {
    info.effects.clear();
    info.failed.clear();
    info.skipped = fx::SkipCount();
    info.skipped.keepDetails = skips;
    results.clear();
    effectFiles.clear();
    std::set<std::tuple<std::string, uint32_t, size_t>> seen;
    for (const auto& p : inputs) {
      std::string err;
      auto fx = fx::loadEffect(p, load, err);
      if (!fx) {
        info.failed.emplace_back(p.string(), err);
        std::fprintf(stderr, "%s: parse failed\n%s", p.string().c_str(), err.c_str());
        continue;
      }
      // User-changeable numeric preprocessor definitions stay symbolic (compile-time
      // inputs) where the effect still parses that way.
      fx::LoadOptions sym = load;
      if (symbolic) sym.symbolic = fx::symbolicMacros(p, load, *fx);
      if (!sym.symbolic.empty()) {
        std::string symErr;
        if (auto s = fx::loadEffect(p, sym, symErr)) fx = std::move(s);
        else sym.symbolic.clear();
      }
      fx::LoadOptions alt = sym;
      alt.width = 2560;
      alt.height = 1440;
      std::string altErr;
      auto fx2 = fx::loadEffect(p, alt, altErr);
      info.effects.push_back(p.string());
      effectFiles.emplace_back(p, fx->sourceFiles);
      for (auto& r : fx::extractRegions(*fx, fx2.get(), ropt, info.skipped)) {
        if (!seen.insert({r.file, r.line, r.removed.size()}).second) continue;  // shared header
        fx::RegionResult rr;
        rr.effect = p.string();
        rr.targetCost = dagCost(r.prog.target, *opt.search.model, r.prog.inputs);
        rr.region = std::move(r);
        results.push_back(std::move(rr));
      }
    }
    if (!regionFilter.empty())
      results.erase(std::remove_if(results.begin(), results.end(),
                                   [&](const fx::RegionResult& rr) {
                                     for (const auto& f : regionFilter)
                                       if (regionMatches(rr.region, f)) return false;
                                     return true;
                                   }),
                    results.end());
  };
  extractAll();

  // Inputs still without a range: ask (--ask), and list them in sopt-facts.txt with a
  // suggestion so they can be filled in and passed back with --facts.
  struct Missing {
    const fx::Fact* fact = nullptr;
    size_t regions = 0;
    std::string example;
  };
  auto missingRanges = [&]() {
    std::map<std::string, Missing> m;
    for (const auto& rr : results)
      for (const auto& f : rr.region.facts)
        if (f.assumed) {
          Missing& x = m[f.key];
          if (!x.fact) {
            x.fact = &f;
            x.example = fs::path(rr.region.file).filename().string() + ":" +
                        std::to_string(rr.region.line) + ": " + rr.region.lhs + " " +
                        toString(rr.region.prog.target, rr.region.prog.inputs);
          }
          ++x.regions;
        }
    return m;
  };
  auto missing = missingRanges();
  if (ask && !missing.empty()) {
    // One question at a time, uniforms and parameters first; after each answer the
    // effects are read again, since values computed from the answer get a range too.
    std::printf("%zu inputs have no known range. Type a range (\"0 1\" or \"[0, 1]\"), Enter for\n"
                "the suggestion, \"s\" to skip (keep the assumed default), \"q\" to stop asking.\n",
                missing.size());
    std::set<std::string> asked;
    for (size_t k = 1;; ++k) {
      const std::pair<const std::string, Missing>* next = nullptr;
      size_t open = 0;
      for (const auto& e : missing) {
        if (asked.count(e.first)) continue;
        ++open;
        auto rank = [](const Missing& m) { return std::make_pair(m.fact->order, -int(m.regions)); };
        if (!next || rank(e.second) < rank(next->second)) next = &e;
      }
      if (!next) break;
      const std::string key = next->first;
      const Missing& x = next->second;
      asked.insert(key);
      std::printf("\n[%zu, %zu open] %s  (%zu region%s, e.g. %s)\n  suggestion [%g, %g]: %s\n> ", k,
                  open, key.c_str(), x.regions, x.regions == 1 ? "" : "s",
                  x.example.substr(0, 160).c_str(), x.fact->suggestLo, x.fact->suggestHi,
                  x.fact->suggestWhy.c_str());
      std::fflush(stdout);
      bool quit = false, answered = false;
      for (;;) {
        std::string answer;
        if (!std::getline(std::cin, answer)) {
          quit = true;
          break;
        }
        while (!answer.empty() && std::isspace(static_cast<unsigned char>(answer.back()))) answer.pop_back();
        double lo = 0, hi = 0;
        if (answer == "q") quit = true;
        else if (answer == "s") {
        } else if (answer.empty()) {
          userRanges[key] = {x.fact->suggestLo, x.fact->suggestHi};
          answered = true;
        } else if (fx::parseRange(answer, lo, hi)) {
          userRanges[key] = {lo, hi};
          answered = true;
        } else {
          std::printf("  not a range, try again> ");
          std::fflush(stdout);
          continue;
        }
        break;
      }
      if (quit) break;
      if (answered) {
        extractAll();
        missing = missingRanges();
      }
    }
    std::printf("\n");
  }
  {
    std::error_code ec;
    fs::create_directories(outDir, ec);
    std::ofstream f(outDir / "sopt-facts.txt", std::ios::binary);
    f << "# Input ranges for sopt-fx --facts: <file> <function> <input> = [lo, hi]\n"
         "# Given ranges are used as facts. Commented lines are inputs without a known range,\n"
         "# with a suggestion: check it, uncomment and rerun with --facts sopt-facts.txt.\n\n";
    char buf[64];
    for (const auto& [key, r] : userRanges) {
      std::snprintf(buf, sizeof(buf), "[%.9g, %.9g]", r.first, r.second);
      f << key << " = " << buf << "\n";
    }
    if (!missing.empty()) f << "\n# No known range:\n";
    for (const auto& [key, x] : missing) {
      std::snprintf(buf, sizeof(buf), "[%.9g, %.9g]", x.fact->suggestLo, x.fact->suggestHi);
      f << "# " << key << " = " << buf << "   # " << x.fact->suggestWhy << "; " << x.regions
        << " region" << (x.regions == 1 ? "" : "s") << "\n";
    }
  }
  std::printf("%zu inputs without a known range (listed in %s)\n", missing.size(),
              (outDir / "sopt-facts.txt").string().c_str());
  std::printf("%zu effects parsed, %zu failed, %zu regions\n", info.effects.size(),
              info.failed.size(), results.size());
  if (skips)
    for (const auto& d : info.skipped.details) std::printf("skip %s\n", d.c_str());

  if (list) {
    for (const auto& rr : results) {
      const fx::Region& r = rr.region;
      std::printf("%s:%s%u  %s %s  (cost %u)\n", r.file.c_str(),
                  r.removed.empty() ? "" : (std::to_string(r.removed.front().first) + "-").c_str(), r.line,
                  r.lhs.c_str(),
                  toString(r.prog.target, r.prog.inputs).c_str(), rr.targetCost);
      std::printf("    budget %s (%s)\n", fx::budgetString(r.prog.budget).c_str(), r.budgetReason.c_str());
      if (!r.guard.empty()) std::printf("    only while %s\n", r.guard.c_str());
      for (size_t k = 0; k < r.prog.inputs.size(); ++k) {
        const auto& d = r.prog.inputs[k];
        std::printf("    %s in [%g, %g]%s  %s%s\n", d.name.c_str(), d.lo, d.hi,
                    d.grid ? (" grid " + std::to_string(d.grid)).c_str() : "", r.facts[k].source.c_str(),
                    r.facts[k].assumed ? " (assumed)" : "");
      }
    }
    return info.failed.empty() ? 0 : 1;
  }

  // Search, regions in parallel (each single-threaded when jobs > 1).
  Options ropt2 = opt;
  ropt2.v3 = false;  // V3 runs below, only on the variants that are written
  if (jobs > 1 && results.size() > 1) ropt2.threads = ropt2.search.threads = 1;  // regions in parallel instead
  // The RAM budget is shared by the regions searched at the same time.
  if (jobs > 1 && results.size() > 1) ropt2.search.concurrent = static_cast<unsigned>(std::min<size_t>(jobs, results.size()));
  std::atomic<size_t> done{0};
  std::mutex printMu;
  // Regions that differ only in their inputs' names (same expression over the same
  // ranges, budget) are searched once: e.g. the same statement per color channel.
  auto searchKey = [](const Program& p) {
    std::vector<InputDecl> anon = p.inputs;
    std::string key;
    char buf[160];
    for (size_t k = 0; k < anon.size(); ++k) {
      const InputDecl& d = anon[k];
      std::snprintf(buf, sizeof(buf), "|%d %.9g %.9g %u %d %.9g", static_cast<int>(d.type), d.lo, d.hi,
                    d.grid, d.compileTime ? 1 : 0, d.value);
      key += buf;
      anon[k].name = "in" + std::to_string(k);
    }
    const Budget& b = p.budget;
    std::snprintf(buf, sizeof(buf), "|%d %.9g %d %.9g %.9g %d %.9g|", static_cast<int>(b.kind), b.eps,
                  b.maxCodeDiff, b.px, b.width, b.vsExact ? 1 : 0, b.loose);
    return key + buf + toString(p.target, anon);
  };
  std::vector<size_t> firstOf(results.size());
  std::vector<size_t> unique;
  {
    std::map<std::string, size_t> seen;
    for (size_t i = 0; i < results.size(); ++i) {
      const auto [it, fresh] = seen.emplace(searchKey(results[i].region.prog), i);
      firstOf[i] = it->second;
      if (fresh) unique.push_back(i);
    }
  }
  std::vector<RunResult> searched(results.size());
  std::vector<double> searchSec(results.size(), 0.0);
  parallelFor(unique.size(), jobs, [&](size_t u) {
    const size_t i = unique[u];
    const auto s0 = std::chrono::steady_clock::now();
    searched[i] = optimize(results[i].region.prog, ropt2);
    searchSec[i] = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
  });
  if (unique.size() < results.size())
    std::printf("%zu regions searched (%zu repeat another one with other input names)\n", unique.size(),
                results.size() - unique.size());
  {
    // Where the time went (summed over the searched regions, all threads).
    double se = 0, ve = 0, su = 0, cu = 0, to = 0;
    for (size_t i : unique) {
      const RunResult& r = searched[i];
      se += r.searchSec, ve += r.verifySec, su += r.subtreeSec, cu += r.cutSec, to += r.totalSec;
    }
    std::printf("search time: enumeration %.0f s, verification %.0f s, subtrees %.0f s, cuts %.0f s, other %.0f s\n",
                se, ve, su, cu, to - se - ve - su - cu);
  }
  parallelFor(results.size(), jobs, [&](size_t i) {
    fx::RegionResult& rr = results[i];
    const auto s0 = std::chrono::steady_clock::now();
    RunResult res = searched[firstOf[i]];
    if (firstOf[i] != i)  // the same programs over this region's input names
      for (auto& a : res.accepted) a.text = toString(a.expr, rr.region.prog.inputs);
    rr.targetCost = res.targetCost;
    rr.limitHit = res.search.limitHit;
    rr.completedCost = res.search.completedCost;
    rr.maxLevel = res.search.maxLevel;
    const uint32_t targetCompiled =
        fx::compiledCost(rr.region.prog.target, *opt.search.model, rr.region.prog.inputs);
    // A texture fetch input is its call text: a variant must not repeat it more often.
    auto count = [](const std::string& text, const std::string& what) {
      size_t n = 0;
      for (size_t p = text.find(what); p != std::string::npos; p = text.find(what, p + what.size())) ++n;
      return n;
    };
    const std::string targetText = toString(rr.region.prog.target, rr.region.prog.inputs);
    for (const auto& a : res.accepted) {
      if (a.cost >= res.targetCost && !a.moreAccurate) continue;
      bool moreFetches = false;
      for (size_t k = 0; k < rr.region.facts.size(); ++k)
        if (rr.region.facts[k].fetch) {
          const std::string& nm = rr.region.prog.inputs[k].name;
          moreFetches = moreFetches || count(a.text, nm) > count(targetText, nm);
        }
      if (moreFetches) continue;
      const uint32_t compiled = fx::compiledCost(a.expr, *opt.search.model, rr.region.prog.inputs);
      const bool cheaper = compiled < targetCompiled;
      if (!cheaper && !(a.moreAccurate && compiled <= targetCompiled + opt.accuracySlack)) {
        ++rr.onlyContraction;
        continue;
      }
      fx::Variant v;
      v.moreAccurate = a.moreAccurate;
      v.accuracyOnly = !cheaper;
      v.expr = a.expr;
      v.text = a.text;
      v.cost = a.cost;
      v.klass = a.klass;
      v.worst = a.worst;
      v.exhaustive = a.exhaustive;
      v.proven = a.proven;
      v.provenFraction = a.provenFraction;
      v.problems = describeProblems(rr.region.prog, a.problems);
      rr.variants.push_back(std::move(v));
    }
    if (accuracyRule(rr.region.prog.budget) && opt.exactRule) rr.targetExactAbs = res.targetExact.exactAbs;
    // Accurate variants first (cheapest first); less accurate ones only if cheaper than
    // every accurate one, after them: the user decides from their accuracy.
    std::vector<fx::Variant> strict, loose, accurate;
    for (auto& v : rr.variants)
      (v.accuracyOnly ? accurate : (v.klass == Klass::LessAccurate ? loose : strict)).push_back(std::move(v));
    auto byCost = [](const fx::Variant& a, const fx::Variant& b) { return a.cost < b.cost; };
    std::stable_sort(strict.begin(), strict.end(), byCost);
    std::stable_sort(loose.begin(), loose.end(), byCost);
    if (strict.size() > numVariants) strict.resize(numVariants);
    if (!strict.empty())
      loose.erase(std::remove_if(loose.begin(), loose.end(),
                                 [&](const fx::Variant& v) { return v.cost >= strict.front().cost; }),
                  loose.end());
    if (loose.size() > numVariants) loose.resize(numVariants);
    std::stable_sort(accurate.begin(), accurate.end(), byCost);
    if (accurate.size() > 2) accurate.resize(2);
    rr.variants = std::move(strict);
    for (auto& v : loose) rr.variants.push_back(std::move(v));
    for (auto& v : accurate) rr.variants.push_back(std::move(v));
    rr.sec = searchSec[firstOf[i]] + std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
    std::lock_guard<std::mutex> lock(printMu);
    const size_t n = ++done;
    std::printf("[%zu/%zu] %s:%u cost %u -> %s\n", n, results.size(),
                fs::path(rr.region.file).filename().string().c_str(), rr.region.line, rr.targetCost,
                rr.variants.empty() ? "-" : std::to_string(rr.variants[0].cost).c_str());
    std::fflush(stdout);
  });

  // Measured machine code: a variant stays if some measured vendor gets faster (it may be
  // slower on another: the report shows both); equal or slower everywhere means no gain,
  // equal usually because the compiler already does it.
  if (isa || sass) {
    info.amd = isa;
    info.nv = sass;
    size_t n = 0;
    for (auto& rr : results) {
      if (rr.variants.empty()) continue;
      std::vector<InputDecl> ins = rr.region.prog.inputs;
      for (size_t k = 0; k < ins.size(); ++k) ins[k].name = "sopt_in" + std::to_string(k);
      std::vector<const Expr*> exprs = {&rr.region.prog.target};
      for (const auto& v : rr.variants) exprs.push_back(&v.expr);
      if (isa) {
        const auto m = measureIsa(exprs, ins, isaCfg);
        rr.targetAmd = m[0].ok ? m[0].cost : -1;
        if (!m[0].ok) std::fprintf(stderr, "%s:%u: amd: %s\n", rr.region.file.c_str(), rr.region.line, m[0].error.c_str());
        for (size_t k = 0; k < rr.variants.size(); ++k) rr.variants[k].amd = m[k + 1].ok ? m[k + 1].cost : -1;
      }
      if (sass) {
        const auto m = measureSass(exprs, ins, sassCfg);
        rr.targetNv = m[0].ok ? m[0].cost : -1;
        if (!m[0].ok) std::fprintf(stderr, "%s:%u: nv: %s\n", rr.region.file.c_str(), rr.region.line, m[0].error.c_str());
        for (size_t k = 0; k < rr.variants.size(); ++k) rr.variants[k].nv = m[k + 1].ok ? m[k + 1].cost : -1;
      }
      std::vector<fx::Variant> kept;
      for (auto& v : rr.variants) {
        bool better = false, measured = false, close = true;
        for (auto [t, c] : {std::pair{rr.targetAmd, v.amd}, std::pair{rr.targetNv, v.nv}}) {
          if (t < 0 || c < 0) continue;
          measured = true;
          better = better || c < t;
          close = close && c <= t + 1;
        }
        if (measured && !better && v.moreAccurate && close) {
          v.accuracyOnly = true;  // accuracy variant: not faster, at most 1 instruction slower
          kept.push_back(std::move(v));
        } else if (measured && !better) {
          ++rr.measuredNoGain;
        } else {
          v.accuracyOnly = false;
          kept.push_back(std::move(v));
        }
      }
      // Largest measured gain first (sum over vendors of the relative change), static
      // cost breaks ties.
      auto gain = [&](const fx::Variant& v) {
        double g = 0;
        for (auto [t, c] : {std::pair{rr.targetAmd, v.amd}, std::pair{rr.targetNv, v.nv}})
          if (t > 0 && c >= 0) g += double(t - c) / t;
        return g;
      };
      std::stable_sort(kept.begin(), kept.end(), [&](const fx::Variant& a, const fx::Variant& b) {
        if (a.accuracyOnly != b.accuracyOnly) return b.accuracyOnly;  // accuracy variants last
        const double ga = gain(a), gb = gain(b);
        if (ga != gb) return ga > gb;
        return a.cost < b.cost;
      });
      rr.variants = std::move(kept);
      std::printf("measured %zu: %s:%u amd %d nv %d, %zu variants kept\n", ++n,
                  fs::path(rr.region.file).filename().string().c_str(), rr.region.line, rr.targetAmd,
                  rr.targetNv, rr.variants.size());
    }
  }

  // Backend normalization: counts after the compilers' optimizers, and whether a variant's
  // code is the original's there ("the compiler already does it" on that backend).
  if (backends) {
    info.spirv = true;
    info.dxbc = !backCfg.fxc.empty();
    for (auto& rr : results) {
      if (rr.variants.empty()) continue;
      std::vector<InputDecl> ins = rr.region.prog.inputs;
      for (size_t k = 0; k < ins.size(); ++k) ins[k].name = "sopt_in" + std::to_string(k);
      std::vector<const Expr*> exprs = {&rr.region.prog.target};
      for (const auto& v : rr.variants) exprs.push_back(&v.expr);
      const auto m = measureBackends(exprs, ins, backCfg);
      if (!m[0].error.empty()) std::fprintf(stderr, "%s:%u: backends: %s\n", rr.region.file.c_str(), rr.region.line, m[0].error.c_str());
      rr.targetSpirv = m[0].spirv;
      rr.targetDxbc = m[0].dxbc;
      for (size_t k = 0; k < rr.variants.size(); ++k) {
        fx::Variant& v = rr.variants[k];
        v.spirv = m[k + 1].spirv;
        v.dxbc = m[k + 1].dxbc;
        v.spirvSame = v.spirv >= 0 && !m[0].spirvCode.empty() && m[k + 1].spirvCode == m[0].spirvCode;
        v.dxbcSame = v.dxbc >= 0 && !m[0].dxbcCode.empty() && m[k + 1].dxbcCode == m[0].dxbcCode;
      }
    }
  }

  // Only variants with an advantage (owner, 2026-09-28): drop one when another variant of
  // the region is at least as fast on every measure (static cost, AMD, NVIDIA, SPIR-V,
  // DXBC) and at least as accurate (error vs exact math, else vs the original; no problem
  // inputs unless it has them too). Equal on everything: the first one stays.
  {
    size_t dropped = 0;
    auto noWorse = [](int a, int b) { return a < 0 || b < 0 || a <= b; };
    for (auto& rr : results) {
      auto err = [&](const fx::Variant& v) {
        return accuracyRule(rr.region.prog.budget) && opt.exactRule ? v.worst.exactAbs : v.worst.maxAbs;
      };
      auto asGood = [&](const fx::Variant& k, const fx::Variant& v) {  // k at least as good as v
        return k.cost <= v.cost && noWorse(k.amd, v.amd) && noWorse(k.nv, v.nv) && noWorse(k.spirv, v.spirv) &&
               noWorse(k.dxbc, v.dxbc) && err(k) <= err(v) && (k.problems.empty() || !v.problems.empty()) &&
               (k.klass != Klass::LessAccurate || v.klass == Klass::LessAccurate);
      };
      const auto& vs = rr.variants;
      std::vector<fx::Variant> kept;
      for (size_t i = 0; i < vs.size(); ++i) {
        bool dominated = false;
        for (size_t j = 0; j < vs.size() && !dominated; ++j)
          // j dominates i: as good, and better somewhere or (equal) earlier in the list.
          dominated = j != i && asGood(vs[j], vs[i]) && (!asGood(vs[i], vs[j]) || j < i);
        if (dominated) ++dropped;
        else kept.push_back(vs[i]);
      }
      rr.variants = std::move(kept);
    }
    if (dropped) std::printf("%zu variants dropped: another variant of the region is as fast everywhere and as accurate\n", dropped);
  }

  // Sampling cannot find a difference confined to a small part of a wide assumed range
  // (e.g. a ramp near 0 in [-1000, 1000]), so such variants are not written.
  if (!allowAssumed)
    for (auto& rr : results) {
      bool assumed = false;
      for (const auto& f : rr.region.facts) assumed = assumed || f.assumed;
      if (assumed && !rr.variants.empty()) rr.unwritten = std::move(rr.variants), rr.variants.clear();
    }

  // V3: a formal bound for the variants that are written (continuous domains; V2 covered
  // the small ones).
  if (opt.v3) {
    BoundOptions bo;
    bo.seconds = opt.v3Time;
    bo.maxBoxes = opt.v3MaxBoxes;
    const auto v0 = std::chrono::steady_clock::now();
    std::atomic<size_t> tried{0}, proven{0};
    parallelFor(results.size(), jobs, [&](size_t i) {
      auto& rr = results[i];
      if (rr.region.prog.budget.kind == Budget::Kind::Exact) return;
      for (auto& v : rr.variants) {
        if (v.exhaustive || v.klass == Klass::LessAccurate) continue;
        const BoundResult b = proveBound(rr.region.prog, v.expr, bo);
        v.proven = b.proven;
        v.provenFraction = b.fraction;
        ++tried;
        proven += b.proven;
      }
    });
    std::printf("V3: %zu of %zu variants proven (%.1f s)\n", proven.load(), tried.load(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - v0).count());
  }

  std::sort(results.begin(), results.end(), [](const fx::RegionResult& a, const fx::RegionResult& b) {
    return std::tie(a.region.file, a.region.line) < std::tie(b.region.file, b.region.line);
  });
  std::string errors;
  auto files = fx::writeVariants(results, outDir, errors);

  // Effects that include a changed header are copied too, so that the output directory
  // is self-contained: an effect finds headers next to it before the include paths.
  std::set<std::string> changed;
  for (const auto& r : results)
    if (!r.variants.empty()) changed.insert(r.region.file);
  std::vector<fs::path> effectsOut;
  for (const auto& [p, srcs] : effectFiles) {
    bool uses = false;
    for (const auto& f : srcs) uses = uses || changed.count(f);
    if (!uses) continue;
    const fs::path dst = outDir / p.filename();
    std::error_code ec;
    if (!changed.count(fx::pathString(p)) && !fs::exists(dst, ec)) {
      fs::copy_file(p, dst, fs::copy_options::overwrite_existing, ec);
      if (ec) errors += "cannot copy " + p.string() + "\n";
      else files.push_back(dst);
    }
    effectsOut.push_back(dst);
  }

  // Every variant must still parse: SOPT_ALL = k selects variant k where it exists.
  size_t checks = 0, checkFailures = 0;
  for (const auto& e : effectsOut) {
    for (size_t k = 0; k <= 2 * numVariants; ++k) {
      fx::LoadOptions lo = load;
      lo.macros.emplace_back("SOPT_ALL", std::to_string(k));
      std::string err;
      ++checks;
      if (!fx::loadEffect(e, lo, err)) {
        ++checkFailures;
        errors += e.string() + " (SOPT_ALL=" + std::to_string(k) + ") does not parse:\n" + err;
      }
    }
  }
  info.checks = checks;
  info.checkFailures = checkFailures;
  info.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  {
    std::ofstream f(outDir / "sopt-report.md", std::ios::binary);
    f << fx::markdownReport(results, info);
  }
  if (!errors.empty()) std::fprintf(stderr, "%s", errors.c_str());
  size_t improved = 0;
  for (const auto& r : results) improved += !r.variants.empty();
  std::printf("%zu of %zu regions have cheaper variants; wrote %zu files and sopt-report.md to %s\n",
              improved, results.size(), files.size(), outDir.string().c_str());
  std::printf("variant check: %zu of %zu parses failed\n", checkFailures, checks);
  return info.failed.empty() && checkFailures == 0 ? 0 : 1;
}
