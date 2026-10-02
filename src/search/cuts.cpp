#include "search/cuts.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>

#include "ir/eval.hpp"
#include "search/subtrees.hpp"

namespace sopt {
namespace {

bool isOp(const Node& n) { return n.op != Op::Input && n.op != Op::Const && n.op != Op::Swizzle; }

// Nodes the root reaches without passing through v.
std::vector<char> aboveOnly(const Expr& e, uint32_t v) {
  std::vector<char> mark(e.nodes.size(), 0);
  mark[e.root] = 1;
  for (size_t i = e.nodes.size(); i-- > 0;) {
    if (!mark[i] || i == v) continue;
    for (uint8_t k = 0; k < e.nodes[i].nargs; ++k) mark[e.nodes[i].args[k]] = 1;
  }
  return mark;
}

// Copy of e (over `inputs`) with its Input nodes replaced by in(k, type).
template <class In>
uint32_t copyMapped(const Expr& e, ExprBuilder& b, In in) {
  std::vector<uint32_t> map(e.nodes.size());
  for (uint32_t i = 0; i < e.nodes.size(); ++i)
    map[i] = e.nodes[i].op == Op::Input ? in(e.nodes[i].input, e.nodes[i].type) : foldCopyNode(e, i, map, b);
  return map[e.root];
}

}  // namespace

std::vector<Cut> findCuts(const Program& prog, const CostModel& model, uint64_t seed) {
  const Expr& e = prog.target;
  const size_t n = e.nodes.size();
  const std::vector<bool> ctime = compileTimeNodes(e, prog.inputs);
  // below[i][k]: node k is in the subexpression of node i.
  std::vector<std::vector<char>> below(n, std::vector<char>(n, 0));
  for (uint32_t i = 0; i < n; ++i) {
    below[i][i] = 1;
    for (uint8_t k = 0; k < e.nodes[i].nargs; ++k)
      for (uint32_t j = 0; j < n; ++j)
        if (below[e.nodes[i].args[k]][j]) below[i][j] = 1;
  }
  PointSet ps;
  std::vector<Cut> cuts;
  for (uint32_t v = 0; v < n; ++v) {
    const Node& nv = e.nodes[v];
    if (v == e.root || ctime[v] || !isOp(nv) || nv.op == Op::Construct || !isFloat(nv.type)) continue;
    const std::vector<char> above = aboveOnly(e, v);
    Cut c;
    c.node = v;
    bool strict = true;
    for (uint32_t j = 0; j < n && strict; ++j) {
      const Node& nj = e.nodes[j];
      if (below[v][j]) {
        c.subOps += isOp(nj) && !ctime[j];
        // The rest must not read what v reads, except through v (compile-time inputs
        // are constants).
        if (j != v && above[j] && nj.op == Op::Input && !prog.inputs[nj.input].compileTime) strict = false;
      } else if (above[j]) {
        c.topOps += isOp(nj) && !ctime[j];
      }
    }
    // Inputs below v and read elsewhere through different Input nodes: hash-consing
    // gives one node per input and type, but an input can appear with other types.
    if (strict)
      for (uint32_t j = 0; j < n && strict; ++j)
        for (uint32_t k = 0; k < n && strict; ++k)
          if (below[v][j] && !below[v][k] && above[k] && e.nodes[j].op == Op::Input && e.nodes[k].op == Op::Input &&
              e.nodes[j].input == e.nodes[k].input && !prog.inputs[e.nodes[j].input].compileTime)
            strict = false;
    if (!strict || c.subOps < 2 || c.topOps < 2) continue;
    // v's range over the region's domain (sampled).
    if (ps.size() == 0) ps = makeRandomPoints(prog, 1u << 14, seed + 11, true);
    const std::vector<float> vals = evalAll(subexpr(e, v), ps, kProfileRef);
    double lo = INFINITY, hi = -INFINITY;
    for (float x : vals)
      if (std::isfinite(x)) {
        lo = std::min(lo, double(x));
        hi = std::max(hi, double(x));
      }
    if (!(hi > lo)) continue;
    c.lo = lo;
    c.hi = hi;
    c.subCost = dagCost(subexpr(e, v), model, prog.inputs);
    std::vector<uint32_t> inputMap;
    const Program top = topProgram(prog, c, inputMap);
    c.topCost = dagCost(top.target, model, top.inputs);
    cuts.push_back(c);
  }
  std::stable_sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) {
    return std::min(a.subCost, a.topCost) > std::min(b.subCost, b.topCost);
  });
  return cuts;
}

Program topProgram(const Program& prog, const Cut& cut, std::vector<uint32_t>& inputMap) {
  const Expr& e = prog.target;
  const std::vector<char> above = aboveOnly(e, cut.node);
  std::vector<uint32_t> newIndex(prog.inputs.size(), UINT32_MAX);
  for (uint32_t i = 0; i < e.nodes.size(); ++i)
    if (above[i] && i != cut.node && e.nodes[i].op == Op::Input) newIndex[e.nodes[i].input] = 0;
  Program top;
  inputMap.clear();
  for (uint32_t k = 0; k < prog.inputs.size(); ++k)
    if (newIndex[k] == 0) {
      newIndex[k] = static_cast<uint32_t>(top.inputs.size());
      top.inputs.push_back(prog.inputs[k]);
      inputMap.push_back(k);
    }
  InputDecl d;
  d.name = "cut";
  d.lo = cut.lo;
  d.hi = cut.hi;
  d.type = e.nodes[cut.node].type;
  const uint32_t cutIndex = static_cast<uint32_t>(top.inputs.size());
  top.inputs.push_back(d);
  inputMap.push_back(UINT32_MAX);

  ExprBuilder b;
  std::vector<uint32_t> map(e.nodes.size(), 0);
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    if (!above[i]) continue;
    const Node& ni = e.nodes[i];
    if (i == cut.node) map[i] = b.input(cutIndex, ni.type);
    else if (ni.op == Op::Input) map[i] = b.input(newIndex[ni.input], ni.type);
    else map[i] = foldCopyNode(e, i, map, b);
  }
  top.target = b.finish(map[e.root]);
  top.budget = prog.budget;
  return top;
}

std::vector<Candidate> cutCandidates(const Program& prog, const Options& opt, uint32_t targetCost,
                                     uint32_t* searches) {
  if (searches) *searches = 0;
  for (const auto& d : prog.inputs)
    if (d.compileTime) return {};
  const CostModel& model = *opt.search.model;
  std::vector<Cut> cuts = findCuts(prog, model, opt.seed);
  if (cuts.size() > opt.maxCuts) cuts.resize(opt.maxCuts);
  const Options topOpt = partOptions(opt, opt.cutTime);
  const Options subOpt = partOptions(opt, opt.subtreeTime);

  std::vector<Candidate> out;
  std::set<std::string> seen;
  for (const Cut& c : cuts) {
    std::vector<uint32_t> inputMap;
    const Program top = topProgram(prog, c, inputMap);
    std::vector<std::pair<Expr, uint32_t>> tops = searchPart(top, topOpt, c.topCost, searches);
    if (tops.empty()) continue;
    if (tops.size() > 4) tops.resize(4);
    std::vector<Expr> subs = {subexpr(prog.target, c.node)};
    if (c.subCost <= opt.subtreeMaxCost) {
      auto found = searchPart(subProgram(prog, c.node), subOpt, c.subCost, searches);
      std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
      for (size_t k = 0; k < found.size() && k < 2; ++k) subs.push_back(found[k].first);
    }
    for (const auto& [t, tc] : tops)
      for (const Expr& s : subs) {
        Expr full;
        try {
          ExprBuilder b;
          const uint32_t sr = copyMapped(s, b, [&](uint32_t k, Type ty) { return b.input(k, ty); });
          const uint32_t r = copyMapped(t, b, [&](uint32_t k, Type ty) {
            return inputMap[k] == UINT32_MAX ? sr : b.input(inputMap[k], ty);
          });
          full = b.finish(r);
        } catch (const std::exception&) {
          continue;
        }
        const uint32_t cost = dagCost(full, model, prog.inputs);
        if (cost >= targetCost || !seen.insert(toString(full, prog.inputs)).second) continue;
        out.push_back({std::move(full), cost});
      }
  }
  return out;
}

}  // namespace sopt
