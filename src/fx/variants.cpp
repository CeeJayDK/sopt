#include "fx/variants.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

namespace sopt::fx {

namespace fs = std::filesystem;

namespace {

std::string indentOf(const std::string& line) {
  return line.substr(0, line.find_first_not_of(" \t") == std::string::npos
                            ? line.size()
                            : line.find_first_not_of(" \t"));
}

const char* budgetText(const Budget& b, char* buf, size_t n) {
  switch (b.kind) {
    case Budget::Kind::Exact: return "exact";
    case Budget::Kind::Color8:
    case Budget::Kind::Color10:
      std::snprintf(buf, n, "color%d, max code diff %d", b.codeBits(), b.maxCodeDiff);
      return buf;
    case Budget::Kind::Texcoord:
      std::snprintf(buf, n, "texcoord, %g px at %g wide", b.px, b.width);
      return buf;
    case Budget::Kind::Abs: std::snprintf(buf, n, "abs %g", b.eps); return buf;
    case Budget::Kind::Rel: std::snprintf(buf, n, "rel %g", b.eps); return buf;
  }
  return "?";
}

std::string escapeCell(std::string s) {
  std::string out;
  for (char c : s) {
    if (c == '|') out += "\\|";
    else if (c == '\n') out += ' ';
    else out += c;
  }
  return out;
}

}  // namespace

std::string budgetString(const Budget& b) {
  char buf[96];
  return budgetText(b, buf, sizeof(buf));
}

uint32_t compiledCost(const Expr& e, const CostModel& m, const std::vector<InputDecl>& inputs) {
  const auto uses = m.fusedAdd ? useCounts(e) : std::vector<uint32_t>();
  const auto ct = compileTimeNodes(e, inputs);
  auto isArith = [&](uint32_t i) {
    const Op op = e.nodes[i].op;
    return op != Op::Input && op != Op::Const && op != Op::Swizzle && op != Op::Construct;
  };
  auto isConst = [&](uint32_t i, float v) {
    const Node& n = e.nodes[i];
    if (n.op != Op::Const) return false;
    for (unsigned k = 0; k < width(n.type); ++k)
      if (n.value[k] != v) return false;
    return true;
  };
  uint32_t cost = 0;
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    if (n.op == Op::Swizzle || n.op == Op::Construct || ct[i]) continue;
    if (const int f = m.fusedAdd ? fusedArg(e, i, uses, m.divIsMul) : -1; f >= 0 && !ct[n.args[f]]) continue;
    // Source modifiers of the consuming instruction.
    if ((n.op == Op::Neg || n.op == Op::Abs) && i != e.root) continue;
    // Output modifier of the producing instruction (clamp(x, 0, 1) is saturate).
    const bool sat = n.op == Op::Saturate ||
                     (n.op == Op::Clamp && isConst(n.args[1], 0.0f) && isConst(n.args[2], 1.0f));
    if (sat && isArith(n.args[0])) continue;
    const bool reduce = info(n.op).shape == Shape::Reduce;
    cost += m.opCost(sat ? Op::Saturate : n.op, width(reduce ? e.nodes[n.args[0]].type : n.type));
  }
  return cost;
}

std::string switchName(const Region& r) {
  std::string stem = pathFrom(r.file).stem().string();
  for (auto& c : stem)
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  std::string name = "SOPT_" + stem + "_";
  if (!r.removed.empty()) name += std::to_string(r.removed.front().first) + "_";
  return name + std::to_string(r.line);
}

int vendorPick(const RegionResult& rr, bool amd) {
  const int target = amd ? rr.targetAmd : rr.targetNv;
  if (target < 0) return 0;
  int best = 0, bestCost = target;
  for (size_t k = 0; k < rr.variants.size(); ++k) {
    const Variant& v = rr.variants[k];
    const int c = amd ? v.amd : v.nv;
    if (v.klass == Klass::LessAccurate || !v.problems.empty() || c < 0 || c >= bestCost) continue;
    best = static_cast<int>(k + 1);
    bestCost = c;
  }
  return best;
}

std::string variantStatement(const Region& r, const std::string& expr) {
  return r.lhs + " " + expr + ";";
}

std::vector<fs::path> writeVariants(const std::vector<RegionResult>& results,
                                    const fs::path& outDir, std::string& errors) {
  std::map<std::string, std::vector<const RegionResult*>> byFile;
  for (const auto& r : results)
    if (!r.variants.empty()) byFile[r.region.file].push_back(&r);
  std::vector<fs::path> written;
  std::error_code ec;
  fs::create_directories(outDir, ec);
  std::map<std::string, std::string> names;  // output name -> source
  for (auto& [file, regs] : byFile) {
    const std::vector<std::string>* lines = sourceLines(file);
    if (!lines) {
      errors += "cannot read " + file + "\n";
      continue;
    }
    const std::string name = pathFrom(file).filename().string();
    if (names.count(name)) {
      errors += "two sources named " + name + ": " + names[name] + " and " + file + " (skipped)\n";
      continue;
    }
    names[name] = file;
    // Regions (single statements and windows) may share lines: keep the largest gains.
    std::sort(regs.begin(), regs.end(), [](const RegionResult* a, const RegionResult* b) {
      const int ga = int(a->targetCost) - int(a->variants[0].cost), gb = int(b->targetCost) - int(b->variants[0].cost);
      if (ga != gb) return ga > gb;
      return a->region.removed.size() > b->region.removed.size();
    });
    struct Piece {
      uint32_t first, last;
      const RegionResult* rr;
      bool root;
    };
    std::vector<Piece> pieces;
    auto overlaps = [&](uint32_t a, uint32_t b) {
      for (const auto& p : pieces)
        if (a <= p.last && p.first <= b) return true;
      return false;
    };
    for (const RegionResult* rr : regs) {
      const Region& r = rr->region;
      bool clash = overlaps(r.line, r.lastLine) || r.lastLine > lines->size();
      for (const auto& [a, b] : r.removed) clash = clash || overlaps(a, b) || b >= r.line;
      if (clash) continue;
      for (const auto& [a, b] : r.removed) pieces.push_back({a, b, rr, false});
      pieces.push_back({r.line, r.lastLine, rr, true});
    }
    std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.first < b.first; });
    std::string out =
        "// Variants generated by sopt (shader superoptimizer). Each SOPT_<file>_<line>\n"
        "// switch selects the original (0) or a verified alternative (1..n) of one\n"
        "// statement or window; SOPT_ALL = k selects alternative k everywhere (the last\n"
        "// one where a region has fewer).\n"
        "#ifndef SOPT_ALL\n#define SOPT_ALL 0\n#endif\n";
    bool anyPick = false;
    for (const Piece& p : pieces) anyPick = anyPick || vendorPick(*p.rr, true) || vendorPick(*p.rr, false);
    if (anyPick)
      out += "// SOPT_AUTO = 1: switches not set otherwise take the variant measured fastest on\n"
             "// the GPU's vendor (__VENDOR__: AMD 0x1002, NVIDIA 0x10DE; others: original).\n"
             "#ifndef SOPT_AUTO\n#define SOPT_AUTO 0\n#endif\n";
    // Switches up front, outside any #if of the source.
    std::set<const RegionResult*> declared;
    for (const Piece& p : pieces) {
      if (!declared.insert(p.rr).second) continue;
      const std::string sw = switchName(p.rr->region);
      const std::string n = std::to_string(p.rr->variants.size());
      const std::string note = " // 0 = original, 1.." + n + " = variants (larger = " + n + ")\n";
      const int amd = vendorPick(*p.rr, true), nv = vendorPick(*p.rr, false);
      if (!amd && !nv) {
        out += "#ifndef " + sw + "\n#define " + sw + " SOPT_ALL" + note + "#endif\n";
        continue;
      }
      out += "#ifndef " + sw + "\n";
      std::string kw = "#if";
      if (amd) out += kw + " SOPT_AUTO && __VENDOR__ == 0x1002\n#define " + sw + " " + std::to_string(amd) + "\n", kw = "#elif";
      if (nv) out += kw + " SOPT_AUTO && __VENDOR__ == 0x10DE\n#define " + sw + " " + std::to_string(nv) + "\n";
      out += "#else\n#define " + sw + " SOPT_ALL" + note + "#endif\n#endif\n";
    }
    uint32_t next = 1;  // next source line to copy
    for (const Piece& p : pieces) {
      const RegionResult* rr = p.rr;
      const Region& r = rr->region;
      for (; next < p.first; ++next) out += (*lines)[next - 1] + "\n";
      const std::string sw = switchName(r);
      // A window across #if lines applies only while they compile as when it was found.
      // Its terms are parenthesized: "(A) && !defined(B)".
      const std::string guard = r.guard.empty() ? std::string() : " && " + r.guard;
      if (!p.root) {
        // A statement inlined into the variants: only the original needs it.
        out += "#if " + sw + " < 1" + (r.guard.empty() ? std::string() : " || !(" + r.guard + ")") + "\n";
        for (; next <= p.last; ++next) out += (*lines)[next - 1] + "\n";
        out += "#endif\n";
        continue;
      }
      const std::string ind = indentOf((*lines)[r.line - 1]);
      for (size_t k = 0; k < rr->variants.size(); ++k) {
        const Variant& v = rr->variants[k];
        // The last variant also takes larger values, so SOPT_ALL = k works for regions
        // with fewer than k variants.
        const bool last = k + 1 == rr->variants.size();
        out += std::string(k == 0 ? "#if " : "#elif ") + sw + (last ? " >= " : " == ") +
               std::to_string(k + 1) + guard + "\n";
        char note[320];
        int len = std::snprintf(note, sizeof(note), " // sopt: %s, cost %u -> %u",
                                klassName(v.klass, r.prog.budget.codeBits()), rr->targetCost, v.cost);
        if ((v.klass == Klass::Accurate || v.klass == Klass::LessAccurate) && rr->targetExactAbs >= 0 && len > 0)
          len += std::snprintf(note + len, sizeof(note) - len, ", max err vs exact %.2g (original %.2g)",
                               v.worst.exactAbs, rr->targetExactAbs);
        if (v.amd >= 0 && rr->targetAmd >= 0 && len > 0 && len < 200)
          len += std::snprintf(note + len, sizeof(note) - len, ", amd %d -> %d", rr->targetAmd, v.amd);
        if (v.nv >= 0 && rr->targetNv >= 0 && len > 0 && len < 200)
          std::snprintf(note + len, sizeof(note) - len, ", nv %d -> %d", rr->targetNv, v.nv);
        out += ind + variantStatement(r, v.text) + note + (v.problems.empty() ? "" : "; " + v.problems) + "\n";
      }
      out += "#else\n";
      for (; next <= p.last; ++next) out += (*lines)[next - 1] + "\n";
      out += "#endif\n";
    }
    for (; next <= lines->size(); ++next) out += (*lines)[next - 1] + "\n";
    const fs::path dst = outDir / name;
    std::ofstream f(dst, std::ios::binary);
    f << out;
    if (!f) {
      errors += "cannot write " + dst.string() + "\n";
      continue;
    }
    written.push_back(dst);
  }
  return written;
}

std::string markdownReport(const std::vector<RegionResult>& results, const ReportInfo& info) {
  std::string s = "# sopt report\n\n";
  size_t withVariants = 0;
  for (const auto& r : results) withVariants += !r.variants.empty();
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "- Effects: %zu parsed, %zu failed\n- Regions searched: %zu, with cheaper "
                "variants: %zu\n- Cost model: %s\n- Variant effects re-parsed: %zu, failed: %zu\n"
                "- Time: %.1f s\n\n",
                info.effects.size(), info.failed.size(), results.size(), withVariants,
                info.costModel.c_str(), info.checks, info.checkFailures, info.seconds);
  s += buf;
  s += "Costs are the static cost model's (quarter-VALU units for rdna3); amd / nv are "
       "measured instructions (fxstat + RGA, ptxas + nvdisasm), with the change against the "
       "original (row 0). Classes: bit-exact; 8-bit identical; within budget; as accurate "
       "(outside the budget only where at least as close to exact math as the original); "
       "less accurate (listed for you to judge by its error). \"vs exact\" is the max error "
       "against exact math. \"auto\" marks what SOPT_AUTO = 1 selects on that vendor. Ranges "
       "marked *assumed* are defaults, not facts: check them before using a variant.\n\n";
  if (!info.failed.empty()) {
    s += "## Effects that failed to parse\n\n";
    for (const auto& [f, e] : info.failed) s += "- `" + f + "`: " + escapeCell(e.substr(0, 300)) + "\n";
    s += "\n";
  }
  s += "## Regions with variants\n\n";
  for (const auto& rr : results) {
    if (rr.variants.empty()) continue;
    const Region& r = rr.region;
    s += "### `" + switchName(r) + "` — " + pathFrom(r.file).filename().string() + ":" +
         (r.removed.empty() ? "" : std::to_string(r.removed.front().first) + "-") + std::to_string(r.line) +
         " (" + r.function + ")\n\n";
    s += "```hlsl\n" + r.original + "\n```\n\n";
    if (!r.guard.empty()) s += "Applies only while `" + r.guard + "` (preprocessor).\n\n";
    s += "Inputs:\n";
    for (size_t k = 0; k < r.prog.inputs.size(); ++k) {
      const auto& d = r.prog.inputs[k];
      std::snprintf(buf, sizeof(buf), "- `%s` in [%g, %g]%s — %s%s\n", d.name.c_str(), d.lo, d.hi,
                    d.grid ? (" grid " + std::to_string(d.grid)).c_str() : "",
                    r.facts[k].source.c_str(), r.facts[k].assumed ? " (*assumed*)" : "");
      s += buf;
    }
    s += std::string("\nBudget: ") + budgetText(r.prog.budget, buf, sizeof(buf)) + " (" +
         r.budgetReason + "). Original cost " + std::to_string(rr.targetCost);
    if (info.amd) s += ", amd " + std::to_string(rr.targetAmd);
    if (info.nv) s += ", nv " + std::to_string(rr.targetNv);
    s += ".";
    const bool exact = rr.targetExactAbs >= 0;
    if (exact) {
      std::snprintf(buf, sizeof(buf), " Original's max error vs exact math: %.3g.", rr.targetExactAbs);
      s += buf;
    }
    // Row 0 is the original; costs with the gain against it; "auto" marks what
    // SOPT_AUTO = 1 picks per vendor.
    const int pickAmd = vendorPick(rr, true), pickNv = vendorPick(rr, false);
    const bool autoCol = pickAmd || pickNv;
    auto withGain = [&](int c, int t) {
      if (c < 0) return std::string("?");
      std::string cell = std::to_string(c);
      if (t > 0 && c != t) {
        std::snprintf(buf, sizeof(buf), " (%+.0f%%)", 100.0 * (c - t) / t);
        cell += buf;
      }
      return cell;
    };
    s += "\n\n| # | code | cost |";
    if (info.amd) s += " amd |";
    if (info.nv) s += " nv |";
    s += std::string(" class | max abs err |") + (exact ? " vs exact |" : "") + " verified |" +
         (autoCol ? " auto |" : "") + "\n|---|---|---|";
    if (info.amd) s += "---|";
    if (info.nv) s += "---|";
    s += std::string(exact ? "---|---|---|---|" : "---|---|---|") + (autoCol ? "---|" : "") + "\n";
    s += "| 0 | `" + escapeCell(toString(r.prog.target, r.prog.inputs)) + "` | " + std::to_string(rr.targetCost) + " |";
    if (info.amd) s += " " + withGain(rr.targetAmd, -1) + " |";
    if (info.nv) s += " " + withGain(rr.targetNv, -1) + " |";
    s += " original | 0 |";
    if (exact) {
      std::snprintf(buf, sizeof(buf), " %.3g |", rr.targetExactAbs);
      s += buf;
    }
    s += std::string(" |") + (autoCol ? " |" : "") + "\n";
    for (size_t k = 0; k < rr.variants.size(); ++k) {
      const Variant& v = rr.variants[k];
      s += "| " + std::to_string(k + 1) + " | `" + escapeCell(v.text) + "` | " +
           withGain(static_cast<int>(v.cost), static_cast<int>(rr.targetCost)) + " |";
      if (info.amd) s += " " + withGain(v.amd, rr.targetAmd) + " |";
      if (info.nv) s += " " + withGain(v.nv, rr.targetNv) + " |";
      std::snprintf(buf, sizeof(buf), " %s | %.3g |", klassName(v.klass, r.prog.budget.codeBits()), v.worst.maxAbs);
      s += buf;
      if (exact) {
        std::snprintf(buf, sizeof(buf), " %.3g |", v.worst.exactAbs);
        s += buf;
      }
      s += std::string(" ") + (v.exhaustive ? "all points" : "sampled") + " |";
      if (autoCol) {
        std::string a;
        if (pickAmd == static_cast<int>(k + 1)) a = "AMD";
        if (pickNv == static_cast<int>(k + 1)) a += a.empty() ? "NVIDIA" : ", NVIDIA";
        s += " " + a + " |";
      }
      s += "\n";
    }
    for (size_t k = 0; k < rr.variants.size(); ++k)
      if (!rr.variants[k].problems.empty())
        s += "\n**Variant " + std::to_string(k + 1) + "** " + rr.variants[k].problems +
             ". Not used by SOPT_AUTO; limiting the input to the fine range avoids the problem.\n";
    s += "\n";
  }
  s += "## Variants on assumed ranges (not written)\n\n"
       "Some input has no known range, so the variant was verified on the default range "
       "only and may be wrong for real values. Give the ranges in sopt-facts.txt (--facts) or\n"
       "with --ask, add ui_min/ui_max, or use --assumed.\n\n";
  for (const auto& rr : results) {
    if (rr.unwritten.empty()) continue;
    const Region& r = rr.region;
    std::string assumed;
    for (const auto& f : r.facts)
      if (f.assumed) assumed += (assumed.empty() ? "" : ", ") + ("`" + f.input + "`");
    s += "- " + pathFrom(r.file).filename().string() + ":" + std::to_string(r.line) + " (assumed: " +
         assumed + "): `" + escapeCell(toString(r.prog.target, r.prog.inputs)) + "` -> `" +
         escapeCell(rr.unwritten[0].text) + "` (cost " + std::to_string(rr.targetCost) + " -> " +
         std::to_string(rr.unwritten[0].cost) + ")\n";
  }
  s += "\n## Regions without cheaper variants\n\n";
  for (const auto& rr : results) {
    if (!rr.variants.empty() || !rr.unwritten.empty()) continue;
    const Region& r = rr.region;
    std::string why = rr.limitHit ? ", search limit hit" : "";
    if (rr.onlyContraction) why += ", only explicit fma/modifiers";
    if (rr.measuredNoGain) why += ", no measured gain";
    std::snprintf(buf, sizeof(buf), "- %s:%u (cost %u%s): `%s`\n", pathFrom(r.file).filename().string().c_str(),
                  r.line, rr.targetCost, why.c_str(),
                  escapeCell(toString(r.prog.target, r.prog.inputs)).c_str());
    s += buf;
  }
  s += "\n## Skipped statements\n\n| reason | count |\n|---|---|\n";
  std::vector<std::pair<uint32_t, std::string>> sk;
  for (const auto& [k, v] : info.skipped.reasons) sk.emplace_back(v, k);
  std::sort(sk.rbegin(), sk.rend());
  for (const auto& [v, k] : sk) s += "| " + escapeCell(k) + " | " + std::to_string(v) + " |\n";
  return s;
}

}  // namespace sopt::fx
