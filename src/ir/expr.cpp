#include "ir/expr.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>

namespace sopt {
namespace {

uint64_t hashNode(const Node& n) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  mix(static_cast<uint64_t>(n.op));
  mix(n.args[0]);
  mix(n.args[1]);
  mix(n.args[2]);
  mix(std::bit_cast<uint32_t>(n.value));
  mix(n.input);
  return h;
}

bool sameNode(const Node& a, const Node& b) {
  return a.op == b.op && a.args[0] == b.args[0] && a.args[1] == b.args[1] &&
         a.args[2] == b.args[2] &&
         std::bit_cast<uint32_t>(a.value) == std::bit_cast<uint32_t>(b.value) &&
         a.input == b.input;
}

}  // namespace

uint32_t ExprBuilder::intern(const Node& n) {
  auto& bucket = map_[hashNode(n)];
  for (uint32_t idx : bucket)
    if (sameNode(nodes_[idx], n)) return idx;
  nodes_.push_back(n);
  const auto idx = static_cast<uint32_t>(nodes_.size() - 1);
  bucket.push_back(idx);
  return idx;
}

uint32_t ExprBuilder::input(uint32_t index) {
  Node n;
  n.op = Op::Input;
  n.input = index;
  return intern(n);
}

uint32_t ExprBuilder::constant(float v) {
  Node n;
  n.op = Op::Const;
  n.value = (v == 0.0f) ? 0.0f : v;  // no signed zero constants
  return intern(n);
}

uint32_t ExprBuilder::op(Op op, uint32_t a, uint32_t b, uint32_t c) {
  Node n;
  n.op = op;
  const auto& oi = info(op);
  n.args[0] = oi.arity > 0 ? a : 0;
  n.args[1] = oi.arity > 1 ? b : 0;
  n.args[2] = oi.arity > 2 ? c : 0;
  if (oi.commutative && n.args[1] < n.args[0]) std::swap(n.args[0], n.args[1]);
  if (op == Op::Mad && n.args[1] < n.args[0]) std::swap(n.args[0], n.args[1]);
  return intern(n);
}

Expr ExprBuilder::finish(uint32_t root) {
  // Keep only nodes reachable from root, preserving topological order.
  std::vector<char> live(nodes_.size(), 0);
  live[root] = 1;
  for (size_t i = nodes_.size(); i-- > 0;) {
    if (!live[i]) continue;
    const auto& n = nodes_[i];
    for (uint8_t k = 0; k < info(n.op).arity; ++k) live[n.args[k]] = 1;
  }
  std::vector<uint32_t> remap(nodes_.size(), 0);
  Expr e;
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (!live[i]) continue;
    Node n = nodes_[i];
    for (uint8_t k = 0; k < info(n.op).arity; ++k) n.args[k] = remap[n.args[k]];
    remap[i] = static_cast<uint32_t>(e.nodes.size());
    e.nodes.push_back(n);
  }
  e.root = remap[root];
  return e;
}

std::vector<uint32_t> useCounts(const Expr& e) {
  std::vector<uint32_t> uses(e.nodes.size(), 0);
  for (const auto& n : e.nodes)
    for (uint8_t k = 0; k < info(n.op).arity; ++k) ++uses[n.args[k]];
  return uses;
}

int fusedArg(const Expr& e, uint32_t node, const std::vector<uint32_t>& uses, bool divIsMul) {
  const Node& n = e.nodes[node];
  if (n.op != Op::Add && n.op != Op::Sub) return -1;
  for (int k = 0; k < 2; ++k) {
    const uint32_t a = n.args[k];
    const Op op = e.nodes[a].op;
    if (uses[a] == 1 && (op == Op::Mul || (divIsMul && op == Op::Div))) return k;
  }
  return -1;
}

uint32_t dagCost(const Expr& e, const CostModel& m) {
  const auto uses = m.fusedAdd ? useCounts(e) : std::vector<uint32_t>();
  uint32_t cost = 0;
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Op op = e.nodes[i].op;
    cost += (m.fusedAdd && fusedArg(e, i, uses, m.divIsMul) >= 0) ? m.fusedAdd : m[op];
  }
  return cost;
}

bool containsInexact(const Expr& e) {
  for (const auto& n : e.nodes)
    if (!info(n.op).exact) return true;
  return false;
}

bool containsOp(const Expr& e, Op op) {
  for (const auto& n : e.nodes)
    if (n.op == op) return true;
  return false;
}

Type nodeType(const Expr& e, uint32_t node) { return info(e.nodes[node].op).result; }

std::string formatFloat(float v) {
  if (std::isnan(v)) return "(0.0 / 0.0)";
  if (std::isinf(v)) return v > 0 ? "(1.0 / 0.0)" : "(-1.0 / 0.0)";
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), v);
  std::string s(buf, res.ptr);
  if (s.find_first_of(".e") == std::string::npos) s += ".0";
  return s;
}

namespace {

uint8_t precOf(const Expr& e, uint32_t idx) {
  const auto& n = e.nodes[idx];
  if (n.op == Op::Const && (n.value < 0.0f || std::signbit(n.value))) return 7;
  return info(n.op).prec;
}

std::string print(const Expr& e, const std::vector<InputDecl>& inputs, uint32_t idx) {
  const auto& n = e.nodes[idx];
  const auto& oi = info(n.op);
  auto sub = [&](uint32_t child, bool paren) {
    std::string s = print(e, inputs, child);
    return paren ? "(" + s + ")" : s;
  };
  switch (oi.syntax) {
    case Syntax::Leaf:
      if (n.op == Op::Input)
        return n.input < inputs.size() ? inputs[n.input].name : "in" + std::to_string(n.input);
      return formatFloat(n.value);
    case Syntax::Call: {
      std::string s(oi.name);
      s += "(";
      for (uint8_t k = 0; k < oi.arity; ++k) {
        if (k) s += ", ";
        s += sub(n.args[k], false);
      }
      return s + ")";
    }
    case Syntax::Prefix:
      return std::string(oi.symbol) + sub(n.args[0], precOf(e, n.args[0]) <= oi.prec);
    case Syntax::Infix: {
      const uint8_t p = oi.prec;
      const bool cmp = p == 3;
      const bool lp = cmp ? precOf(e, n.args[0]) <= p : precOf(e, n.args[0]) < p;
      const bool rp = precOf(e, n.args[1]) <= p;
      return sub(n.args[0], lp) + " " + std::string(oi.symbol) + " " + sub(n.args[1], rp);
    }
    case Syntax::Ternary:
      return sub(n.args[0], precOf(e, n.args[0]) < 3) + " ? " +
             sub(n.args[1], precOf(e, n.args[1]) <= 2) + " : " +
             sub(n.args[2], precOf(e, n.args[2]) <= 2);
  }
  return "?";
}

}  // namespace

std::string toString(const Expr& e, const std::vector<InputDecl>& inputs) {
  if (e.nodes.empty()) return "";
  return print(e, inputs, e.root);
}

}  // namespace sopt
