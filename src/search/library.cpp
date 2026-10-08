#include "search/library.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "ir/eval.hpp"
#include "ir/parser.hpp"
#include "search/subtrees.hpp"
#include "verify/verify.hpp"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sopt {

extern const char* const kBuiltinLibrary;  // library/rewrites.txt, generated at build time

namespace {

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Variable names of a pattern: identifiers that are neither called nor swizzles.
std::vector<std::string> patternVars(const std::string& s) {
  std::vector<std::string> vars;
  for (size_t i = 0; i < s.size();) {
    if (std::isdigit(static_cast<unsigned char>(s[i])) ||
        (s[i] == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      // a number, including exponents such as 1e-6
      while (i < s.size() && (identChar(s[i]) || s[i] == '.' ||
                              ((s[i] == '-' || s[i] == '+') && (s[i - 1] == 'e' || s[i - 1] == 'E'))))
        ++i;
      continue;
    }
    if (!identStart(s[i])) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < s.size() && identChar(s[j])) ++j;
    size_t before = i;
    while (before > 0 && std::isspace(static_cast<unsigned char>(s[before - 1]))) --before;
    size_t after = j;
    while (after < s.size() && std::isspace(static_cast<unsigned char>(s[after]))) ++after;
    const bool swizzle = before > 0 && s[before - 1] == '.';
    const bool call = after < s.size() && s[after] == '(';
    const std::string name = s.substr(i, j - i);
    if (!swizzle && !call && std::find(vars.begin(), vars.end(), name) == vars.end()) vars.push_back(name);
    i = j;
  }
  return vars;
}

// Splits at top-level commas (not inside brackets or parentheses).
std::vector<std::string> splitConds(const std::string& s) {
  std::vector<std::string> out;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || (s[i] == ',' && depth == 0)) {
      const std::string c = trim(std::string_view(s).substr(start, i - start));
      if (!c.empty()) out.push_back(c);
      start = i + 1;
    } else if (s[i] == '[' || s[i] == '(') {
      ++depth;
    } else if (s[i] == ']' || s[i] == ')') {
      --depth;
    }
  }
  return out;
}

double parseNumber(const std::string& s, const std::string& where) {
  const std::string t = trim(s);
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (t.empty() || end != t.c_str() + t.size()) throw ParseError(where + ": expected a number, got '" + t + "'");
  return v;
}

// Constant subexpressions folded (the parser keeps -1.0 as neg(1.0)).
Expr folded(const Expr& e) {
  ExprBuilder b;
  return b.finish(insertExpr(e, b));
}

// Copies node k of src into b with operands mapped; constants folded.
uint32_t copyNode(const Expr& src, uint32_t k, const std::vector<uint32_t>& map, ExprBuilder& b) {
  const Node& n = src.nodes[k];
  if (n.op == Op::Input) return b.input(n.input, n.type);
  return foldCopyNode(src, k, map, b);
}

struct Matcher {
  const Expr& p;
  const Expr& t;
  std::vector<uint32_t> bind;

  bool args(const Node& pn, const Node& tn, bool swap) {
    const unsigned n = operandCount(pn);
    for (unsigned k = 0; k < n; ++k) {
      const unsigned tk = swap && k < 2 ? 1 - k : k;
      if (!match(pn.args[k], tn.args[tk])) return false;
    }
    return true;
  }

  bool match(uint32_t pi, uint32_t ti) {
    const Node& pn = p.nodes[pi];
    const Node& tn = t.nodes[ti];
    if (pn.op == Op::Input) {
      uint32_t& b = bind[pn.input];
      if (b == UINT32_MAX) {
        b = ti;
        return true;
      }
      return b == ti;
    }
    if (pn.op == Op::Const) {
      if (tn.op != Op::Const) return false;
      const unsigned pw = width(pn.type), tw = width(tn.type);
      if (pw != 1 && pw != tw) return false;
      for (unsigned k = 0; k < tw; ++k)
        if (tn.value[k] != pn.value[pw == 1 ? 0 : k]) return false;
      return true;
    }
    if (pn.op != tn.op || operandCount(pn) != operandCount(tn)) return false;
    if (pn.op == Op::Swizzle &&
        (width(pn.type) != width(tn.type) || std::memcmp(pn.swz, tn.swz, width(pn.type)) != 0))
      return false;
    const std::vector<uint32_t> saved = bind;
    if (args(pn, tn, false)) return true;
    bind = saved;
    if (info(pn.op).commutative || pn.op == Op::Mad) {
      if (args(pn, tn, true)) return true;
      bind = saved;
    }
    return false;
  }
};

bool condsHold(const RewriteRule& r, const Expr& e, const std::vector<uint32_t>& bind,
               const std::vector<InputDecl>& inputs, const PointSet& samples) {
  std::vector<bool> ct;
  for (const RuleCond& c : r.conds) {
    const uint32_t node = bind[c.var];
    if (node == UINT32_MAX) continue;
    if (c.kind == RuleCond::Kind::Const) {
      if (ct.empty()) ct = compileTimeNodes(e, inputs);
      if (!ct[node]) return false;
      continue;
    }
    const Node& n = e.nodes[node];
    std::vector<float> vals;
    if (n.op == Op::Const) {
      vals.assign(n.value, n.value + width(n.type));
    } else {
      if (samples.size() == 0) return false;
      vals = evalAll(subexpr(e, node), samples, kProfileRef);
    }
    for (float v : vals) {
      if (!std::isfinite(v)) continue;
      if (c.kind == RuleCond::Kind::NotEq) {
        if (v == c.lo) return false;
      } else if (v < c.lo || v > c.hi || (c.loOpen && v == c.lo) || (c.hiOpen && v == c.hi)) {
        return false;
      }
    }
  }
  return true;
}

// e with node i replaced by the rule's replacement (variables bound by `bind`), constants
// folded and identities removed; nullopt if the types do not fit.
std::optional<Expr> apply(const Expr& e, uint32_t i, const RewriteRule& r, const std::vector<uint32_t>& bind) {
  try {
    ExprBuilder b;
    std::vector<uint32_t> map(e.nodes.size());
    for (uint32_t k = 0; k < e.nodes.size(); ++k) {
      if (k != i) {
        map[k] = copyNode(e, k, map, b);
        continue;
      }
      const Expr& rhs = r.rhs;
      std::vector<uint32_t> rm(rhs.nodes.size());
      for (uint32_t j = 0; j < rhs.nodes.size(); ++j) {
        const Node& n = rhs.nodes[j];
        switch (n.op) {
          case Op::Input: rm[j] = map[bind[n.input]]; break;
          case Op::Const: rm[j] = b.constant(n.type, n.value); break;
          case Op::Swizzle: rm[j] = b.swizzle(rm[n.args[0]], n.swz, width(n.type)); break;
          case Op::Construct: {
            uint32_t a[4];
            for (unsigned q = 0; q < n.nargs; ++q) a[q] = rm[n.args[q]];
            rm[j] = b.construct(a, n.nargs);
            break;
          }
          default: {
            const unsigned c = operandCount(n);
            rm[j] = b.op(n.op, rm[n.args[0]], c > 1 ? rm[n.args[1]] : 0, c > 2 ? rm[n.args[2]] : 0);
          }
        }
      }
      map[k] = rm[rhs.root];
    }
    Expr g = simplifyIdentities(folded(b.finish(map[e.root])));
    if (g.nodes[g.root].type != e.nodes[e.root].type) return std::nullopt;
    return g;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string exeDir() {
#if defined(_WIN32)
  char buf[MAX_PATH];
  const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return std::filesystem::path(std::string(buf, n)).parent_path().string();
#else
  std::error_code ec;
  const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::string() : p.parent_path().string();
#endif
}

}  // namespace

std::string executableDir() { return exeDir(); }

Library parseLibrary(std::string_view text, const std::string& name) {
  Library lib;
  lib.path = name;
  // A rule may span lines (owner, 2026-10-08: sopt-found.txt writes pattern, "->" and replacement on
  // lines of their own): a line starting with "->" or the word "where", or following one that ends in
  // "->", continues the rule before it.
  std::vector<std::pair<int, std::string>> rules;
  {
    std::istringstream in{std::string(text)};
    std::string raw;
    int n = 0;
    auto startsWord = [](const std::string& l, const char* w) {
      const size_t k = std::strlen(w);
      return l.compare(0, k, w) == 0 && (l.size() == k || !identChar(l[k]));
    };
    while (std::getline(in, raw)) {
      ++n;
      std::string l = trim(raw.substr(0, raw.find('#')));
      if (l.empty()) continue;
      const bool cont = !rules.empty() && (l.compare(0, 2, "->") == 0 || startsWord(l, "where") ||
                                           (rules.back().second.size() >= 2 &&
                                            rules.back().second.compare(rules.back().second.size() - 2, 2, "->") == 0));
      if (cont) rules.back().second += " " + l;
      else rules.emplace_back(n, l);
    }
  }
  for (const auto& [lineNo, line] : rules) {
    const std::string where = name + ":" + std::to_string(lineNo);
    const size_t arrow = line.find("->");
    if (arrow == std::string::npos) throw ParseError(where + ": expected 'pattern -> replacement'");
    std::string lhsText = trim(std::string_view(line).substr(0, arrow));
    std::string rest = line.substr(arrow + 2);
    std::string condText;
    for (size_t w = rest.find("where"); w != std::string::npos; w = rest.find("where", w + 1)) {
      const bool wordStart = w == 0 || !identChar(rest[w - 1]);
      const bool wordEnd = w + 5 >= rest.size() || !identChar(rest[w + 5]);
      if (wordStart && wordEnd) {
        condText = rest.substr(w + 5);
        rest = rest.substr(0, w);
        break;
      }
    }
    const std::string rhsText = trim(rest);
    RewriteRule r;
    r.source = where;
    r.text = lhsText + " -> " + rhsText + (condText.empty() ? "" : "   where " + trim(condText));
    for (const std::string& v : patternVars(lhsText)) {
      InputDecl d;
      d.name = v;
      d.lo = -100.0;
      d.hi = 100.0;
      r.vars.push_back(d);
    }
    auto varIndex = [&](const std::string& v) -> uint32_t {
      for (uint32_t k = 0; k < r.vars.size(); ++k)
        if (r.vars[k].name == v) return k;
      throw ParseError(where + ": '" + v + "' in a condition is not a variable of the pattern");
    };
    // Sides of a variable's domain set by a condition (the default [-100, 100] applies to the others).
    std::vector<bool> loSet(r.vars.size(), false), hiSet(r.vars.size(), false);
    for (const std::string& c : splitConds(condText)) {
      size_t j = 0;
      while (j < c.size() && identChar(c[j])) ++j;
      const std::string v = c.substr(0, j);
      const std::string op = trim(std::string_view(c).substr(j));
      if (v.empty()) throw ParseError(where + ": bad condition '" + c + "'");
      RuleCond rc;
      rc.var = varIndex(v);
      InputDecl& d = r.vars[rc.var];
      if (op == "const") {
        rc.kind = RuleCond::Kind::Const;
      } else if (op.rfind(":", 0) == 0) {
        const std::string ty = trim(std::string_view(op).substr(1));
        if (ty == "float" || ty == "float1") d.type = Type::Float;
        else if (ty == "float2") d.type = Type::Float2;
        else if (ty == "float3") d.type = Type::Float3;
        else if (ty == "float4") d.type = Type::Float4;
        else throw ParseError(where + ": unknown type '" + ty + "'");
        continue;
      } else if (op.rfind("in", 0) == 0 && op.size() > 2 && !identChar(op[2])) {
        const std::string iv = trim(std::string_view(op).substr(2));
        const size_t comma = iv.find(',');
        if (iv.size() < 5 || (iv.front() != '[' && iv.front() != '(') || (iv.back() != ']' && iv.back() != ')') ||
            comma == std::string::npos)
          throw ParseError(where + ": expected 'in [lo, hi]' in '" + c + "'");
        rc.lo = parseNumber(iv.substr(1, comma - 1), where);
        rc.hi = parseNumber(iv.substr(comma + 1, iv.size() - comma - 2), where);
        rc.loOpen = iv.front() == '(';
        rc.hiOpen = iv.back() == ')';
      } else {
        static const char* const ops[] = {">=", "<=", "!=", ">", "<"};
        const char* found = nullptr;
        for (const char* o : ops)
          if (op.rfind(o, 0) == 0) {
            found = o;
            break;
          }
        if (!found) throw ParseError(where + ": bad condition '" + c + "'");
        const double x = parseNumber(op.substr(std::strlen(found)), where);
        const std::string o = found;
        if (o == "!=") {
          rc.kind = RuleCond::Kind::NotEq;
          rc.lo = x;
        } else if (o == ">=" || o == ">") {
          rc.lo = x;
          rc.loOpen = o == ">";
        } else {
          rc.hi = x;
          rc.hiOpen = o == "<";
        }
      }
      if (rc.kind == RuleCond::Kind::Range) {
        // The rule check samples the variable's domain: a condition's bound replaces the default
        // one on its side (x in [256, 7680] lies outside [-100, 100]) and narrows an earlier one.
        if (std::isfinite(rc.lo)) {
          d.lo = loSet[rc.var] ? std::max(d.lo, rc.lo) : rc.lo;
          loSet[rc.var] = true;
          if (rc.loOpen && d.lo == rc.lo) d.lo = std::nextafter(d.lo, INFINITY);
        }
        if (std::isfinite(rc.hi)) {
          d.hi = hiSet[rc.var] ? std::min(d.hi, rc.hi) : rc.hi;
          hiSet[rc.var] = true;
          if (rc.hiOpen && d.hi == rc.hi) d.hi = std::nextafter(d.hi, -INFINITY);
        }
      }
      r.conds.push_back(rc);
    }
    for (size_t k = 0; k < r.vars.size(); ++k) {
      InputDecl& d = r.vars[k];
      // One side given beyond the other's default: the default side follows it.
      if (loSet[k] && !hiSet[k] && d.hi < d.lo) d.hi = d.lo + 200.0;
      if (hiSet[k] && !loSet[k] && d.lo > d.hi) d.lo = d.hi - 200.0;
      if (!(d.lo <= d.hi)) throw ParseError(where + ": empty range for " + d.name);
    }
    try {
      r.lhs = folded(parseExpr(lhsText, r.vars));
      r.rhs = folded(parseExpr(rhsText, r.vars));
    } catch (const std::exception& ex) {
      throw ParseError(where + ": " + ex.what());
    }
    if (r.lhs.nodes[r.lhs.root].op == Op::Input) throw ParseError(where + ": the pattern is a bare variable");
    lib.byRoot[static_cast<size_t>(r.lhs.nodes[r.lhs.root].op)].push_back(static_cast<uint32_t>(lib.rules.size()));
    lib.rules.push_back(std::move(r));
  }
  return lib;
}

Library loadLibrary(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open library " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return parseLibrary(ss.str(), path);
}

const Library& defaultLibrary() {
  static const Library lib = [] {
    if (const char* env = std::getenv("SOPT_LIBRARY"); env && *env) return loadLibrary(env);
    const std::string dir = exeDir();
    if (!dir.empty()) {
      namespace fs = std::filesystem;
      for (const char* up : {".", "..", "../.."}) {
        const fs::path p = fs::path(dir) / up / "library" / "rewrites.txt";
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return loadLibrary(fs::weakly_canonical(p, ec).string());
      }
    }
    return parseLibrary(kBuiltinLibrary, "built-in library");
  }();
  return lib;
}

RuleCheck checkRule(const RewriteRule& r, size_t points, uint64_t seed) {
  RuleCheck out;
  Program prog;
  prog.inputs = r.vars;
  // Constants are not sampled at tiny magnitudes (specialValues): a * b of two 1e-20 constants
  // underflows, but shader constants are not that small.
  for (const RuleCond& c : r.conds)
    if (c.kind == RuleCond::Kind::Const) prog.inputs[c.var].compileTime = true;
  prog.target = r.lhs;
  prog.budget.kind = Budget::Kind::Rel;
  prog.budget.eps = 1e-6;
  if (r.lhs.nodes[r.lhs.root].type != r.rhs.nodes[r.rhs.root].type) {
    out.note = "pattern and replacement have different types";
    return out;
  }
  const PointSet ps = makeRandomPoints(prog, points, seed, true);
  Metrics worst;
  for (const auto& prof : kAllProfiles) {
    const Metrics m = compare(prog, r.rhs, ps, prof, 1);
    worst.merge(m);
    if (!m.pass) {
      std::ostringstream os;
      os << "fails under profile " << prof.name << " at";
      for (size_t k = 0; k < m.failPoint.size(); ++k) os << (k ? ", " : " ") << m.failPoint[k];
      out.note = os.str();
      out.maxRel = worst.maxRel;
      return out;
    }
  }
  if (worst.checked == 0) {
    out.note = "the pattern is not finite anywhere in the domain";
    return out;
  }
  out.pass = true;
  out.maxRel = worst.maxRel;
  return out;
}

std::vector<LibraryForm> libraryRewrites(const Program& prog, const Library& lib, const PointSet& samples,
                                         const CostModel& model, unsigned maxSteps, size_t maxForms) {
  std::vector<LibraryForm> out;
  if (lib.rules.empty()) return out;
  std::unordered_set<std::string> seen{toString(prog.target, prog.inputs)};
  std::vector<LibraryForm> frontier{{prog.target, 0, {}}};
  for (unsigned step = 0; step < maxSteps && !frontier.empty() && out.size() < maxForms; ++step) {
    std::vector<LibraryForm> next;
    for (const LibraryForm& f : frontier) {
      const Expr& e = f.expr;
      for (uint32_t i = 0; i < e.nodes.size() && out.size() < maxForms; ++i) {
        if (e.nodes[i].op == Op::Input || e.nodes[i].op == Op::Const) continue;
        for (uint32_t ri : lib.byRoot[static_cast<size_t>(e.nodes[i].op)]) {
          if (out.size() >= maxForms) break;
          const RewriteRule& r = lib.rules[ri];
          Matcher m{r.lhs, e, std::vector<uint32_t>(r.vars.size(), UINT32_MAX)};
          if (!m.match(r.lhs.root, i)) continue;
          if (!condsHold(r, e, m.bind, prog.inputs, samples)) continue;
          std::optional<Expr> g = apply(e, i, r, m.bind);
          if (!g || !seen.insert(toString(*g, prog.inputs)).second) continue;
          LibraryForm nf{std::move(*g), 0, f.rules};
          nf.rules.push_back(ri);
          nf.cost = dagCost(nf.expr, model, prog.inputs);
          next.push_back(nf);
          out.push_back(std::move(nf));
        }
      }
    }
    frontier = std::move(next);
  }
  std::stable_sort(out.begin(), out.end(), [](const LibraryForm& a, const LibraryForm& b) {
    return a.cost != b.cost ? a.cost < b.cost : a.rules.size() < b.rules.size();
  });
  return out;
}

}  // namespace sopt
