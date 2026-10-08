#include "search/reshape.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace sopt {
namespace {

// Rate rank of a value: constants 0, compile-time inputs 1, uniforms 2, per-pixel values 3,
// fetches 4 + their source order; an op takes its latest operand's.
std::vector<uint32_t> ranks(const Expr& e, const std::vector<InputDecl>& inputs) {
  std::vector<uint32_t> r(e.nodes.size(), 0);
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    if (n.op == Op::Input) {
      const InputDecl& d = inputs[n.input];
      r[i] = d.compileTime                           ? 1
             : d.rate == InputDecl::Rate::Uniform    ? 2
             : d.rate == InputDecl::Rate::Fetch      ? 4 + d.fetchOrder
                                                     : 3;
    } else {
      for (unsigned k = 0; k < operandCount(n); ++k) r[i] = std::max(r[i], r[n.args[k]]);
    }
  }
  return r;
}

class Reshaper {
 public:
  Reshaper(const Expr& e, const std::vector<InputDecl>& inputs, bool distribute)
      : e_(e), rank_(ranks(e, inputs)), uses_(useCounts(e)), distribute_(distribute) {}

  Expr run() { return b_.finish(build(e_.root)); }

 private:
  struct Term {
    uint32_t node;  // in the new expression
    uint32_t rank;
    bool neg;
  };

  bool sumNode(uint32_t i) const {
    const Op op = e_.nodes[i].op;
    return op == Op::Add || op == Op::Sub || op == Op::Mad;
  }
  // Operand j of a node of type t is flattened into it: same op family, same type, used once.
  bool inlineSum(uint32_t j, Type t) const { return sumNode(j) && e_.nodes[j].type == t && uses_[j] == 1; }
  bool inlineMul(uint32_t j, Type t) const { return e_.nodes[j].op == Op::Mul && e_.nodes[j].type == t && uses_[j] == 1; }

  void sumTerms(uint32_t i, bool neg, Type t, std::vector<Term>& out) {
    const Node& n = e_.nodes[i];
    auto operand = [&](uint32_t j, bool ng) {
      if (inlineSum(j, t)) sumTerms(j, ng, t, out);
      else out.push_back({build(j), rank_[j], ng});
    };
    if (n.op == Op::Mad) {
      std::vector<std::pair<uint32_t, uint32_t>> f;  // (old node, rank) of the product's factors
      mulFactors(n.args[0], e_.nodes[n.args[0]].type, f);
      mulFactors(n.args[1], e_.nodes[n.args[1]].type, f);
      out.push_back({product(f), std::max(rank_[n.args[0]], rank_[n.args[1]]), neg});
      operand(n.args[2], neg);
    } else {
      operand(n.args[0], neg);
      operand(n.args[1], n.op == Op::Sub ? !neg : neg);
    }
  }

  void mulFactors(uint32_t j, Type t, std::vector<std::pair<uint32_t, uint32_t>>& out) {
    if (inlineMul(j, t)) {
      mulFactors(e_.nodes[j].args[0], t, out);
      mulFactors(e_.nodes[j].args[1], t, out);
    } else {
      out.push_back({j, rank_[j]});
    }
  }

  // Factors (old nodes) multiplied in rate order; scalar constants are folded together.
  uint32_t product(std::vector<std::pair<uint32_t, uint32_t>> f, uint32_t extra = UINT32_MAX) {
    std::stable_sort(f.begin(), f.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::vector<uint32_t> nodes;
    float k = 1.0f;
    bool haveK = false;
    for (const auto& [j, r] : f) {
      const Node& n = e_.nodes[j];
      if (n.op == Op::Const && n.type == Type::Float) {
        k = haveK ? k * n.value[0] : n.value[0];
        haveK = true;
      } else {
        nodes.push_back(build(j));
      }
    }
    std::vector<uint32_t> all;
    if (haveK && k != 1.0f) all.push_back(b_.constant(k));
    if (extra != UINT32_MAX) all.push_back(extra);
    all.insert(all.end(), nodes.begin(), nodes.end());
    if (all.empty()) return b_.constant(haveK ? k : 1.0f);
    uint32_t acc = all[0];
    for (size_t i = 1; i < all.size(); ++i) acc = b_.op(Op::Mul, acc, all[i]);
    return acc;
  }

  uint32_t sum(std::vector<Term> terms) {
    std::stable_sort(terms.begin(), terms.end(), [](const Term& a, const Term& b) { return a.rank < b.rank; });
    // Start from a positive term (a negative first one would need a negation).
    for (size_t i = 0; i < terms.size(); ++i)
      if (!terms[i].neg) {
        std::rotate(terms.begin(), terms.begin() + i, terms.begin() + i + 1);
        break;
      }
    uint32_t acc = terms[0].neg ? b_.op(Op::Neg, terms[0].node) : terms[0].node;
    for (size_t i = 1; i < terms.size(); ++i) {
      const Node m = b_.nodes()[terms[i].node];
      // A product term is one mad with the sum so far.
      if (!terms[i].neg && m.op == Op::Mul) acc = b_.op(Op::Mad, m.args[0], m.args[1], acc);
      else acc = b_.op(terms[i].neg ? Op::Sub : Op::Add, acc, terms[i].node);
    }
    return acc;
  }

  uint32_t build(uint32_t i) {
    if (auto it = done_.find(i); it != done_.end()) return it->second;
    const Node& n = e_.nodes[i];
    uint32_t r;
    if (n.op == Op::Input) {
      r = b_.input(n.input, n.type);
    } else if (n.op == Op::Const) {
      r = b_.constant(n.type, n.value);
    } else if (sumNode(i) && isFloat(n.type)) {
      std::vector<Term> terms;
      sumTerms(i, false, n.type, terms);
      r = sum(std::move(terms));
    } else if (n.op == Op::Mul && isFloat(n.type)) {
      std::vector<std::pair<uint32_t, uint32_t>> f;
      mulFactors(n.args[0], n.type, f);
      mulFactors(n.args[1], n.type, f);
      r = distribute_ ? distributed(f, n.type) : UINT32_MAX;
      if (r == UINT32_MAX) r = product(std::move(f));
    } else {
      uint32_t a[4] = {0, 0, 0, 0};
      for (unsigned k = 0; k < operandCount(n); ++k) a[k] = build(n.args[k]);
      if (n.op == Op::Swizzle) r = b_.swizzle(a[0], n.swz, width(n.type));
      else if (n.op == Op::Construct) r = b_.construct(a, n.nargs);
      else r = b_.op(n.op, a[0], a[1], a[2]);
    }
    done_[i] = r;
    return r;
  }

  // (sum) * w with w of constants / compile-time inputs / uniforms and a sum that reads a fetch:
  // the terms times w, so the late terms enter through one fma each. UINT32_MAX if it does not apply.
  uint32_t distributed(const std::vector<std::pair<uint32_t, uint32_t>>& f, Type t) {
    size_t s = f.size();
    for (size_t k = 0; k < f.size(); ++k) {
      if (inlineSum(f[k].first, t) && rank_[f[k].first] >= 4) {
        if (s != f.size()) return UINT32_MAX;  // more than one sum
        s = k;
      } else if (f[k].second > 2) {
        return UINT32_MAX;  // a per-pixel or fetched weight: no gain from spreading it
      }
    }
    if (s == f.size()) return UINT32_MAX;
    std::vector<std::pair<uint32_t, uint32_t>> w;
    for (size_t k = 0; k < f.size(); ++k)
      if (k != s) w.push_back(f[k]);
    // The sum's terms as old nodes (products of their factors) with their signs.
    struct OldTerm {
      std::vector<std::pair<uint32_t, uint32_t>> factors;
      uint32_t rank;
      bool neg;
    };
    std::vector<OldTerm> terms;
    auto collect = [&](auto&& self, uint32_t j, bool neg) -> void {
      const Node& n = e_.nodes[j];
      if (!inlineSum(j, t) && j != f[s].first) {
        OldTerm o{{}, rank_[j], neg};
        mulFactors(j, e_.nodes[j].type, o.factors);
        terms.push_back(std::move(o));
        return;
      }
      if (n.op == Op::Mad) {
        OldTerm o{{}, std::max(rank_[n.args[0]], rank_[n.args[1]]), neg};
        mulFactors(n.args[0], e_.nodes[n.args[0]].type, o.factors);
        mulFactors(n.args[1], e_.nodes[n.args[1]].type, o.factors);
        terms.push_back(std::move(o));
        self(self, n.args[2], neg);
      } else {
        self(self, n.args[0], neg);
        self(self, n.args[1], n.op == Op::Sub ? !neg : neg);
      }
    };
    collect(collect, f[s].first, false);
    std::vector<Term> out;
    for (auto& o : terms) {
      std::vector<std::pair<uint32_t, uint32_t>> all = w;
      all.insert(all.end(), o.factors.begin(), o.factors.end());
      out.push_back({product(std::move(all)), o.rank, o.neg});
    }
    return sum(std::move(out));
  }

  const Expr& e_;
  std::vector<uint32_t> rank_;
  std::vector<uint32_t> uses_;
  bool distribute_;
  ExprBuilder b_;
  std::unordered_map<uint32_t, uint32_t> done_;
};

}  // namespace

std::vector<Expr> reshapeForms(const Expr& e, const std::vector<InputDecl>& inputs, const CostModel& model) {
  std::vector<Expr> out;
  std::vector<std::string> seen = {toString(e, inputs)};
  const ScheduleMetrics m0 = scheduleMetrics(e, model, inputs);
  const uint32_t c0 = dagCost(e, model, inputs);
  for (bool distribute : {false, true}) {
    try {
      Expr r = Reshaper(e, inputs, distribute).run();
      const std::string s = toString(r, inputs);
      if (std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
      const ScheduleMetrics m = scheduleMetrics(r, model, inputs);
      if (dagCost(r, model, inputs) >= c0 && m.perfCost >= m0.perfCost && m.tail >= m0.tail && m.critical >= m0.critical)
        continue;
      seen.push_back(s);
      out.push_back(std::move(r));
    } catch (const std::invalid_argument&) {
      // operand types that do not fit in the new order: no form
    }
  }
  return out;
}

}  // namespace sopt
