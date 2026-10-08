#include "fx/blend.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>

#include "fx/codegen.hpp"
#include "fx/variants.hpp"
#include "verify/verify.hpp"

namespace sopt::fx {
namespace {

using source::findWord;
using source::joinLines;
using source::nameEnd;
using source::skipSpace;
using source::Text;
using source::trim;

std::string noSpace(const std::string& s) {
  std::string out;
  for (char c : s)
    if (!std::isspace(static_cast<unsigned char>(c))) out += c;
  return out;
}

std::string lastName(const std::string& s) {
  const size_t c = s.rfind("::");
  return c == std::string::npos ? s : s.substr(c + 2);
}

// The texture channels a swizzle suffix (".rgb", ".xyz", "") picks, -1 if not a plain one.
std::vector<int> channels(const std::string& suffix) {
  if (suffix.empty()) return {0, 1, 2, 3};
  if (suffix[0] != '.') return {};
  std::vector<int> out;
  for (size_t i = 1; i < suffix.size(); ++i) {
    const char c = suffix[i];
    const int k = c == 'r' || c == 'x' ? 0 : c == 'g' || c == 'y' ? 1 : c == 'b' || c == 'z' ? 2 : c == 'a' || c == 'w' ? 3 : -1;
    if (k < 0) return {};
    out.push_back(k);
  }
  return out;
}

// A read of the back buffer at the pixel's own texcoord ("tex2D(ReShade::BackBuffer, texcoord).rgb", spaces
// removed): its channels, and the sampler's sRGB flag. Empty if it is not one.
std::vector<int> ownBackBufferRead(const Codegen& cg, const std::string& text, const std::set<std::string>& coords,
                                   bool& srgb) {
  std::string s = text, call;
  if (s.rfind("tex2D(", 0) == 0) call = "tex2D(";
  else if (s.rfind("tex2Dlod(", 0) == 0) call = "tex2Dlod(";
  else return {};
  const size_t close = s.find(')', call.size());
  if (close == std::string::npos) return {};
  // tex2Dlod's coordinate is float4(texcoord, 0, 0): the closing parenthesis of the call comes later.
  std::string args = s.substr(call.size());
  size_t end = 0;
  int depth = 1;
  for (; end < args.size() && depth > 0; ++end) depth += args[end] == '(' ? 1 : args[end] == ')' ? -1 : 0;
  if (depth != 0) return {};
  const std::string inner = args.substr(0, end - 1), suffix = args.substr(end);
  const size_t comma = inner.find(',');
  if (comma == std::string::npos) return {};
  const std::string sampler = lastName(inner.substr(0, comma));
  std::string coord = inner.substr(comma + 1);
  if (call == "tex2Dlod(") {
    bool ok = false;
    for (const auto& c : coords)
      for (const char* z : {",0,0)", ",0.0,0.0)", ",0.,0.)"})
        ok = ok || coord == "float4(" + c + z;
    if (!ok) return {};
  } else if (!coords.count(coord)) {
    return {};
  }
  bool found = false;
  for (const auto& [id, si] : cg.samplers) {
    if (lastName(si.name) != sampler) continue;
    if (si.textureSemantic != "COLOR") return {};
    found = true;
    srgb = si.srgb;
  }
  return found ? channels(suffix) : std::vector<int>{};
}

// Per color channel A + B * d over the region's inputs, built into b: node ids, kZero / kOne.
constexpr int kZero = -1, kOne = -2;
struct Lin {
  bool ok = false;
  int a = kZero, b = kZero;
};

struct Linear {
  const Expr& e;
  ExprBuilder& b;
  std::vector<bool> dep;  // node depends on d
  uint32_t dIn;           // d's input index
  unsigned dWidth;        // 3 (d = rgb) or 4 (d = rgba, rgb taken by .rgb)
  std::map<uint32_t, uint32_t> copies;
  std::map<uint32_t, Lin> memo;

  uint32_t one() { return b.constant(1.0f); }
  uint32_t val(int x) { return x == kOne ? one() : static_cast<uint32_t>(x); }
  int add(int x, int y) { return x == kZero ? y : y == kZero ? x : static_cast<int>(b.op(Op::Add, val(x), val(y))); }
  int sub(int x, int y) {
    if (y == kZero) return x;
    if (x == kZero) return static_cast<int>(b.op(Op::Neg, val(y)));
    return static_cast<int>(b.op(Op::Sub, val(x), val(y)));
  }
  int mul(int x, uint32_t y) { return x == kZero ? kZero : x == kOne ? static_cast<int>(y) : static_cast<int>(b.op(Op::Mul, val(x), y)); }
  int div(int x, uint32_t y) { return x == kZero ? kZero : static_cast<int>(b.op(Op::Div, val(x), y)); }
  int neg(int x) { return x == kZero ? kZero : static_cast<int>(b.op(Op::Neg, val(x))); }

  uint32_t copy(uint32_t i) {
    if (const auto it = copies.find(i); it != copies.end()) return it->second;
    const Node& n = e.nodes[i];
    uint32_t r;
    if (n.op == Op::Input) r = b.input(n.input, n.type);
    else if (n.op == Op::Const) r = b.constant(n.type, n.value);
    else if (n.op == Op::Swizzle) r = b.swizzle(copy(n.args[0]), n.swz, width(n.type));
    else if (n.op == Op::Construct) {
      uint32_t args[4];
      for (unsigned k = 0; k < n.nargs; ++k) args[k] = copy(n.args[k]);
      r = b.construct(args, n.nargs);
    } else {
      const unsigned c = operandCount(n);
      r = b.op(n.op, copy(n.args[0]), c > 1 ? copy(n.args[1]) : 0, c > 2 ? copy(n.args[2]) : 0);
    }
    copies[i] = r;
    return r;
  }

  // The rgb of d: the input itself (float3) or its .rgb.
  bool isRgb(uint32_t i) const {
    const Node& n = e.nodes[i];
    if (n.op == Op::Input && n.input == dIn && dWidth == 3) return true;
    if (n.op == Op::Swizzle && width(n.type) == 3 && n.swz[0] == 0 && n.swz[1] == 1 && n.swz[2] == 2) {
      const Node& s = e.nodes[n.args[0]];
      return s.op == Op::Input && s.input == dIn && dWidth == 4;
    }
    return false;
  }

  Lin lin(uint32_t i) {
    if (const auto it = memo.find(i); it != memo.end()) return it->second;
    Lin r;
    const Node& n = e.nodes[i];
    if (!dep[i]) {
      r = {true, static_cast<int>(copy(i)), kZero};
    } else if (isRgb(i)) {
      r = {true, kZero, kOne};
    } else {
      auto arg = [&](int k) { return lin(n.args[k]); };
      switch (n.op) {
        case Op::Add:
        case Op::Sub: {
          const Lin x = arg(0), y = arg(1);
          if (x.ok && y.ok) {
            r.ok = true;
            r.a = n.op == Op::Add ? add(x.a, y.a) : sub(x.a, y.a);
            r.b = n.op == Op::Add ? add(x.b, y.b) : sub(x.b, y.b);
          }
          break;
        }
        case Op::Neg: {
          const Lin x = arg(0);
          if (x.ok) r = {true, neg(x.a), neg(x.b)};
          break;
        }
        case Op::Mul:
        case Op::Mad: {
          const int dk = dep[n.args[0]] ? 0 : 1;  // the operand that depends on d
          if (dep[n.args[0]] && dep[n.args[1]]) break;
          const Lin x = arg(dk);
          if (!x.ok) break;
          const uint32_t y = copy(n.args[1 - dk]);
          r = {true, mul(x.a, y), mul(x.b, y)};
          if (n.op == Op::Mad) {
            const Lin c = arg(2);
            if (!c.ok) r.ok = false;
            else r = {true, add(r.a, c.a), add(r.b, c.b)};
          }
          break;
        }
        case Op::Div: {
          if (dep[n.args[1]]) break;
          const Lin x = arg(0);
          if (x.ok) r = {true, div(x.a, copy(n.args[1])), div(x.b, copy(n.args[1]))};
          break;
        }
        case Op::Lerp: {  // x0 + (x1 - x0) * t
          if (dep[n.args[2]]) break;
          const Lin x0 = arg(0), x1 = arg(1);
          if (!x0.ok || !x1.ok) break;
          const uint32_t t = copy(n.args[2]);
          auto mix = [&](int p, int q) -> int {
            if (p == kZero) return mul(q, t);
            if (q == kZero) return mul(p, b.op(Op::Sub, one(), t));
            return add(p, mul(sub(q, p), t));
          };
          r = {true, mix(x0.a, x1.a), mix(x0.b, x1.b)};
          break;
        }
        default:
          break;
      }
    }
    memo[i] = r;
    return r;
  }
};

std::optional<SourceRewrite> blendShader(const Effect& fx, const Function& ps, const Region& r, uint64_t seed) {
  const Codegen& cg = *fx.cg;
  const std::string file = ps.loc.source;
  const std::vector<std::string>* lines = sourceLines(file);
  if (!lines || ps.loc.line == 0 || ps.loc.line > lines->size()) return std::nullopt;

  // Where the color leaves the shader (owner, 2026-10-08: out parameters and color.rgb = ...; return color; too):
  //   Return:      the region is the return statement (the only one: an early return would write an
  //                unblended color);
  //   StoreReturn: the region is the last store to a float4 local, color = ... or color.rgb = ..., and
  //                the next statement is return color; (its alpha is the back buffer's or unused);
  //   OutParam:    the region is the last store to an out SV_Target / COLOR parameter (no return).
  enum class Out { Return, StoreReturn, OutParam } out = Out::Return;
  size_t returns = 0;
  for (const auto& s : ps.stmts) returns += s.kind == Statement::Kind::Return;
  const Statement* st = nullptr;   // StoreReturn / OutParam: the region's store
  const Statement* ret = nullptr;  // StoreReturn: the return after it
  std::string outName, outSuffix;  // the stored variable and ".rgb" / ""
  const bool ownStore = [&] {
    if (r.kind == Region::Kind::Return) return false;
    if (r.kind != Region::Kind::Store) return true;
    for (size_t i = 0; i < ps.stmts.size(); ++i) {
      const Statement& t = ps.stmts[i];
      if (t.kind == Statement::Kind::Store && t.loc.line >= r.line && t.loc.line <= r.lastLine) {
        st = &t;
        if (i + 1 < ps.stmts.size() && ps.stmts[i + 1].kind == Statement::Kind::Return) ret = &ps.stmts[i + 1];
      }
    }
    return false;
  }();
  if (ownStore) return std::nullopt;
  if (r.kind == Region::Kind::Return) {
    if (returns != 1) return std::nullopt;
  } else {
    if (!st) return std::nullopt;
    const auto v = cg.variables.find(st->var);
    if (v == cg.variables.end()) return std::nullopt;
    for (const auto& t : ps.stmts)  // the last store to it
      if ((t.kind == Statement::Kind::Store || t.kind == Statement::Kind::Init) && t.var == st->var && t.seq > st->seq)
        return std::nullopt;
    std::string lhs = trim(r.lhs);
    if (lhs.empty() || lhs.back() != '=') return std::nullopt;
    lhs = trim(lhs.substr(0, lhs.size() - 1));
    const size_t dot = lhs.find('.');
    outName = lhs.substr(0, dot);
    outSuffix = dot == std::string::npos ? "" : lhs.substr(dot);
    if (outName != v->second.name || (outSuffix != "" && outSuffix != ".rgb" && outSuffix != ".xyz")) return std::nullopt;
    const reshadefx::type& vt = v->second.type;
    if (!vt.is_floating_point() || vt.rows != 4 || vt.cols != 1) return std::nullopt;
    std::string sem = v->second.semantic;
    for (auto& c : sem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (v->second.kind == Variable::Kind::Param && vt.has(reshadefx::type::q_out) &&
        (sem == "SV_TARGET" || sem == "SV_TARGET0" || sem == "COLOR" || sem == "COLOR0")) {
      if (returns != 0) return std::nullopt;
      out = Out::OutParam;
    } else if (v->second.kind == Variable::Kind::Local && returns == 1 && ret && ret->block == st->block &&
               ret->loc.line > 0 && ret->loc.line <= lines->size()) {
      const std::string rl = noSpace((*lines)[ret->loc.line - 1]);
      if (rl != "return" + outName + ";") return std::nullopt;
      out = Out::StoreReturn;
    } else {
      return std::nullopt;
    }
  }

  // The pixel shader's texture coordinates (TEXCOORD0 parameters it never writes).
  std::set<std::string> coords;
  for (uint32_t id : ps.params) {
    const auto v = cg.variables.find(id);
    if (v == cg.variables.end()) return std::nullopt;
    std::string sem = v->second.semantic;
    for (auto& c : sem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (sem != "TEXCOORD" && sem != "TEXCOORD0") continue;
    bool stored = false;
    for (const auto& s : ps.stmts) stored = stored || (s.kind == Statement::Kind::Store && s.var == id);
    if (!stored) coords.insert(v->second.name);
  }
  if (coords.empty()) return std::nullopt;

  // d: an input that is the back buffer at the pixel (a fetch, or a local initialized with one and
  // never written again).
  const Program& p = r.prog;
  int dIn = -1;
  unsigned dWidth = 0;
  bool srgb = false;
  for (size_t k = 0; k < p.inputs.size() && dIn < 0; ++k) {
    const std::string& name = p.inputs[k].name;
    std::vector<int> ch;
    if (k < r.facts.size() && r.facts[k].fetch) {
      ch = ownBackBufferRead(cg, noSpace(name), coords, srgb);
    } else {
      const size_t dot = name.find('.');
      const std::string base = name.substr(0, dot), suffix = dot == std::string::npos ? "" : name.substr(dot);
      for (const auto& s : ps.stmts) {
        if (s.kind != Statement::Kind::Init) continue;
        const auto v = cg.variables.find(s.var);
        if (v == cg.variables.end() || v->second.kind != Variable::Kind::Local || v->second.name != base) continue;
        bool stored = false;  // the region's own store comes after its reads
        for (const auto& t : ps.stmts)
          stored = stored || (t.kind == Statement::Kind::Store && t.var == s.var && &t != st);
        if (stored || s.loc.line == 0 || s.loc.line > lines->size()) break;
        const std::string& l = (*lines)[s.loc.line - 1];
        const size_t eq = l.find('='), semi = l.find(';');
        if (eq == std::string::npos || semi == std::string::npos || semi < eq) break;
        const std::vector<int> init = ownBackBufferRead(cg, noSpace(l.substr(eq + 1, semi - eq - 1)), coords, srgb);
        std::vector<int> pick = channels(suffix);
        if (suffix.empty()) pick.resize(width(p.inputs[k].type));  // the whole local: its own width
        for (int c : pick)
          if (static_cast<size_t>(c) < init.size()) ch.push_back(init[c]);
        if (ch.size() != pick.size()) ch.clear();
        break;
      }
    }
    if ((ch == std::vector<int>{0, 1, 2} && width(p.inputs[k].type) == 3) ||
        (ch == std::vector<int>{0, 1, 2, 3} && width(p.inputs[k].type) == 4)) {
      dIn = static_cast<int>(k);
      dWidth = width(p.inputs[k].type);
    }
  }
  if (dIn < 0) return std::nullopt;

  // The passes: back buffer target, no blend state, the sampler's sRGB setting, PostProcessVS.
  size_t passes = 0;
  for (const auto& t : cg.mod().techniques)
    for (const auto& pa : t.passes) {
      if (pa.ps_entry_point != ps.uniqueName) continue;
      const Function* vs = cg.function(pa.vs_entry_point);
      if (!pa.render_target_names[0].empty() || pa.blend_enable[0] || pa.srgb_write_enable != srgb ||
          pa.render_target_write_mask[0] != 0xF || !vs || vs->name != "PostProcessVS")
        return std::nullopt;
      ++passes;
    }
  if (passes == 0) return std::nullopt;

  // The color: the root (float3), or float4(rgb, d.a).
  const Expr& e = p.target;
  const Node& root = e.nodes[e.root];
  uint32_t rgbNode = e.root;
  bool alphaKeep = false;
  if (width(root.type) == 4) {
    if (root.op != Op::Construct || root.nargs != 2 || width(nodeType(e, root.args[0])) != 3) return std::nullopt;
    const Node& a = e.nodes[root.args[1]];
    const Node* src = a.op == Op::Swizzle ? &e.nodes[a.args[0]] : nullptr;
    if (!src || width(a.type) != 1 || a.swz[0] != 3 || src->op != Op::Input || static_cast<int>(src->input) != dIn)
      return std::nullopt;
    rgbNode = root.args[0];
    alphaKeep = true;
  } else if (width(root.type) != 3) {
    return std::nullopt;
  }

  ExprBuilder b;
  Linear L{e, b, std::vector<bool>(e.nodes.size(), false), static_cast<uint32_t>(dIn), dWidth, {}, {}};
  for (size_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    bool d = n.op == Op::Input && static_cast<int>(n.input) == dIn;
    for (unsigned k = 0; k < operandCount(n); ++k) d = d || L.dep[n.args[k]];
    L.dep[i] = d;
  }
  if (!L.dep[rgbNode]) return std::nullopt;
  if (alphaKeep) {  // d.a only in the alpha
    for (size_t i = 0; i < e.nodes.size(); ++i) {
      const Node& n = e.nodes[i];
      if (n.op == Op::Swizzle && width(n.type) == 1 && n.swz[0] == 3 && i != root.args[1] &&
          e.nodes[n.args[0]].op == Op::Input && static_cast<int>(e.nodes[n.args[0]].input) == dIn)
        return std::nullopt;
    }
  }

  enum class Mode { Premul, Mul, Add, Screen, Min, Max } mode;
  int A = kZero, B = kZero;
  const Node& rn = e.nodes[rgbNode];
  if ((rn.op == Op::Min || rn.op == Op::Max) && (L.isRgb(rn.args[0]) != L.isRgb(rn.args[1])) &&
      !L.dep[L.isRgb(rn.args[0]) ? rn.args[1] : rn.args[0]]) {
    mode = rn.op == Op::Min ? Mode::Min : Mode::Max;
    A = static_cast<int>(L.copy(L.isRgb(rn.args[0]) ? rn.args[1] : rn.args[0]));
  } else {
    const Lin l = L.lin(rgbNode);
    if (!l.ok || l.b == kZero) return std::nullopt;
    A = l.a;
    B = l.b;
    if (A == kZero) mode = Mode::Mul;
    else if (B == kOne) mode = Mode::Add;
    else if (width(b.nodes()[B].type) == 1) mode = Mode::Premul;
    else mode = Mode::Screen;  // checked below: B = 1 - A
  }

  // The blend as an expression of the region's inputs, with the source clamped (8 / 10-bit targets).
  auto blended = [&](bool clamp) {
    auto sat = [&](uint32_t x) { return clamp ? b.op(Op::Saturate, x) : x; };
    const uint8_t rgb[3] = {0, 1, 2};
    const uint32_t d = dWidth == 3 ? b.input(dIn, Type::Float3) : b.swizzle(b.input(dIn, Type::Float4), rgb, 3);
    uint32_t c = 0;
    switch (mode) {
      case Mode::Premul: c = b.op(Op::Add, sat(L.val(A)), b.op(Op::Mul, sat(L.val(B)), d)); break;
      case Mode::Mul: c = b.op(Op::Mul, sat(L.val(B)), d); break;
      case Mode::Add: c = b.op(Op::Add, sat(L.val(A)), d); break;
      case Mode::Screen: c = b.op(Op::Add, sat(L.val(A)), b.op(Op::Mul, b.op(Op::Sub, b.constant(1.0f), sat(L.val(A))), d)); break;
      case Mode::Min: c = b.op(Op::Min, d, sat(L.val(A))); break;
      case Mode::Max: c = b.op(Op::Max, d, sat(L.val(A))); break;
    }
    if (alphaKeep) {
      const uint8_t a3[1] = {3};
      const uint32_t args[2] = {c, b.swizzle(b.input(dIn, Type::Float4), a3, 1)};
      c = b.construct(args, 2);
    }
    return b.finish(c);
  };

  // The blend unit rounds the source to the target's format before blending: one code more than the
  // pixel shader output's budget (0 codes). A store's own budget does not know it is the output.
  Program pb = p;
  if (out == Out::Return) {
    if (p.budget.kind != Budget::Kind::Color8) return std::nullopt;
  } else {
    pb.budget = Budget{};
    pb.budget.kind = Budget::Kind::Color8;
  }
  pb.budget.maxCodeDiff = std::max(pb.budget.maxCodeDiff, 1);
  auto passesAll = [&](const Program& prog, const Expr& cand) {
    const PointSet pts = makeRandomPoints(prog, size_t{1} << 16, seed, true);
    for (const auto& prof : kAllProfiles)
      if (!compare(prog, cand, pts, prof, 1).pass) return false;
    return true;
  };
  if (mode == Mode::Screen) {  // B = 1 - A at every point
    Program q = p;
    q.target = b.finish(static_cast<uint32_t>(B));
    const Expr oneMinusA = b.finish(b.op(Op::Sub, b.constant(1.0f), L.val(A)));
    q.budget = Budget{};
    q.budget.kind = Budget::Kind::Abs;
    q.budget.eps = 1e-6;
    q.budget.vsExact = false;
    if (!passesAll(q, oneMinusA)) return std::nullopt;
  }
  if (!passesAll(pb, blended(true))) return std::nullopt;
  std::string guard;
  {
    Program p10 = pb;
    p10.budget.kind = Budget::Kind::Color10;
    for (size_t k = 0; k < p10.inputs.size(); ++k)
      if (static_cast<int>(k) == dIn || (k < r.facts.size() && r.facts[k].source.rfind("BackBuffer (8-bit", 0) == 0))
        p10.inputs[k].grid = 1023;
    if (!passesAll(p10, blended(true))) guard += " && BUFFER_COLOR_BIT_DEPTH == 8";
    Program ph = pb;
    ph.budget.kind = Budget::Kind::Rel;
    ph.budget.eps = 1.0 / 2048.0;
    for (size_t k = 0; k < ph.inputs.size(); ++k)
      if (static_cast<int>(k) == dIn || (k < r.facts.size() && r.facts[k].source.rfind("BackBuffer", 0) == 0))
        ph.inputs[k].lo = kScRgbLo, ph.inputs[k].hi = kScRgbHi, ph.inputs[k].grid = 0;
    if (!passesAll(ph, blended(false))) guard += " && BUFFER_COLOR_SPACE <= 1";
  }

  // The source text.
  std::vector<InputDecl> codeInputs = p.inputs;
  for (auto& d : codeInputs)
    if (d.compileTime && isBufferSizeMacro(d.name)) d.name = "float(" + d.name + ")";
  auto text = [&](int x) { return toString(b.finish(L.val(x)), codeInputs); };
  std::string src, states;
  const bool float4Out = mode == Mode::Premul;
  switch (mode) {
    case Mode::Premul: src = "float4(" + text(A) + ", " + text(B) + ")"; states = "SrcBlend = ONE; DestBlend = SRCALPHA;"; break;
    case Mode::Mul: src = text(B); states = "SrcBlend = DESTCOLOR; DestBlend = ZERO;"; break;
    case Mode::Add: src = text(A); states = "SrcBlend = ONE; DestBlend = ONE;"; break;
    case Mode::Screen: src = text(A); states = "SrcBlend = ONE; DestBlend = INVSRCCOLOR;"; break;
    case Mode::Min: src = text(A); states = "BlendOp = MIN;"; break;
    case Mode::Max: src = text(A); states = "BlendOp = MAX;"; break;
  }
  if (!float4Out) {
    const Type st = mode == Mode::Mul ? b.nodes()[L.val(B)].type : b.nodes()[L.val(A)].type;
    if (width(st) == 1) src = "float3(" + src + ")";
  }
  states = "BlendEnable = true; " + states + " SrcBlendAlpha = ZERO; DestBlendAlpha = ONE;";

  // The header's return type: float4 for the premultiplied source (stores: a float4 variable).
  const std::string& head = (*lines)[ps.loc.line - 1];
  const size_t fn = head.find(ps.name);
  if (fn == std::string::npos) return std::nullopt;
  const std::string retType = out == Out::Return ? trim(head.substr(0, fn)) : "float4";
  if (retType != "float3" && retType != "float4") return std::nullopt;
  if (!float4Out && retType == "float4") src = "float4(" + src + ", 0.0)";

  SourceRewrite rw;
  rw.file = file;
  rw.line = r.line;
  rw.function = ps.name;
  rw.kind = "to the blend stage";
  rw.tag = "B";
  static const char* modeName[] = {"premultiplied: ONE, SRCALPHA", "multiply: DESTCOLOR, ZERO", "add: ONE, ONE",
                                   "screen: ONE, INVSRCCOLOR",     "BlendOp MIN",               "BlendOp MAX"};
  rw.description = std::string("final blend with the back buffer as blend states (") + modeName[static_cast<int>(mode)] +
                   (out == Out::Return ? "" : "; the back buffer's alpha is kept") +
                   "; check in ReShade: the blend unit is not measured)";

  if (out == Out::Return && float4Out && retType == "float3") {
    std::string h = head;
    const size_t t = h.find("float3");
    h.replace(t, 6, "float4");
    rw.edits.push_back({ps.loc.line, ps.loc.line, {h}, guard});
  }
  const std::string& first = (*lines)[r.line - 1];
  const std::string ind = first.substr(0, first.find_first_not_of(" \t"));
  if (out == Out::OutParam)
    rw.edits.push_back({r.line, r.lastLine, {ind + outName + " = " + src + ";"}, guard});
  else
    rw.edits.push_back({r.line, r.lastLine, {ind + "return " + src + ";"}, guard});
  if (out == Out::StoreReturn) rw.edits.push_back({ret->loc.line, ret->loc.line, {}, guard});
  for (const auto& [a, bb] : r.removed) rw.edits.push_back({a, bb, {}, guard});

  // The passes: the states after "PixelShader = <ps>;".
  const Text txt = joinLines(*lines);
  const std::string& s = txt.s;
  std::map<uint32_t, std::string> passLines;
  size_t found = 0;
  for (auto it = findWord(s, "PixelShader", 0, s.size()); !it.empty(); it.pop_back()) {
    const size_t at = it.back();
    size_t q = skipSpace(s, at + 11);
    if (q >= s.size() || s[q] != '=') continue;
    q = skipSpace(s, q + 1);
    const size_t qe = nameEnd(s, q);
    if (lastName(s.substr(q, qe - q)) != ps.name) continue;
    const size_t semi = skipSpace(s, qe);
    if (semi >= s.size() || s[semi] != ';') return std::nullopt;
    const uint32_t ln = txt.lineOf(semi);
    std::string& l = passLines.try_emplace(ln, (*lines)[ln - 1]).first->second;
    const size_t inLine = semi - txt.starts[ln - 1];
    // Positions after the insertion point are unchanged since the passes are visited from the end.
    l.insert(inLine + 1, " " + states);
    ++found;
  }
  if (found != passes) return std::nullopt;
  for (const auto& [ln, l] : passLines) rw.edits.push_back({ln, ln, {l}, guard});
  std::sort(rw.edits.begin(), rw.edits.end(), [](const LineEdit& x, const LineEdit& y) { return x.first < y.first; });
  for (size_t k = 1; k < rw.edits.size(); ++k)
    if (rw.edits[k].first <= rw.edits[k - 1].last) return std::nullopt;
  return rw;
}

}  // namespace

std::vector<SourceRewrite> blendRewrites(const Effect& fx, const RegionOptions& opt, uint64_t seed) {
  std::vector<SourceRewrite> out;
  if (!fx.cg || fx.hlsl) return out;
  const Codegen& cg = *fx.cg;
  std::set<std::string> called;
  for (const auto& f : cg.functions) called.insert(f->calls.begin(), f->calls.end());
  // Pixel shaders whose passes draw to the back buffer without a blend state.
  std::vector<const Function*> shaders;
  for (const auto& f : cg.functions) {
    const auto et = cg.entryPoints.find(f->uniqueName);
    if (et == cg.entryPoints.end() || et->second != reshadefx::shader_type::pixel || called.count(f->uniqueName)) continue;
    bool ok = false;
    for (const auto& t : cg.mod().techniques)
      for (const auto& pa : t.passes)
        if (pa.ps_entry_point == f->uniqueName) ok = pa.render_target_names[0].empty() && !pa.blend_enable[0];
    if (ok) shaders.push_back(f.get());
  }
  if (shaders.empty()) return out;
  // Their return statements, down to a single operation (lerp(color, layer, t)).
  RegionOptions ro = opt;
  ro.minOps = 1;
  SkipCount skipped;
  const std::vector<Region> regions = extractRegions(fx, nullptr, ro, skipped);
  for (const Function* f : shaders) {
    // The return statement's or the final store's region, the largest window first.
    std::vector<const Region*> cands;
    for (const Region& r : regions)
      if ((r.kind == Region::Kind::Return || r.kind == Region::Kind::Store) && r.function == f->name &&
          r.file == f->loc.source && r.guard.empty())
        cands.push_back(&r);
    std::stable_sort(cands.begin(), cands.end(), [](const Region* a, const Region* b) {
      if ((a->kind == Region::Kind::Return) != (b->kind == Region::Kind::Return)) return a->kind == Region::Kind::Return;
      return a->removed.size() > b->removed.size();
    });
    for (const Region* r : cands)
      if (auto rw = blendShader(fx, *f, *r, seed)) {
        out.push_back(std::move(*rw));
        break;
      }
  }
  return out;
}

}  // namespace sopt::fx
