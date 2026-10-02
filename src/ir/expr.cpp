#include "ir/expr.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace sopt {
namespace {

uint64_t hashNode(const Node& n) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  mix(static_cast<uint64_t>(n.op));
  mix(static_cast<uint64_t>(n.type));
  mix(n.nargs);
  for (int k = 0; k < 4; ++k) {
    mix(n.args[k]);
    mix(n.swz[k]);
    mix(std::bit_cast<uint32_t>(n.value[k]));
  }
  mix(n.input);
  return h;
}

bool sameNode(const Node& a, const Node& b) {
  if (a.op != b.op || a.type != b.type || a.nargs != b.nargs || a.input != b.input) return false;
  for (int k = 0; k < 4; ++k)
    if (a.args[k] != b.args[k] || a.swz[k] != b.swz[k] ||
        std::bit_cast<uint32_t>(a.value[k]) != std::bit_cast<uint32_t>(b.value[k]))
      return false;
  return true;
}

}  // namespace

std::vector<InputDecl> slotDecls(const std::vector<InputDecl>& inputs) {
  std::vector<InputDecl> out;
  for (const auto& d : inputs) {
    const unsigned w = width(d.type);
    for (unsigned c = 0; c < w; ++c) {
      InputDecl s = d;
      s.type = Type::Float;
      if (w > 1) s.name += std::string(".") + "xyzw"[c];
      out.push_back(s);
    }
  }
  return out;
}

std::vector<uint32_t> inputSlots(const std::vector<InputDecl>& inputs) {
  std::vector<uint32_t> out;
  uint32_t slot = 0;
  for (const auto& d : inputs) {
    out.push_back(slot);
    slot += width(d.type);
  }
  return out;
}

std::optional<Type> inferType(Op op, const Type* args, unsigned nargs, unsigned swzCount) {
  for (unsigned k = 0; k < nargs; ++k)
    if (!isFloat(args[k]) && !(info(op).shape == Shape::Select && k == 0)) return std::nullopt;
  switch (info(op).shape) {
    case Shape::Leaf: return std::nullopt;
    case Shape::Comp:
    case Shape::Select: {
      const unsigned first = info(op).shape == Shape::Select ? 1 : 0;
      if (first == 1 && args[0] != Type::Bool) return std::nullopt;
      unsigned w = 1;
      for (unsigned k = first; k < nargs; ++k) w = std::max<unsigned>(w, width(args[k]));
      for (unsigned k = first; k < nargs; ++k)
        if (width(args[k]) != 1 && width(args[k]) != w) return std::nullopt;
      return floatType(w);
    }
    case Shape::Cmp:
      for (unsigned k = 0; k < nargs; ++k)
        if (args[k] != Type::Float) return std::nullopt;
      return Type::Bool;
    case Shape::Reduce:
      if (nargs == 2 && args[0] != args[1]) return std::nullopt;
      return Type::Float;
    case Shape::Same: return args[0];
    case Shape::Swizzle:
      if (swzCount < 1 || swzCount > 4) return std::nullopt;
      return floatType(swzCount);
    case Shape::Construct: {
      unsigned w = 0;
      for (unsigned k = 0; k < nargs; ++k) w += width(args[k]);
      if (w < 2 || w > 4) return std::nullopt;
      return floatType(w);
    }
  }
  return std::nullopt;
}

unsigned operandCount(const Node& n) { return n.nargs; }

uint32_t ExprBuilder::intern(const Node& n) {
  auto& bucket = map_[hashNode(n)];
  for (uint32_t idx : bucket)
    if (sameNode(nodes_[idx], n)) return idx;
  nodes_.push_back(n);
  const auto idx = static_cast<uint32_t>(nodes_.size() - 1);
  bucket.push_back(idx);
  return idx;
}

uint32_t ExprBuilder::input(uint32_t index, Type type) {
  Node n;
  n.op = Op::Input;
  n.type = type;
  n.input = index;
  return intern(n);
}

uint32_t ExprBuilder::constant(float v) { return constant(Type::Float, &v); }

uint32_t ExprBuilder::constant(Type type, const float* v) {
  Node n;
  n.op = Op::Const;
  n.type = type;
  for (unsigned k = 0; k < width(type); ++k)
    n.value[k] = (v[k] == 0.0f) ? 0.0f : v[k];  // no signed zero constants
  return intern(n);
}

uint32_t ExprBuilder::op(Op op, uint32_t a, uint32_t b, uint32_t c) {
  Node n;
  n.op = op;
  const auto& oi = info(op);
  n.nargs = oi.arity;
  n.args[0] = oi.arity > 0 ? a : 0;
  n.args[1] = oi.arity > 1 ? b : 0;
  n.args[2] = oi.arity > 2 ? c : 0;
  if (oi.commutative && n.args[1] < n.args[0]) std::swap(n.args[0], n.args[1]);
  if (op == Op::Mad && n.args[1] < n.args[0]) std::swap(n.args[0], n.args[1]);
  Type ts[3];
  for (unsigned k = 0; k < n.nargs; ++k) ts[k] = nodes_[n.args[k]].type;
  const auto t = inferType(op, ts, n.nargs);
  if (!t) throw std::invalid_argument("operand types don't fit " + std::string(oi.name));
  n.type = *t;
  return intern(n);
}

uint32_t ExprBuilder::swizzle(uint32_t a, const uint8_t* comps, unsigned count) {
  Node n;
  n.op = Op::Swizzle;
  n.nargs = 1;
  n.args[0] = a;
  const Type src = nodes_[a].type;
  const auto t = inferType(Op::Swizzle, &src, 1, count);
  if (!t) throw std::invalid_argument("bad swizzle");
  for (unsigned k = 0; k < count; ++k) {
    if (comps[k] >= width(src)) throw std::invalid_argument("swizzle component out of range");
    n.swz[k] = comps[k];
  }
  n.type = *t;
  // .x of a float1, .xyz of a float3: the operand itself.
  bool identity = count == width(src);
  for (unsigned k = 0; k < count && identity; ++k) identity = comps[k] == k;
  if (identity) return a;
  return intern(n);
}

uint32_t ExprBuilder::construct(const uint32_t* args, unsigned nargs) {
  if (nargs < 1 || nargs > 4) throw std::invalid_argument("bad constructor");
  Node n;
  n.op = Op::Construct;
  n.nargs = static_cast<uint8_t>(nargs);
  Type ts[4];
  for (unsigned k = 0; k < nargs; ++k) {
    n.args[k] = args[k];
    ts[k] = nodes_[args[k]].type;
  }
  const auto t = inferType(Op::Construct, ts, nargs);
  if (!t) throw std::invalid_argument("constructor needs 2 to 4 float components");
  n.type = *t;
  return intern(n);
}

Expr ExprBuilder::finish(uint32_t root) {
  // Keep only nodes reachable from root, preserving topological order.
  std::vector<char> live(nodes_.size(), 0);
  live[root] = 1;
  for (size_t i = nodes_.size(); i-- > 0;) {
    if (!live[i]) continue;
    const auto& n = nodes_[i];
    for (uint8_t k = 0; k < n.nargs; ++k) live[n.args[k]] = 1;
  }
  std::vector<uint32_t> remap(nodes_.size(), 0);
  Expr e;
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (!live[i]) continue;
    Node n = nodes_[i];
    for (uint8_t k = 0; k < n.nargs; ++k) n.args[k] = remap[n.args[k]];
    remap[i] = static_cast<uint32_t>(e.nodes.size());
    e.nodes.push_back(n);
  }
  e.root = remap[root];
  return e;
}

std::vector<uint32_t> useCounts(const Expr& e) {
  std::vector<uint32_t> uses(e.nodes.size(), 0);
  for (const auto& n : e.nodes)
    for (uint8_t k = 0; k < n.nargs; ++k) ++uses[n.args[k]];
  return uses;
}

int fusedArg(const Expr& e, uint32_t node, const std::vector<uint32_t>& uses, bool divIsMul) {
  const Node& n = e.nodes[node];
  if (n.op != Op::Add && n.op != Op::Sub) return -1;
  for (int k = 0; k < 2; ++k) {
    const uint32_t a = n.args[k];
    const Node& m = e.nodes[a];
    // Same width: a broadcast scalar product feeding a vector add is not one fma each.
    if (uses[a] == 1 && m.type == n.type && (m.op == Op::Mul || (divIsMul && m.op == Op::Div)))
      return k;
  }
  return -1;
}

bool takesOmod(Op op) {
  switch (op) {
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Mad: case Op::Div: case Op::Lerp: case Op::Dot:
    case Op::Rcp: case Op::Rsqrt: case Op::Sqrt: case Op::Length:
    case Op::Exp: case Op::Log: case Op::Exp2: case Op::Log2: case Op::Sin: case Op::Cos: case Op::Pow:
    case Op::Floor: case Op::Frac: case Op::Round: case Op::Ceil:
      return true;
    default: return false;
  }
}

bool isOmodScale(float v) {
  const float a = std::fabs(v);
  return a == 2.0f || a == 4.0f || a == 0.5f;
}

std::vector<bool> amdFoldedNodes(const Expr& e, const std::vector<uint32_t>& uses, const CostModel& m) {
  std::vector<bool> folded(e.nodes.size(), false);
  if (!m.amdFolds) return folded;
  std::vector<bool> fused(e.nodes.size(), false);  // a mul contracted into its consumer's fma
  if (m.fusedAdd)
    for (uint32_t i = 0; i < e.nodes.size(); ++i)
      if (const int f = fusedArg(e, i, uses, m.divIsMul); f >= 0) fused[e.nodes[i].args[f]] = true;
  auto omodConst = [&](uint32_t i) {
    const Node& c = e.nodes[i];
    if (c.op != Op::Const || !isOmodScale(c.value[0])) return false;
    for (unsigned k = 1; k < width(c.type); ++k)
      if (c.value[k] != c.value[0]) return false;
    return true;
  };
  auto isMinMax = [](Op op) { return op == Op::Min || op == Op::Max; };
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    if (n.op == Op::Mul && !fused[i]) {
      for (int k = 0; k < 2 && !folded[i]; ++k) {
        const uint32_t v = n.args[1 - k];
        const Node& p = e.nodes[v];
        folded[i] = omodConst(n.args[k]) && uses[v] == 1 && p.type == n.type && takesOmod(p.op) && !folded[v];
      }
    } else if (isMinMax(n.op)) {
      for (int k = 0; k < 2 && !folded[i]; ++k) {
        const uint32_t v = n.args[k];
        const Node& p = e.nodes[v];
        folded[i] = isMinMax(p.op) && uses[v] == 1 && p.type == n.type && !folded[v];
      }
    }
  }
  return folded;
}

uint32_t dagCost(const Expr& e, const CostModel& m) {
  const auto uses = (m.fusedAdd || m.amdFolds) ? useCounts(e) : std::vector<uint32_t>();
  const auto folded = amdFoldedNodes(e, uses, m);
  uint32_t cost = 0;
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    const bool reduce = info(n.op).shape == Shape::Reduce;
    const unsigned w = width(reduce ? e.nodes[n.args[0]].type : n.type);
    cost += folded[i] ? w
            : (m.fusedAdd && fusedArg(e, i, uses, m.divIsMul) >= 0) ? w * m.fusedAdd
                                                                     : m.opCost(n.op, w);
  }
  return cost;
}

std::vector<bool> compileTimeNodes(const Expr& e, const std::vector<InputDecl>& inputs) {
  std::vector<bool> ct(e.nodes.size(), false);
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    if (n.op == Op::Const) {
      ct[i] = true;
    } else if (n.op == Op::Input) {
      ct[i] = n.input < inputs.size() && inputs[n.input].compileTime;
    } else {
      bool all = true;
      for (unsigned k = 0; k < operandCount(n); ++k) all = all && ct[n.args[k]];
      ct[i] = all;
    }
  }
  return ct;
}

uint32_t dagCost(const Expr& e, const CostModel& m, const std::vector<InputDecl>& inputs) {
  const auto ct = compileTimeNodes(e, inputs);
  const auto uses = (m.fusedAdd || m.amdFolds) ? useCounts(e) : std::vector<uint32_t>();
  const auto folded = amdFoldedNodes(e, uses, m);
  uint32_t cost = 0;
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    if (ct[i]) continue;
    const Node& n = e.nodes[i];
    const bool reduce = info(n.op).shape == Shape::Reduce;
    const unsigned w = width(reduce ? e.nodes[n.args[0]].type : n.type);
    const int f = m.fusedAdd ? fusedArg(e, i, uses, m.divIsMul) : -1;
    bool fold = folded[i];
    for (unsigned k = 0; fold && k < operandCount(n); ++k) fold = !ct[n.args[k]] || e.nodes[n.args[k]].op == Op::Const;
    cost += fold ? w : (f >= 0 && !ct[n.args[f]]) ? w * m.fusedAdd : m.opCost(n.op, w);
  }
  return cost;
}

bool containsInexact(const Expr& e) {
  for (const auto& n : e.nodes)
    if (!info(n.op).exact) return true;
  return false;
}

bool isSugarCall(std::string_view name, size_t arity) {
  if (arity == 1) return name == "radians" || name == "degrees" || name == "log10" || name == "tan";
  return arity == 2 && name == "cross";
}

uint32_t buildSugarCall(ExprBuilder& b, std::string_view name, const uint32_t* args, size_t arity) {
  if (!isSugarCall(name, arity)) throw std::invalid_argument("unknown intrinsic " + std::string(name));
  const auto& ns = b.nodes();
  // Operands are built one statement at a time: argument evaluation order is unspecified,
  // and node order decides how the DAG prints.
  if (name == "radians" || name == "degrees") {
    const uint32_t k = b.constant(name == "radians" ? 0.017453292519943295f : 57.29577951308232f);
    return b.op(Op::Mul, args[0], k);
  }
  if (name == "log10") {
    const uint32_t l = b.op(Op::Log2, args[0]);
    return b.op(Op::Mul, l, b.constant(0.30102999566398120f));
  }
  if (name == "tan") {
    const uint32_t s = b.op(Op::Sin, args[0]);
    return b.op(Op::Div, s, b.op(Op::Cos, args[0]));
  }
  if (ns[args[0]].type != Type::Float3 || ns[args[1]].type != Type::Float3)
    throw std::invalid_argument("cross needs float3 operands");
  static constexpr uint8_t kYzx[3] = {1, 2, 0}, kZxy[3] = {2, 0, 1};
  const uint32_t ay = b.swizzle(args[0], kYzx, 3), bz = b.swizzle(args[1], kZxy, 3);
  const uint32_t l = b.op(Op::Mul, ay, bz);
  const uint32_t az = b.swizzle(args[0], kZxy, 3), by = b.swizzle(args[1], kYzx, 3);
  const uint32_t r = b.op(Op::Mul, az, by);
  return b.op(Op::Sub, l, r);
}

bool containsOp(const Expr& e, Op op) {
  for (const auto& n : e.nodes)
    if (n.op == op) return true;
  return false;
}

bool needsPrecise(const Expr& e) {
  auto big = [&](uint32_t i, float sign, const Node* other) {
    const Node& c = e.nodes[i];
    if (c.op != Op::Const) return false;
    for (unsigned k = 0; k < width(c.type); ++k) {
      const float v = c.value[k], o = other ? other->value[std::min<unsigned>(k, width(other->type) - 1)] : v;
      if (!(std::fabs(v) >= 4194304.0f) || (other && v != sign * o)) return false;
    }
    return true;
  };
  for (const auto& n : e.nodes) {
    if ((n.op != Op::Sub && n.op != Op::Add) || n.nargs < 2) continue;
    const float sign = n.op == Op::Sub ? 1.0f : -1.0f;
    for (int side = 0; side < (n.op == Op::Add ? 2 : 1); ++side) {
      const Node& inner = e.nodes[n.args[side]];
      const uint32_t outer = n.args[1 - side];
      if (!big(outer, 1.0f, nullptr)) continue;
      if (inner.op == Op::Add)
        for (int j = 0; j < 2; ++j)
          if (big(inner.args[j], sign, &e.nodes[outer])) return true;
      if (inner.op == Op::Mad && big(inner.args[2], sign, &e.nodes[outer])) return true;  // mad(a, b, c) - c
    }
  }
  return false;
}

Type nodeType(const Expr& e, uint32_t node) { return e.nodes[node].type; }

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
  if (n.op == Op::Const && width(n.type) == 1 && (n.value[0] < 0.0f || std::signbit(n.value[0])))
    return 7;
  return info(n.op).prec;
}

// A vector constant with all components equal (float3(0.5, 0.5, 0.5)).
bool splatConst(const Node& c) {
  if (c.op != Op::Const || width(c.type) < 2) return false;
  for (unsigned k = 1; k < width(c.type); ++k)
    if (c.value[k] != c.value[0]) return false;
  return true;
}

std::string print(const Expr& e, const std::vector<InputDecl>& inputs, uint32_t idx) {
  const auto& n = e.nodes[idx];
  const auto& oi = info(n.op);
  // Componentwise ops broadcast scalars (here and in HLSL): a splat constant next to a
  // non-constant vector operand prints as a scalar, mad(v, 0.5, 0.5).
  bool scalarSplats = false;
  if (oi.shape == Shape::Comp)
    for (uint8_t k = 0; k < n.nargs; ++k) {
      const Node& c = e.nodes[n.args[k]];
      scalarSplats = scalarSplats || (c.op != Op::Const && width(c.type) > 1);
    }
  auto sub = [&](uint32_t child, bool paren) {
    const Node& c = e.nodes[child];
    std::string s = scalarSplats && splatConst(c) ? formatFloat(c.value[0]) : print(e, inputs, child);
    return paren ? "(" + s + ")" : s;
  };
  // a + -c prints as a - c.
  auto negConst = [&](uint32_t child) {
    const Node& c = e.nodes[child];
    return c.op == Op::Const && (width(c.type) == 1 || (scalarSplats && splatConst(c))) && c.value[0] < 0.0f;
  };
  if (n.op == Op::Add && n.nargs == 2 && (negConst(n.args[0]) != negConst(n.args[1]))) {
    const uint32_t v = negConst(n.args[1]) ? n.args[0] : n.args[1];
    const Node& c = e.nodes[negConst(n.args[1]) ? n.args[1] : n.args[0]];
    const uint8_t p = info(Op::Sub).prec;
    return sub(v, precOf(e, v) < p) + " - " + formatFloat(-c.value[0]);
  }
  switch (oi.syntax) {
    case Syntax::Leaf:
      if (n.op == Op::Input)
        return n.input < inputs.size() ? inputs[n.input].name : "in" + std::to_string(n.input);
      if (width(n.type) == 1) return formatFloat(n.value[0]);
      {
        std::string s = "float" + std::to_string(width(n.type)) + "(";
        for (unsigned k = 0; k < width(n.type); ++k) s += (k ? ", " : "") + formatFloat(n.value[k]);
        return s + ")";
      }
    case Syntax::Call:
    case Syntax::Construct: {
      std::string s = oi.syntax == Syntax::Construct ? "float" + std::to_string(width(n.type))
                                                     : std::string(oi.name);
      s += "(";
      for (uint8_t k = 0; k < n.nargs; ++k) {
        if (k) s += ", ";
        s += sub(n.args[k], false);
      }
      return s + ")";
    }
    case Syntax::Swizzle: {
      std::string s = sub(n.args[0], precOf(e, n.args[0]) < 9) + ".";  // c.x, f(c).x, (a + b).x
      for (unsigned k = 0; k < width(n.type); ++k) s += "xyzw"[n.swz[k]];
      return s;
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
