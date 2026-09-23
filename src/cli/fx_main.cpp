#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>

#include "fx/frontend.hpp"
#include "fx/variants.hpp"
#include "measure/isa.hpp"
#include "measure/sass.hpp"
#include "measure/tools.hpp"
#include "search/driver.hpp"

using namespace sopt;
namespace fs = std::filesystem;

namespace {

void usage() {
  std::puts(
      "usage: sopt-fx [options] <file.fx | directory>...\n"
      "Finds cheaper verified alternatives to arithmetic statements of pixel shaders and\n"
      "writes variant .fx files with a preprocessor switch per statement plus a report.\n"
      "  -I DIR            include directory (ReShade.fxh etc.), repeatable\n"
      "  -D NAME[=VALUE]   preprocessor definition, repeatable\n"
      "  -o DIR            output directory (default sopt-out)\n"
      "  --list            only list the regions and their facts, no search\n"
      "  --skips           list skipped statements with the reason\n"
      "  --variants N      alternatives per region in the variant files (default 3)\n"
      "  --time S          search time limit per region and iteration (default 5)\n"
      "  --max-bank N      bank entry limit per region (default 500000)\n"
      "  --v1 N            verification sample points (default 262144)\n"
      "  --jobs N          regions searched in parallel (default: all cores)\n"
      "  --max-inputs N    skip statements reading more variables (default 4)\n"
      "  --cost-model M    rdna3 | nvidia | generic (default rdna3)\n"
      "  --isa             measure original and variants with fxstat + RGA (AMD); keep\n"
      "                    variants that are cheaper there ($SOPT_FXSTAT, $SOPT_RGA)\n"
      "  --sass            same with ptxas + nvdisasm (NVIDIA; $SOPT_PTXAS, $SOPT_NVDISASM)\n"
      "  --sm N            NVIDIA target for --sass (default 89)\n"
      "  --assumed         also write variants of regions whose input ranges are assumed\n"
      "                    defaults (no fact); otherwise they are only in the report");
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
  size_t numVariants = 3;
  unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
  Options opt;
  opt.search.timeLimitSec = 5.0;
  opt.search.maxBank = 500'000;
  opt.v1Points = 1u << 18;
  opt.maxIterations = 4;
  bool isa = false, sass = false, allowAssumed = false;
  IsaConfig isaCfg;
  if (const char* v = std::getenv("SOPT_FXSTAT")) isaCfg.fxstat = v;
  if (const char* v = std::getenv("SOPT_RGA")) isaCfg.rga = v;
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
    if (a == "-I") load.includePaths.push_back(next());
    else if (a.rfind("-I", 0) == 0 && a.size() > 2) load.includePaths.push_back(a.substr(2));
    else if (a == "-D" || (a.rfind("-D", 0) == 0 && a.size() > 2)) {
      const std::string d = a == "-D" ? next() : a.substr(2);
      const size_t eq = d.find('=');
      load.macros.emplace_back(d.substr(0, eq), eq == std::string::npos ? "1" : d.substr(eq + 1));
    } else if (a == "-o") outDir = next();
    else if (a == "--list") list = true;
    else if (a == "--skips") skips = true;
    else if (a == "--variants") numVariants = std::strtoul(next(), nullptr, 10);
    else if (a == "--time") opt.search.timeLimitSec = std::strtod(next(), nullptr);
    else if (a == "--max-bank") opt.search.maxBank = std::strtoull(next(), nullptr, 10);
    else if (a == "--v1") opt.v1Points = std::strtoull(next(), nullptr, 10);
    else if (a == "--jobs") jobs = std::max(1ul, std::strtoul(next(), nullptr, 10));
    else if (a == "--max-inputs") ropt.maxInputs = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--cost-model") {
      opt.search.model = costModelByName(next());
      if (!opt.search.model) { std::fprintf(stderr, "unknown cost model\n"); return 2; }
    } else if (a == "--isa") isa = true;
    else if (a == "--assumed") allowAssumed = true;
    else if (a == "--sass") sass = true;
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

  // Front end: every effect twice (two resolutions, see extractRegions).
  fx::ReportInfo info;
  info.costModel = std::string(opt.search.model->name);
  info.skipped.keepDetails = skips;
  std::vector<fx::RegionResult> results;
  std::set<std::tuple<std::string, uint32_t, size_t>> seen;
  std::vector<std::pair<fs::path, std::vector<std::string>>> effectFiles;  // effect, its sources
  for (const auto& p : inputs) {
    std::string err;
    auto fx = fx::loadEffect(p, load, err);
    if (!fx) {
      info.failed.emplace_back(p.string(), err);
      std::fprintf(stderr, "%s: parse failed\n%s", p.string().c_str(), err.c_str());
      continue;
    }
    fx::LoadOptions alt = load;
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
      rr.targetCost = dagCost(r.prog.target, *opt.search.model);
      rr.region = std::move(r);
      results.push_back(std::move(rr));
    }
  }
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
  if (jobs > 1) ropt2.threads = 1;
  std::atomic<size_t> done{0};
  std::mutex printMu;
  parallelFor(results.size(), jobs, [&](size_t i) {
    fx::RegionResult& rr = results[i];
    const auto s0 = std::chrono::steady_clock::now();
    RunResult res = optimize(rr.region.prog, ropt2);
    rr.targetCost = res.targetCost;
    rr.limitHit = res.search.limitHit;
    rr.completedCost = res.search.completedCost;
    const uint32_t targetCompiled = fx::compiledCost(rr.region.prog.target, *opt.search.model);
    for (const auto& a : res.accepted) {
      if (a.cost >= res.targetCost) continue;
      if (fx::compiledCost(a.expr, *opt.search.model) >= targetCompiled) {
        ++rr.onlyContraction;
        continue;
      }
      fx::Variant v;
      v.expr = a.expr;
      v.text = a.text;
      v.cost = a.cost;
      v.klass = a.klass;
      v.worst = a.worst;
      v.exhaustive = a.exhaustive;
      rr.variants.push_back(std::move(v));
    }
    std::stable_sort(rr.variants.begin(), rr.variants.end(),
                     [](const fx::Variant& a, const fx::Variant& b) { return a.cost < b.cost; });
    if (rr.variants.size() > numVariants) rr.variants.resize(numVariants);
    rr.sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
    std::lock_guard<std::mutex> lock(printMu);
    const size_t n = ++done;
    std::printf("[%zu/%zu] %s:%u cost %u -> %s\n", n, results.size(),
                fs::path(rr.region.file).filename().string().c_str(), rr.region.line, rr.targetCost,
                rr.variants.empty() ? "-" : std::to_string(rr.variants[0].cost).c_str());
    std::fflush(stdout);
  });

  // Measured machine code: a variant stays only if no measured vendor gets slower and
  // one gets faster (design 4.5); equal everywhere means the compiler already does it.
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
        bool worse = false, better = false, measured = false;
        for (auto [t, c] : {std::pair{rr.targetAmd, v.amd}, std::pair{rr.targetNv, v.nv}}) {
          if (t < 0 || c < 0) continue;
          measured = true;
          worse = worse || c > t;
          better = better || c < t;
        }
        if (measured && (worse || !better)) ++rr.measuredNoGain;
        else kept.push_back(std::move(v));
      }
      // Measured cost first (AMD, then NVIDIA), static cost breaks ties.
      std::stable_sort(kept.begin(), kept.end(), [](const fx::Variant& a, const fx::Variant& b) {
        if (a.amd != b.amd) return a.amd >= 0 && (b.amd < 0 || a.amd < b.amd);
        if (a.nv != b.nv) return a.nv >= 0 && (b.nv < 0 || a.nv < b.nv);
        return a.cost < b.cost;
      });
      rr.variants = std::move(kept);
      std::printf("measured %zu: %s:%u amd %d nv %d, %zu variants kept\n", ++n,
                  fs::path(rr.region.file).filename().string().c_str(), rr.region.line, rr.targetAmd,
                  rr.targetNv, rr.variants.size());
    }
  }

  // Sampling cannot find a difference confined to a small part of a wide assumed range
  // (e.g. a ramp near 0 in [-1000, 1000]), so such variants are not written.
  if (!allowAssumed)
    for (auto& rr : results) {
      bool assumed = false;
      for (const auto& f : rr.region.facts) assumed = assumed || f.assumed;
      if (assumed && !rr.variants.empty()) rr.unwritten = std::move(rr.variants), rr.variants.clear();
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
    for (size_t k = 0; k <= numVariants; ++k) {
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
