#include "search/stoke.hpp"

#include <chrono>
#include <cmath>
#include <set>
#include <stdexcept>

#include "ir/eval.hpp"
#include "ir/expr.hpp"
#include "verify/points.hpp"
#include "verify/verify.hpp"

namespace sopt {
namespace {

double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Ops an edit may put in place of another of the same shape and arity: the base ones and those of the target.
std::vector<Op> opsLike(Op op, const Expr& target) {
  std::vector<Op> out;
  const OpInfo& oi = info(op);
  for (size_t k = 0; k < static_cast<size_t>(Op::Count); ++k) {
    const Op o = static_cast<Op>(k);
    const OpInfo& x = info(o);
    if (o == op || x.shape != oi.shape || x.arity != oi.arity || o == Op::Input || o == Op::Const ||
        o == Op::Swizzle || o == Op::Construct)
      continue;
    bool inTarget = false;
    for (const Node& n : target.nodes) inTarget = inTarget || n.op == o;
    if (x.base || inTarget) out.push_back(o);
  }
  return out;
}

// The edited node list rebuilt (hash-consed, constants folded, dead nodes dropped); nullopt where types do not fit.
std::optional<Expr> rebuild(const Expr& e) {
  try {
    ExprBuilder b;
    std::vector<uint32_t> map(e.nodes.size());
    for (uint32_t i = 0; i <= e.root; ++i) {
      const Node& n = e.nodes[i];
      map[i] = n.op == Op::Input ? b.input(n.input, n.type) : foldCopyNode(e, i, map, b);
    }
    return b.finish(map[e.root]);
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
}

}  // namespace

std::vector<Candidate> stokeCandidates(const Program& prog, const CostModel& model, uint32_t targetCost, double seconds,
                                       uint64_t seed) {
  std::vector<Candidate> out;
  if (seconds <= 0.0 || targetCost < 2) return out;
  const PointSet ps = makeRandomPoints(prog, 128, seed, true);
  const std::vector<float> target = evalAll(prog.target, ps, kProfileRef);
  const Type rt = prog.target.nodes[prog.target.root].type;
  // Constants an edit may use: the target's, and a few common ones.
  std::vector<float> pool = {0.0f, 1.0f, 2.0f, 0.5f, -1.0f};
  for (const Node& n : prog.target.nodes)
    if (n.op == Op::Const && n.type == Type::Float) pool.push_back(n.value[0]);
  // Wrong test points (outside the budget of the target there); points where the target is not finite do not count.
  auto wrong = [&](const Expr& e) {
    const std::vector<float> v = evalAll(e, ps, kProfileRef);
    uint32_t bad = 0;
    for (size_t k = 0; k < v.size(); ++k)
      if (std::isfinite(target[k]) && !pointWithinBudget(prog.budget, target[k], v[k])) ++bad;
    return bad;
  };
  const double penalty = std::max(1.0, targetCost / 4.0);  // per wrong point, in cost units
  auto energy = [&](uint32_t bad, uint32_t cost) { return bad * penalty + cost; };
  Rng rng(seed ^ 0x5702E5u);
  auto pick = [&](size_t n) { return static_cast<size_t>(rng.next() % std::max<size_t>(n, 1)); };
  Expr cur = prog.target;
  uint32_t curBad = 0, curCost = targetCost;
  std::set<std::string> seen;
  const double end = now() + seconds;
  const double temperature = 1.0;
  for (uint64_t step = 0; now() < end && out.size() < 32; ++step) {
    if (step % 20000 == 19999) {  // restart from the target now and then
      cur = prog.target;
      curBad = 0;
      curCost = targetCost;
    }
    Expr m = cur;
    const uint32_t i = static_cast<uint32_t>(pick(m.root + 1));
    Node& n = m.nodes[i];
    if (n.op == Op::Input || n.op == Op::Const) continue;
    const unsigned na = operandCount(n);
    switch (pick(4)) {
      case 0: {  // another op of the same shape
        const std::vector<Op> like = opsLike(n.op, prog.target);
        if (like.empty()) continue;
        n.op = like[pick(like.size())];
        break;
      }
      case 1: {  // an operand: an earlier node or a constant
        if (na == 0 || i == 0) continue;
        const unsigned k = static_cast<unsigned>(pick(na));
        if (rng.next() & 1) {
          n.args[k] = static_cast<uint32_t>(pick(i));
        } else {
          Node c;
          c.op = Op::Const;
          c.type = Type::Float;
          c.value[0] = pool[pick(pool.size())];
          m.nodes.insert(m.nodes.begin(), c);  // index 0: every reference moves up by one
          for (Node& x : m.nodes)
            if (&x != &m.nodes[0])
              for (unsigned a = 0; a < operandCount(x); ++a) ++x.args[a];
          ++m.root;
          m.nodes[i + 1].args[k] = 0;
        }
        break;
      }
      case 2: {  // the node replaced by one of its operands everywhere
        if (na == 0) continue;
        const uint32_t by = n.args[pick(na)];
        for (uint32_t j = i + 1; j < m.nodes.size(); ++j)
          for (unsigned a = 0; a < operandCount(m.nodes[j]); ++a)
            if (m.nodes[j].args[a] == i) m.nodes[j].args[a] = by;
        if (m.root == i) m.root = by;
        break;
      }
      default:  // operands swapped
        if (na < 2) continue;
        std::swap(n.args[0], n.args[1 + pick(na - 1)]);
        break;
    }
    const std::optional<Expr> next = rebuild(m);
    if (!next || next->nodes[next->root].type != rt) continue;
    const uint32_t bad = wrong(*next), cost = dagCost(*next, model, prog.inputs);
    const double dE = energy(bad, cost) - energy(curBad, curCost);
    if (dE > 0.0 && rng.uniform() >= std::exp(-dE / temperature)) continue;
    cur = *next;
    curBad = bad;
    curCost = cost;
    if (bad == 0 && cost < targetCost && seen.insert(toString(cur, prog.inputs)).second) out.push_back({cur, cost});
  }
  return out;
}

}  // namespace sopt
