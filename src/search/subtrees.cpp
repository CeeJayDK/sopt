#include "search/subtrees.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

#include "ir/eval.hpp"

namespace sopt {
namespace {

// Copies node i of src into b (operands already mapped).
uint32_t copyNode(const Expr& src, uint32_t i, const std::vector<uint32_t>& map, ExprBuilder& b) {
  const Node& n = src.nodes[i];
  if (n.op == Op::Input) return b.input(n.input, n.type);
  return foldCopyNode(src, i, map, b);
}


// Results of subtree searches, shared by all regions of a run (the same subexpression
// over the same domain recurs, e.g. per color channel or in shared headers).
std::mutex cacheMu;
std::map<std::string, std::vector<std::pair<Expr, uint32_t>>> cache;

std::string cacheKey(const Program& p, const Options& o) {
  std::string key = toString(p.target, p.inputs);
  char buf[160];
  for (const auto& d : p.inputs) {
    std::snprintf(buf, sizeof(buf), "|%s %d %.9g %.9g %u %d %.9g", d.name.c_str(), static_cast<int>(d.type), d.lo,
                  d.hi, d.grid, d.compileTime ? 1 : 0, d.value);
    key += buf;
  }
  std::snprintf(buf, sizeof(buf), "|%d %.9g %d %d|%.3g|", static_cast<int>(p.budget.kind), p.budget.eps,
                p.budget.vsExact ? 1 : 0, p.budget.errorScale ? 1 : 0, o.search.timeLimitSec);
  return key + buf + std::string(o.search.model->name);
}

}  // namespace

Options partOptions(const Options& opt, double seconds) {
  Options inner = opt;
  inner.subtrees = false;
  inner.cuts = false;
  inner.v3 = false;  // only the region's own variants get a proof
  inner.accuracyVariants = false;
  inner.loose = 0.0;
  inner.maxAlternatives = 4;
  inner.maxIterations = 3;
  inner.v1Points = std::min<size_t>(opt.v1Points, 1u << 16);
  inner.search.timeLimitSec = seconds;
  return inner;
}

Program subProgram(const Program& prog, uint32_t node) {
  Program p;
  p.inputs = prog.inputs;
  p.target = subexpr(prog.target, node);
  p.budget.kind = prog.budget.kind == Budget::Kind::Exact ? Budget::Kind::Exact : Budget::Kind::Rel;
  p.budget.eps = 1e-6;
  p.budget.vsExact = prog.budget.vsExact;
  p.budget.errorScale = prog.budget.errorScale;
  return p;
}

std::vector<std::pair<Expr, uint32_t>> searchPart(const Program& p, const Options& inner, uint32_t cost,
                                                  uint32_t* searches) {
  const std::string key = cacheKey(p, inner);
  std::vector<std::pair<Expr, uint32_t>> found;
  {
    std::lock_guard<std::mutex> lock(cacheMu);
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
  }
  if (searches) ++*searches;
  try {
    const RunResult r = optimize(p, inner);
    for (const auto& a : r.accepted)
      if (a.klass != Klass::LessAccurate && a.cost < cost) found.emplace_back(a.expr, a.cost);
  } catch (const std::exception&) {
    found.clear();  // one part failing must not stop the region
  }
  std::lock_guard<std::mutex> lock(cacheMu);
  cache.emplace(key, found);
  return found;
}

uint32_t insertExpr(const Expr& e, ExprBuilder& b) {
  std::vector<uint32_t> map(e.nodes.size());
  for (uint32_t i = 0; i < e.nodes.size(); ++i) map[i] = copyNode(e, i, map, b);
  return map[e.root];
}

Expr simplifyIdentities(const Expr& e) {
  ExprBuilder b;
  const auto& ns = b.nodes();
  auto is = [&](uint32_t n, float v) {
    if (ns[n].op != Op::Const) return false;
    for (unsigned k = 0; k < width(ns[n].type); ++k)
      if (ns[n].value[k] != v) return false;
    return true;
  };
  std::vector<uint32_t> map(e.nodes.size());
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    const Type t = n.type;
    const uint32_t a = n.nargs > 0 ? map[n.args[0]] : 0, c1 = n.nargs > 1 ? map[n.args[1]] : 0,
                   c2 = n.nargs > 2 ? map[n.args[2]] : 0;
    uint32_t r = UINT32_MAX;
    // keep(x): x itself when it already has the node's type.
    auto keep = [&](uint32_t x) { return ns[x].type == t ? x : UINT32_MAX; };
    switch (n.op) {
      case Op::Mad:
        if (is(c2, 0.0f)) r = b.op(Op::Mul, a, c1);
        else if (is(a, 1.0f)) r = b.op(Op::Add, c1, c2);
        else if (is(c1, 1.0f)) r = b.op(Op::Add, a, c2);
        else if (is(a, -1.0f)) r = b.op(Op::Sub, c2, c1);
        else if (is(c1, -1.0f)) r = b.op(Op::Sub, c2, a);
        if (r != UINT32_MAX && ns[r].type != t) r = UINT32_MAX;
        break;
      case Op::Mul:
        if (is(a, 1.0f)) r = keep(c1);
        else if (is(c1, 1.0f)) r = keep(a);
        break;
      case Op::Add:
        if (is(a, 0.0f)) r = keep(c1);
        else if (is(c1, 0.0f)) r = keep(a);
        break;
      case Op::Sub:
        if (is(c1, 0.0f)) r = keep(a);
        break;
      default:
        break;
    }
    map[i] = r != UINT32_MAX ? r : copyNode(e, i, map, b);
  }
  return b.finish(map[e.root]);
}

Expr subexpr(const Expr& e, uint32_t i) {
  ExprBuilder b;
  std::vector<uint32_t> map(e.nodes.size());
  for (uint32_t k = 0; k <= i; ++k) map[k] = copyNode(e, k, map, b);
  return b.finish(map[i]);
}

Expr replaceNodes(const Expr& e, const std::vector<std::pair<uint32_t, const Expr*>>& repl) {
  ExprBuilder b;
  std::vector<uint32_t> map(e.nodes.size());
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Expr* r = nullptr;
    for (const auto& [node, x] : repl)
      if (node == i) r = x;
    map[i] = r ? insertExpr(*r, b) : copyNode(e, i, map, b);
  }
  return b.finish(map[e.root]);
}

std::vector<Candidate> subtreeCandidates(const Program& prog, const Options& opt, uint32_t targetCost,
                                         uint32_t* searches) {
  const Expr& e = prog.target;
  const CostModel& model = *opt.search.model;
  const std::vector<bool> ctime = compileTimeNodes(e, prog.inputs);
  // below[i][k]: node k is in the subexpression of node i.
  std::vector<std::vector<char>> below(e.nodes.size(), std::vector<char>(e.nodes.size(), 0));
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    below[i][i] = 1;
    for (uint8_t k = 0; k < e.nodes[i].nargs; ++k)
      for (uint32_t j = 0; j < e.nodes.size(); ++j)
        if (below[e.nodes[i].args[k]][j]) below[i][j] = 1;
  }

  // Subexpressions worth searching: at least two operations, cheap enough to search.
  struct Sub {
    uint32_t node, cost;
  };
  std::vector<Sub> subs;
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Op op = e.nodes[i].op;
    if (i == e.root || ctime[i] || op == Op::Input || op == Op::Const || op == Op::Swizzle || op == Op::Construct)
      continue;
    if (!isFloat(e.nodes[i].type)) continue;  // comparisons (bool) are not search targets
    uint32_t ops = 0;
    for (uint32_t j = 0; j < e.nodes.size(); ++j) {
      const Op oj = e.nodes[j].op;
      ops += below[i][j] && !ctime[j] && oj != Op::Input && oj != Op::Const && oj != Op::Swizzle;
    }
    if (ops < 2) continue;
    const uint32_t c = dagCost(subexpr(e, i), model, prog.inputs);
    if (c >= 2 && c <= opt.subtreeMaxCost) subs.push_back({i, c});
  }
  // Largest first: they have the most to gain; keep at most maxSubtrees.
  std::stable_sort(subs.begin(), subs.end(), [](const Sub& a, const Sub& b) { return a.cost > b.cost; });
  if (subs.size() > opt.maxSubtrees) subs.resize(opt.maxSubtrees);

  const Options inner = partOptions(opt, opt.subtreeTime);

  struct Repl {
    uint32_t node, saving;
    Expr expr;
  };
  std::vector<Repl> repls;
  uint32_t n = 0;
  for (const Sub& s : subs) {
    for (auto& [x, c] : searchPart(subProgram(prog, s.node), inner, s.cost, &n))
      repls.push_back({s.node, s.cost - c, std::move(x)});
  }
  if (searches) *searches = n;

  std::vector<Candidate> out;
  std::set<std::string> seen;
  auto add = [&](const std::vector<std::pair<uint32_t, const Expr*>>& r) {
    Expr full;
    try {
      full = replaceNodes(e, r);
    } catch (const std::exception&) {
      return;
    }
    const uint32_t c = dagCost(full, model, prog.inputs);
    if (c >= targetCost || !seen.insert(toString(full, prog.inputs)).second) return;
    out.push_back({std::move(full), c});
  };
  for (const Repl& r : repls) add({{r.node, &r.expr}});
  // The best replacement per node, greedily combined over disjoint subexpressions.
  std::stable_sort(repls.begin(), repls.end(), [](const Repl& a, const Repl& b) { return a.saving > b.saving; });
  std::vector<std::pair<uint32_t, const Expr*>> chosen;
  for (const Repl& r : repls) {
    bool disjoint = true;
    for (const auto& [node, x] : chosen) disjoint = disjoint && !below[node][r.node] && !below[r.node][node];
    if (!disjoint) continue;
    chosen.emplace_back(r.node, &r.expr);
    if (chosen.size() > 1) add(chosen);
  }
  return out;
}

}  // namespace sopt
