#include "fx/frontend.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

namespace sopt::fx {

namespace fs = std::filesystem;
using reshadefx::tokenid;

// ---------------------------------------------------------------------------
// Loading

std::string pathString(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

fs::path pathFrom(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

const std::vector<std::string>* sourceLines(const std::string& file) {
  static std::unordered_map<std::string, std::vector<std::string>> cache;
  auto it = cache.find(file);
  if (it != cache.end()) return &it->second;
  std::ifstream f(pathFrom(file), std::ios::binary);
  if (!f) return nullptr;
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return &(cache[file] = std::move(lines));
}

namespace {

// (file, line) -> preprocessed text, from the "#line N "file"" markers in the output.
void mapLines(const std::string& out, std::map<std::pair<std::string, uint32_t>, std::string>& m) {
  std::string file;
  uint32_t line = 1;
  size_t pos = 0;
  while (pos <= out.size()) {
    size_t end = out.find('\n', pos);
    if (end == std::string::npos) end = out.size();
    const std::string text = out.substr(pos, end - pos);
    pos = end + 1;
    if (text.rfind("#line ", 0) == 0) {
      line = static_cast<uint32_t>(std::strtoul(text.c_str() + 6, nullptr, 10));
      const size_t q = text.find('"');
      if (q != std::string::npos) file = text.substr(q + 1, text.rfind('"') - q - 1);
      continue;
    }
    auto& slot = m[{file, line}];
    if (!slot.empty()) slot += ' ';
    slot += text;
    ++line;
    if (end == out.size()) break;
  }
}

}  // namespace

std::unique_ptr<Effect> loadEffect(const fs::path& path, const LoadOptions& opt,
                                   std::string& errors) {
  reshadefx::preprocessor pp;
  pp.add_include_path(path.parent_path());
  for (const auto& p : opt.includePaths) pp.add_include_path(p);
  // As ReShade 6 defines them (runtime.cpp), D3D11 renderer, SDR 8-bit back buffer.
  pp.add_macro_definition("__RESHADE__", "60800");
  pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
  pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
  pp.add_macro_definition("__VENDOR__", "0");
  pp.add_macro_definition("__DEVICE__", "0");
  pp.add_macro_definition("__RENDERER__", "0xb000");
  pp.add_macro_definition("__APPLICATION__", "0");
  pp.add_macro_definition("BUFFER_WIDTH", std::to_string(opt.width));
  pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(opt.height));
  pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
  pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
  pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");
  pp.add_macro_definition("BUFFER_COLOR_FORMAT", "28");
  pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");
  for (const auto& [k, v] : opt.macros) pp.add_macro_definition(k, v);
  pp.append_string(
      "#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
      "#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
      "#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
      "#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
      "#define tex2Dgather0 tex2DgatherR\n"
      "#define tex2Dgather1 tex2DgatherG\n"
      "#define tex2Dgather2 tex2DgatherB\n"
      "#define tex2Dgather3 tex2DgatherA\n");
  if (!pp.append_file(path)) {
    errors = pp.errors();
    return nullptr;
  }
  auto fx = std::make_unique<Effect>();
  fx->path = path;
  fx->cg = std::make_unique<Codegen>();
  reshadefx::parser parser;
  if (!parser.parse(pp.output(), fx->cg.get())) {
    errors = pp.errors() + parser.errors();
    return nullptr;
  }
  mapLines(pp.output(), fx->ppLines);
  fx->sourceFiles.push_back(pathString(path));
  for (const auto& f : pp.included_files()) fx->sourceFiles.push_back(pathString(f));
  return fx;
}

namespace {

// ---------------------------------------------------------------------------
// Source text

// Tokens of FX text without whitespace and comments (for comparing a source line
// with its preprocessed form).
std::vector<std::string> tokens(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    if (c == '/' && i + 1 < n && s[i + 1] == '/') break;
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      const size_t e = s.find("*/", i + 2);
      i = e == std::string::npos ? n : e + 2;
      continue;
    }
    size_t j = i + 1;
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
      while (j < n && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.'))
        ++j;
    }
    out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

bool isIdent(char c);

// Position of a texture fetch call ("tex2D(", "tex2Dlod (", ...) at i.
bool fetchAt(const std::string& t, size_t i) {
  if (i + 5 >= t.size() || t.compare(i, 3, "tex") != 0 || (i && isIdent(t[i - 1]))) return false;
  if (!std::isdigit(static_cast<unsigned char>(t[i + 3])) || t[i + 4] != 'D') return false;
  size_t j = i + 5;
  while (j < t.size() && isIdent(t[j])) ++j;
  while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
  return j < t.size() && t[j] == '(';
}

// The text with every outermost texture fetch call replaced by "tex_fetch": fetches
// are region inputs copied verbatim, so macros inside them do not matter.
std::string withoutFetches(const std::string& t) {
  std::string out;
  for (size_t i = 0; i < t.size(); ++i) {
    if (!fetchAt(t, i)) {
      out += t[i];
      continue;
    }
    size_t k = t.find('(', i);
    int depth = 0;
    for (; k < t.size(); ++k) {
      if (t[k] == '(') ++depth;
      if (t[k] == ')' && --depth == 0) break;
    }
    out += " tex_fetch ";
    i = k;
  }
  return out;
}

// Removes comments (replaced by spaces, so columns stay) from lines [first, last].
struct StatementText {
  uint32_t first = 0, last = 0;  // 1-based lines
  std::string text;              // comments removed, lines joined with ' ', up to ';'
  std::string original;          // original lines joined with '\n'
};

// The statement starting at the first non-blank character of `line`, ending with the
// first ';' at bracket depth 0. It must be alone on its lines (besides comments).
bool statementAt(const std::vector<std::string>& lines, uint32_t line, StatementText& st,
                 std::string& why) {
  if (line == 0 || line > lines.size()) { why = "line out of range"; return false; }
  st.first = line;
  int depth = 0;
  bool inBlock = false;
  std::string text;
  for (uint32_t l = line; l <= lines.size() && l < line + 24; ++l) {
    const std::string& s = lines[l - 1];
    for (size_t i = 0; i < s.size(); ++i) {
      const char c = s[i];
      if (inBlock) {
        if (c == '*' && i + 1 < s.size() && s[i + 1] == '/') { inBlock = false; ++i; }
        text += ' ';
        continue;
      }
      if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') break;
      if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') { inBlock = true; ++i; text += ' '; continue; }
      if (c == '"') { why = "string literal"; return false; }
      if (c == '(' || c == '[') ++depth;
      if (c == ')' || c == ']') --depth;
      if (c == '{' || c == '}') { why = "not a simple statement"; return false; }
      if (c == ';' && depth == 0) {
        // Only blanks or a comment may follow on this line.
        const std::string rest = s.substr(i + 1);
        const size_t k = rest.find_first_not_of(" \t");
        if (k != std::string::npos && rest.compare(k, 2, "//") != 0) {
          const size_t e = rest.find("*/", k);
          if (rest.compare(k, 2, "/*") != 0 || e == std::string::npos ||
              rest.find_first_not_of(" \t", e + 2) != std::string::npos) {
            why = "several statements on one line";
            return false;
          }
        }
        st.last = l;
        st.text = text;
        st.original.clear();
        for (uint32_t k2 = line; k2 <= l; ++k2) {
          if (k2 > line) st.original += '\n';
          st.original += lines[k2 - 1];
        }
        const size_t b = st.text.find_first_not_of(" \t");
        st.text = b == std::string::npos ? std::string() : st.text.substr(b);
        while (!st.text.empty() && std::isspace(static_cast<unsigned char>(st.text.back()))) st.text.pop_back();
        if (inBlock) { why = "comment"; return false; }
        return true;
      }
      text += c;
    }
    text += ' ';
  }
  why = "statement end not found";
  return false;
}

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Position of the assignment operator at depth 0 ('=' or 'op=' for + - * /), or npos.
size_t assignmentOp(const std::string& s, char& compound) {
  int depth = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '(' || c == '[') ++depth;
    if (c == ')' || c == ']') --depth;
    if (c != '=' || depth != 0) continue;
    if (i + 1 < s.size() && s[i + 1] == '=') return std::string::npos;  // ==
    const char p = i ? s[i - 1] : ' ';
    if (p == '<' || p == '>' || p == '!' || p == '=' || p == '%' || p == '&' || p == '|' || p == '^')
      return std::string::npos;
    compound = (p == '+' || p == '-' || p == '*' || p == '/') ? p : 0;
    return compound ? i - 1 : i;
  }
  return std::string::npos;
}

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return {};
  const size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

bool hasDepth0Comma(const std::string& s) {
  int depth = 0;
  for (char c : s) {
    if (c == '(' || c == '[') ++depth;
    if (c == ')' || c == ']') --depth;
    if (c == ',' && depth == 0) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Ranges

struct Range {
  double lo = 0, hi = 0;
  uint32_t grid = 0;
  bool known = false;
  bool assumed = false;  // depends on a default range somewhere
  std::string why;

  static Range of(double lo, double hi, std::string why, uint32_t grid = 0) {
    Range r;
    r.lo = lo; r.hi = hi; r.grid = grid; r.known = std::isfinite(lo) && std::isfinite(hi) && lo <= hi;
    r.why = std::move(why);
    return r;
  }
  static Range unknown() { return {}; }
};

Range derived(double lo, double hi, std::initializer_list<const Range*> from) {
  for (const Range* r : from)
    if (!r->known) return Range::unknown();
  Range r = Range::of(lo, hi, "derived");
  for (const Range* f : from) r.assumed = r.assumed || f->assumed;
  if (!r.known) return Range::unknown();
  return r;
}

Range unite(const Range& a, const Range& b) {
  if (!a.known || !b.known) return Range::unknown();
  Range r = Range::of(std::min(a.lo, b.lo), std::max(a.hi, b.hi),
                      a.why == b.why ? a.why : std::string("derived"));
  r.assumed = a.assumed || b.assumed;
  if (a.grid == b.grid && a.lo == b.lo && a.hi == b.hi) r.grid = a.grid;
  return r;
}

Range mulR(const Range& a, const Range& b) {
  const double c[4] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
  return derived(*std::min_element(c, c + 4), *std::max_element(c, c + 4), {&a, &b});
}
Range addR(const Range& a, const Range& b) { return derived(a.lo + b.lo, a.hi + b.hi, {&a, &b}); }
Range subR(const Range& a, const Range& b) { return derived(a.lo - b.hi, a.hi - b.lo, {&a, &b}); }
Range rcpR(const Range& a) {
  if (!a.known || (a.lo <= 0 && a.hi >= 0)) return Range::unknown();
  return derived(1.0 / a.hi, 1.0 / a.lo, {&a});
}
Range monotone(const Range& a, double (*f)(double)) { return derived(f(a.lo), f(a.hi), {&a}); }
Range minR(const Range& a, const Range& b) { return derived(std::min(a.lo, b.lo), std::min(a.hi, b.hi), {&a, &b}); }
Range maxR(const Range& a, const Range& b) { return derived(std::max(a.lo, b.lo), std::max(a.hi, b.hi), {&a, &b}); }
Range clampR(const Range& a, double lo, double hi) {
  if (!a.known) return Range::of(lo, hi, "derived");
  return derived(std::clamp(a.lo, lo, hi), std::clamp(a.hi, lo, hi), {&a});
}

std::string semanticKey(std::string s);
Range semanticConvention(const std::string& semantic);

bool isTexFetch(const std::string& n) {
  return n.rfind("tex1D", 0) == 0 || n.rfind("tex2D", 0) == 0 || n.rfind("tex3D", 0) == 0;
}

// ---------------------------------------------------------------------------
// Region extraction

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool isFloatType(const reshadefx::type& t) {
  return (t.base == reshadefx::type::t_float) && t.cols == 1 && t.rows >= 1 && t.rows <= 4 &&
         !t.is_array();
}

struct Unsupported : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Extractor {
 public:
  Extractor(const Effect& fx, const RegionOptions& opt) : fx_(fx), cg_(*fx.cg), opt_(opt) {
    for (const auto& [id, v] : cg_.values)
      for (uint32_t a : v.args) users_[a].push_back(id);
    for (const auto& [id, v] : cg_.values)
      if (v.kind == Value::Kind::Chain || v.kind == Value::Kind::Load) users_[v.base].push_back(id);
    for (const auto& f : cg_.functions) {
      for (const auto& s : f->stmts) {
        if (s.kind != Statement::Kind::Return) defs_[s.var].push_back(&s);
        stmtUses_[s.value].push_back(&s);
      }
      for (uint32_t p : f->params) paramOf_[p] = f.get(), varFunction_[p] = f.get();
      for (const auto& st : f->stmts)
        if (st.kind != Statement::Kind::Return) varFunction_.emplace(st.var, f.get());
    }
    for (const auto& [id, v] : cg_.values) {
      if (v.kind != Value::Kind::Call) continue;
      const Function* g = cg_.function(v.name);
      if (!g) continue;
      for (size_t k = 0; k < v.args.size() && k < g->params.size(); ++k) {
        const auto pv = cg_.variables.find(g->params[k]);
        if (pv != cg_.variables.end() && pv->second.type.has(reshadefx::type::q_out) &&
            cg_.variables.count(v.args[k]))
          outArgOf_[v.args[k]] = {g, k, v.seq};
      }
    }
  }

  std::vector<Region> run(SkipCount& skipped);

 private:
  bool shapeOf(const Statement& s, Region& reg, std::string& why);
  bool buildRegion(const Statement& s, Region& reg, std::string& why);
  void treeValues(uint32_t id, std::unordered_set<uint32_t>& out) const;
  void findTemps(const Statement& use, std::vector<const Statement*>& defs, int depth);
  struct Leaf {
    uint32_t var = 0;
    std::string prefix;          // FX text of variable + member/index chain
    reshadefx::type type{};      // type of the prefix (float1..4)
    uint32_t mask = 0;           // used components
    uint32_t loadValue = 0;      // a load of it (for its range)
    uint32_t input = 0;          // IR input index
    std::vector<uint8_t> remap;  // component -> input component
    bool fetch = false;          // texture fetch: prefix is its call text
    std::string semantic;        // of the struct member the prefix ends in, if any
  };
  Leaf& fetchLeaf(uint32_t id);

  // Pass 1 / 2 over the value tree.
  void collect(uint32_t id);
  uint32_t build(uint32_t id, ExprBuilder& b);
  // Splits a load into leaf prefix and the vector part of the chain.
  Leaf& leafOf(const Value& v, size_t& vecStart);
  uint32_t applyVectorChain(uint32_t node, const std::vector<reshadefx::expression::operation>& chain,
                            size_t start, ExprBuilder& b, const Leaf* leaf);
  std::string varText(uint32_t var) const;

  Range range(uint32_t valueId);
  Range varRange(uint32_t var, uint32_t seq, uint32_t block);
  Range varRangeRaw(uint32_t var, uint32_t seq, uint32_t block);
  std::string varKey(uint32_t var) const;  // UserRanges key of a variable
  std::unordered_map<uint32_t, const Function*> varFunction_;  // local/param -> function
  Range samplerRange(uint32_t valueId);
  Budget budgetFor(const Function& f, const Statement& s, std::string& reason);
  void useKinds(uint32_t valueId, bool& cmp, bool& coord, bool& other, int depth);
  static bool outOnly(const Variable& v);
  void suggest(const Leaf& l, Fact& f) const;
  Range outParamRange(const Function& g, size_t index);
  Range pixelInputRange(const Function& ps, const std::string& semantic);
  struct OutArg {
    const Function* callee = nullptr;
    size_t index = 0;
    uint32_t seq = 0;  // of the call
  };
  std::unordered_map<uint32_t, OutArg> outArgOf_;  // temporary variable -> call

  const Effect& fx_;
  const Codegen& cg_;
  const RegionOptions& opt_;
  std::unordered_map<uint32_t, std::vector<uint32_t>> users_;
  std::unordered_map<uint32_t, std::vector<const Statement*>> defs_;
  std::unordered_map<uint32_t, std::vector<const Statement*>> stmtUses_;
  std::unordered_map<uint32_t, const Function*> paramOf_;
  std::unordered_map<uint32_t, Range> rangeMemo_;
  std::unordered_set<uint32_t> varBusy_;
  std::unordered_set<uint64_t> localBusy_;  // (variable, load seq)

  // current region
  std::vector<Leaf> leaves_;
  std::unordered_map<uint32_t, uint32_t> built_;
  bool usesPow_ = false;
  std::unordered_map<uint32_t, uint32_t> inline_;  // temporary variable -> its value
  std::unordered_map<uint32_t, std::string> fetchText_;  // fetch value -> call text
  void mapFetches(const Statement& s, const std::string& text);
};

std::string Extractor::varText(uint32_t var) const {
  const auto it = cg_.variables.find(var);
  if (it == cg_.variables.end()) throw Unsupported("not a variable");
  const Variable& v = it->second;
  switch (v.kind) {
    case Variable::Kind::Local:
    case Variable::Kind::Param: return v.name;
    case Variable::Kind::Uniform: {
      if (v.uniqueName == "V" + v.name) return v.name;
      // "VNs__name" -> "Ns::name"
      std::string ns = v.uniqueName.substr(1, v.uniqueName.size() - 1 - v.name.size());
      std::string out;
      for (size_t i = 0; i < ns.size(); ++i) {
        if (ns[i] == '_' && i + 1 < ns.size() && ns[i + 1] == '_') { out += "::"; ++i; }
        else out += ns[i];
      }
      if (out.rfind("::", 0) == 0) out.erase(0, 2);  // global scope
      return out + v.name;
    }
    case Variable::Kind::Global: break;
  }
  throw Unsupported("global variable");
}

Extractor::Leaf& Extractor::leafOf(const Value& v, size_t& vecStart) {
  const Variable* var = nullptr;
  if (auto it = cg_.variables.find(v.base); it != cg_.variables.end()) var = &it->second;
  if (!var) throw Unsupported("load of a non-variable");
  std::string text = varText(v.base);
  std::string semantic;
  reshadefx::type t = var->type;
  size_t i = 0;
  for (; i < v.chain.size(); ++i) {
    const auto& op = v.chain[i];
    if (!op.from.is_array() && !op.from.is_struct() && !op.from.is_matrix()) break;
    if (op.op == reshadefx::expression::operation::op_member) {
      text += "." + cg_.structMemberName(op.from.struct_definition, op.index);
      semantic = cg_.structMemberSemantic(op.from.struct_definition, op.index);
    } else if (op.op == reshadefx::expression::operation::op_constant_index && op.from.is_array()) {
      text += "[" + std::to_string(op.index) + "]";
    } else {
      throw Unsupported("access chain");
    }
    t = op.to;
  }
  vecStart = i;
  if (!isFloatType(t)) throw Unsupported("non-float variable");
  for (auto& l : leaves_)
    if (l.var == v.base && l.prefix == text) return l;
  Leaf l;
  l.var = v.base;
  l.prefix = text;
  l.type = t;
  // Semantics of pixel shader inputs only (members of an entry point's struct parameter).
  if (var->kind == Variable::Kind::Param) {
    const auto pf = paramOf_.find(v.base);
    if (pf != paramOf_.end() && pf->second->type == reshadefx::shader_type::pixel) l.semantic = semantic;
  }
  leaves_.push_back(l);
  return leaves_.back();
}

// A texture fetch of the root statement as an input, named by its call text.
Extractor::Leaf& Extractor::fetchLeaf(uint32_t id) {
  const auto it = fetchText_.find(id);
  if (it == fetchText_.end()) throw Unsupported("texture fetch");
  const Value& v = cg_.values.at(id);
  if (!isFloatType(v.type)) throw Unsupported("non-float texture fetch");
  for (auto& l : leaves_)
    if (l.fetch && l.prefix == it->second) return l;
  Leaf l;
  l.prefix = it->second;
  l.type = v.type;
  l.loadValue = id;
  l.fetch = true;
  leaves_.push_back(l);
  return leaves_.back();
}

// Components of a float1..4 value used by the vector part of a chain.
uint32_t chainMask(const std::vector<reshadefx::expression::operation>& chain, size_t start,
                   unsigned rows) {
  if (start >= chain.size()) return (1u << rows) - 1;
  const auto& op = chain[start];
  using O = reshadefx::expression::operation;
  if (op.op == O::op_swizzle) {
    uint32_t m = 0;
    for (int k = 0; k < 4 && op.swizzle[k] >= 0; ++k) m |= 1u << op.swizzle[k];
    return m;
  }
  if (op.op == O::op_constant_index) return 1u << op.index;
  if (op.op == O::op_cast && isFloatType(op.to) && op.to.rows < op.from.rows)
    return (1u << op.to.rows) - 1;
  if (op.op == O::op_cast && isFloatType(op.from) && isFloatType(op.to)) return (1u << rows) - 1;
  throw Unsupported("access chain");
}

void Extractor::collect(uint32_t id) {
  const auto it = cg_.values.find(id);
  if (it == cg_.values.end()) throw Unsupported("value");
  const Value& v = it->second;
  using K = Value::Kind;
  switch (v.kind) {
    case K::Const:
      if (!isFloatType(v.type)) throw Unsupported("non-float constant");
      return;
    case K::Load: {
      if (const auto it = inline_.find(v.base); it != inline_.end()) {
        collect(it->second);
        chainMask(v.chain, 0, 4);  // validates
        return;
      }
      size_t vs = 0;
      Leaf& l = leafOf(v, vs);
      l.mask |= chainMask(v.chain, vs, l.type.rows);
      if (!l.loadValue) l.loadValue = id;
      return;
    }
    case K::Chain:
      if (fetchText_.count(v.base)) {
        Leaf& l = fetchLeaf(v.base);
        l.mask |= chainMask(v.chain, 0, l.type.rows);
        return;
      }
      collect(v.base);
      chainMask(v.chain, 0, 4);  // validates
      return;
    case K::Unary:
      if (v.op != tokenid::minus && v.op != tokenid::plus) throw Unsupported("unary operator");
      if (!isFloatType(v.type)) throw Unsupported("non-float arithmetic");
      collect(v.args[0]);
      return;
    case K::Binary: {
      const bool cmp = v.op == tokenid::less || v.op == tokenid::less_equal ||
                       v.op == tokenid::greater || v.op == tokenid::greater_equal ||
                       v.op == tokenid::equal_equal || v.op == tokenid::exclaim_equal;
      const bool arith = v.op == tokenid::plus || v.op == tokenid::minus ||
                         v.op == tokenid::star || v.op == tokenid::slash ||
                         v.op == tokenid::plus_equal || v.op == tokenid::minus_equal ||
                         v.op == tokenid::star_equal || v.op == tokenid::slash_equal;
      if (!cmp && !arith) throw Unsupported("binary operator");
      if (arith && !isFloatType(v.type)) throw Unsupported("non-float arithmetic");
      if (cmp && !(v.type.is_boolean() && v.type.is_scalar())) throw Unsupported("vector comparison");
      for (uint32_t a : v.args) {
        const auto& av = cg_.values.at(a);
        if (!isFloatType(av.type)) throw Unsupported("non-float arithmetic");
        collect(a);
      }
      return;
    }
    case K::Ternary: {
      const auto& c = cg_.values.at(v.args[0]);
      if (!(c.type.is_boolean() && c.type.is_scalar())) throw Unsupported("vector select");
      if (!isFloatType(v.type)) throw Unsupported("non-float select");
      for (uint32_t a : v.args) collect(a);
      return;
    }
    case K::Intrinsic: {
      if (fetchText_.count(id)) {
        Leaf& l = fetchLeaf(id);
        l.mask |= (1u << l.type.rows) - 1;
        return;
      }
      const auto op = opFromCall(v.name, static_cast<uint8_t>(v.args.size()));
      if (!op) throw Unsupported("intrinsic " + v.name);
      if (!isFloatType(v.type)) throw Unsupported("non-float intrinsic");
      if (*op == Op::Pow || *op == Op::Exp || *op == Op::Log || *op == Op::Sin || *op == Op::Cos)
        usesPow_ = true;
      for (uint32_t a : v.args) {
        const auto ai = cg_.values.find(a);
        if (ai == cg_.values.end() || !isFloatType(ai->second.type)) throw Unsupported("intrinsic argument");
        collect(a);
      }
      return;
    }
    case K::Construct:
      if (!isFloatType(v.type)) throw Unsupported("matrix/non-float constructor");
      for (uint32_t a : v.args) {
        const auto ai = cg_.values.find(a);
        if (ai == cg_.values.end() || !isFloatType(ai->second.type)) throw Unsupported("constructor argument");
        collect(a);
      }
      return;
    case K::Call: throw Unsupported("function call");
    case K::Phi: throw Unsupported("short-circuit or branch value");
  }
}

uint32_t Extractor::applyVectorChain(uint32_t node,
                                     const std::vector<reshadefx::expression::operation>& chain,
                                     size_t start, ExprBuilder& b, const Leaf* leaf) {
  using O = reshadefx::expression::operation;
  for (size_t i = start; i < chain.size(); ++i) {
    const auto& op = chain[i];
    const bool first = i == start && leaf;
    auto comp = [&](int c) -> uint8_t { return first ? leaf->remap[c] : static_cast<uint8_t>(c); };
    const Type nt = b.nodes()[node].type;
    if (op.op == O::op_swizzle || op.op == O::op_constant_index ||
        (op.op == O::op_cast && op.to.rows < op.from.rows)) {
      uint8_t sw[4];
      unsigned n = 0;
      if (op.op == O::op_swizzle)
        for (int k = 0; k < 4 && op.swizzle[k] >= 0; ++k) sw[n++] = comp(op.swizzle[k]);
      else if (op.op == O::op_constant_index)
        sw[n++] = comp(static_cast<int>(op.index));
      else
        for (unsigned k = 0; k < op.to.rows; ++k) sw[n++] = comp(static_cast<int>(k));
      bool identity = n == width(nt);
      for (unsigned k = 0; k < n; ++k) identity = identity && sw[k] == k;
      // x.xxx of a scalar is HLSL's implicit broadcast: componentwise ops broadcast
      // float1 operands, so the IR keeps the scalar.
      bool broadcast = width(nt) == 1;
      for (unsigned k = 0; k < n; ++k) broadcast = broadcast && sw[k] == 0;
      if (!identity && !broadcast) node = b.swizzle(node, sw, n);
    } else if (op.op == O::op_cast) {
      // float -> floatN broadcast: componentwise ops broadcast float1 operands
    } else {
      throw Unsupported("access chain");
    }
  }
  return node;
}

// float3(c, c, c) next to a float3 operand: componentwise ops broadcast a scalar, so
// the IR (and the printed variant) uses c.
void scalarizeConstants(uint32_t* a, size_t n, ExprBuilder& b) {
  unsigned w = 1;
  for (size_t k = 0; k < n; ++k)
    if (b.nodes()[a[k]].op != Op::Const) w = std::max<unsigned>(w, width(b.nodes()[a[k]].type));
  if (w == 1) return;
  for (size_t k = 0; k < n; ++k) {
    const Node& c = b.nodes()[a[k]];
    if (c.op != Op::Const || width(c.type) != w) continue;
    bool same = true;
    for (unsigned j = 1; j < w; ++j) same = same && c.value[j] == c.value[0];
    if (same) a[k] = b.constant(c.value[0]);
  }
}

uint32_t Extractor::build(uint32_t id, ExprBuilder& b) {
  if (auto it = built_.find(id); it != built_.end()) return it->second;
  const Value& v = cg_.values.at(id);
  using K = Value::Kind;
  uint32_t r = 0;
  switch (v.kind) {
    case K::Const: {
      float c[4];
      for (unsigned k = 0; k < v.type.rows; ++k) c[k] = v.constant.as_float[k];
      r = b.constant(floatType(v.type.rows), c);
      break;
    }
    case K::Load: {
      if (const auto it = inline_.find(v.base); it != inline_.end()) {
        r = applyVectorChain(build(it->second, b), v.chain, 0, b, nullptr);
        break;
      }
      size_t vs = 0;
      const Leaf& l = leafOf(v, vs);
      r = applyVectorChain(b.input(l.input, floatType(std::popcount(l.mask))), v.chain, vs, b, &l);
      break;
    }
    case K::Chain:
      if (fetchText_.count(v.base)) {
        const Leaf& l = fetchLeaf(v.base);
        r = applyVectorChain(b.input(l.input, floatType(std::popcount(l.mask))), v.chain, 0, b, &l);
      } else {
        r = applyVectorChain(build(v.base, b), v.chain, 0, b, nullptr);
      }
      break;
    case K::Unary:
      r = v.op == tokenid::minus ? b.op(Op::Neg, build(v.args[0], b)) : build(v.args[0], b);
      break;
    case K::Binary: {
      Op op = Op::Add;
      switch (v.op) {
        case tokenid::plus: case tokenid::plus_equal: op = Op::Add; break;
        case tokenid::minus: case tokenid::minus_equal: op = Op::Sub; break;
        case tokenid::star: case tokenid::star_equal: op = Op::Mul; break;
        case tokenid::slash: case tokenid::slash_equal: op = Op::Div; break;
        case tokenid::less: op = Op::Lt; break;
        case tokenid::less_equal: op = Op::Le; break;
        case tokenid::greater: op = Op::Gt; break;
        case tokenid::greater_equal: op = Op::Ge; break;
        case tokenid::equal_equal: op = Op::Eq; break;
        case tokenid::exclaim_equal: op = Op::Ne; break;
        default: throw Unsupported("binary operator");
      }
      uint32_t a[2] = {build(v.args[0], b), build(v.args[1], b)};
      if (info(op).shape == Shape::Comp) scalarizeConstants(a, 2, b);
      r = b.op(op, a[0], a[1]);
      break;
    }
    case K::Ternary: {
      uint32_t a[2] = {build(v.args[1], b), build(v.args[2], b)};
      scalarizeConstants(a, 2, b);
      r = b.op(Op::Select, build(v.args[0], b), a[0], a[1]);
      break;
    }
    case K::Intrinsic: {
      if (fetchText_.count(id)) {
        const Leaf& l = fetchLeaf(id);
        r = b.input(l.input, floatType(std::popcount(l.mask)));
        break;
      }
      const Op op = *opFromCall(v.name, static_cast<uint8_t>(v.args.size()));
      uint32_t a[3] = {0, 0, 0};
      for (size_t k = 0; k < v.args.size(); ++k) a[k] = build(v.args[k], b);
      if (info(op).shape == Shape::Comp) scalarizeConstants(a, v.args.size(), b);
      r = b.op(op, a[0], a[1], a[2]);
      break;
    }
    case K::Construct: {
      uint32_t a[4];
      if (v.args.size() > 4) throw Unsupported("constructor");
      for (size_t k = 0; k < v.args.size(); ++k) a[k] = build(v.args[k], b);
      // The parser splits vector arguments into components (float4(v, 1) has
      // v.x, v.y, v.z, 1): merge runs of swizzles of one value back.
      uint32_t m[4];
      unsigned nm = 0;
      for (size_t k = 0; k < v.args.size();) {
        const Node& n = b.nodes()[a[k]];
        if (n.op != Op::Swizzle) { m[nm++] = a[k++]; continue; }
        uint8_t sw[4];
        unsigned cnt = 0;
        const uint32_t src = n.args[0];
        while (k < v.args.size() && cnt < 4) {
          const Node& o = b.nodes()[a[k]];
          if (o.op != Op::Swizzle || o.args[0] != src) break;
          for (unsigned c = 0; c < width(o.type) && cnt < 4; ++c) sw[cnt++] = o.swz[c];
          ++k;
        }
        bool identity = cnt == width(b.nodes()[src].type);
        for (unsigned c = 0; c < cnt; ++c) identity = identity && sw[c] == c;
        m[nm++] = identity ? src : b.swizzle(src, sw, cnt);
      }
      r = nm == 1 ? m[0] : b.construct(m, nm);
      break;
    }
    default: throw Unsupported("value");
  }
  // HLSL broadcasts a scalar in a vector context (a cast in the chain); the IR keeps
  // the scalar, componentwise ops broadcast it.
  built_[id] = r;
  return r;
}

Range Extractor::samplerRange(uint32_t valueId) {
  const auto it = cg_.values.find(valueId);
  if (it == cg_.values.end()) return Range::unknown();
  const auto si = cg_.samplers.find(it->second.base);
  if (si == cg_.samplers.end()) return Range::unknown();
  const SamplerInfo& s = si->second;
  using F = reshadefx::texture_format;
  const std::string sem = upper(s.textureSemantic);
  if (sem == "COLOR" || sem == "SV_TARGET")
    return s.srgb ? Range::of(0, 1, "BackBuffer (sRGB sampler)")
                  : Range::of(0, 1, "BackBuffer (8-bit SDR assumed)", 255);
  if (sem == "DEPTH") return Range::of(0, 1, "depth buffer");
  switch (s.format) {
    case F::r8: case F::rg8: case F::rgba8:
      return s.srgb ? Range::of(0, 1, "8-bit texture (sRGB)") : Range::of(0, 1, "8-bit texture", 255);
    case F::rgb10a2: return Range::of(0, 1, "10-bit texture", 1023);
    case F::r16: case F::rg16: case F::rgba16: return Range::of(0, 1, "16-bit unorm texture");
    default: return Range::unknown();
  }
}

Range Extractor::range(uint32_t id) {
  if (auto it = rangeMemo_.find(id); it != rangeMemo_.end()) return it->second;
  if (cg_.variables.count(id)) {
    // Call arguments are passed as variables (the parser copies them).
    const auto& d = defs_[id];
    return d.empty() ? Range::unknown() : range(d.back()->value);
  }
  const auto vi = cg_.values.find(id);
  if (vi == cg_.values.end()) return Range::unknown();
  const Value& v = vi->second;
  using K = Value::Kind;
  Range r;
  auto arg = [&](size_t k) { return range(v.args[k]); };
  switch (v.kind) {
    case K::Const: {
      if (v.type.base != reshadefx::type::t_float && v.type.base != reshadefx::type::t_int &&
          v.type.base != reshadefx::type::t_uint)
        break;
      double lo = INFINITY, hi = -INFINITY;
      for (unsigned k = 0; k < v.type.components() && k < 16; ++k) {
        const double x = v.type.base == reshadefx::type::t_float ? v.constant.as_float[k]
                         : v.type.base == reshadefx::type::t_int ? v.constant.as_int[k]
                                                                 : v.constant.as_uint[k];
        lo = std::min(lo, x);
        hi = std::max(hi, x);
      }
      r = Range::of(lo, hi, "constant");
      break;
    }
    case K::Load: r = varRange(v.base, v.seq, v.block); break;
    case K::Chain: r = range(v.base); break;
    case K::Unary:
      if (v.op == tokenid::minus) { Range a = arg(0); r = derived(-a.hi, -a.lo, {&a}); }
      else if (v.op == tokenid::plus) r = arg(0);
      break;
    case K::Binary: {
      Range a = arg(0), b = arg(1);
      switch (v.op) {
        case tokenid::plus: case tokenid::plus_equal: r = addR(a, b); break;
        case tokenid::minus: case tokenid::minus_equal: r = subR(a, b); break;
        case tokenid::star: case tokenid::star_equal: r = mulR(a, b); break;
        case tokenid::slash: case tokenid::slash_equal: r = mulR(a, rcpR(b)); break;
        default: break;
      }
      break;
    }
    case K::Ternary: r = unite(arg(1), arg(2)); break;
    case K::Phi: r = unite(arg(1), arg(2)); break;
    case K::Construct: {
      if (v.args.empty()) break;
      r = arg(0);
      for (size_t k = 1; k < v.args.size(); ++k) r = unite(r, arg(k));
      break;
    }
    case K::Intrinsic: {
      const std::string& n = v.name;
      const size_t na = v.args.size();
      if (isTexFetch(n) && na >= 1) {
        if (n.find("size") == std::string::npos) r = samplerRange(v.args[0]);
        break;
      }
      if (n == "saturate") r = clampR(arg(0), 0, 1);
      else if (n == "abs") {
        Range a = arg(0);
        if (a.known) {
          const double lo = (a.lo <= 0 && a.hi >= 0) ? 0 : std::min(std::fabs(a.lo), std::fabs(a.hi));
          r = derived(lo, std::max(std::fabs(a.lo), std::fabs(a.hi)), {&a});
        }
      } else if (n == "sqrt") r = monotone(clampR(arg(0), 0, INFINITY), [](double x) { return std::sqrt(x); });
      else if (n == "rsqrt") r = rcpR(monotone(clampR(arg(0), 0, INFINITY), [](double x) { return std::sqrt(x); }));
      else if (n == "rcp") r = rcpR(arg(0));
      else if (n == "exp") r = monotone(arg(0), [](double x) { return std::exp(x); });
      else if (n == "exp2") r = monotone(arg(0), [](double x) { return std::exp2(x); });
      else if (n == "log" || n == "log2" || n == "log10") {
        Range a = arg(0);
        if (a.known && a.lo > 0)
          r = monotone(a, n == "log" ? static_cast<double (*)(double)>([](double x) { return std::log(x); })
                          : n == "log2" ? static_cast<double (*)(double)>([](double x) { return std::log2(x); })
                                        : static_cast<double (*)(double)>([](double x) { return std::log10(x); }));
      } else if (n == "floor") r = monotone(arg(0), [](double x) { return std::floor(x); });
      else if (n == "ceil") r = monotone(arg(0), [](double x) { return std::ceil(x); });
      else if (n == "round") r = monotone(arg(0), [](double x) { return std::round(x); });
      else if (n == "trunc") r = monotone(arg(0), [](double x) { return std::trunc(x); });
      else if (n == "frac") r = Range::of(0, 1, "derived");
      else if (n == "sign" || n == "sin" || n == "cos" || n == "normalize") r = Range::of(-1, 1, "derived");
      else if (n == "step" || n == "smoothstep") r = Range::of(0, 1, "derived");
      else if (n == "min" && na == 2) r = minR(arg(0), arg(1));
      else if (n == "max" && na == 2) r = maxR(arg(0), arg(1));
      else if (n == "clamp" && na == 3) r = minR(maxR(arg(0), arg(1)), arg(2));
      else if (n == "mad" && na == 3) r = addR(mulR(arg(0), arg(1)), arg(2));
      else if (n == "lerp" && na == 3) {
        Range a = arg(0), b = arg(1), t = arg(2);
        r = addR(a, mulR(subR(b, a), t));
      } else if (n == "dot" && na == 2) {
        const double w = cg_.values.count(v.args[0]) ? cg_.values.at(v.args[0]).type.rows : 4;
        Range p = mulR(arg(0), arg(1));
        r = derived(w * p.lo, w * p.hi, {&p});
      } else if ((n == "length" && na == 1) || (n == "distance" && na == 2)) {
        const double w = cg_.values.count(v.args[0]) ? cg_.values.at(v.args[0]).type.rows : 4;
        Range a = n == "length" ? arg(0) : subR(arg(0), arg(1));
        if (a.known) r = derived(0, std::sqrt(w) * std::max(std::fabs(a.lo), std::fabs(a.hi)), {&a});
      } else if (n == "pow" && na == 2) {
        Range a = clampR(arg(0), 0, INFINITY), b = arg(1);
        if (a.known && b.known && (a.lo > 0 || b.lo > 0)) {
          double lo = INFINITY, hi = -INFINITY;
          for (double x : {a.lo, a.hi})
            for (double y : {b.lo, b.hi}) {
              const double p = std::pow(x, y);
              lo = std::min(lo, p);
              hi = std::max(hi, p);
            }
          if (b.lo <= 0 && b.hi >= 0) { lo = std::min(lo, 1.0); hi = std::max(hi, 1.0); }
          r = derived(lo, hi, {&a, &b});
        }
      }
      break;
    }
    case K::Call: break;
  }
  if (r.known && !(std::isfinite(r.lo) && std::isfinite(r.hi))) r = Range::unknown();
  rangeMemo_[id] = r;
  return r;
}

// "<file> <function> <name>" (uniforms: function "global").
std::string Extractor::varKey(uint32_t var) const {
  const auto vi = cg_.variables.find(var);
  if (vi == cg_.variables.end()) return {};
  const Variable& v = vi->second;
  const auto fi = varFunction_.find(var);
  const std::string function = v.kind == Variable::Kind::Uniform ? "global"
                               : fi != varFunction_.end()       ? fi->second->name
                                                                : std::string();
  if (function.empty()) return {};
  return pathFrom(v.loc.source).filename().string() + " " + function + " " +
         (v.kind == Variable::Kind::Uniform ? varText(var) : v.name);
}

// The analysed range, or the user's where the analysis has no fact.
Range Extractor::varRange(uint32_t var, uint32_t seq, uint32_t block) {
  Range r = varRangeRaw(var, seq, block);
  if ((!r.known || r.assumed) && opt_.userRanges) {
    const auto u = opt_.userRanges->find(varKey(var));
    if (u != opt_.userRanges->end()) r = Range::of(u->second.first, u->second.second, "user (facts file)");
  }
  return r;
}

Range Extractor::varRangeRaw(uint32_t var, uint32_t seq, uint32_t block) {
  const auto vi = cg_.variables.find(var);
  if (vi == cg_.variables.end()) return Range::unknown();
  const Variable& v = vi->second;
  switch (v.kind) {
    case Variable::Kind::Uniform: {
      double lo = NAN, hi = NAN;
      std::string uiType, source;
      for (const auto& a : v.annotations) {
        auto num = [&]() -> double {
          if (a.type.is_floating_point()) return a.value.as_float[0];
          if (a.type.is_integral()) return a.type.is_signed() ? a.value.as_int[0] : a.value.as_uint[0];
          return NAN;
        };
        if (a.name == "ui_min") lo = num();
        else if (a.name == "ui_max") hi = num();
        else if (a.name == "ui_type") uiType = a.value.string_data;
        else if (a.name == "source") source = a.value.string_data;
      }
      if (!source.empty()) {
        if (source == "timer") return Range::of(0, 1e7, "source = timer (ms)");
        if (source == "frametime") return Range::of(0, 1000, "source = frametime (ms)");
        if (source == "pingpong") return Range::of(lo == lo ? lo : 0, hi == hi ? hi : 1, "source = pingpong");
        return Range::unknown();
      }
      if (std::isfinite(lo) && std::isfinite(hi))
        return Range::of(std::min(lo, hi), std::max(lo, hi), "ui_min/ui_max");
      if (uiType == "color") return Range::of(0, 1, "ui_type = color");
      return Range::unknown();
    }
    case Variable::Kind::Param:
    case Variable::Kind::Local: {
      // Range on entry: parameters from their semantic or call sites, locals none.
      // A temporary passed as out/inout argument: after the call, the callee's value.
      if (const auto oa = outArgOf_.find(var); oa != outArgOf_.end() && seq > oa->second.seq)
        return outParamRange(*oa->second.callee, oa->second.index);
      auto entry = [&]() -> Range {
        if (v.kind != Variable::Kind::Param || outOnly(v)) return Range::unknown();
        const auto pf = paramOf_.find(var);
        if (pf == paramOf_.end()) return Range::unknown();
        const Function* f = pf->second;
        const std::string sem = upper(v.semantic);
        if (sem == "SV_POSITION" || sem == "VPOS")
          return Range::of(0, 16384, "SV_Position (pixels, up to the 16384 hardware limit)");
        // Pixel shader input: what the vertex shaders of its passes write, else the
        // semantic's convention.
        if (f->type == reshadefx::shader_type::pixel) {
          const Range r = pixelInputRange(*f, sem);
          return r.known ? r : semanticConvention(sem);
        }
        // Helper function parameter: union over the call sites.
        if (!varBusy_.insert(var).second) return Range::unknown();
        const size_t index = std::find(f->params.begin(), f->params.end(), var) - f->params.begin();
        Range r;
        bool any = false;
        for (const auto& [id, cv] : cg_.values) {
          if (cv.kind != Value::Kind::Call || cv.name != f->uniqueName || index >= cv.args.size()) continue;
          const Range a = range(cv.args[index]);
          r = any ? unite(r, a) : a;
          any = true;
        }
        varBusy_.erase(var);
        if (any && r.known && r.why != "constant") r.why = "call sites";
        return any ? r : Range::unknown();
      };
      const auto di = defs_.find(var);
      if (di == defs_.end()) return entry();
      // Reaching definitions: the last one before the load if it is in the load's
      // block (it kills the others), else all earlier ones plus those later in a loop
      // around the load, plus the entry value.
      std::vector<const Statement*> reach;
      const Statement* last = nullptr;
      for (const Statement* d : di->second)
        if (d->seq < seq && (!last || d->seq > last->seq)) last = d;
      bool killed = false;
      if (last && last->block == block && last->chain.empty()) {
        reach.push_back(last);
        killed = true;
      } else {
        for (const Statement* d : di->second) {
          bool inLoop = false;
          for (const auto& [lo, hi] : cg_.loops)
            inLoop = inLoop || (lo <= seq && seq <= hi && lo <= d->seq && d->seq <= hi);
          if (d->seq < seq || inLoop) reach.push_back(d);
        }
      }
      if (reach.empty()) return entry();
      const uint64_t key = (uint64_t(var) << 32) | seq;
      if (!localBusy_.insert(key).second) return Range::unknown();
      Range r;
      bool full = false;
      for (size_t k = 0; k < reach.size(); ++k) {
        const Range dr = range(reach[k]->value);
        r = k ? unite(r, dr) : dr;
        full = full || reach[k]->chain.empty();
      }
      localBusy_.erase(key);
      if (!killed && v.kind == Variable::Kind::Param && !outOnly(v)) return unite(r, entry());
      return full ? r : Range::unknown();
    }
    case Variable::Kind::Global: break;
  }
  return Range::unknown();
}

bool Extractor::outOnly(const Variable& v) {
  return v.type.has(reshadefx::type::q_out) && !v.type.has(reshadefx::type::q_in);
}

// "TEXCOORD0" and "TEXCOORD" are the same semantic.
std::string semanticKey(std::string s) {
  s = upper(s);
  size_t d = s.size();
  while (d > 0 && std::isdigit(static_cast<unsigned char>(s[d - 1]))) --d;
  const std::string index = s.substr(d);
  return s.substr(0, d) + (index.empty() ? "0" : std::to_string(std::stoul(index)));
}

// Range of a function's out parameter after it returns. ReShade's PostProcessVS draws a
// full-screen triangle: its texcoord is 0..2 at the vertices, 0..1 on screen.
Range Extractor::outParamRange(const Function& g, size_t index) {
  if (index >= g.params.size()) return Range::unknown();
  const uint32_t p = g.params[index];
  const Variable& pv = cg_.variables.at(p);
  if (g.name == "PostProcessVS" && semanticKey(pv.semantic) == "TEXCOORD0")
    return Range::of(0, 1, "TEXCOORD from PostProcessVS");
  // All its definitions (the end of the function is after all of them).
  Range r = varRange(p, UINT32_MAX, UINT32_MAX);
  if (r.known && r.why != "constant") r.why = "vertex shader " + g.name;
  return r;
}

// A pixel shader input: the union over the passes that use the shader of what their
// vertex shader writes to the same semantic.
Range Extractor::pixelInputRange(const Function& ps, const std::string& semantic) {
  const std::string key = semanticKey(semantic);
  Range r;
  bool any = false;
  for (const auto& t : cg_.mod().techniques)
    for (const auto& pass : t.passes) {
      if (pass.ps_entry_point != ps.uniqueName) continue;
      const Function* vs = cg_.function(pass.vs_entry_point);
      if (!vs) return Range::unknown();
      Range out;
      bool found = false;
      for (size_t k = 0; k < vs->params.size(); ++k) {
        const Variable& pv = cg_.variables.at(vs->params[k]);
        if (pv.type.has(reshadefx::type::q_out) && semanticKey(pv.semantic) == key) {
          out = outParamRange(*vs, k);
          found = true;
        }
      }
      if (!found) return Range::unknown();
      r = any ? unite(r, out) : out;
      any = true;
    }
  return any ? r : Range::unknown();
}

// Ranges that semantics imply for pixel shader inputs: SV_Position is in pixels;
// TEXCOORD0..9 are texture coordinates by convention (programmers name them so), [0, 1].
Range semanticConvention(const std::string& semantic) {
  const std::string key = semanticKey(semantic);
  if (key == "SV_POSITION0" || key == "VPOS0") return Range::of(0, 16384, "SV_Position (pixels, up to the 16384 hardware limit)");
  if (key.rfind("TEXCOORD", 0) == 0) return Range::of(0, 1, "TEXCOORD semantic (convention)");
  return Range::unknown();
}

// How the value stored by a statement is used: in comparisons, as texture coordinates,
// otherwise.
void Extractor::useKinds(uint32_t id, bool& cmp, bool& coord, bool& other, int depth) {
  if (depth > 4) { other = true; return; }
  const auto ui = users_.find(id);
  if (ui != users_.end())
    for (uint32_t u : ui->second) {
      const Value& uv = cg_.values.at(u);
      switch (uv.kind) {
        case Value::Kind::Chain:
        case Value::Kind::Load: useKinds(u, cmp, coord, other, depth + 1); break;
        case Value::Kind::Binary:
          if (uv.op == tokenid::less || uv.op == tokenid::less_equal || uv.op == tokenid::greater ||
              uv.op == tokenid::greater_equal || uv.op == tokenid::equal_equal ||
              uv.op == tokenid::exclaim_equal)
            cmp = true;
          else
            other = true;
          break;
        case Value::Kind::Ternary:
          if (uv.args[0] == id) cmp = true; else other = true;
          break;
        case Value::Kind::Intrinsic:
          if (isTexFetch(uv.name) && uv.args.size() >= 2 && uv.args[1] == id) coord = true;
          else other = true;
          break;
        default: other = true; break;
      }
    }
  const auto si = stmtUses_.find(id);
  if (si != stmtUses_.end())
    for (const Statement* s : si->second) {
      // Copied into another variable (also call arguments): follow its loads.
      if (s->kind == Statement::Kind::Return) { other = true; continue; }
      bool found = false;
      for (const auto& [lid, lv] : cg_.values)
        if (lv.kind == Value::Kind::Load && lv.base == s->var && lv.seq > s->seq) {
          found = true;
          useKinds(lid, cmp, coord, other, depth + 1);
        }
      if (!found) other = true;
    }
}

Budget Extractor::budgetFor(const Function& f, const Statement& s, std::string& reason) {
  Budget b;
  if (s.kind == Statement::Kind::Return) {
    // A pixel shader's result written to an 8-bit target without blending.
    if (f.type == reshadefx::shader_type::pixel && !f.returnType.is_struct()) {
      bool eightBit = true, anyPass = false;
      for (const auto& t : cg_.mod().techniques)
        for (const auto& p : t.passes) {
          if (p.ps_entry_point != f.uniqueName) continue;
          anyPass = true;
          if (p.blend_enable[0] || p.srgb_write_enable) eightBit = false;
          if (!p.render_target_names[0].empty()) {
            bool fmt8 = false;
            for (const auto& tex : cg_.mod().textures)
              if (tex.unique_name == p.render_target_names[0] || tex.name == p.render_target_names[0])
                fmt8 = tex.format == reshadefx::texture_format::rgba8 ||
                       tex.format == reshadefx::texture_format::r8 ||
                       tex.format == reshadefx::texture_format::rg8;
            eightBit = eightBit && fmt8;
          }
        }
      if (anyPass && eightBit) {
        b.kind = Budget::Kind::Color8;
        b.maxCodeDiff = 0;
        reason = "pixel shader output, 8-bit target, no blending";
        return b;
      }
    }
    b.kind = Budget::Kind::Rel;
    b.eps = opt_.relEps;
    reason = "returned value";
    return b;
  }
  bool cmp = false, coord = false, other = false;
  for (const auto& [lid, lv] : cg_.values)
    if (lv.kind == Value::Kind::Load && lv.base == s.var && lv.seq > s.seq)
      useKinds(lid, cmp, coord, other, 0);
  if (cmp) {
    b.kind = Budget::Kind::Exact;
    reason = "used in a comparison";
  } else if (coord && !other) {
    b.kind = Budget::Kind::Texcoord;
    b.px = opt_.texcoordPx;
    b.eps = opt_.texcoordPx / 3840.0;
    reason = "used as texture coordinate";
  } else {
    b.kind = Budget::Kind::Rel;
    b.eps = opt_.relEps;
    reason = "intermediate value";
  }
  return b;
}

// Statement text and shape: fills file, lines, original, kind and lhs.
bool Extractor::shapeOf(const Statement& s, Region& reg, std::string& why) {
  const std::string file = s.loc.source;
  const std::vector<std::string>* lines = file.empty() ? nullptr : sourceLines(file);
  if (!lines) { why = "no source text"; return false; }
  const auto val = cg_.values.find(s.value);
  if (val == cg_.values.end()) { why = "value is a variable"; return false; }
  reg.file = file;
  reg.line = s.loc.line;
  StatementText st;
  if (!statementAt(*lines, s.loc.line, st, why)) return false;
  reg.lastLine = st.last;
  reg.original = st.original;
  reg.text = st.text;
  std::string name;
  if (s.kind != Statement::Kind::Return) {
    const auto vi = cg_.variables.find(s.var);
    if (vi == cg_.variables.end() || vi->second.kind == Variable::Kind::Global) {
      why = "store to a global";
      return false;
    }
    name = vi->second.name;
  }
  const std::string& body = st.text;
  if (s.kind == Statement::Kind::Return) {
    if (body.rfind("return", 0) != 0 || (body.size() > 6 && isIdent(body[6]))) {
      why = "return value not at statement start";
      return false;
    }
    reg.kind = Region::Kind::Return;
    reg.lhs = "return";
  } else {
    char compound = 0;
    const size_t eq = assignmentOp(body, compound);
    if (eq == std::string::npos) { why = "no assignment"; return false; }
    const std::string lhs = trim(body.substr(0, eq));
    if (hasDepth0Comma(body)) { why = "several declarators"; return false; }
    if (s.kind == Statement::Kind::Init) {
      // "type name", possibly with qualifiers; name last.
      if (lhs.size() < name.size() || lhs.compare(lhs.size() - name.size(), name.size(), name) != 0 ||
          lhs.size() == name.size() || isIdent(lhs[lhs.size() - name.size() - 1]) || compound ||
          lhs.find_first_of("[]:(") != std::string::npos) {
        why = "declaration shape";
        return false;
      }
      reg.kind = Region::Kind::Init;
    } else {
      if (lhs.compare(0, name.size(), name) != 0 || (lhs.size() > name.size() && isIdent(lhs[name.size()])) ||
          lhs.find('(') != std::string::npos) {
        why = "assignment shape";
        return false;
      }
      reg.kind = Region::Kind::Store;
    }
    reg.lhs = lhs + " =";
  }
  reg.prog.outputName = name.empty() ? "r" : name;
  // Macros: the source tokens must equal the preprocessed tokens.
  std::string src, pp;
  for (uint32_t l = st.first; l <= st.last; ++l) {
    src += (*lines)[l - 1] + '\n';
    const auto it = fx_.ppLines.find({file, l});
    if (it == fx_.ppLines.end()) { why = "uses a macro"; return false; }
    pp += it->second + '\n';
  }
  if (tokens(withoutFetches(src)) != tokens(withoutFetches(pp))) { why = "uses a macro"; return false; }
  return true;
}

// IR of a statement's value (with the temporaries in inline_ replaced by their
// definitions), inputs and facts.
bool Extractor::buildRegion(const Statement& s, Region& reg, std::string& why) {
  leaves_.clear();
  built_.clear();
  usesPow_ = false;
  reg.prog.inputs.clear();
  reg.facts.clear();
  mapFetches(s, reg.text);
  try {
    collect(s.value);
    if (leaves_.size() > opt_.maxInputs) throw Unsupported("too many inputs");
    uint32_t slots = 0;
    for (auto& l : leaves_) {
      l.remap.assign(4, 0);
      uint8_t k = 0;
      for (int c = 0; c < 4; ++c)
        if (l.mask & (1u << c)) l.remap[c] = k++;
      slots += k;
    }
    if (slots > opt_.maxSlots) throw Unsupported("too many inputs");
    for (size_t i = 0; i < leaves_.size(); ++i) {
      Leaf& l = leaves_[i];
      l.input = static_cast<uint32_t>(i);
      const unsigned w = static_cast<unsigned>(std::popcount(l.mask));
      InputDecl d;
      d.name = l.prefix;
      if (w < l.type.rows) {
        static const char* xyzw = "xyzw";
        d.name += '.';
        for (int c = 0; c < 4; ++c)
          if (l.mask & (1u << c)) d.name += xyzw[c];
      }
      d.type = floatType(w);
      Range r = range(l.loadValue);
      Fact fact;
      fact.input = d.name;
      const bool plainVar = !l.fetch && cg_.variables.count(l.var) && l.prefix == varText(l.var);
      fact.key = plainVar ? varKey(l.var) : std::string();
      if (fact.key.empty())
        fact.key = pathFrom(reg.file).filename().string() + " " + reg.function + " " + l.prefix;
      // Ask for uniforms and parameters first: other values are often computed from them.
      const auto kv = cg_.variables.find(l.var);
      fact.order = l.fetch ? 2
                   : kv == cg_.variables.end() ? 3
                   : kv->second.kind == Variable::Kind::Uniform ? 0
                   : kv->second.kind == Variable::Kind::Param ? 1 : 3;
      if (!r.known && !l.semantic.empty()) r = semanticConvention(l.semantic);
      if (!r.known || r.assumed) {
        const auto u = opt_.userRanges ? opt_.userRanges->find(fact.key) : UserRanges::const_iterator();
        if (opt_.userRanges && u != opt_.userRanges->end()) {
          r = Range::of(u->second.first, u->second.second, "user (facts file)");
        } else if (!r.known) {
          r = Range::of(opt_.defaultLo, opt_.defaultHi, "assumed");
          r.assumed = true;
        }
      }
      if (r.assumed) suggest(l, fact);
      d.lo = r.lo;
      d.hi = r.hi;
      d.grid = r.grid;
      fact.source = r.why;
      fact.assumed = r.assumed;
      fact.fetch = l.fetch;
      if (d.lo == d.hi) throw Unsupported("input is a constant");  // the compiler folds it
      reg.prog.inputs.push_back(d);
      reg.facts.push_back(fact);
    }
    ExprBuilder b;
    const uint32_t root = build(s.value, b);
    reg.prog.target = b.finish(root);
  } catch (const Unsupported& e) {
    why = e.what();
    return false;
  } catch (const std::invalid_argument&) {
    why = "types";
    return false;
  }
  const Type rt = reg.prog.target.nodes[reg.prog.target.root].type;
  if (!isFloat(rt)) { why = "boolean value"; return false; }
  uint32_t ops = 0;
  for (const auto& n : reg.prog.target.nodes) ops += n.op != Op::Input && n.op != Op::Const;
  if (ops < opt_.minOps) { why = "fewer than minOps operations"; return false; }
  if (ops > opt_.maxOps) { why = "more than maxOps operations"; return false; }
  if (reg.prog.inputs.empty()) { why = "constant expression"; return false; }
  return true;
}

// Texture fetches of a statement, matched to their call text in the statement: both
// are in source order when no fetch is nested in another's arguments.
void Extractor::mapFetches(const Statement& s, const std::string& text) {
  fetchText_.clear();
  std::unordered_set<uint32_t> tree;
  treeValues(s.value, tree);
  std::vector<std::pair<uint32_t, uint32_t>> fetches;  // seq, id
  for (uint32_t id : tree) {
    const auto it = cg_.values.find(id);
    if (it != cg_.values.end() && it->second.kind == Value::Kind::Intrinsic && isTexFetch(it->second.name))
      fetches.emplace_back(it->second.seq, id);
  }
  if (fetches.empty()) return;
  std::sort(fetches.begin(), fetches.end());
  std::vector<std::string> calls;
  for (size_t i = 0; i + 5 < text.size(); ++i) {
    if (!fetchAt(text, i)) continue;
    size_t j = i + 5;
    while (j < text.size() && isIdent(text[j])) ++j;
    while (j < text.size() && text[j] == ' ') ++j;
    if (j >= text.size() || text[j] != '(') continue;
    int depth = 0;
    size_t k = j;
    for (; k < text.size(); ++k) {
      if (text[k] == '(') ++depth;
      if (text[k] == ')' && --depth == 0) break;
    }
    if (k >= text.size()) return;
    for (size_t m = i + 1; m < k; ++m)
      if (fetchAt(text, m)) return;  // nested fetch
    calls.push_back(text.substr(i, k + 1 - i));
    i = k;
  }
  if (calls.size() != fetches.size()) return;
  for (size_t k = 0; k < calls.size(); ++k) fetchText_[fetches[k].second] = calls[k];
}

// A range to suggest for an input without facts, from its name, a uniform's default
// value or the texture it is read from.
void Extractor::suggest(const Leaf& l, Fact& f) const {
  std::string n;
  for (char c : l.prefix) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const size_t dot = n.find_last_of(".:");
  const std::string last = dot == std::string::npos ? n : n.substr(dot + 1);
  auto has = [&](std::initializer_list<const char*> words) {
    for (const char* w : words)
      if (last.find(w) != std::string::npos) return true;
    return false;
  };
  auto set = [&](double lo, double hi, const char* why) {
    f.suggestLo = lo;
    f.suggestHi = hi;
    f.suggestWhy = why;
  };
  if (l.fetch) return set(0, 1, "texture read (float format)");
  std::string semantic = l.semantic;
  if (const auto it = cg_.variables.find(l.var); semantic.empty() && it != cg_.variables.end())
    semantic = it->second.semantic;
  if (upper(semantic).rfind("COLOR", 0) == 0) return set(0, 1, "COLOR semantic (usually a color)");
  if (const auto it = cg_.variables.find(l.var); it != cg_.variables.end() &&
      it->second.kind == Variable::Kind::Uniform && it->second.hasDefault &&
      it->second.type.is_floating_point()) {
    const double d = it->second.defaultValue.as_float[0];
    if (d > 0) return set(0, 2 * d, "uniform, twice its default");
    if (d < 0) return set(2 * d, 0, "uniform, twice its default");
  }
  if (has({"uv", "coord", "tex"})) return set(0, 1, "name looks like a texture coordinate");
  if (has({"col", "rgb", "luma", "lum"})) return set(0, 1, "name looks like a color");
  if (has({"depth"})) return set(0, 1, "name looks like depth");
  if (has({"pos", "pixel"})) return set(0, 16384, "name looks like a pixel position");
  set(opt_.defaultLo, opt_.defaultHi, "no guess (the default)");
}

// Values of a statement's tree (through Chain bases and operands).
void Extractor::treeValues(uint32_t id, std::unordered_set<uint32_t>& out) const {
  if (!out.insert(id).second) return;
  const auto it = cg_.values.find(id);
  if (it == cg_.values.end()) return;
  if (it->second.kind == Value::Kind::Chain) treeValues(it->second.base, out);
  for (uint32_t a : it->second.args) treeValues(a, out);
}

// Temporaries that can be inlined into statement `use`: locals declared once in the
// same block before it, read only by it, whose own inputs are not written in
// between. Recursively for their definitions. Adds them to inline_ and `defs`.
void Extractor::findTemps(const Statement& use, std::vector<const Statement*>& defs, int depth) {
  if (depth > 3 || defs.size() >= opt_.maxStatements - 1) return;
  std::unordered_set<uint32_t> tree;
  treeValues(use.value, tree);
  for (uint32_t id : tree) {
    const auto vit = cg_.values.find(id);
    if (vit == cg_.values.end()) continue;
    const Value& v = vit->second;
    if (v.kind != Value::Kind::Load || inline_.count(v.base)) continue;
    const auto vi = cg_.variables.find(v.base);
    if (vi == cg_.variables.end() || vi->second.kind != Variable::Kind::Local) continue;
    const auto di = defs_.find(v.base);
    if (di == defs_.end() || di->second.size() != 1) continue;
    const Statement* d = di->second[0];
    if (d->kind != Statement::Kind::Init || d->block != use.block || d->seq >= use.seq) continue;
    if (d->loc.source != use.loc.source) continue;
    bool onlyHere = true;
    for (const auto& [lid, lv] : cg_.values)
      if (lv.kind == Value::Kind::Load && lv.base == v.base && !tree.count(lid)) onlyHere = false;
    if (!onlyHere) continue;
    // Its inputs must not change between the definition and the use.
    std::unordered_set<uint32_t> dtree;
    treeValues(d->value, dtree);
    bool stable = true;
    for (uint32_t x : dtree) {
      const auto xit = cg_.values.find(x);
      if (xit == cg_.values.end() || xit->second.kind != Value::Kind::Load) continue;
      const Value& xv = xit->second;
      const auto xd = defs_.find(xv.base);
      if (xd != defs_.end())
        for (const Statement* w : xd->second) stable = stable && !(w->seq > d->seq && w->seq < use.seq);
    }
    if (!stable) continue;
    Region shape;
    std::string why;
    if (!shapeOf(*d, shape, why)) continue;
    if (defs.size() >= opt_.maxStatements - 1) return;
    inline_[v.base] = d->value;
    defs.push_back(d);
    findTemps(*d, defs, depth + 1);
  }
}

std::vector<Region> Extractor::run(SkipCount& skipped) {
#define SKIPADD(r) skipped.add((r), s.loc.source, s.loc.line)
  // Functions reachable from pixel shaders.
  std::set<std::string> reach;
  std::vector<const Function*> work;
  for (const auto& f : cg_.functions)
    if (f->type == reshadefx::shader_type::pixel && reach.insert(f->uniqueName).second) work.push_back(f.get());
  while (!work.empty()) {
    const Function* f = work.back();
    work.pop_back();
    for (const auto& c : f->calls)
      if (const Function* g = cg_.function(c); g && reach.insert(c).second) work.push_back(g);
  }

  std::vector<Region> out;
  for (const auto& fp : cg_.functions) {
    const Function& f = *fp;
    if (!reach.count(f.uniqueName)) continue;
    for (const Statement& s : f.stmts) {
      const auto val = cg_.values.find(s.value);
      if (val != cg_.values.end() &&
          (val->second.kind == Value::Kind::Load || val->second.kind == Value::Kind::Const)) {
        SKIPADD("copy or constant");
        continue;
      }
      Region reg;
      reg.function = f.name;
      std::string why;
      if (!shapeOf(s, reg, why)) { SKIPADD(why); continue; }
      Region single = reg;
      inline_.clear();
      if (buildRegion(s, single, why)) {
        single.prog.budget = budgetFor(f, s, single.budgetReason);
        out.push_back(single);
      } else {
        SKIPADD(why);
      }
      // Window: the statement with the single-use temporaries it reads.
      if (opt_.maxStatements < 2) continue;
      std::vector<const Statement*> defs;
      inline_.clear();
      findTemps(s, defs, 0);
      if (defs.empty()) continue;
      std::sort(defs.begin(), defs.end(), [](const Statement* a, const Statement* b) { return a->seq < b->seq; });
      Region win = reg;
      std::string text;
      for (const Statement* d : defs) {
        Region shape;
        shapeOf(*d, shape, why);
        win.removed.emplace_back(shape.line, shape.lastLine);
        text += shape.original + "\n";
      }
      win.original = text + reg.original;
      if (buildRegion(s, win, why)) {
        win.prog.budget = budgetFor(f, s, win.budgetReason);
        out.push_back(std::move(win));
      }
      inline_.clear();
    }
  }
#undef SKIPADD
  return out;
}

}  // namespace

bool parseRange(const std::string& text, double& lo, double& hi) {
  std::string t;
  for (char c : text) t += (c == '[' || c == ']' || c == ',') ? ' ' : c;
  std::istringstream in(t);
  if (!(in >> lo >> hi)) return false;
  std::string rest;
  if (in >> rest) return false;
  if (lo > hi) std::swap(lo, hi);
  return true;
}

bool readUserRanges(const fs::path& file, UserRanges& out, std::string& error) {
  std::ifstream f(file);
  if (!f) {
    error = "cannot read " + file.string();
    return false;
  }
  std::string line;
  for (int no = 1; std::getline(f, line); ++no) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (const size_t h = line.find('#'); h != std::string::npos) line.erase(h);
    const std::string t = trim(line);
    if (t.empty()) continue;
    const size_t eq = t.rfind('=');
    std::istringstream key(eq == std::string::npos ? std::string() : t.substr(0, eq));
    std::string file_, function, input;
    double lo = 0, hi = 0;
    key >> file_ >> function;
    std::getline(key, input);
    input = trim(input);
    if (eq == std::string::npos || input.empty() || !parseRange(t.substr(eq + 1), lo, hi)) {
      error = file.string() + ":" + std::to_string(no) + ": expected '<file> <function> <input> = [lo, hi]'";
      return false;
    }
    out[file_ + " " + function + " " + input] = {lo, hi};
  }
  return true;
}

std::vector<Region> extractRegions(const Effect& fx, const Effect* alt, const RegionOptions& opt,
                                   SkipCount& skipped) {
  std::vector<Region> regions = Extractor(fx, opt).run(skipped);
  if (!alt) return regions;
  SkipCount ignored;
  const std::vector<Region> other = Extractor(*alt, opt).run(ignored);
  std::map<std::tuple<std::string, uint32_t, int, size_t>, std::string> texts;
  for (const auto& r : other)
    texts[{r.file, r.line, static_cast<int>(r.kind), r.removed.size()}] = toString(r.prog.target, r.prog.inputs);
  std::vector<Region> kept;
  for (auto& r : regions) {
    const auto it = texts.find({r.file, r.line, static_cast<int>(r.kind), r.removed.size()});
    if (it != texts.end() && it->second != toString(r.prog.target, r.prog.inputs)) {
      skipped.add("depends on BUFFER_WIDTH/HEIGHT", r.file, r.line);
      continue;
    }
    kept.push_back(std::move(r));
  }
  return kept;
}

}  // namespace sopt::fx
