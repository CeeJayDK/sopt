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

#include "cli/console.hpp"
#include "fx/blend.hpp"
#include "fx/frontend.hpp"
#include "fx/hoist.hpp"
#include "fx/variants.hpp"
#include "measure/isa.hpp"
#include "measure/backends.hpp"
#include "measure/driverstats.hpp"
#include "measure/sass.hpp"
#include "measure/tools.hpp"
#include "search/driver.hpp"
#include "search/library.hpp"
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
      "usage: sopt-fx [options] <file.fx | file.hlsl | directory>...   (sopt " SOPT_VERSION ")\n"
      "Finds cheaper verified alternatives to arithmetic statements of pixel and compute\n"
      "shaders and writes variant .fx files with a preprocessor switch per statement plus a\n"
      "report. ReShade FX by default; .hlsl / .hlsli files are plain HLSL (SM5 pixel shaders,\n"
      "or compute shaders when the entry point has [numthreads]); .frag / .fs / .glsl files are\n"
      "GLSL fragment shaders.\n"
      "  --version         print the version\n"
      "  -I DIR            include directory (ReShade.fxh etc.), repeatable\n"
      "  -D NAME[=VALUE]   preprocessor definition, repeatable\n"
      "  -o DIR            output directory (default sopt-out)\n"
      "  --hlsl            read every input as plain HLSL (default: by extension)\n"
      "  --glsl            read every input as a GLSL fragment shader (default: by extension)\n"
      "  --entry NAME      HLSL / GLSL entry point (default main)\n"
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
      "  --cost-model M    rdna3 | amd-rdna2 | amd-rdna4 | amd-gcn5 | amd-terascale2 | nvidia | nvidia-maxwell | nvidia-pascal | nvidia-turing | nvidia-ampere | nvidia-blackwell | intel-gen12 | intel-gen9 | intel-gen7.5 | generic (default rdna3)\n"
      "  --isa             measure original and variants with fxstat + RGA (AMD); variants\n"
      "                    must be cheaper for some measured vendor ($SOPT_FXSTAT, $SOPT_RGA)\n"
      "  --sass            same with ptxas + nvdisasm (NVIDIA; $SOPT_PTXAS, $SOPT_NVDISASM)\n"
      "  --sm N            NVIDIA target for --sass (default 89)\n"
      "  --backends        compile original and variants per backend and compare after the\n"
      "                    compilers' optimizers: SPIR-V (fxstat, $SOPT_FXSTAT; spirv-dis,\n"
      "                    $SOPT_SPIRV_DIS) and DXBC via Microsoft's fxc ($SOPT_FXC =\n"
      "                    sopt-fxc.exe, default: next to sopt-fx; run with $SOPT_WINE,\n"
      "                    default wine, off Windows)\n"
      "  --export-spirv DIR  write original and variants as the SPIR-V ReShade hands the driver\n"
      "                    (fxstat, $SOPT_FXSTAT) to DIR, for ShaderInfo --batch DIR on a PC\n"
      "  --driver-stats F  the driver statistics ShaderInfo --batch wrote (F, with the export's\n"
      "                    manifest.txt next to it): Intel instruction counts as a measured vendor\n"
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
      "  --no-amd-folds    rdna3 without AMD context effects (default: x * +-2/4/0.5 is an output\n"
      "                    modifier, min/max over min/max one instruction)\n"
      "  --no-two-phase    one phase with the slack (default: only strictly cheaper hits first,\n"
      "                    the deepest search, then the slack's alternatives near the best in\n"
      "                    the last quarter of the time)\n"
      "  --bits            also enumerate integer ops and bit casts (asuint, asfloat, & | ^ << >>)\n"
      "  --no-library      no rule library (default: rewrite with library/rewrites.txt or\n"
      "                    $SOPT_LIBRARY before the search; the forms are candidates and seed it)\n"
      "  --library-file F  the rule library in F\n"
      "  --no-top-down     no top-down split (default: after each level, look up the missing\n"
      "                    operand b of op(a, b) = target for each new entry a: add, sub, mul, div)\n"
      "  --slack N         best-so-far bound: keep hits and parts of hits up to N above the\n"
      "                    cheapest hit found so far (default 1; 0 when the bank is full, -1 when\n"
      "                    it is also past half the time); --no-best-bound: bound = the original\n"
      "  --no-cuts         no cut points (default: when the search hits a limit, split the\n"
      "                    target at values the rest depends on for all inputs below them\n"
      "                    and search both parts on their own, --cut-time S per part, default 1)\n"
      "  --quant-oe N      quantized dedup: values equal after rounding away the low N\n"
      "                    mantissa bits on the test points count as one (default 0 = bit-exact)\n"
      "  --no-schedule     no scheduling measures (default: for regions with uniforms or texture\n"
      "                    fetches, reshaped forms (math grouped by rate, the last fetch entering\n"
      "                    last) are candidates, ties are broken by the performance mode cost and\n"
      "                    the tail after the last fetch, and variants faster in performance mode\n"
      "                    or with a shorter tail are written as \"(not faster)\" variants)\n"
      "  --perf-mode-first the performance mode cost (uniforms are constants) is the main cost\n"
      "  --no-classic      no classical source rewrites (default: local arrays of constants\n"
      "                    indexed at run time become static const tables, switch SOPT_<file>_T<line>;\n"
      "                    pixel shader math of uniforms only, or affine in the texture coordinates,\n"
      "                    moves to the vertex shader, switch SOPT_<file>_V<line>; kept where --isa\n"
      "                    measures a gain)\n"
      "  --no-hoist        no moving to the vertex shader (the table rewrites stay)\n"
      "  --no-blend-stage  no final back buffer blends as blend states (default: a pixel shader\n"
      "                    that returns lerp / multiply / add / screen / min / max of the back buffer\n"
      "                    at its pixel returns the blend's source, switch SOPT_<file>_B<line>)\n"
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
      "  --no-buffer-inputs bake BUFFER_WIDTH / BUFFER_HEIGHT in (and skip regions that depend\n"
      "                    on them) instead of compile-time inputs in [1, max width]\n"
      "  --max-width N     largest render target width: SV_Position in [0, N], texture\n"
      "                    coordinates within 0.01 px at N (default 7680 = 8K; the\n"
      "                    hardware limit is 16384)\n"
      "  --no-format-checks  no back buffer format checks (default: variants of regions that\n"
      "                    read or write the back buffer are also checked for 10-bit and scRGB\n"
      "                    back buffers and guarded where they fail)");
}

void collect(const fs::path& p, std::vector<fs::path>& out) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) {
    std::vector<fs::path> sub;
    for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); ++it)
      if (it->is_regular_file() && (it->path().extension() == ".fx" || it->path().extension() == ".hlsl" ||
                                    it->path().extension() == ".frag" || it->path().extension() == ".fs"))
        sub.push_back(it->path());
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
  bool forceHlsl = false;     // --hlsl
  bool forceGlsl = false;     // --glsl
  std::string entry = "main";  // --entry
  fx::RegionOptions ropt;
  bool formatChecks = true;  // back buffer formats: 10-bit and scRGB checks, see below
  bool classic = true;       // classical source rewrites (fx/classic.hpp), --no-classic
  bool hoist = true;         // ... and moving math to the vertex shader (fx/hoist.hpp), --no-hoist
  bool blendStage = true;    // ... and final back buffer blends as blend states (fx/blend.hpp), --no-blend-stage
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
  bool isa = false, sass = false, backends = false, allowAssumed = false, ask = false, symbolic = true,
       bufferInputs = true;
  fs::path factsFile, exportSpirv;
  std::vector<fs::path> driverStatsFiles;
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
  else {
    // The SweetOpt download ships sopt-fxc.exe next to sopt-fx (owner, 2026-10-06).
    const std::string dir = executableDir();
    std::error_code ec;
    if (!dir.empty() && std::filesystem::exists(std::filesystem::path(dir) / "sopt-fxc.exe", ec))
      backCfg.fxc = (std::filesystem::path(dir) / "sopt-fxc.exe").string();
  }
#ifndef _WIN32
  backCfg.wine = "wine";
#endif
  if (const char* v = std::getenv("SOPT_WINE")) backCfg.wine = v;
  bool noAmdFolds = false;
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
    else if (a == "--hlsl") forceHlsl = true;
    else if (a == "--glsl") forceGlsl = true;
    else if (a == "--entry") entry = next();
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
    else if (a == "--no-top-down") opt.search.topDown = false;
    else if (a == "--library") opt.library = true;
    else if (a == "--no-library") opt.library = false;
    else if (a == "--bits") opt.search.bits = true;
    else if (a == "--two-phase") opt.search.twoPhase = true;
    else if (a == "--no-two-phase") opt.search.twoPhase = false;
    else if (a == "--no-amd-folds") noAmdFolds = true;
    else if (a == "--library-file") {
      static Library lib;  // alive for the whole run
      const char* f = next();
      try {
        lib = loadLibrary(f);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
      }
      opt.library = true;
      opt.libraryRules = &lib;
    }
    else if (a == "--disk") opt.search.diskDir = next();
    else if (a == "--disk-max") opt.search.diskBudget = static_cast<size_t>(std::strtod(next(), nullptr) * 1073741824.0);
    else if (a == "--max-mem") opt.search.memBudget = static_cast<size_t>(std::strtod(next(), nullptr) * 1048576.0);
    else if (a == "--tests") opt.numTests = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--no-v3") opt.v3 = false;
    else if (a == "--no-schedule") opt.schedule = false;
    else if (a == "--perf-mode-first") opt.perfFirst = true;
    else if (a == "--v3-time") opt.v3Time = std::strtod(next(), nullptr);
    else if (a == "--quant-oe") opt.search.quantBits = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--cut-time") opt.cutTime = std::strtod(next(), nullptr);
    else if (a == "--no-shared-leaves") opt.search.sharedLeaves = false;
    else if (a == "--subtree-time") opt.subtreeTime = std::strtod(next(), nullptr);
    else if (a == "--subtree-max-cost") opt.subtreeMaxCost = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--facts") factsFile = next();
    else if (a == "--ask") ask = true;
    else if (a == "--no-macro-inputs") symbolic = false;
    else if (a == "--buffer-inputs") bufferInputs = true;
    else if (a == "--no-buffer-inputs") bufferInputs = false;
    else if (a == "--max-width") ropt.maxWidth = std::strtod(next(), nullptr);
    else if (a == "--no-format-checks") formatChecks = false;
    else if (a == "--no-classic") classic = false;
    else if (a == "--no-hoist") hoist = false;
    else if (a == "--no-blend-stage") blendStage = false;
    else if (a == "--sass") sass = true;
    else if (a == "--backends") backends = true;
    else if (a == "--export-spirv") exportSpirv = next();
    else if (a == "--driver-stats") driverStatsFiles.push_back(next());
    else if (a == "--sm") sassCfg.sm = std::atoi(next());
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (a == "--version") { std::puts("sopt-fx (SweetOpt) " SOPT_VERSION); return 0; }
    else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    else collect(a, inputs);
  }
  if (noAmdFolds) opt.search.model = withoutAmdFolds(opt.search.model);
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

  const console::Style con = console::init();
  console::titleBox(con, std::string("SweetOpt ") + SOPT_VERSION + "  -  the super sweet shader optimizer  -  by CeeJay.dk");
  console::section(con, "Reading " + std::to_string(inputs.size()) + " effect file" + (inputs.size() == 1 ? "" : "s"));

  // Front end: every effect twice (two resolutions, see extractRegions).
  fx::ReportInfo info;
  info.costModel = std::string(opt.search.model->name);
  info.perfFirst = opt.perfFirst;
  std::vector<fx::RegionResult> results;
  std::vector<std::pair<fs::path, std::vector<std::string>>> effectFiles;  // effect, its sources
  std::vector<fx::SourceRewrite> rewrites;  // classical rewrites (--no-classic)
  std::vector<fs::path> rewriteEffects;     // the effect each one was found in
  // Plain HLSL by extension (or --hlsl), with the entry point.
  auto loadFor = [&](const fs::path& p) {
    fx::LoadOptions o = load;
    const std::string ext = p.extension().string();
    o.glsl = forceGlsl || ext == ".frag" || ext == ".fs" || ext == ".glsl";
    o.hlsl = !o.glsl && (forceHlsl || ext == ".hlsl" || ext == ".hlsli");
    o.entry = entry;
    return o;
  };
  // Fact keys that apply to every read: HLSL textures (no format, no range) and groupshared
  // variables, with what kind of read they are.
  std::map<std::string, std::string> resourceKeys;
  auto extractAll = [&]() {
    info.effects.clear();
    info.failed.clear();
    info.skipped = fx::SkipCount();
    info.skipped.keepDetails = skips;
    results.clear();
    effectFiles.clear();
    rewrites.clear();
    rewriteEffects.clear();
    std::set<std::pair<std::string, std::string>> rewriteSeen;  // (file, switch tag + line)
    resourceKeys.clear();
    std::set<std::tuple<std::string, uint32_t, size_t>> seen;
    for (const auto& p : inputs) {
      std::string err;
      const fx::LoadOptions load = loadFor(p);
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
      // BUFFER_WIDTH / BUFFER_HEIGHT too (int compile-time inputs), except where a
      // constant is needed.
      if (symbolic && bufferInputs && !load.hlsl && !load.glsl) {
        fx::LoadOptions bs = sym;
        std::string bsErr;
        if (auto s = fx::loadEffectBufferSymbolic(p, bs, bsErr)) {
          fx = std::move(s);
          sym = bs;
        }
      }
      fx::LoadOptions alt = sym;
      alt.width = fx::kAltWidth;
      alt.height = fx::kAltHeight;
      std::string altErr;
      // Plain HLSL has no BUFFER_WIDTH / BUFFER_HEIGHT: no second parse.
      auto fx2 = load.hlsl || load.glsl ? nullptr : fx::loadEffect(p, alt, altErr);
      info.effects.push_back(p.string());
      effectFiles.emplace_back(p, fx->sourceFiles);
      if (classic && !load.hlsl && !load.glsl)
        for (auto& rw : fx::tableRewrites(*fx))
          if (rewriteSeen.insert({rw.file, rw.tag + std::to_string(rw.line)}).second) {  // a header shared by several effects
            rewriteEffects.push_back(p);
            rewrites.push_back(std::move(rw));
          }
      if (fx->hlsl)
        for (const auto& t : fx::unrangedTextures(*fx))
          resourceKeys[fx::textureFactKey(*fx, t)] = fx->hlslBuffers.count(t) ? "buffer read" : "texture read";
      for (const auto& g : fx::groupsharedVariables(*fx))
        resourceKeys[fx::groupsharedFactKey(*fx, g)] = "groupshared read";
      // The regions once more with the back buffer as scRGB: inputs whose range changes
      // depend on the back buffer (checked for HDR below).
      std::map<std::tuple<std::string, uint32_t, size_t>, fx::Region> hdr;
      if (formatChecks) {
        fx::RegionOptions hopt = ropt;
        hopt.hdrBackBuffer = true;
        fx::SkipCount hs;
        for (auto& h : fx::extractRegions(*fx, fx2.get(), hopt, hs))
          hdr.emplace(std::make_tuple(h.file, h.line, h.removed.size()), std::move(h));
      }
      std::vector<fx::Region> effectRegions;  // for the vertex shader rewrites
      for (auto& r : fx::extractRegions(*fx, fx2.get(), ropt, info.skipped)) {
        if (classic && hoist && !fx->hlsl) effectRegions.push_back(r);
        if (!seen.insert({r.file, r.line, r.removed.size()}).second) continue;  // shared header
        fx::RegionResult rr;
        rr.effect = p.string();
        if (formatChecks) {
          const auto h = hdr.find({r.file, r.line, r.removed.size()});
          bool bb = false;  // reads the back buffer directly
          for (const auto& f : r.facts) bb = bb || f.source.rfind("BackBuffer", 0) == 0;
          if (h != hdr.end() && h->second.prog.inputs.size() == r.prog.inputs.size() &&
              toString(h->second.prog.target, h->second.prog.inputs) == toString(r.prog.target, r.prog.inputs)) {
            bool differs = false;
            for (size_t k = 0; k < r.prog.inputs.size(); ++k)
              differs = differs || h->second.prog.inputs[k].lo != r.prog.inputs[k].lo ||
                        h->second.prog.inputs[k].hi != r.prog.inputs[k].hi;
            if (differs) rr.hdrInputs = h->second.prog.inputs;
          } else if (bb) {
            // Not found the same way: the back buffer inputs at the scRGB range.
            rr.hdrInputs = r.prog.inputs;
            for (size_t k = 0; k < r.facts.size() && k < rr.hdrInputs.size(); ++k)
              if (r.facts[k].source.rfind("BackBuffer", 0) == 0)
                rr.hdrInputs[k].lo = fx::kScRgbLo, rr.hdrInputs[k].hi = fx::kScRgbHi, rr.hdrInputs[k].grid = 0;
          }
        }
        rr.targetCost = dagCost(r.prog.target, *opt.search.model, r.prog.inputs);
        rr.region = std::move(r);
        results.push_back(std::move(rr));
      }
      std::vector<fx::SourceRewrite> more;
      if (hoist) more = fx::hoistRewrites(*fx, effectRegions, *opt.search.model);
      if (blendStage)
        for (auto& rw : fx::blendRewrites(*fx, ropt, opt.seed)) more.push_back(std::move(rw));
      for (auto& rw : more)
        if (rewriteSeen.insert({rw.file, rw.tag + std::to_string(rw.line)}).second) {
          rewriteEffects.push_back(p);
          rewrites.push_back(std::move(rw));
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
                        toString(rr.region.prog.target, rr.region.prog.inputs) + rr.region.rhs;
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
    bool header = false;
    for (const auto& [key, what] : resourceKeys) {
      if (userRanges.count(key) || missing.count(key)) continue;
      if (!header) f << "\n# HLSL textures and buffers, groupshared memory (no known range; applies to every read):\n";
      header = true;
      f << "# " << key << " = [0, 1]   # " << what << "\n";
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
      std::printf("%s:%s%u  %s %s%s  (cost %u)\n", r.file.c_str(),
                  r.removed.empty() ? "" : (std::to_string(r.removed.front().first) + "-").c_str(), r.line,
                  r.lhs.c_str(), (r.glsl ? toGlsl : toString)(r.prog.target, r.prog.inputs).c_str(), r.rhs.c_str(), rr.targetCost);
      std::printf("    budget %s (%s)\n", fx::budgetString(r.prog.budget).c_str(), r.budgetReason.c_str());
      if (!r.guard.empty()) std::printf("    only while %s\n", r.guard.c_str());
      for (size_t k = 0; k < r.prog.inputs.size(); ++k) {
        const auto& d = r.prog.inputs[k];
        std::printf("    %s in [%g, %g]%s  %s%s\n", d.name.c_str(), d.lo, d.hi,
                    d.grid ? (" grid " + std::to_string(d.grid)).c_str() : "", r.facts[k].source.c_str(),
                    r.facts[k].assumed ? " (assumed)" : "");
      }
    }
    for (const auto& rw : rewrites)
      std::printf("rewrite %s:%u (%s)  %s\n", rw.file.c_str(), rw.line, rw.function.c_str(), rw.description.c_str());
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
      std::snprintf(buf, sizeof(buf), "|%d %.9g %.9g %u %d %.9g %d %u", static_cast<int>(d.type), d.lo, d.hi,
                    d.grid, d.compileTime ? 1 : 0, d.value, static_cast<int>(d.rate), d.fetchOrder);
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
  {
    char head[160];
    std::snprintf(head, sizeof(head), "Searching %zu region%s (%g s search time each, %u at a time)", unique.size(),
                  unique.size() == 1 ? "" : "s", opt.search.timeLimitSec,
                  static_cast<unsigned>(std::min<size_t>(jobs, std::max<size_t>(unique.size(), 1))));
    console::section(con, head);
  }
  console::Progress searchBar(con, unique.size());
  parallelFor(unique.size(), jobs, [&](size_t u) {
    const size_t i = unique[u];
    const auto s0 = std::chrono::steady_clock::now();
    searched[i] = optimize(results[i].region.prog, ropt2);
    searchSec[i] = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
    searchBar.step();
  });
  searchBar.finish();
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
  console::section(con, "Results");
  parallelFor(results.size(), jobs, [&](size_t i) {
    fx::RegionResult& rr = results[i];
    const auto s0 = std::chrono::steady_clock::now();
    RunResult res = searched[firstOf[i]];
    if (firstOf[i] != i || rr.region.glsl)  // the same programs over this region's input names; GLSL syntax
      for (auto& a : res.accepted)
        a.text = rr.region.glsl ? toGlsl(a.expr, rr.region.prog.inputs) : toString(a.expr, rr.region.prog.inputs);
    rr.targetCost = res.targetCost;
    rr.limitHit = res.search.limitHit;
    rr.completedCost = res.search.completedCost;
    rr.maxLevel = res.search.maxLevel;
    // Compiled costs in the main mode and the other one (performance mode: uniforms folded).
    const std::vector<InputDecl> perfIns = perfInputs(rr.region.prog.inputs);
    const std::vector<InputDecl>& mainIns = opt.perfFirst ? perfIns : rr.region.prog.inputs;
    const std::vector<InputDecl>& otherIns = opt.perfFirst ? rr.region.prog.inputs : perfIns;
    const uint32_t targetCompiled = fx::compiledCost(rr.region.prog.target, *opt.search.model, mainIns);
    const uint32_t targetOtherCompiled = fx::compiledCost(rr.region.prog.target, *opt.search.model, otherIns);
    rr.schedule = opt.schedule && hasScheduleInputs(rr.region.prog.inputs);
    rr.targetOtherCost = opt.perfFirst ? res.targetNormalCost : res.targetPerfCost;
    rr.targetTail = res.targetTail;
    // A texture fetch input is its call text: a variant must not repeat it more often.
    auto count = [](const std::string& text, const std::string& what) {
      size_t n = 0;
      for (size_t p = text.find(what); p != std::string::npos; p = text.find(what, p + what.size())) ++n;
      return n;
    };
    const std::string targetText = (rr.region.glsl ? toGlsl : toString)(rr.region.prog.target, rr.region.prog.inputs);
    for (const auto& a : res.accepted) {
      if (a.cost >= res.targetCost && !a.moreAccurate && !a.otherModeFaster && !a.betterScheduling) continue;
      bool moreFetches = false;
      for (size_t k = 0; k < rr.region.facts.size(); ++k)
        if (rr.region.facts[k].fetch) {
          const std::string& nm = rr.region.prog.inputs[k].name;
          moreFetches = moreFetches || count(a.text, nm) > count(targetText, nm);
        }
      if (moreFetches) continue;
      // The same code as the original once printed (a - c and a + -c print alike): no variant.
      if (a.text == targetText) continue;
      const uint32_t compiled = fx::compiledCost(a.expr, *opt.search.model, mainIns);
      const bool cheaper = compiled < targetCompiled;
      // Not faster but maybe fewer registers (owner, 2026-10-04): only measurement shows it.
      const bool registerCandidate = (isa || sass) && compiled <= targetCompiled + opt.accuracySlack;
      // Scheduling (owner, 2026-10-08): faster in the other mode, or a shorter tail at no more cost.
      const bool perfFaster = rr.schedule && !cheaper && a.otherModeFaster && compiled <= targetCompiled + opt.accuracySlack &&
                              fx::compiledCost(a.expr, *opt.search.model, otherIns) < targetOtherCompiled;
      const bool betterScheduling = rr.schedule && !cheaper && a.betterScheduling && compiled <= targetCompiled;
      if (!cheaper && !(a.moreAccurate && compiled <= targetCompiled + opt.accuracySlack) && !registerCandidate &&
          !perfFaster && !betterScheduling) {
        ++rr.onlyContraction;
        continue;
      }
      fx::Variant v;
      v.moreAccurate = a.moreAccurate;
      v.notFaster = !cheaper;
      v.perfFirst = opt.perfFirst;
      v.perfFaster = perfFaster;
      v.betterScheduling = betterScheduling;
      v.otherCost = opt.perfFirst ? a.normalCost : a.perfCost;
      v.tail = a.tail;
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
    std::vector<fx::Variant> strict, loose, accurate, registers, scheduling;
    for (auto& v : rr.variants)
      (v.notFaster ? (v.moreAccurate                            ? accurate
                      : v.perfFaster || v.betterScheduling       ? scheduling
                                                                 : registers)
                   : (v.klass == Klass::LessAccurate ? loose : strict))
          .push_back(std::move(v));
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
    // Register candidates (measured below): the cheapest two accurate ones.
    registers.erase(std::remove_if(registers.begin(), registers.end(),
                                   [](const fx::Variant& v) { return v.klass == Klass::LessAccurate; }),
                    registers.end());
    std::stable_sort(registers.begin(), registers.end(), byCost);
    if (registers.size() > 2) registers.resize(2);
    // Scheduling variants: the two best by the other mode's cost, then the tail.
    std::stable_sort(scheduling.begin(), scheduling.end(), [](const fx::Variant& a, const fx::Variant& b) {
      if (a.otherCost != b.otherCost) return a.otherCost < b.otherCost;
      if (a.tail != b.tail) return a.tail < b.tail;
      return a.cost < b.cost;
    });
    if (scheduling.size() > 2) scheduling.resize(2);
    rr.variants = std::move(strict);
    for (auto& v : loose) rr.variants.push_back(std::move(v));
    for (auto& v : accurate) rr.variants.push_back(std::move(v));
    for (auto& v : registers) rr.variants.push_back(std::move(v));
    for (auto& v : scheduling) rr.variants.push_back(std::move(v));
    rr.sec = searchSec[firstOf[i]] + std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
    std::lock_guard<std::mutex> lock(printMu);
    const size_t n = ++done;
    std::printf("%s[%zu/%zu]%s %s:%u cost %u -> %s%s%s\n", con.c("\x1b[90m"), n, results.size(), con.reset(),
                fs::path(rr.region.file).filename().string().c_str(), rr.region.line, rr.targetCost,
                rr.variants.empty() ? con.c("\x1b[90m") : con.c("\x1b[1;92m"),
                rr.variants.empty() ? "-" : std::to_string(rr.variants[0].cost).c_str(), con.reset());
    std::fflush(stdout);
  });

  // Driver statistics round trip (owner, 2026-10-04): export what ReShade hands the driver, read back
  // the Intel driver's instruction counts (ShaderInfo --batch on the PC), matched by region and text.
  auto regionInputs = [](const fx::RegionResult& rr) {
    std::vector<InputDecl> ins = rr.region.prog.inputs;
    for (size_t k = 0; k < ins.size(); ++k) ins[k].name = "sopt_in" + std::to_string(k);
    return ins;
  };
  if (!exportSpirv.empty()) {
    if (isaCfg.fxstat.empty()) {
      std::fprintf(stderr, "--export-spirv needs fxstat ($SOPT_FXSTAT)\n");
      return 1;
    }
    std::string manifest, err;
    size_t shaders = 0;
    for (const auto& rr : results) {
      if (rr.variants.empty()) continue;
      std::vector<const Expr*> exprs = {&rr.region.prog.target};
      for (const auto& v : rr.variants) exprs.push_back(&v.expr);
      std::string name = fs::path(rr.region.file).stem().string() + "_" + std::to_string(rr.region.line);
      for (char& ch : name)
        if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
      const auto names = exportDriverShaders(exprs, regionInputs(rr), isaCfg.fxstat, exportSpirv, name, err);
      for (size_t k = 0; k < names.size(); ++k) {
        if (names[k].empty()) continue;
        ++shaders;
        manifest += names[k] + "\t" + driverKey(rr.region.file, rr.region.line, k ? rr.variants[k - 1].text : "orig") + "\n";
      }
    }
    std::ofstream(exportSpirv / "manifest.txt", std::ios::binary) << manifest;
    std::printf("%zu shaders exported to %s (for ShaderInfo --batch)%s%s\n", shaders, exportSpirv.string().c_str(),
                err.empty() ? "" : "; first error: ", err.c_str());
  }
  bool intel = false;
  for (const fs::path& file : driverStatsFiles) {
    DriverStats ds;
    std::string err;
    if (!loadDriverStats(file, ds, err)) {
      std::fprintf(stderr, "--driver-stats: %s\n", err.c_str());
      return 1;
    }
    if (ds.vendor != 0x8086) {
      std::printf("--driver-stats %s: %s is not an Intel GPU; only Intel counts are used so far\n", file.string().c_str(),
                  ds.gpu.c_str());
      continue;
    }
    size_t matched = 0;
    auto lookup = [&](const fx::RegionResult& rr, const std::string& text) {
      const auto it = ds.byKey.find(driverKey(rr.region.file, rr.region.line, text));
      return it == ds.byKey.end() ? -1 : driverCost(ds, it->second);
    };
    for (auto& rr : results) {
      if (rr.variants.empty()) continue;
      rr.targetIntel = lookup(rr, "orig");
      for (auto& v : rr.variants) matched += (v.intel = lookup(rr, v.text)) >= 0;
    }
    intel = true;
    std::printf("Intel driver statistics (%s): %zu variants matched\n", ds.gpu.c_str(), matched);
  }
  info.intel = intel;

  // Measured machine code: a variant stays if some measured vendor gets faster (it may be
  // slower on another: the report shows both); equal or slower everywhere means no gain,
  // equal usually because the compiler already does it.
  if (isa || sass || intel) {
    info.amd = isa;
    info.nv = sass;
    size_t n = 0, toMeasure = 0;
    for (const auto& rr : results) toMeasure += !rr.variants.empty();
    bool anyGlsl = false, anyFx = false;
    for (const auto& rr : results)
      if (!rr.variants.empty()) (rr.region.glsl ? anyGlsl : anyFx) = true;
    std::string what = !isa ? "" : anyGlsl && !anyFx ? "AMD: RGA (GLSL)" : anyGlsl ? "AMD: fxstat + RGA, RGA (GLSL)" : "AMD: fxstat + RGA";
    if (sass) what += std::string(what.empty() ? "" : ", ") + "NVIDIA: ptxas";
    if (intel) what += std::string(what.empty() ? "" : ", ") + "Intel: driver statistics";
    console::section(con, "Measuring machine code (" + what + ")");
    console::Progress measureBar(con, toMeasure);
    for (auto& rr : results) {
      if (rr.variants.empty()) continue;
      std::vector<InputDecl> ins = rr.region.prog.inputs;
      for (size_t k = 0; k < ins.size(); ++k) ins[k].name = "sopt_in" + std::to_string(k);
      // Performance mode as the compiler sees it: uniforms are constants (a typical value: not 0, 1 or 0.5,
      // which would fold more than a user's setting). One mode is measured as the main one, the other
      // for the scheduling measures (regions with uniforms).
      std::vector<InputDecl> perfMeas = ins;
      bool uniforms = false;
      for (auto& d : perfMeas)
        if (d.rate == InputDecl::Rate::Uniform) {
          d.compileTime = true;
          d.value = d.lo + 0.37 * (d.hi - d.lo);
          uniforms = true;
        }
      const bool otherMode = rr.schedule && uniforms;
      if (opt.perfFirst && uniforms) std::swap(ins, perfMeas);  // main = performance mode
      std::vector<const Expr*> exprs = {&rr.region.prog.target};
      for (const auto& v : rr.variants) exprs.push_back(&v.expr);
      IsaConfig regionIsa = isaCfg;
      regionIsa.glsl = rr.region.glsl;  // GLSL: RGA's own GLSL path, as the game's driver would see it
      if (isa && otherMode) {
        const auto m = measureIsa(exprs, perfMeas, regionIsa);
        rr.targetAmdOther = m[0].ok ? m[0].cost : -1;
        for (size_t k = 0; k < rr.variants.size(); ++k) rr.variants[k].amdOther = m[k + 1].ok ? m[k + 1].cost : -1;
      }
      if (sass && otherMode) {
        const auto m = measureSass(exprs, perfMeas, sassCfg);
        rr.targetNvOther = m[0].ok ? m[0].cost : -1;
        for (size_t k = 0; k < rr.variants.size(); ++k) rr.variants[k].nvOther = m[k + 1].ok ? m[k + 1].cost : -1;
      }
      if (isa) {
        const auto m = measureIsa(exprs, ins, regionIsa);
        rr.targetAmd = m[0].ok ? m[0].cost : -1;
        if (!m[0].ok) std::fprintf(stderr, "%s:%u: amd: %s\n", rr.region.file.c_str(), rr.region.line, m[0].error.c_str());
        for (size_t k = 0; k < rr.variants.size(); ++k) rr.variants[k].amd = m[k + 1].ok ? m[k + 1].cost : -1;
        auto regs = [](const IsaCost& c, int v) { return c.ok && v >= 0 ? v : -1; };
        rr.targetAmdVgprs = regs(m[0], m[0].vgprs);
        rr.targetAmdSgprs = regs(m[0], m[0].sgprs);
        for (size_t k = 0; k < rr.variants.size(); ++k) {
          rr.variants[k].amdVgprs = regs(m[k + 1], m[k + 1].vgprs);
          rr.variants[k].amdSgprs = regs(m[k + 1], m[k + 1].sgprs);
        }
      }
      if (sass) {
        const auto m = measureSass(exprs, ins, sassCfg);
        rr.targetNv = m[0].ok ? m[0].cost : -1;
        if (!m[0].ok) std::fprintf(stderr, "%s:%u: nv: %s\n", rr.region.file.c_str(), rr.region.line, m[0].error.c_str());
        for (size_t k = 0; k < rr.variants.size(); ++k) rr.variants[k].nv = m[k + 1].ok ? m[k + 1].cost : -1;
        rr.targetNvRegs = m[0].ok && m[0].regs > 0 ? m[0].regs : -1;
        for (size_t k = 0; k < rr.variants.size(); ++k)
          rr.variants[k].nvRegs = m[k + 1].ok && m[k + 1].regs > 0 ? m[k + 1].regs : -1;
      }
      std::vector<fx::Variant> kept;
      for (auto& v : rr.variants) {
        bool better = false, measured = false, close = true;
        for (auto [t, c] : {std::pair{rr.targetAmd, v.amd}, std::pair{rr.targetNv, v.nv}, std::pair{rr.targetIntel, v.intel}}) {
          if (t < 0 || c < 0) continue;
          measured = true;
          better = better || c < t;
          close = close && c <= t + 1;
        }
        // Registers (AMD VGPRs, NVIDIA registers per thread): fewer on a measured vendor, more on none.
        bool fewer = false, more = false;
        for (auto [t, c] : {std::pair{rr.targetAmdVgprs, v.amdVgprs}, std::pair{rr.targetNvRegs, v.nvRegs}}) {
          if (t < 0 || c < 0) continue;
          fewer = fewer || c < t;
          more = more || c > t;
        }
        v.fewerRegisters = fewer && !more;
        // Faster in the other mode: where that mode was measured, it must be measurably faster there.
        if (v.perfFaster) {
          bool otherMeasured = false, otherBetter = false;
          for (auto [t, c] : {std::pair{rr.targetAmdOther, v.amdOther}, std::pair{rr.targetNvOther, v.nvOther}}) {
            if (t < 0 || c < 0) continue;
            otherMeasured = true;
            otherBetter = otherBetter || c < t;
          }
          if (otherMeasured && !otherBetter) v.perfFaster = false;
        }
        const bool scheduling = v.perfFaster || v.betterScheduling;
        if (measured && !better && close && (v.moreAccurate || v.fewerRegisters || scheduling)) {
          v.notFaster = true;  // accuracy / register / scheduling variant: not faster, at most 1 instruction slower
          kept.push_back(std::move(v));
        } else if (measured && !better) {
          if (!v.notFaster || v.moreAccurate || scheduling) ++rr.measuredNoGain;  // register candidates never claimed a gain
        } else if (!measured && v.notFaster && !v.moreAccurate && !scheduling) {
          // a register candidate whose registers could not be measured
        } else {
          v.notFaster = false;
          kept.push_back(std::move(v));
        }
      }
      // Largest measured gain first (sum over vendors of the relative change), static
      // cost breaks ties.
      auto gain = [&](const fx::Variant& v) {
        double g = 0;
        for (auto [t, c] : {std::pair{rr.targetAmd, v.amd}, std::pair{rr.targetNv, v.nv}, std::pair{rr.targetIntel, v.intel}})
          if (t > 0 && c >= 0) g += double(t - c) / t;
        return g;
      };
      std::stable_sort(kept.begin(), kept.end(), [&](const fx::Variant& a, const fx::Variant& b) {
        if (a.notFaster != b.notFaster) return b.notFaster;  // accuracy / register variants last
        const double ga = gain(a), gb = gain(b);
        if (ga != gb) return ga > gb;
        return a.cost < b.cost;
      });
      rr.variants = std::move(kept);
      char line[512];
      std::snprintf(line, sizeof(line), "measured %zu: %s:%u amd %d nv %d intel %d, %zu variants kept", ++n,
                    fs::path(rr.region.file).filename().string().c_str(), rr.region.line, rr.targetAmd,
                    rr.targetNv, rr.targetIntel, rr.variants.size());
      measureBar.print(line);
      measureBar.step();
    }
    measureBar.finish();
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
        return k.cost <= v.cost && noWorse(k.amd, v.amd) && noWorse(k.nv, v.nv) && noWorse(k.intel, v.intel) &&
               noWorse(k.spirv, v.spirv) &&
               noWorse(k.dxbc, v.dxbc) && noWorse(k.amdVgprs, v.amdVgprs) && noWorse(k.nvRegs, v.nvRegs) &&
               err(k) <= err(v) && (k.problems.empty() || !v.problems.empty()) &&
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
    if (dropped) std::printf("%zu variants dropped: another variant of the region is as fast everywhere, as accurate and uses no more registers\n", dropped);
  }

  // Sampling cannot find a difference confined to a small part of a wide assumed range
  // (e.g. a ramp near 0 in [-1000, 1000]), so such variants are not written.
  if (!allowAssumed)
    for (auto& rr : results) {
      bool assumed = false;
      for (const auto& f : rr.region.facts) assumed = assumed || f.assumed;
      if (assumed && !rr.variants.empty()) rr.unwritten = std::move(rr.variants), rr.variants.clear();
    }

  // Back buffer formats (owner, 2026-09-30): the variants above were verified for an
  // 8-bit SDR back buffer. Regions that read it are checked for a 10-bit one (input grid
  // 1023; a back buffer output counts 10-bit codes) and for scRGB (FP16, inputs from
  // hdrInputs; a back buffer output within one FP16 ulp, rel 2^-11). A variant that fails
  // is written with a guard, so that those formats get the original.
  if (formatChecks) {
    const auto f0 = std::chrono::steady_clock::now();
    std::atomic<size_t> checked{0}, guard10{0}, guardHdr{0};
    parallelFor(results.size(), jobs, [&](size_t i) {
      auto& rr = results[i];
      if (rr.variants.empty()) return;
      const fx::Region& r = rr.region;
      const bool toBackBuffer = r.budgetReason.find("back buffer") != std::string::npos;
      Program p10 = r.prog;
      bool bb8 = false;
      for (size_t k = 0; k < r.facts.size() && k < p10.inputs.size(); ++k)
        if (r.facts[k].source.rfind("BackBuffer (8-bit", 0) == 0) p10.inputs[k].grid = 1023, bb8 = true;
      if (toBackBuffer && p10.budget.kind == Budget::Kind::Color8) p10.budget.kind = Budget::Kind::Color10, bb8 = true;
      Program ph = r.prog;
      const bool hdrCheck = !rr.hdrInputs.empty() || toBackBuffer;
      if (!rr.hdrInputs.empty()) ph.inputs = rr.hdrInputs;
      if (toBackBuffer && (ph.budget.kind == Budget::Kind::Color8 || ph.budget.kind == Budget::Kind::Color10)) {
        ph.budget.kind = Budget::Kind::Rel;
        ph.budget.eps = 1.0 / 2048.0;
      }
      auto passes = [&](Program p, const fx::Variant& v) {
        p.budget.vsExact = p.budget.vsExact && opt.exactRule;
        p.budget.loose = v.klass == Klass::LessAccurate ? opt.loose : 0.0;
        const uint64_t dom = domainSize(p, uint64_t{1} << 22);
        const PointSet ps = dom ? PointSet() : makeRandomPoints(p, size_t{1} << 18, opt.seed + 5, true);
        for (const auto& prof : kAllProfiles) {
          const Metrics m = dom ? compareExhaustive(p, v.expr, prof, 1) : compare(p, v.expr, ps, prof, 1);
          if (!m.loosePass) return false;
        }
        return true;
      };
      for (auto& v : rr.variants) {
        std::string g;
        if (bb8 && !passes(p10, v)) g = "BUFFER_COLOR_BIT_DEPTH == 8", ++guard10;
        if (hdrCheck && !passes(ph, v)) g += std::string(g.empty() ? "" : " && ") + "BUFFER_COLOR_SPACE <= 1", ++guardHdr;
        v.formatGuard = g;
        checked += bb8 || hdrCheck;
      }
    });
    std::printf("back buffer formats: %zu variants checked, %zu only for 8-bit, %zu only for SDR (%.1f s)\n",
                checked.load(), guard10.load(), guardHdr.load(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - f0).count());
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
  // Classical rewrites: each one's switch off and on, the effect it was found in, measured with RGA
  // (pixel and compute shaders summed) without and with performance mode.
  // Written alone into a scratch folder (with the effect copied next to its changed headers); kept
  // only where it measurably helps (fewer instructions or less scratch, with or without performance
  // mode): arrays read in loops the compiler unrolls fold anyway.
  if (isa && !rewrites.empty()) {
    const fs::path tmp = outDir / ".sopt-rewrite-check";
    std::string tmpErrors;
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fx::writeVariants({}, tmp, tmpErrors, rewrites);
    // Pixel / compute shader ISA of src with defines (RGA), summed: cost, scratch, max VGPRs.
    struct Isa {
      int cost = -1, scratch = -1, vgprs = -1, vmem = -1;
    };
    auto measure = [&](const fs::path& src, const std::string& inc, const std::string& defines, bool perf) {
      int status = 0;
      const std::string json = runCommand(quote(isaCfg.fxstat) + " --json --rga " + quote(isaCfg.rga) + inc +
                                              (perf ? " --performance-mode" : " --no-performance-mode") + defines + " " +
                                              quote(src.string()) + " 2>/dev/null",
                                          status);
      Isa r;
      for (size_t p = json.find("\"stage\": \""); p != std::string::npos; p = json.find("\"stage\": \"", p + 1)) {
        const std::string stage = json.substr(p + 10, 7);
        if (stage != "pixel\"," && stage != "compute") continue;
        const size_t isaPos = json.find("\"isa\":", p);
        if (isaPos == std::string::npos) continue;
        const size_t end = json.find('}', isaPos);
        auto get = [&](const char* key) {
          const std::string k2 = std::string("\"") + key + "\":";
          const size_t q = json.find(k2, isaPos);
          return q == std::string::npos || q > end ? -1 : std::atoi(json.c_str() + q + k2.size());
        };
        const int c = get("cost"), sc = get("scratch"), v = get("vgprs"), vm = get("vmem");
        if (c < 0) continue;
        r.cost = (r.cost < 0 ? 0 : r.cost) + c;
        r.vmem = (r.vmem < 0 ? 0 : r.vmem) + std::max(vm, 0);
        r.scratch = (r.scratch < 0 ? 0 : r.scratch) + std::max(sc, 0);
        r.vgprs = std::max(r.vgprs, v);
      }
      return r;
    };
    // Where each rewrite is compiled from; the original state (every switch 0) once per effect and mode.
    std::vector<fs::path> srcs(rewrites.size());
    std::vector<std::string> incs(rewrites.size());
    std::map<std::string, size_t> effectIndex;  // src -> index into befores
    std::vector<std::pair<fs::path, std::string>> effects;
    for (size_t k = 0; k < rewrites.size(); ++k) {
      const fs::path& eff = rewriteEffects[k];
      srcs[k] = tmp / eff.filename();
      if (!fs::exists(srcs[k], ec)) fs::copy_file(eff, srcs[k], fs::copy_options::overwrite_existing, ec);
      if (ec) srcs[k] = eff;
      incs[k] = " -I " + quote(tmp.string()) + " -I " + quote(eff.parent_path().string());
      for (const auto& d : loadFor(eff).includePaths) incs[k] += " -I " + quote(d.string());
      if (effectIndex.emplace(srcs[k].string(), effects.size()).second) effects.emplace_back(srcs[k], incs[k]);
    }
    std::vector<Isa> before(2 * effects.size()), after(2 * rewrites.size());
    const size_t numJobs = before.size() + after.size();
    parallelFor(numJobs, jobs, [&](size_t j) {
      if (j < before.size()) {
        const auto& [src, inc] = effects[j / 2];
        before[j] = measure(src, inc, "", j % 2 == 1);
      } else {
        const size_t i = j - before.size(), k = i / 2;
        after[i] = measure(srcs[k], incs[k], " -D " + fx::rewriteSwitch(rewrites[k]) + "=1", i % 2 == 1);
      }
    });
    for (size_t k = 0; k < rewrites.size(); ++k) {
      fx::SourceRewrite& rw = rewrites[k];
      const size_t e = effectIndex[srcs[k].string()];
      rw.amdBefore = before[2 * e].cost;
      rw.scratchBefore = before[2 * e].scratch;
      rw.vgprBefore = before[2 * e].vgprs;
      rw.vmemBefore = before[2 * e].vmem;
      rw.vmemAfter = after[2 * k].vmem;
      rw.amdPerfBefore = before[2 * e + 1].cost;
      rw.amdAfter = after[2 * k].cost;
      rw.scratchAfter = after[2 * k].scratch;
      rw.vgprAfter = after[2 * k].vgprs;
      rw.amdPerfAfter = after[2 * k + 1].cost;
      std::printf("rewrite %s:%u (%s): amd %d -> %d, scratch %d -> %d, reads %d -> %d; performance mode %d -> %d\n",
                  fs::path(rw.file).filename().string().c_str(), rw.line, rw.kind.c_str(), rw.amdBefore, rw.amdAfter,
                  rw.scratchBefore, rw.scratchAfter, rw.vmemBefore, rw.vmemAfter, rw.amdPerfBefore, rw.amdPerfAfter);
    }
    fs::remove_all(tmp, ec);
    std::vector<fx::SourceRewrite> kept;
    for (auto& rw : rewrites) {
      const bool measured = rw.amdBefore >= 0 && rw.amdAfter >= 0;
      // A texture read less counts too (the blend stage: the back buffer read goes, the same instructions).
      const bool gain = rw.amdAfter < rw.amdBefore || rw.scratchAfter < rw.scratchBefore ||
                        (rw.vmemAfter >= 0 && rw.vmemAfter < rw.vmemBefore) ||
                        (rw.amdPerfBefore >= 0 && rw.amdPerfAfter >= 0 && rw.amdPerfAfter < rw.amdPerfBefore);
      const bool perfWorse = rw.amdPerfAfter > rw.amdPerfBefore && rw.amdPerfBefore >= 0;
      // Faster without performance mode, slower with it (owner, 2026-10-08: Phosphor 46 -> 18 but 11 -> 12):
      // applied only without performance mode (ReShade's __RESHADE_PERFORMANCE_MODE__), so never slower.
      if (measured && rw.amdAfter < rw.amdBefore && perfWorse) {
        for (auto& e : rw.edits)
          if (e.extra.find("__RESHADE_PERFORMANCE_MODE__") == std::string::npos) e.extra += " && !__RESHADE_PERFORMANCE_MODE__";
        rw.description += " (not in performance mode, where it would be slower)";
        rw.amdPerfAfter = rw.amdPerfBefore;
        kept.push_back(std::move(rw));
        continue;
      }
      const bool worse = rw.amdAfter > rw.amdBefore || perfWorse;
      if (measured ? gain && !worse : rw.otherEntries > 0) kept.push_back(std::move(rw));
    }
    std::printf("classical rewrites: %zu of %zu kept (measured gain)\n", kept.size(), rewrites.size());
    rewrites = std::move(kept);
  } else {
    // Not measured: only arrays with a non-constant entry (all-constant ones the compilers fold already).
    rewrites.erase(std::remove_if(rewrites.begin(), rewrites.end(),
                                  [](const fx::SourceRewrite& rw) { return rw.otherEntries == 0; }),
                   rewrites.end());
  }
  std::string errors;
  auto files = fx::writeVariants(results, outDir, errors, rewrites);

  // Effects that include a changed header are copied too, so that the output directory
  // is self-contained: an effect finds headers next to it before the include paths.
  std::set<std::string> changed;
  for (const auto& r : results)
    if (!r.variants.empty()) changed.insert(r.region.file);
  for (const auto& rw : rewrites) changed.insert(rw.file);
  std::vector<std::pair<fs::path, fs::path>> effectsOut;  // written effect, its original
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
    effectsOut.emplace_back(dst, p);
  }

  console::section(con, "Writing");
  // Every variant must still parse: SOPT_ALL = k selects variant k where it exists.
  size_t checks = 0, checkFailures = 0;
  for (const auto& [e, original] : effectsOut) {
    for (size_t k = 0; k <= 2 * numVariants; ++k) {
      fx::LoadOptions lo = loadFor(e);
      // The variant files are meant to lie over the original folder: headers and `#if exists` tests
      // that are not in the output directory resolve there (SuperDepth3D's AXAA.fxh).
      lo.includePaths.insert(lo.includePaths.begin(), original.parent_path());
      lo.macros.emplace_back("SOPT_ALL", std::to_string(k));
      std::string err;
      ++checks;
      if (!fx::loadEffect(e, lo, err)) {
        ++checkFailures;
        errors += e.string() + " (SOPT_ALL=" + std::to_string(k) + ") does not parse:\n" + err;
      }
    }
  }
  info.rewrites = &rewrites;
  info.checks = checks;
  info.checkFailures = checkFailures;
  info.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  {
    std::ofstream f(outDir / "sopt-report.md", std::ios::binary);
    for (const auto& rr : results) info.schedule = info.schedule || rr.schedule;
    f << fx::markdownReport(results, info);
  }
  {
    std::ofstream f(outDir / "sopt-found.txt", std::ios::binary);
    f << fx::foundRewrites(results);
  }
  if (!errors.empty()) std::fprintf(stderr, "%s", errors.c_str());
  size_t improved = 0;
  for (const auto& r : results) improved += !r.variants.empty();
  std::printf("%s%zu of %zu regions have cheaper variants%s; wrote %zu files, sopt-report.md and sopt-found.txt to %s\n",
              con.c(improved ? "\x1b[1;92m" : "\x1b[1;97m"), improved, results.size(), con.reset(), files.size(),
              outDir.string().c_str());
  std::printf("variant check: %s%zu of %zu parses failed%s\n", con.c(checkFailures ? "\x1b[1;91m" : ""), checkFailures,
              checks, con.reset());
  return info.failed.empty() && checkFailures == 0 ? 0 : 1;
}
