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
  const auto uses = (m.fusedAdd || m.amdFolds) ? useCounts(e) : std::vector<uint32_t>();
  const auto ct = compileTimeNodes(e, inputs);
  const auto folded = amdFoldedNodes(e, uses, m);
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
    // Output modifier (omod) of the producing instruction, or part of a 3-operand min / max.
    if (folded[i] && (!ct[n.args[0]] || e.nodes[n.args[0]].op == Op::Const) &&
        (!ct[n.args[1]] || e.nodes[n.args[1]].op == Op::Const))
      continue;
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

// Class text of a variant: "within budget", "too exact, more accurate (not faster)",
// "bit-exact, fewer registers (not faster)", ...
std::string variantClass(const Variant& v, int codeBits) {
  std::string s = klassName(v.klass, codeBits);
  if (v.moreAccurate) s += ", more accurate";
  if (v.fewerRegisters && v.notFaster) s += ", fewer registers";
  if (v.notFaster) s += " (not faster)";
  return s;
}

int vendorPick(const RegionResult& rr, bool amd, bool dx) {
  if (rr.region.hlsl) return 0;  // plain HLSL has no __VENDOR__ / __RENDERER__
  const int target = amd ? rr.targetAmd : rr.targetNv;
  if (target < 0) return 0;
  int best = 0, bestCost = target;
  for (size_t k = 0; k < rr.variants.size(); ++k) {
    const Variant& v = rr.variants[k];
    const int c = amd ? v.amd : v.nv;
    if (v.klass == Klass::LessAccurate || v.klass == Klass::Accurate || !v.problems.empty() || v.notFaster || c < 0 || c >= bestCost) continue;
    if (dx && (v.dxbcSame || (v.dxbc >= 0 && rr.targetDxbc >= 0 && v.dxbc > rr.targetDxbc))) continue;
    best = static_cast<int>(k + 1);
    bestCost = c;
  }
  return best;
}

std::string variantStatement(const Region& r, const std::string& expr) {
  return r.lhs + " " + expr + r.rhs + ";";
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
    bool anyTooExact = false;
    for (const Piece& p : pieces)
      for (const Variant& v : p.rr->variants) anyTooExact = anyTooExact || v.klass == Klass::Accurate;
    if (anyTooExact)
      out += "// SOPT_TOO_EXACT = 0 turns off the \"too exact\" variants: closer to exact math than the\n"
             "// float32 original, so they differ from it where it rounds (fine or better for most\n"
             "// effects; wrong where the effect relies on the rounding).\n"
             "#ifndef SOPT_TOO_EXACT\n#define SOPT_TOO_EXACT 1\n#endif\n";
    if (anyPick)
      out += "// SOPT_AUTO = 1: switches not set otherwise take the variant measured fastest on\n"
             "// the GPU's vendor (__VENDOR__: AMD 0x1002, NVIDIA 0x10DE; others: original) and\n"
             "// API (__RENDERER__ < 0x10000: DX9-DX12, where fxc's DXBC reaches the driver).\n"
             "#ifndef SOPT_AUTO\n#define SOPT_AUTO 0\n#endif\n";
    // Switches up front, outside any #if of the source.
    std::set<const RegionResult*> declared;
    for (const Piece& p : pieces) {
      if (!declared.insert(p.rr).second) continue;
      const std::string sw = switchName(p.rr->region);
      const std::string n = std::to_string(p.rr->variants.size());
      const std::string note = " // 0 = original, 1.." + n + " = variants (larger = " + n + ")\n";
      const int amd = vendorPick(*p.rr, true), nv = vendorPick(*p.rr, false);
      const int amdDx = vendorPick(*p.rr, true, true), nvDx = vendorPick(*p.rr, false, true);
      if (!amd && !nv) {
        out += "#ifndef " + sw + "\n#define " + sw + " SOPT_ALL" + note + "#endif\n";
        continue;
      }
      out += "#ifndef " + sw + "\n";
      std::string kw = "#if";
      // Per vendor; a DX9-DX12 line first where fxc makes the pick differ (0 = original).
      auto vendor = [&](const char* id, int pick, int pickDx) {
        if (pickDx != pick) {
          out += kw + " SOPT_AUTO && __VENDOR__ == " + id + " && __RENDERER__ < 0x10000\n#define " + sw + " " +
                 std::to_string(pickDx) + "\n";
          kw = "#elif";
        }
        if (pick) out += kw + " SOPT_AUTO && __VENDOR__ == " + id + "\n#define " + sw + " " + std::to_string(pick) + "\n", kw = "#elif";
      };
      vendor("0x1002", amd, amdDx);
      vendor("0x10DE", nv, nvDx);
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
      // Condition under which variant k is used (a back buffer format guard included).
      // A too-exact variant applies only while SOPT_TOO_EXACT is set.
      bool anyFormat = false;
      for (const Variant& v : rr->variants)
        anyFormat = anyFormat || !v.formatGuard.empty() || v.klass == Klass::Accurate;
      auto cond = [&](size_t k) {
        const bool last = k + 1 == rr->variants.size();
        const std::string& fg = rr->variants[k].formatGuard;
        return sw + (last ? " >= " : " == ") + std::to_string(k + 1) + guard + (fg.empty() ? "" : " && (" + fg + ")") +
               (rr->variants[k].klass == Klass::Accurate ? " && SOPT_TOO_EXACT" : "");
      };
      if (!p.root) {
        // A statement inlined into the variants: only the original needs it.
        if (anyFormat) {
          std::string any;
          for (size_t k = 0; k < rr->variants.size(); ++k) any += (k ? " || (" : "(") + cond(k) + ")";
          out += "#if !(" + any + ")\n";
        } else {
          out += "#if " + sw + " < 1" + (r.guard.empty() ? std::string() : " || !(" + r.guard + ")") + "\n";
        }
        for (; next <= p.last; ++next) out += (*lines)[next - 1] + "\n";
        out += "#endif\n";
        continue;
      }
      const std::string ind = indentOf((*lines)[r.line - 1]);
      for (size_t k = 0; k < rr->variants.size(); ++k) {
        const Variant& v = rr->variants[k];
        // The last variant also takes larger values, so SOPT_ALL = k works for regions
        // with fewer than k variants.
        out += std::string(k == 0 ? "#if " : "#elif ") + cond(k) + "\n";
        char note[320];
        int len = std::snprintf(note, sizeof(note), " // sopt: %s, cost %u -> %u",
                                variantClass(v, r.prog.budget.codeBits()).c_str(), rr->targetCost, v.cost);
        if ((v.klass == Klass::Accurate || v.klass == Klass::LessAccurate) && rr->targetExactAbs >= 0 && len > 0)
          len += std::snprintf(note + len, sizeof(note) - len, ", max err vs exact %.2g (original %.2g)",
                               v.worst.exactAbs, rr->targetExactAbs);
        if (v.amd >= 0 && rr->targetAmd >= 0 && len > 0 && len < 200)
          len += std::snprintf(note + len, sizeof(note) - len, ", amd %d -> %d", rr->targetAmd, v.amd);
        if (v.nv >= 0 && rr->targetNv >= 0 && len > 0 && len < 200)
          len += std::snprintf(note + len, sizeof(note) - len, ", nv %d -> %d", rr->targetNv, v.nv);
        std::string back;
        // Register counts only where they change (register pressure).
        if (v.amdVgprs >= 0 && rr->targetAmdVgprs >= 0 && v.amdVgprs != rr->targetAmdVgprs)
          back += ", vgpr " + std::to_string(rr->targetAmdVgprs) + " -> " + std::to_string(v.amdVgprs);
        if (v.nvRegs >= 0 && rr->targetNvRegs >= 0 && v.nvRegs != rr->targetNvRegs)
          back += ", nv regs " + std::to_string(rr->targetNvRegs) + " -> " + std::to_string(v.nvRegs);
        if (v.spirv >= 0 && rr->targetSpirv >= 0)
          back += v.spirvSame ? ", spirv: same code as original"
                              : ", spirv " + std::to_string(rr->targetSpirv) + " -> " + std::to_string(v.spirv);
        if (v.dxbc >= 0 && rr->targetDxbc >= 0)
          back += v.dxbcSame ? ", dxbc: same code as original"
                             : ", dxbc " + std::to_string(rr->targetDxbc) + " -> " + std::to_string(v.dxbc);
        if (!v.formatGuard.empty()) back += "; only where " + v.formatGuard;
        std::string text = v.text;
        // BUFFER_WIDTH / BUFFER_HEIGHT are int literals: BUFFER_WIDTH / BUFFER_HEIGHT in the
        // code would be an integer division, so the code converts them.
        bool intInputs = false;
        std::vector<InputDecl> codeInputs = r.prog.inputs;
        for (auto& d : codeInputs)
          if (d.compileTime && isBufferSizeMacro(d.name)) {
            d.name = "float(" + d.name + ")";
            intInputs = true;
          }
        if (intInputs) text = toString(v.expr, codeInputs);
        if (needsPrecise(v.expr)) {
          // fxc -O3 folds (v + c) - c to v: the add-round trick only survives as precise.
          const unsigned w = width(v.expr.nodes[v.expr.root].type);
          const std::string tmp = "__sopt_p" + std::to_string(r.line) + "_" + std::to_string(k + 1);
          out += ind + "precise float" + (w > 1 ? std::to_string(w) : std::string()) + " " + tmp + " = " + text + ";\n";
          text = tmp;
        }
        out += ind + variantStatement(r, text) + note + back + (v.problems.empty() ? "" : "; " + v.problems) + "\n";
      }
      out += "#else\n";
      for (; next <= p.last; ++next) out += (*lines)[next - 1] + "\n";
      out += "#endif\n";
    }
    for (; next <= lines->size(); ++next) out += (*lines)[next - 1] + "\n";
    // A UTF-8 byte order mark must stay the first bytes of the file (DisplayDepth.fx has
    // one): ahead of the generated header, not after it.
    const std::string bom = "\xEF\xBB\xBF";
    if (const size_t pos = out.find(bom); pos != std::string::npos) out = bom + out.erase(pos, bom.size());
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
       "original (row 0); amd vgpr / nv regs: registers of the measured shader (whole-shader "
       "counts with the test scaffolding, so only the change against row 0 matters: more "
       "registers can mean fewer waves in flight). Classes: bit-exact; 8-bit identical; within budget; too exact "
       "(outside the budget only where at least as close to exact math as the original); "
       "less accurate (listed for you to judge by its error); \"more accurate\": at most a "
       "quarter of the original's error against exact math, \"(not faster)\": kept for its "
       "accuracy at up to one instruction more. spirv / dxbc (--backends): instructions after "
       "the compilers' optimizers (SPIR-V: fxstat's spirv-opt passes; DXBC: Microsoft's fxc "
       "-O3, what DX9-DX11 games get), \"same\" = identical code to the original's there, "
       "i.e. the compiler already does it on that backend. \"vs exact\" is the max error "
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
    if (info.amd) {
      s += ", amd " + std::to_string(rr.targetAmd);
      if (rr.targetAmdVgprs >= 0)
        s += " (" + std::to_string(rr.targetAmdVgprs) + " vgpr, " + std::to_string(rr.targetAmdSgprs) + " sgpr)";
    }
    if (info.nv) {
      s += ", nv " + std::to_string(rr.targetNv);
      if (rr.targetNvRegs >= 0) s += " (" + std::to_string(rr.targetNvRegs) + " regs)";
    }
    s += ".";
    const bool exact = rr.targetExactAbs >= 0;
    if (exact) {
      std::snprintf(buf, sizeof(buf), " Original's max error vs exact math: %.3g.", rr.targetExactAbs);
      s += buf;
    }
    // Row 0 is the original; costs with the gain against it; "auto" marks what
    // SOPT_AUTO = 1 picks per vendor.
    const int pickAmd = vendorPick(rr, true), pickNv = vendorPick(rr, false);
    const int pickAmdDx = vendorPick(rr, true, true), pickNvDx = vendorPick(rr, false, true);
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
    auto regsCell = [&](int c, int t) {
      if (c < 0) return std::string("?");
      std::string cell = std::to_string(c);
      if (t >= 0 && c != t) {
        std::snprintf(buf, sizeof(buf), " (%+d)", c - t);
        cell += buf;
      }
      return cell;
    };
    s += "\n\n| # | code | cost |";
    if (info.amd) s += " amd | amd vgpr |";
    if (info.nv) s += " nv | nv regs |";
    if (info.spirv) s += " spirv |";
    if (info.dxbc) s += " dxbc |";
    s += std::string(" class | max abs err |") + (exact ? " vs exact |" : "") + " verified |" +
         (autoCol ? " auto |" : "") + "\n|---|---|---|";
    if (info.amd) s += "---|---|";
    if (info.nv) s += "---|---|";
    if (info.spirv) s += "---|";
    if (info.dxbc) s += "---|";
    s += std::string(exact ? "---|---|---|---|" : "---|---|---|") + (autoCol ? "---|" : "") + "\n";
    s += "| 0 | `" + escapeCell(toString(r.prog.target, r.prog.inputs)) + "` | " + std::to_string(rr.targetCost) + " |";
    if (info.amd) s += " " + withGain(rr.targetAmd, -1) + " | " + regsCell(rr.targetAmdVgprs, -1) + " |";
    if (info.nv) s += " " + withGain(rr.targetNv, -1) + " | " + regsCell(rr.targetNvRegs, -1) + " |";
    if (info.spirv) s += " " + withGain(rr.targetSpirv, -1) + " |";
    if (info.dxbc) s += " " + withGain(rr.targetDxbc, -1) + " |";
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
      if (info.amd) s += " " + withGain(v.amd, rr.targetAmd) + " | " + regsCell(v.amdVgprs, rr.targetAmdVgprs) + " |";
      if (info.nv) s += " " + withGain(v.nv, rr.targetNv) + " | " + regsCell(v.nvRegs, rr.targetNvRegs) + " |";
      if (info.spirv) s += " " + (v.spirvSame ? std::string("same") : withGain(v.spirv, rr.targetSpirv)) + " |";
      if (info.dxbc) s += " " + (v.dxbcSame ? std::string("same") : withGain(v.dxbc, rr.targetDxbc)) + " |";
      std::string cls = variantClass(v, r.prog.budget.codeBits());
      if (!v.formatGuard.empty()) cls += "; only where `" + v.formatGuard + "`";
      std::snprintf(buf, sizeof(buf), " %s | %.3g |", cls.c_str(), v.worst.maxAbs);
      s += buf;
      if (exact) {
        std::snprintf(buf, sizeof(buf), " %.3g |", v.worst.exactAbs);
        s += buf;
      }
      if (v.exhaustive) s += " all points |";
      else if (v.proven) s += " proven |";
      else if (v.provenFraction > 0.0) {
        const double pc = 100.0 * v.provenFraction;
        if (pc >= 99.99) std::snprintf(buf, sizeof(buf), " sampled, proven on >99.99%% |");
        else std::snprintf(buf, sizeof(buf), " sampled, proven on %.4g%% |", pc);
        s += buf;
      } else s += " sampled |";
      if (autoCol) {
        std::string a;
        const int idx = static_cast<int>(k + 1);
        // "AMD", "NVIDIA (Vulkan/GL)", "AMD (DX)": the API only where the picks differ.
        auto mark = [&](const char* name, int pick, int pickDx) {
          std::string m;
          if (pick == idx && pickDx == idx) m = name;
          else if (pick == idx) m = std::string(name) + " (Vulkan/GL)";
          else if (pickDx == idx) m = std::string(name) + " (DX)";
          if (!m.empty()) a += (a.empty() ? "" : ", ") + m;
        };
        mark("AMD", pickAmd, pickAmdDx);
        mark("NVIDIA", pickNv, pickNvDx);
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
    std::string why = rr.limitHit ? ", search limit hit (levels complete to " + std::to_string(rr.completedCost) +
                                        " of " + std::to_string(rr.maxLevel) + ")"
                                  : "";
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

std::string foundRewrites(const std::vector<RegionResult>& results) {
  std::string s =
      "# sopt: faster variants found (sopt-fx), in the rewrite library's format\n"
      "# (library/rewrites.txt). Names are the region's inputs; ranges are what sopt-fx knew\n"
      "# (facts, or assumed where marked). Generalize a rule before adding it to the library\n"
      "# and check it there with sopt --check-library.\n";
  std::set<std::string> seen;
  char buf[256];
  auto ident = [](const std::string& n) {
    if (n.empty() || !(std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_')) return false;
    for (char c : n)
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
  };
  for (const auto& rr : results) {
    const Region& r = rr.region;
    // Inputs that are not plain names (texture fetches, member chains) become in1, in2, ...
    std::vector<InputDecl> named = r.prog.inputs;
    std::string legend;
    int k = 0;
    for (auto& d : named)
      if (!ident(d.name)) {
        const std::string alias = "in" + std::to_string(++k);
        legend += "#   " + alias + " = " + d.name + "\n";
        d.name = alias;
      }
    std::string where;
    for (size_t i = 0; i < named.size(); ++i) {
      const auto& d = named[i];
      std::snprintf(buf, sizeof(buf), "%s%s in [%.9g, %.9g]", where.empty() ? "" : ", ", d.name.c_str(), d.lo, d.hi);
      where += buf;
      if (d.type != Type::Float) where += ", " + d.name + " : float" + std::to_string(width(d.type));
    }
    const std::string lhs = toString(r.prog.target, named);
    auto emit = [&](const Variant& v, bool assumed) {
      if (v.notFaster) return;
      std::string rule = lhs + " -> " + toString(v.expr, named);
      if (!where.empty()) rule += "   where " + where;
      if (!seen.insert(rule).second) return;
      s += "\n# " + pathFrom(r.file).filename().string() + ":" +
           (r.removed.empty() ? "" : std::to_string(r.removed.front().first) + "-") + std::to_string(r.line) +
           " (" + r.function + ")  cost " + std::to_string(rr.targetCost) + " -> " + std::to_string(v.cost);
      if (rr.targetAmd >= 0 && v.amd >= 0) s += ", amd " + std::to_string(rr.targetAmd) + " -> " + std::to_string(v.amd);
      if (rr.targetNv >= 0 && v.nv >= 0) s += ", nv " + std::to_string(rr.targetNv) + " -> " + std::to_string(v.nv);
      if (rr.targetDxbc >= 0 && v.dxbc >= 0)
        s += ", dxbc " + std::to_string(rr.targetDxbc) + " -> " + (v.dxbcSame ? std::string("same") : std::to_string(v.dxbc));
      s += ", " + variantClass(v, r.prog.budget.codeBits());
      std::snprintf(buf, sizeof(buf), ", max abs err %.3g", v.worst.maxAbs);
      s += buf;
      if (rr.targetExactAbs >= 0) {
        std::snprintf(buf, sizeof(buf), ", vs exact %.3g (original %.3g)", v.worst.exactAbs, rr.targetExactAbs);
        s += buf;
      }
      if (v.proven) s += ", proven (V3)";
      if (assumed) s += ", assumed ranges";
      if (!v.problems.empty()) s += ", " + v.problems;
      s += "\n" + legend + rule + "\n";
    };
    for (const auto& v : rr.variants) emit(v, false);
    for (const auto& v : rr.unwritten) emit(v, true);
  }
  return s;
}

}  // namespace sopt::fx
