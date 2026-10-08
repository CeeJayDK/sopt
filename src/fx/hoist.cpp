#include "fx/hoist.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>

#include "fx/codegen.hpp"
#include "fx/variants.hpp"

namespace sopt::fx {
namespace {

using source::identChar;
using source::joinLines;
using source::matching;
using source::skipSpace;
using source::Text;
using source::trim;

// "TEXCOORD0" and "TexCoord" are one semantic.
std::string semanticKey(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (s.empty() || !std::isdigit(static_cast<unsigned char>(s.back()))) s += '0';
  return s;
}

bool isPosition(const std::string& key) { return key == "SV_POSITION0" || key == "POSITION0"; }

int texcoordIndex(const std::string& key) {
  if (key.rfind("TEXCOORD", 0) != 0) return -1;
  return std::atoi(key.c_str() + 8);
}

// The text of each top-level comma-separated item in [a, b).
std::vector<std::pair<size_t, size_t>> splitItems(const std::string& s, size_t a, size_t b) {
  std::vector<std::pair<size_t, size_t>> out;
  size_t start = a;
  int depth = 0;
  for (size_t i = a; i <= b; ++i) {
    if (i < b && (s.compare(i, 2, "//") == 0 || s.compare(i, 2, "/*") == 0)) {
      i = skipSpace(s, i) - 1;
      continue;
    }
    const char c = i < b ? s[i] : ',';
    if (c == '(' || c == '[' || c == '{' || c == '<') ++depth;
    else if (c == ')' || c == ']' || c == '}' || c == '>') --depth;
    else if (c == ',' && depth <= 0) {
      out.push_back({start, i});
      start = i + 1;
    }
  }
  return out;
}

// Comments removed, whitespace collapsed.
std::string plain(const std::string& s, size_t a, size_t b) {
  std::string out;
  for (size_t i = a; i < b;) {
    if (s.compare(i, 2, "//") == 0 || s.compare(i, 2, "/*") == 0 || std::isspace(static_cast<unsigned char>(s[i]))) {
      i = std::min(skipSpace(s, i), b);
      if (!out.empty()) out += ' ';
      continue;
    }
    out += s[i++];
  }
  return trim(out);
}

// Positions of `word` as a whole identifier outside comments in [a, b).
std::vector<size_t> findWord(const std::string& s, const std::string& word, size_t a, size_t b) {
  std::vector<size_t> out;
  for (size_t i = a; i < b; ++i) {
    if (s.compare(i, 2, "//") == 0 || s.compare(i, 2, "/*") == 0) {
      i = skipSpace(s, i) - 1;
      continue;
    }
    if (s.compare(i, word.size(), word) == 0 && (i == 0 || !identChar(s[i - 1])) &&
        (i + word.size() >= s.size() || !identChar(s[i + word.size()])))
      out.push_back(i);
  }
  return out;
}

// A qualified name (A::B::c) starting at p; returns its end.
size_t nameEnd(const std::string& s, size_t p) {
  while (p < s.size() && (identChar(s[p]) || (s[p] == ':' && p + 1 < s.size() && s[p + 1] == ':')))
    p += s[p] == ':' ? 2 : 1;
  return p;
}

// One value moved to the vertex shader.
struct Hoist {
  std::string text;  // FX expression in the pixel shader's names
  unsigned width = 1;
  bool flat = false;      // uniforms only: nointerpolation
  bool perfGate = false;  // folds in performance mode: switched only without it
  std::string slot;       // "sopt_v0.xy"
};

// Degree of each node in the varyings: 0 = constant per draw (uniforms, constants), 1 = affine in
// the interpolated inputs, -1 = neither.
std::vector<int> degrees(const Expr& e, const std::vector<int>& inputDeg) {
  std::vector<int> d(e.nodes.size(), -1);
  for (size_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    auto a = [&](int k) { return d[n.args[k]]; };
    int r = -1;
    switch (n.op) {
      case Op::Input: r = inputDeg[n.input]; break;
      case Op::Const: r = 0; break;
      case Op::Add:
      case Op::Sub: r = a(0) >= 0 && a(1) >= 0 ? std::max(a(0), a(1)) : -1; break;
      case Op::Neg: r = a(0); break;
      case Op::Mul:
      case Op::Dot: r = a(0) == 0 ? a(1) : a(1) == 0 ? a(0) : -1; break;
      case Op::Div: r = a(1) == 0 ? a(0) : -1; break;
      case Op::Mad: {
        const int m = a(0) == 0 ? a(1) : a(1) == 0 ? a(0) : -1;
        r = m >= 0 && a(2) >= 0 ? std::max(m, a(2)) : -1;
        break;
      }
      case Op::Lerp:  // a + t * (b - a)
        r = a(2) == 0 && a(0) >= 0 && a(1) >= 0 ? std::max(a(0), a(1))
            : a(0) == 0 && a(1) == 0 && a(2) >= 0 ? a(2) : -1;
        break;
      case Op::Swizzle:
      case Op::Construct:
        r = 0;
        for (unsigned k = 0; k < n.nargs && r >= 0; ++k) r = a(k) < 0 ? -1 : std::max(r, a(k));
        break;
      default:  // anything else: only of values constant per draw
        r = 0;
        for (unsigned k = 0; k < operandCount(n) && r >= 0; ++k) r = a(k) == 0 ? 0 : -1;
        break;
    }
    d[i] = r;
  }
  return d;
}

// Nodes reachable from v (v included).
std::vector<bool> below(const Expr& e, uint32_t v) {
  std::vector<bool> in(e.nodes.size(), false);
  in[v] = true;
  for (size_t i = v + 1; i-- > 0;)
    if (in[i])
      for (unsigned k = 0; k < operandCount(e.nodes[i]); ++k) in[e.nodes[i].args[k]] = true;
  return in;
}

uint32_t costOf(const std::vector<bool>& in, const std::vector<uint32_t>& costs) {
  uint32_t c = 0;
  for (size_t i = 0; i < in.size(); ++i) c += in[i] ? costs[i] : 0;
  return c;
}

std::optional<SourceRewrite> hoistShader(const Effect& fx, const Function& ps, const Function& vs,
                                         const std::vector<const Region*>& regions, const CostModel& m) {
  const Codegen& cg = *fx.cg;
  const std::string file = ps.loc.source;
  const std::vector<std::string>* lines = sourceLines(file);
  const std::vector<std::string>* vsLines = sourceLines(vs.loc.source);
  if (!lines || !vsLines || ps.loc.line == 0 || ps.loc.line > lines->size() || vs.loc.line == 0 ||
      vs.loc.line > vsLines->size())
    return std::nullopt;
  if (!vs.returnType.is_void()) return std::nullopt;
  if (vs.loc.source == file && vs.loc.line >= ps.loc.line) return std::nullopt;  // the wrapper calls it

  // The vertex shader's outputs by semantic; the pixel shader's interpolated inputs among them.
  std::map<std::string, const Variable*> vsOut;
  std::set<int> usedTexcoords;
  for (uint32_t id : vs.params) {
    const auto v = cg.variables.find(id);
    if (v == cg.variables.end() || v->second.type.is_struct() || v->second.type.is_array()) return std::nullopt;
    const std::string key = semanticKey(v->second.semantic);
    if (v->second.type.has(reshadefx::type::q_out)) vsOut[key] = &v->second;
    usedTexcoords.insert(texcoordIndex(key));
  }
  std::map<std::string, std::string> psNameOf;  // semantic -> pixel shader parameter name
  std::set<std::string> varyings, psNames;
  for (uint32_t id : ps.params) {
    const auto v = cg.variables.find(id);
    if (v == cg.variables.end()) return std::nullopt;
    const Variable& p = v->second;
    psNames.insert(p.name);
    const std::string key = semanticKey(p.semantic);
    usedTexcoords.insert(texcoordIndex(key));
    if (p.type.is_struct()) continue;
    psNameOf[key] = p.name;
    bool stored = false;
    for (const auto& s : ps.stmts) stored = stored || (s.kind == Statement::Kind::Store && s.var == id);
    const unsigned modifiers = reshadefx::type::q_nointerpolation | reshadefx::type::q_centroid;
    if (vsOut.count(key) && !isPosition(key) && p.type.is_floating_point() && !(p.type.qualifiers & modifiers) &&
        !(vsOut[key]->type.qualifiers & modifiers) && !stored)
      varyings.insert(p.name);
  }

  // Non-overlapping regions, the largest windows first.
  struct Span {
    const Region* r;
    uint32_t first, last;
  };
  std::vector<Span> spans;
  for (const Region* r : regions) {
    uint32_t first = r->line;
    for (const auto& [a, b] : r->removed) first = std::min(first, a);
    spans.push_back({r, first, r->lastLine});
  }
  std::stable_sort(spans.begin(), spans.end(),
                   [](const Span& a, const Span& b) { return a.last - a.first > b.last - b.first; });
  std::vector<Span> chosen;
  for (const Span& s : spans) {
    bool clash = false;
    for (const Span& c : chosen) clash = clash || (s.first <= c.last && c.first <= s.last);
    if (!clash) chosen.push_back(s);
  }

  // Worth it: more than an interpolation (~3 instructions per component on RDNA) or a flat load
  // (~2), in the cost model's add units.
  const double unit = std::max(1u, m.opCost(Op::Add, 1));
  std::vector<Hoist> hoists;
  struct Statement2 {
    const Region* r;
    std::vector<std::pair<uint32_t, size_t>> picks;  // (node, hoist index)
  };
  std::vector<Statement2> stmts;
  for (const Span& s : chosen) {
    const Region& r = *s.r;
    const Program& p = r.prog;
    std::vector<int> inputDeg(p.inputs.size(), -1);
    bool any = false;
    for (size_t k = 0; k < p.inputs.size(); ++k) {
      const InputDecl& d = p.inputs[k];
      const std::string base = d.name.substr(0, d.name.find_first_of(".["));
      if (d.compileTime || (k < r.facts.size() && r.facts[k].order == 0 && !r.facts[k].fetch)) inputDeg[k] = 0;
      else if (varyings.count(base)) inputDeg[k] = 1;
      any = any || inputDeg[k] >= 0;
    }
    if (!any) continue;
    const Expr& e = p.target;
    const std::vector<int> deg = degrees(e, inputDeg);
    const std::vector<uint32_t> costs = nodeCosts(e, m, p.inputs);
    const std::vector<uint32_t> perfCosts = nodeCosts(e, m, perfInputs(p.inputs));
    std::vector<InputDecl> codeInputs = p.inputs;
    for (auto& d : codeInputs)
      if (d.compileTime && isBufferSizeMacro(d.name)) d.name = "float(" + d.name + ")";
    Statement2 st{&r, {}};
    std::vector<bool> seen(e.nodes.size(), false);
    // Top down: the largest subexpressions worth moving.
    std::vector<uint32_t> todo{e.root};
    while (!todo.empty()) {
      const uint32_t v = todo.back();
      todo.pop_back();
      if (seen[v]) continue;
      seen[v] = true;
      const Node& n = e.nodes[v];
      if (n.op == Op::Input || n.op == Op::Const) continue;
      const unsigned w = width(n.type);
      if (deg[v] >= 0 && isFloat(n.type)) {
        const std::vector<bool> in = below(e, v);
        const double c = costOf(in, costs) / unit, pc = costOf(in, perfCosts) / unit;
        const bool flat = deg[v] == 0;
        const double need = (flat ? 2.0 : 3.0) * w;
        if (c > need) {
          Expr sub;
          {
            ExprBuilder b;
            std::vector<uint32_t> map(e.nodes.size(), 0);
            for (size_t i = 0; i <= v; ++i) {
              if (!in[i]) continue;
              const Node& x = e.nodes[i];
              if (x.op == Op::Input) map[i] = b.input(x.input, x.type);
              else if (x.op == Op::Const) map[i] = b.constant(x.type, x.value);
              else if (x.op == Op::Swizzle) map[i] = b.swizzle(map[x.args[0]], x.swz, width(x.type));
              else if (x.op == Op::Construct) {
                uint32_t args[4];
                for (unsigned k = 0; k < x.nargs; ++k) args[k] = map[x.args[k]];
                map[i] = b.construct(args, x.nargs);
              } else {
                map[i] = b.op(x.op, map[x.args[0]], operandCount(x) > 1 ? map[x.args[1]] : 0,
                              operandCount(x) > 2 ? map[x.args[2]] : 0);
              }
            }
            sub = b.finish(map[v]);
          }
          Hoist h;
          h.text = toString(sub, codeInputs);
          h.width = w;
          h.flat = flat;
          h.perfGate = pc <= need;  // in performance mode it is cheap or folded
          size_t idx = hoists.size();
          for (size_t k = 0; k < hoists.size(); ++k)
            if (hoists[k].text == h.text && hoists[k].flat == h.flat) idx = k;
          if (idx == hoists.size()) hoists.push_back(h);
          else hoists[idx].perfGate = hoists[idx].perfGate && h.perfGate;
          st.picks.push_back({v, idx});
          continue;
        }
      }
      for (unsigned k = 0; k < operandCount(n); ++k) todo.push_back(n.args[k]);
    }
    if (!st.picks.empty()) stmts.push_back(std::move(st));
  }
  if (hoists.empty()) return std::nullopt;

  // Slots: float4 outputs, interpolated (sopt_v<k>) and flat (sopt_f<k>), at most 4 of each.
  struct Slot {
    std::string name;
    bool flat;
    unsigned used = 0;
    int texcoord = 0;
  };
  std::vector<Slot> slots;
  int nextTexcoord = 0;
  for (Hoist& h : hoists) {
    Slot* s = nullptr;
    for (Slot& x : slots)
      if (x.flat == h.flat && x.used + h.width <= 4) {
        s = &x;
        break;
      }
    if (!s) {
      size_t count = 0;
      for (const Slot& x : slots) count += x.flat == h.flat;
      if (count >= 4) continue;  // no room: stays in the pixel shader
      while (usedTexcoords.count(nextTexcoord)) ++nextTexcoord;
      slots.push_back({std::string(h.flat ? "sopt_f" : "sopt_v") + std::to_string(count), h.flat, 0, nextTexcoord});
      usedTexcoords.insert(nextTexcoord);
      s = &slots.back();
    }
    static const char* xyzw = "xyzw";
    h.slot = s->name + "." + std::string(xyzw + s->used, h.width);
    s->used += h.width;
  }

  // The passes: "VertexShader = X;" in every pass block whose pixel shader is this one.
  const Text text = joinLines(*lines);
  const std::string& src = text.s;
  std::string vsText;
  std::vector<std::pair<size_t, size_t>> vsRefs;  // [start, end) of each pass's vertex shader name
  for (size_t at : findWord(src, "PixelShader", 0, src.size())) {
    size_t q = skipSpace(src, at + 11);
    if (q >= src.size() || src[q] != '=') continue;
    q = skipSpace(src, q + 1);
    const size_t qe = nameEnd(src, q);
    std::string name = src.substr(q, qe - q);
    if (const size_t c = name.rfind("::"); c != std::string::npos) name = name.substr(c + 2);
    if (name != ps.name) continue;
    // The enclosing pass block.
    size_t open = std::string::npos;
    for (size_t pp : findWord(src, "pass", 0, at)) {
      const size_t brace = src.find('{', pp);
      if (brace != std::string::npos && brace < at) open = brace;
    }
    if (open == std::string::npos) return std::nullopt;
    const size_t close = matching(src, open);
    if (close == std::string::npos || close < at) return std::nullopt;
    const std::vector<size_t> refs = findWord(src, "VertexShader", open, close);
    if (refs.size() != 1) return std::nullopt;
    size_t v = skipSpace(src, refs[0] + 12);
    if (v >= src.size() || src[v] != '=') return std::nullopt;
    v = skipSpace(src, v + 1);
    const size_t ve = nameEnd(src, v);
    const std::string t = src.substr(v, ve - v);
    if (t.empty() || (!vsText.empty() && t != vsText)) return std::nullopt;
    vsText = t;
    vsRefs.push_back({v, ve});
  }
  size_t passes = 0;
  for (const auto& t : cg.mod().techniques)
    for (const auto& p : t.passes) passes += p.ps_entry_point == ps.uniqueName;
  if (vsRefs.empty() || vsRefs.size() != passes) return std::nullopt;

  // The vertex shader's parameter list, renamed to the pixel shader's names where they meet.
  const Text vtext = joinLines(*vsLines);
  std::vector<std::string> wrapperParams, callArgs;
  {
    const std::string& vs_ = vtext.s;
    const size_t at = vs_.find(vs.name, vtext.starts[vs.loc.line - 1]);
    if (at == std::string::npos) return std::nullopt;
    const size_t open = skipSpace(vs_, at + vs.name.size());
    if (open >= vs_.size() || vs_[open] != '(') return std::nullopt;
    const size_t close = matching(vs_, open);
    if (close == std::string::npos) return std::nullopt;
    const auto items = splitItems(vs_, open + 1, close);
    if (items.size() != vs.params.size() && !(vs.params.empty() && items.size() == 1)) return std::nullopt;
    std::set<std::string> names;
    for (size_t k = 0; k < vs.params.size(); ++k) {
      const Variable& p = cg.variables.at(vs.params[k]);
      std::string t = plain(vs_, items[k].first, items[k].second);
      // "... name : SEMANTIC": the name before the colon.
      const size_t colon = t.find(':');
      if (colon == std::string::npos) return std::nullopt;
      std::string head = trim(t.substr(0, colon));
      const size_t ns = head.find_last_of(" \t");
      if (ns == std::string::npos || head.substr(ns + 1) != p.name) return std::nullopt;
      const std::string key = semanticKey(p.semantic);
      std::string name = p.name;
      if (p.type.has(reshadefx::type::q_out) && psNameOf.count(key)) name = psNameOf[key];
      else if (psNames.count(name)) return std::nullopt;  // would hide a pixel shader name
      if (name.rfind("sopt_", 0) == 0 || !names.insert(name).second) return std::nullopt;
      wrapperParams.push_back(head.substr(0, ns + 1) + name + " " + t.substr(colon));
      callArgs.push_back(name);
    }
  }

  // The pixel shader's header: the new inputs before its closing parenthesis.
  const std::string& head = (*lines)[ps.loc.line - 1];
  const size_t fn = head.find(ps.name);
  if (fn == std::string::npos || trim(head.substr(0, fn)).empty()) return std::nullopt;
  const size_t popen = skipSpace(src, text.starts[ps.loc.line - 1] + fn + ps.name.size());
  if (popen >= src.size() || src[popen] != '(') return std::nullopt;
  const size_t pclose = matching(src, popen);
  if (pclose == std::string::npos) return std::nullopt;
  const uint32_t headLast = text.lineOf(pclose);
  for (const Span& s : chosen)
    if (s.first <= headLast) return std::nullopt;

  bool anyFlat = false;
  for (const Slot& s : slots) anyFlat = anyFlat || s.flat;
  const std::string renderer = anyFlat ? " && __RENDERER__ >= 0xa000" : "";
  // Unique names are 'F' + the scope ("__" at global scope, "__Ns__" in a namespace) + the name.
  std::string nsPrefix;
  if (ps.uniqueName.size() > ps.name.size() + 1)
    nsPrefix = ps.uniqueName.substr(1, ps.uniqueName.size() - 1 - ps.name.size());
  if (nsPrefix.rfind("__", 0) == 0) nsPrefix.erase(0, 2);
  for (size_t k; (k = nsPrefix.find("__")) != std::string::npos;) nsPrefix.replace(k, 2, "::");
  const std::string wrapper = "sopt_VS_" + ps.name;

  SourceRewrite rw;
  rw.file = file;
  rw.line = ps.loc.line;
  rw.function = ps.name;
  rw.kind = "to the vertex shader";
  rw.tag = "V";

  // Wrapper and header.
  {
    std::string params, psParams;
    for (const std::string& p : wrapperParams) params += (params.empty() ? "" : ", ") + p;
    for (const Slot& s : slots) {
      const std::string sem = "TEXCOORD" + std::to_string(s.texcoord);
      params += std::string(params.empty() ? "" : ", ") + (s.flat ? "nointerpolation " : "") + "out float4 " + s.name +
                " : " + sem;
      psParams += std::string(", ") + (s.flat ? "nointerpolation " : "") + "float4 " + s.name + " : " + sem;
    }
    LineEdit e{ps.loc.line, headLast, {}, renderer};
    e.lines.push_back("void " + wrapper + "(" + params + ")");
    e.lines.push_back("{");
    std::string call = vsText + "(";
    for (size_t k = 0; k < callArgs.size(); ++k) call += (k ? ", " : "") + callArgs[k];
    e.lines.push_back("\t" + call + ");");
    for (const Slot& s : slots) e.lines.push_back("\t" + s.name + " = 0.0;");
    for (const Hoist& h : hoists)
      if (!h.slot.empty()) e.lines.push_back("\t" + h.slot + " = " + h.text + ";");
    e.lines.push_back("}");
    e.lines.push_back("");
    // The header lines with the inputs added before the closing parenthesis.
    std::string headText;
    for (uint32_t l = ps.loc.line; l <= headLast; ++l) headText += (l > ps.loc.line ? "\n" : "") + (*lines)[l - 1];
    const size_t closeInHead = pclose - text.starts[ps.loc.line - 1];
    size_t ins = closeInHead;
    while (ins > 0 && std::isspace(static_cast<unsigned char>(headText[ins - 1]))) --ins;
    headText.insert(ins, psParams);
    for (size_t a = 0; a <= headText.size();) {
      size_t b = headText.find('\n', a);
      if (b == std::string::npos) b = headText.size();
      e.lines.push_back(headText.substr(a, b - a));
      a = b + 1;
    }
    rw.edits.push_back(std::move(e));
  }

  // The statements, with the moved values read from the inputs.
  size_t moved = 0, movedFlat = 0;
  for (const Statement2& st : stmts) {
    const Region& r = *st.r;
    const Program& p = r.prog;
    std::vector<InputDecl> inputs = p.inputs;
    for (auto& d : inputs)
      if (d.compileTime && isBufferSizeMacro(d.name)) d.name = "float(" + d.name + ")";
    std::map<uint32_t, uint32_t> replaced;  // node -> new input
    bool gate = false;
    for (const auto& [v, h] : st.picks) {
      if (hoists[h].slot.empty()) continue;
      InputDecl d;
      d.name = hoists[h].slot;
      d.type = floatType(hoists[h].width);
      replaced[v] = static_cast<uint32_t>(inputs.size());
      inputs.push_back(d);
      gate = gate || hoists[h].perfGate;
      ++(hoists[h].flat ? movedFlat : moved);
    }
    if (replaced.empty()) continue;
    const Expr& e = p.target;
    ExprBuilder b;
    std::vector<uint32_t> map(e.nodes.size(), 0);
    for (size_t i = 0; i < e.nodes.size(); ++i) {
      const Node& x = e.nodes[i];
      if (const auto it = replaced.find(static_cast<uint32_t>(i)); it != replaced.end())
        map[i] = b.input(it->second, x.type);
      else if (x.op == Op::Input) map[i] = b.input(x.input, x.type);
      else if (x.op == Op::Const) map[i] = b.constant(x.type, x.value);
      else if (x.op == Op::Swizzle) map[i] = b.swizzle(map[x.args[0]], x.swz, width(x.type));
      else if (x.op == Op::Construct) {
        uint32_t args[4];
        for (unsigned k = 0; k < x.nargs; ++k) args[k] = map[x.args[k]];
        map[i] = b.construct(args, x.nargs);
      } else {
        map[i] = b.op(x.op, map[x.args[0]], operandCount(x) > 1 ? map[x.args[1]] : 0,
                      operandCount(x) > 2 ? map[x.args[2]] : 0);
      }
    }
    const Expr ne = b.finish(map[e.root]);
    const std::string extra = renderer + (gate ? " && !__RESHADE_PERFORMANCE_MODE__" : "");
    const std::string& first = (*lines)[r.line - 1];
    const std::string ind = first.substr(0, first.find_first_not_of(" \t"));
    rw.edits.push_back({r.line, r.lastLine, {ind + variantStatement(r, toString(ne, inputs))}, extra});
    for (const auto& [a, bb] : r.removed) rw.edits.push_back({a, bb, {}, extra});
  }
  if (moved + movedFlat == 0) return std::nullopt;

  // The passes.
  std::map<uint32_t, std::string> passLines;
  for (auto it = vsRefs.rbegin(); it != vsRefs.rend(); ++it) {
    const uint32_t ln = text.lineOf(it->first);
    if (text.lineOf(it->second) != ln) return std::nullopt;
    std::string& l = passLines.try_emplace(ln, (*lines)[ln - 1]).first->second;
    const size_t a = it->first - text.starts[ln - 1];
    l.replace(a, it->second - it->first, nsPrefix + wrapper);
  }
  for (const auto& [ln, l] : passLines) {
    for (const Span& s : chosen)
      if (ln >= s.first && ln <= s.last) return std::nullopt;
    if (ln >= ps.loc.line && ln <= headLast) return std::nullopt;
    rw.edits.push_back({ln, ln, {l}, renderer});
  }
  std::sort(rw.edits.begin(), rw.edits.end(), [](const LineEdit& a, const LineEdit& b) { return a.first < b.first; });
  for (size_t k = 1; k < rw.edits.size(); ++k)
    if (rw.edits[k].first <= rw.edits[k - 1].last) return std::nullopt;

  size_t comps = 0, flatComps = 0;
  for (const Hoist& h : hoists)
    if (!h.slot.empty()) (h.flat ? flatComps : comps) += h.width;
  rw.description = "pixel shader math to the vertex shader " + wrapper + " (";
  if (comps) rw.description += std::to_string(comps) + " interpolated component" + (comps > 1 ? "s" : "");
  if (comps && flatComps) rw.description += ", ";
  if (flatComps) rw.description += std::to_string(flatComps) + " flat (uniforms only)";
  rw.description += ")";
  return rw;
}

}  // namespace

std::vector<SourceRewrite> hoistRewrites(const Effect& fx, const std::vector<Region>& regions, const CostModel& m) {
  std::vector<SourceRewrite> out;
  if (!fx.cg || fx.hlsl) return out;
  const Codegen& cg = *fx.cg;
  std::map<std::string, std::set<std::string>> vsOf;  // pixel shader -> vertex shaders of its passes
  for (const auto& t : cg.mod().techniques)
    for (const auto& p : t.passes)
      if (!p.ps_entry_point.empty()) vsOf[p.ps_entry_point].insert(p.vs_entry_point);
  std::set<std::string> called;
  for (const auto& f : cg.functions) called.insert(f->calls.begin(), f->calls.end());
  for (const auto& f : cg.functions) {
    const auto it = vsOf.find(f->uniqueName);
    if (it == vsOf.end() || it->second.size() != 1 || called.count(f->uniqueName)) continue;
    const Function* vs = cg.function(*it->second.begin());
    if (!vs) continue;
    std::vector<const Region*> mine;
    for (const Region& r : regions)
      if (r.function == f->name && r.file == f->loc.source && r.guard.empty() && r.kind != Region::Kind::Write &&
          r.budgetReason.rfind("precise", 0) != 0)
        mine.push_back(&r);
    if (mine.empty()) continue;
    if (auto rw = hoistShader(fx, *f, *vs, mine, m)) out.push_back(std::move(*rw));
  }
  return out;
}

}  // namespace sopt::fx
