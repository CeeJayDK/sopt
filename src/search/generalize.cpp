// Programs with compile-time inputs (preprocessor definitions such as a far plane F):
// the search runs with them set to their current value, then the numeric constants of
// each candidate are generalized into expressions of the compile-time inputs (e.g.
// 0.001001001 = 1 / (F - 1) for F = 1000) and the result is verified over the whole
// range of the inputs. The compiler folds those expressions, so they cost nothing.
#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "ir/eval.hpp"
#include "search/driver.hpp"

namespace sopt {
namespace {

// Copies node `i` of `src` into `b`, mapping operands through `map`; folds nodes whose
// operands are all constants.
uint32_t copyNode(const Expr& src, uint32_t i, const std::vector<uint32_t>& map, ExprBuilder& b) {
  const Node& n = src.nodes[i];
  const auto& ns = b.nodes();
  bool allConst = n.op != Op::Input && n.op != Op::Const;
  for (unsigned k = 0; k < operandCount(n); ++k) allConst = allConst && ns[map[n.args[k]]].op == Op::Const;
  if (allConst) {
    Node m = n;
    Type ts[4];
    float buf[4][4];
    const float* ptr[4][4];
    for (unsigned k = 0; k < operandCount(n); ++k) {
      m.args[k] = map[n.args[k]];
      ts[k] = ns[m.args[k]].type;
      for (unsigned c = 0; c < 4; ++c) {
        buf[k][c] = ns[m.args[k]].value[c];
        ptr[k][c] = &buf[k][c];
      }
    }
    float res[4] = {0, 0, 0, 0};
    float* out[4] = {&res[0], &res[1], &res[2], &res[3]};
    std::vector<float> tmp;
    evalNode(m, ts, ptr, out, 1, kProfileRef, tmp);
    return b.constant(n.type, res);
  }
  switch (n.op) {
    case Op::Const: return b.constant(n.type, n.value);
    case Op::Swizzle: return b.swizzle(map[n.args[0]], n.swz, width(n.type));
    case Op::Construct: {
      uint32_t a[4];
      for (unsigned k = 0; k < n.nargs; ++k) a[k] = map[n.args[k]];
      return b.construct(a, n.nargs);
    }
    default:
      return b.op(n.op, map[n.args[0]], operandCount(n) > 1 ? map[n.args[1]] : 0,
                  operandCount(n) > 2 ? map[n.args[2]] : 0);
  }
}

// Small expressions of the compile-time inputs, by their value at the current setting.
struct CtExpr {
  Op op = Op::Input;  // Input: aux = input index; Const: val = the constant
  int a = -1, b = -1;
  uint32_t aux = 0;
  float at[3] = {0, 0, 0};  // value at the current setting and two other points
  int size = 0;             // number of ops
};

std::vector<CtExpr> ctBank(const Program& prog, const std::vector<float>& extraConsts) {
  std::vector<CtExpr> bank;
  std::unordered_set<uint64_t> seen;
  // Evaluation points: the current values, then two others inside each range.
  std::vector<std::array<float, 3>> inputAt(prog.inputs.size());
  for (size_t i = 0; i < prog.inputs.size(); ++i) {
    const auto& d = prog.inputs[i];
    inputAt[i] = {static_cast<float>(d.value), static_cast<float>(d.lo + 0.3183 * (d.hi - d.lo)),
                  static_cast<float>(d.lo + 0.7071 * (d.hi - d.lo))};
  }
  auto key = [](const float* v) {
    uint64_t h = 0;
    for (int k = 0; k < 3; ++k) h = h * 0x9E3779B97F4A7C15ull + std::bit_cast<uint32_t>(v[k]);
    return h;
  };
  auto add = [&](CtExpr e) {
    for (float v : e.at)
      if (!std::isfinite(v)) return;
    if (e.at[0] == e.at[1] && e.at[1] == e.at[2] && e.op != Op::Const) return;  // no input dependence
    if (!seen.insert(key(e.at)).second) return;
    bank.push_back(e);
  };
  for (uint32_t i = 0; i < prog.inputs.size(); ++i)
    if (prog.inputs[i].compileTime && prog.inputs[i].type == Type::Float) {
      CtExpr e;
      e.op = Op::Input;
      e.aux = i;
      for (int k = 0; k < 3; ++k) e.at[k] = inputAt[i][k];
      add(e);
    }
  if (bank.empty()) return bank;
  std::vector<float> consts = {1.0f, 2.0f, 0.5f};
  for (float c : extraConsts)
    if (c != 0.0f && std::find(consts.begin(), consts.end(), c) == consts.end()) consts.push_back(c);
  for (float c : consts) {
    CtExpr e;
    e.op = Op::Const;
    e.at[0] = e.at[1] = e.at[2] = c;
    add(e);
  }
  // Up to four operations: unary and binary combinations by size.
  const Op unary[] = {Op::Neg, Op::Rcp, Op::Sqrt};
  const Op binary[] = {Op::Add, Op::Sub, Op::Mul, Op::Div};
  for (int size = 1; size <= 4 && bank.size() < 300000; ++size) {
    const size_t end = bank.size();
    for (size_t x = 0; x < end; ++x) {
      if (bank[x].size != size - 1) continue;
      for (Op op : unary) {
        CtExpr e{op, static_cast<int>(x), -1, 0, {}, size};
        for (int k = 0; k < 3; ++k) e.at[k] = evalScalar(op, bank[x].at[k], 0, 0, kProfileRef);
        add(e);
      }
    }
    for (size_t x = 0; x < end; ++x)
      for (size_t y = 0; y < end; ++y) {
        if (bank[x].size + bank[y].size != size - 1) continue;
        for (Op op : binary) {
          if ((op == Op::Add || op == Op::Mul) && y < x) continue;
          CtExpr e{op, static_cast<int>(x), static_cast<int>(y), 0, {}, size};
          for (int k = 0; k < 3; ++k) e.at[k] = evalScalar(op, bank[x].at[k], bank[y].at[k], 0, kProfileRef);
          add(e);
        }
      }
  }
  return bank;
}

uint32_t buildCt(const std::vector<CtExpr>& bank, int i, const Program& prog, ExprBuilder& b) {
  const CtExpr& e = bank[i];
  switch (e.op) {
    case Op::Input: return b.input(e.aux, prog.inputs[e.aux].type);
    case Op::Const: return b.constant(e.at[0]);
    default:
      if (e.b < 0) return b.op(e.op, buildCt(bank, e.a, prog, b));
      return b.op(e.op, buildCt(bank, e.a, prog, b), buildCt(bank, e.b, prog, b));
  }
}

}  // namespace

RunResult optimizeSpecialized(const Program& prog, const Options& opt) {
  const double t0 = nowSeconds();
  // The program with the compile-time inputs replaced by their current values.
  Program spec;
  spec.budget = prog.budget;
  spec.outputName = prog.outputName;
  std::vector<uint32_t> newIndex(prog.inputs.size(), UINT32_MAX), oldIndex;
  for (uint32_t i = 0; i < prog.inputs.size(); ++i)
    if (!prog.inputs[i].compileTime) {
      newIndex[i] = static_cast<uint32_t>(spec.inputs.size());
      oldIndex.push_back(i);
      spec.inputs.push_back(prog.inputs[i]);
    }
  {
    ExprBuilder b;
    std::vector<uint32_t> map(prog.target.nodes.size());
    for (uint32_t i = 0; i < prog.target.nodes.size(); ++i) {
      const Node& n = prog.target.nodes[i];
      if (n.op == Op::Input) {
        const InputDecl& d = prog.inputs[n.input];
        if (d.compileTime) {
          const float v[4] = {float(d.value), float(d.value), float(d.value), float(d.value)};
          map[i] = b.constant(d.type, v);
        } else {
          map[i] = b.input(newIndex[n.input], d.type);
        }
      } else {
        map[i] = copyNode(prog.target, i, map, b);
      }
    }
    spec.target = b.finish(map[prog.target.root]);
  }

  Options inner = opt;
  inner.specialize = false;
  RunResult res;
  res.targetCost = dagCost(prog.target, *opt.search.model, prog.inputs);
  res.targetText = toString(prog.target, prog.inputs);
  if (res.targetCost == 0 || spec.inputs.empty()) return res;
  RunResult sr = optimize(spec, inner);
  res.search = sr.search;
  res.iterations = sr.iterations;
  res.counterexamples = sr.counterexamples;
  res.searchSec = sr.searchSec;

  // Generalize and verify over the full range (V1 sampling, every profile).
  const double tv = nowSeconds();
  std::vector<float> targetConsts;
  for (const auto& n : prog.target.nodes)
    if (n.op == Op::Const && n.type == Type::Float) targetConsts.push_back(n.value[0]);
  const std::vector<CtExpr> bank = ctBank(prog, targetConsts);
  std::vector<int> byValue(bank.size());
  for (size_t i = 0; i < bank.size(); ++i) byValue[i] = static_cast<int>(i);
  std::sort(byValue.begin(), byValue.end(), [&](int x, int y) {
    return bank[x].at[0] != bank[y].at[0] ? bank[x].at[0] < bank[y].at[0] : bank[x].size < bank[y].size;
  });
  auto matches = [&](float c) {
    std::vector<int> out;
    // Fitted constants are close to the true value (outer ones solved by least squares
    // less so, so an odd expression can be closer than the right one); near a large
    // setting (F = 1000) neighbours like 1 / (F - 2) are only 1e-6 away. Simplest first,
    // then closest; the verification decides.
    const float tol = 2e-5f * std::max(std::fabs(c), 1e-30f);
    auto it = std::lower_bound(byValue.begin(), byValue.end(), c - tol,
                               [&](int i, float v) { return bank[i].at[0] < v; });
    for (; it != byValue.end() && bank[*it].at[0] <= c + tol; ++it)
      if (bank[*it].op != Op::Const) out.push_back(*it);
    std::stable_sort(out.begin(), out.end(), [&](int x, int y) {
      if (bank[x].size != bank[y].size) return bank[x].size < bank[y].size;
      return std::fabs(bank[x].at[0] - c) < std::fabs(bank[y].at[0] - c);
    });
    if (out.size() > 12) out.resize(12);
    return out;
  };

  const PointSet quick = makeRandomPoints(prog, opt.stage2Points, opt.seed + 1, true);
  const std::vector<float> quickTarget = evalAll(prog.target, quick, kProfileRef);
  const PointSet v1 = makeRandomPoints(prog, opt.v1Points, opt.seed + 2, true);
  std::vector<std::vector<float>> v1Target;
  for (const auto& prof : kAllProfiles) v1Target.push_back(evalAll(prog.target, v1, prof));

  for (const Accepted& a : sr.accepted) {
    if (res.accepted.size() >= opt.maxAlternatives) break;
    // Scalar constants of the candidate and their options: the literal, or an
    // expression of the compile-time inputs with (about) the same value.
    std::vector<uint32_t> constNodes;
    std::vector<std::vector<int>> options;  // -1 = literal
    for (uint32_t i = 0; i < a.expr.nodes.size(); ++i) {
      const Node& n = a.expr.nodes[i];
      if (n.op != Op::Const || n.type != Type::Float) continue;
      constNodes.push_back(i);
      std::vector<int> o = {-1};
      for (int m : matches(n.value[0])) o.push_back(m);
      options.push_back(o);
    }
    // Combinations of option ranks (0 = the literal, k = the k-th smallest expression),
    // smallest rank sum first, at most 1024; the quick check below is cheap.
    std::vector<std::vector<int>> ranks = {{}};
    for (const auto& o : options) {
      std::vector<std::vector<int>> next;
      for (const auto& c : ranks)
        for (int k = 0; k < static_cast<int>(o.size()); ++k) {
          auto d = c;
          d.push_back(k);
          next.push_back(std::move(d));
        }
      ranks = std::move(next);
      if (ranks.size() > 20000) break;
    }
    std::stable_sort(ranks.begin(), ranks.end(), [](const auto& x, const auto& y) {
      int sx = 0, sy = 0;
      for (int v : x) sx += v;
      for (int v : y) sy += v;
      return sx < sy;
    });
    if (ranks.size() > 1024) ranks.resize(1024);
    std::vector<std::vector<int>> combos;
    for (const auto& r : ranks) {
      if (r.size() != options.size()) continue;
      std::vector<int> c;
      for (size_t k = 0; k < r.size(); ++k) c.push_back(options[k][r[k]]);
      combos.push_back(std::move(c));
    }
    for (const auto& combo : combos) {
      ExprBuilder b;
      std::vector<uint32_t> map(a.expr.nodes.size());
      for (uint32_t i = 0; i < a.expr.nodes.size(); ++i) {
        const Node& n = a.expr.nodes[i];
        const auto ci = std::find(constNodes.begin(), constNodes.end(), i);
        if (ci != constNodes.end() && combo[ci - constNodes.begin()] >= 0)
          map[i] = buildCt(bank, combo[ci - constNodes.begin()], prog, b);
        else if (n.op == Op::Input)
          map[i] = b.input(oldIndex[n.input], n.type);
        else
          map[i] = copyNode(a.expr, i, map, b);
      }
      Expr g = b.finish(map[a.expr.root]);
      const uint32_t cost = dagCost(g, *opt.search.model, prog.inputs);
      if (cost >= res.targetCost) continue;
      if (!compare(prog, g, quick, kProfileRef, 1, &quickTarget).pass) continue;
      Metrics worst;
      for (size_t p = 0; p < kAllProfiles.size() && worst.pass; ++p)
        worst.merge(compare(prog, g, v1, kAllProfiles[p], opt.threads, &v1Target[p]));
      if (!worst.pass) {
        ++res.rejectedV1;
        continue;
      }
      Accepted r;
      r.text = toString(g, prog.inputs);
      r.cost = cost;
      r.klass = classify(prog, g, worst);
      r.worst = worst;
      r.expr = std::move(g);
      res.accepted.push_back(std::move(r));
      break;  // the simplest generalization that holds
    }
  }
  std::stable_sort(res.accepted.begin(), res.accepted.end(),
                   [](const Accepted& x, const Accepted& y) { return x.cost < y.cost; });
  res.verifySec = sr.verifySec + (nowSeconds() - tv);
  res.totalSec = nowSeconds() - t0;
  return res;
}

}  // namespace sopt
