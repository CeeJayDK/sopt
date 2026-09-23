#include "ir/parser.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "ir/eval.hpp"

namespace sopt {
namespace {

enum class Tok { End, Num, Ident, Punct };

struct Token {
  Tok kind = Tok::End;
  std::string text;
  double num = 0.0;
};

std::vector<Token> tokenize(std::string_view s, int line) {
  std::vector<Token> out;
  size_t i = 0;
  auto err = [&](const std::string& msg) {
    throw ParseError("line " + std::to_string(line) + ": " + msg);
  };
  while (i < s.size()) {
    const char ch = s[i];
    if (std::isspace(static_cast<unsigned char>(ch))) { ++i; continue; }
    if (ch == '#') break;
    if (std::isdigit(static_cast<unsigned char>(ch)) ||
        (ch == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      const std::string rest(s.substr(i));
      char* end = nullptr;
      const double v = std::strtod(rest.c_str(), &end);
      size_t len = static_cast<size_t>(end - rest.c_str());
      if (len == 0) err("bad number");
      if (i + len < s.size() && (s[i + len] == 'f' || s[i + len] == 'F')) ++len;
      out.push_back({Tok::Num, std::string(s.substr(i, len)), v});
      i += len;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
      size_t j = i;
      while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
      out.push_back({Tok::Ident, std::string(s.substr(i, j - i)), 0.0});
      i = j;
      continue;
    }
    static const char* two[] = {"<=", ">=", "==", "!="};
    bool matched = false;
    for (const char* t : two) {
      if (s.substr(i, 2) == t) {
        out.push_back({Tok::Punct, t, 0.0});
        i += 2;
        matched = true;
        break;
      }
    }
    if (matched) continue;
    if (std::string_view("+-*/()<>?:,=[].").find(ch) != std::string_view::npos) {
      out.push_back({Tok::Punct, std::string(1, ch), 0.0});
      ++i;
      continue;
    }
    err(std::string("unexpected character '") + ch + "'");
  }
  out.push_back({Tok::End, "", 0.0});
  return out;
}

class ExprParser {
 public:
  ExprParser(const std::vector<Token>& toks, size_t pos, const std::vector<InputDecl>& inputs,
             ExprBuilder& b, int line)
      : t_(toks), p_(pos), inputs_(inputs), b_(b), line_(line) {}

  uint32_t parse() { return ternary(); }
  size_t pos() const { return p_; }

 private:
  [[noreturn]] void err(const std::string& msg) const {
    throw ParseError("line " + std::to_string(line_) + ": " + msg);
  }
  const Token& peek() const { return t_[p_]; }
  bool isPunct(std::string_view s) const { return peek().kind == Tok::Punct && peek().text == s; }
  void expect(std::string_view s) {
    if (!isPunct(s)) err("expected '" + std::string(s) + "'");
    ++p_;
  }

  Type typeOf(uint32_t n) const { return b_.nodes()[n].type; }
  bool isConst(uint32_t n) const { return b_.nodes()[n].op == Op::Const; }

  static std::string typeName(Type t) {
    if (t == Type::Bool) return "bool";
    return width(t) == 1 ? "float" : "float" + std::to_string(width(t));
  }

  // Folds a node whose operands are all constants.
  uint32_t fold(const Node& node) {
    const auto& ns = b_.nodes();
    Type ts[4];
    float buf[4][4];
    const float* ptr[4][4];
    for (unsigned k = 0; k < node.nargs; ++k) {
      ts[k] = ns[node.args[k]].type;
      for (unsigned c = 0; c < 4; ++c) {
        buf[k][c] = ns[node.args[k]].value[c];
        ptr[k][c] = &buf[k][c];
      }
    }
    float res[4] = {0, 0, 0, 0};
    float* out[4] = {&res[0], &res[1], &res[2], &res[3]};
    std::vector<float> tmp;
    evalNode(node, ts, ptr, out, 1, kProfileRef, tmp);
    return b_.constant(node.type, res);
  }

  // Builds an op node, folding it if all operands are constants.
  uint32_t make(Op op, uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    const auto& oi = info(op);
    const uint32_t args[3] = {a, b, c};
    Type ts[3];
    for (uint8_t k = 0; k < oi.arity; ++k) ts[k] = typeOf(args[k]);
    const auto t = inferType(op, ts, oi.arity);
    if (!t) {
      std::string msg = std::string("operand types don't fit ") + std::string(oi.name) + "(";
      for (uint8_t k = 0; k < oi.arity; ++k) msg += (k ? ", " : "") + typeName(ts[k]);
      err(msg + ")" + (oi.shape == Shape::Cmp ? ": comparisons are scalar" : ""));
    }
    bool allConst = true;
    for (uint8_t k = 0; k < oi.arity; ++k) allConst = allConst && isConst(args[k]);
    if (allConst && isFloat(*t)) {
      Node node;
      node.op = op;
      node.type = *t;
      node.nargs = oi.arity;
      for (uint8_t k = 0; k < oi.arity; ++k) node.args[k] = args[k];
      return fold(node);
    }
    return b_.op(op, a, b, c);
  }

  uint32_t makeSwizzle(uint32_t a, const std::string& letters) {
    uint8_t comps[4];
    if (letters.empty() || letters.size() > 4) err("bad swizzle ." + letters);
    for (size_t k = 0; k < letters.size(); ++k) {
      const size_t xyzw = std::string_view("xyzw").find(letters[k]);
      const size_t rgba = std::string_view("rgba").find(letters[k]);
      if (xyzw == std::string::npos && rgba == std::string::npos) err("bad swizzle ." + letters);
      comps[k] = static_cast<uint8_t>(xyzw != std::string::npos ? xyzw : rgba);
      if (comps[k] >= width(typeOf(a))) err("swizzle ." + letters + " out of range");
    }
    const auto count = static_cast<unsigned>(letters.size());
    if (isConst(a)) {
      const Node& n = b_.nodes()[a];
      float v[4];
      for (unsigned k = 0; k < count; ++k) v[k] = n.value[comps[k]];
      return b_.constant(floatType(count), v);
    }
    return b_.swizzle(a, comps, count);
  }

  uint32_t makeConstruct(const std::vector<uint32_t>& args, unsigned w) {
    unsigned total = 0;
    bool allConst = true;
    for (uint32_t a : args) {
      if (!isFloat(typeOf(a))) err("constructor operands must be float");
      total += width(typeOf(a));
      allConst = allConst && isConst(a);
    }
    if (args.size() == 1 && total == 1) {  // float3(x): broadcast
      if (allConst) {
        const float v = b_.nodes()[args[0]].value[0];
        const float vs[4] = {v, v, v, v};
        return b_.constant(floatType(w), vs);
      }
      const uint8_t comps[4] = {0, 0, 0, 0};
      return b_.swizzle(args[0], comps, w);
    }
    if (total != w)
      err("float" + std::to_string(w) + " constructor needs " + std::to_string(w) + " components");
    if (allConst) {
      float v[4];
      unsigned c = 0;
      for (uint32_t a : args)
        for (unsigned j = 0; j < width(typeOf(a)); ++j) v[c++] = b_.nodes()[a].value[j];
      return b_.constant(floatType(w), v);
    }
    return b_.construct(args.data(), static_cast<unsigned>(args.size()));
  }

  uint32_t ternary() {
    const uint32_t cond = comparison();
    if (!isPunct("?")) return cond;
    ++p_;
    const uint32_t x = ternary();
    expect(":");
    const uint32_t y = ternary();
    if (typeOf(cond) != Type::Bool) err("condition must be a comparison");
    return make(Op::Select, cond, x, y);
  }

  uint32_t comparison() {
    const uint32_t lhs = additive();
    static const std::pair<const char*, Op> ops[] = {
        {"<", Op::Lt}, {"<=", Op::Le}, {">", Op::Gt}, {">=", Op::Ge}, {"==", Op::Eq}, {"!=", Op::Ne}};
    for (const auto& [s, op] : ops) {
      if (isPunct(s)) {
        ++p_;
        return make(op, lhs, additive());
      }
    }
    return lhs;
  }

  uint32_t additive() {
    uint32_t lhs = multiplicative();
    while (isPunct("+") || isPunct("-")) {
      const Op op = peek().text == "+" ? Op::Add : Op::Sub;
      ++p_;
      lhs = make(op, lhs, multiplicative());
    }
    return lhs;
  }

  uint32_t multiplicative() {
    uint32_t lhs = unary();
    while (isPunct("*") || isPunct("/")) {
      const Op op = peek().text == "*" ? Op::Mul : Op::Div;
      ++p_;
      lhs = make(op, lhs, unary());
    }
    return lhs;
  }

  uint32_t unary() {
    if (isPunct("-")) {
      ++p_;
      return make(Op::Neg, unary());
    }
    if (isPunct("+")) {
      ++p_;
      return unary();
    }
    return primary();
  }

  uint32_t primary() {
    uint32_t v = atom();
    while (isPunct(".")) {
      ++p_;
      if (peek().kind != Tok::Ident) err("expected swizzle after '.'");
      v = makeSwizzle(v, peek().text);
      ++p_;
    }
    return v;
  }

  uint32_t atom() {
    const Token tok = peek();
    if (tok.kind == Tok::Num) {
      ++p_;
      return b_.constant(static_cast<float>(tok.num));
    }
    if (isPunct("(")) {
      ++p_;
      const uint32_t v = ternary();
      expect(")");
      return v;
    }
    if (tok.kind != Tok::Ident) err("expected expression");
    ++p_;
    if (isPunct("(")) {
      ++p_;
      std::vector<uint32_t> args;
      if (!isPunct(")")) {
        args.push_back(ternary());
        while (isPunct(",")) {
          ++p_;
          args.push_back(ternary());
        }
      }
      expect(")");
      if (tok.text == "float2" || tok.text == "float3" || tok.text == "float4")
        return makeConstruct(args, static_cast<unsigned>(tok.text[5] - '0'));
      const auto op = opFromCall(tok.text, static_cast<uint8_t>(args.size()));
      if (!op)
        err("unknown function " + tok.text + " with " + std::to_string(args.size()) + " arguments");
      return make(*op, args[0], args.size() > 1 ? args[1] : 0, args.size() > 2 ? args[2] : 0);
    }
    for (uint32_t i = 0; i < inputs_.size(); ++i)
      if (inputs_[i].name == tok.text) return b_.input(i, inputs_[i].type);
    err("unknown identifier " + tok.text);
  }

  const std::vector<Token>& t_;
  size_t p_;
  const std::vector<InputDecl>& inputs_;
  ExprBuilder& b_;
  int line_;
};

double parseSignedNumber(const std::vector<Token>& t, size_t& p, int line) {
  double sign = 1.0;
  if (t[p].kind == Tok::Punct && t[p].text == "-") {
    sign = -1.0;
    ++p;
  }
  if (t[p].kind != Tok::Num) throw ParseError("line " + std::to_string(line) + ": expected number");
  return sign * t[p++].num;
}

}  // namespace

Expr parseExpr(std::string_view text, const std::vector<InputDecl>& inputs) {
  const auto toks = tokenize(text, 0);
  ExprBuilder b;
  ExprParser ep(toks, 0, inputs, b, 0);
  const uint32_t root = ep.parse();
  if (toks[ep.pos()].kind != Tok::End) throw ParseError("trailing tokens in expression");
  return b.finish(root);
}

Program parseProgram(std::string_view text) {
  Program prog;
  std::string exprText;
  int exprLine = 0;
  bool haveBudget = false;

  std::istringstream in{std::string(text)};
  std::string lineStr;
  int line = 0;
  while (std::getline(in, lineStr)) {
    ++line;
    const auto t = tokenize(lineStr, line);
    if (t[0].kind == Tok::End) continue;
    auto err = [&](const std::string& msg) {
      throw ParseError("line " + std::to_string(line) + ": " + msg);
    };
    auto kw = [&](size_t i, std::string_view s) { return t[i].kind != Tok::End && t[i].text == s; };
    if (t[0].kind != Tok::Ident) err("expected statement");

    if (t[0].text == "input") {
      if (!exprText.empty()) err("inputs must be declared before the output");
      // "const": a compile-time constant (a preprocessor definition), folded by the compiler.
      const size_t o = kw(3, "const") ? 1 : 0;
      const bool typeOk = kw(3 + o, "float") || kw(3 + o, "float2") || kw(3 + o, "float3") || kw(3 + o, "float4");
      if (t[1].kind != Tok::Ident || !kw(2, ":") || !typeOk || !kw(4 + o, "in") || !kw(5 + o, "["))
        err("expected: input <name> : [const] float[2|3|4] in [lo, hi] [grid N] [= value]");
      InputDecl d;
      d.name = t[1].text;
      d.compileTime = o == 1;
      d.type = t[3 + o].text == "float" ? Type::Float : floatType(static_cast<unsigned>(t[3 + o].text[5] - '0'));
      size_t p = 6 + o;
      d.lo = parseSignedNumber(t, p, line);
      if (!kw(p, ",")) err("expected ','");
      ++p;
      d.hi = parseSignedNumber(t, p, line);
      if (!kw(p, "]")) err("expected ']'");
      ++p;
      if (!(d.lo <= d.hi)) err("empty interval");
      if (kw(p, "grid")) {
        ++p;
        if (t[p].kind != Tok::Num || t[p].num < 1) err("grid needs a positive integer");
        d.grid = static_cast<uint32_t>(t[p].num);
        ++p;
      }
      if (kw(p, "=")) {  // current value of a const input
        ++p;
        if (!d.compileTime) err("only const inputs have a value");
        d.value = parseSignedNumber(t, p, line);
      } else if (d.compileTime) {
        d.value = 0.5 * (d.lo + d.hi);
      }
      if (t[p].kind != Tok::End) err("trailing tokens");
      for (const auto& other : prog.inputs)
        if (other.name == d.name) err("duplicate input " + d.name);
      prog.inputs.push_back(d);
    } else if (t[0].text == "output") {
      if (!exprText.empty()) err("only one output is supported in M1");
      if (t[1].kind != Tok::Ident || !kw(2, "=")) err("expected: output <name> = <expr>");
      prog.outputName = t[1].text;
      const auto eq = lineStr.find('=');
      exprText = lineStr.substr(eq + 1);
      const auto hash = exprText.find('#');
      if (hash != std::string::npos) exprText.resize(hash);
      exprLine = line;
    } else if (t[0].text == "budget") {
      if (exprText.empty()) err("budget must come after the output");
      if (t[1].kind != Tok::Ident || !kw(2, ":")) err("expected: budget <name> : <kind>");
      if (t[1].text != prog.outputName) err("budget refers to unknown output " + t[1].text);
      size_t p = 4;
      if (kw(3, "exact") || kw(3, "condition") || kw(3, "temporal") || kw(3, "depth")) {
        prog.budget.kind = Budget::Kind::Exact;  // design 4.2: these uses are bit-exact
      } else if (kw(3, "texcoord")) {
        prog.budget.kind = Budget::Kind::Texcoord;
        prog.budget.px = 0.25;
        if (t[4].kind == Tok::Num) {
          prog.budget.px = t[4].num;
          p = 5;
        }
        prog.budget.eps = prog.budget.px / 3840.0;
      } else if (kw(3, "color8") || kw(3, "color10")) {
        prog.budget.kind = kw(3, "color8") ? Budget::Kind::Color8 : Budget::Kind::Color10;
        if (kw(4, "maxdiff")) {
          if (t[5].kind != Tok::Num) err("maxdiff needs a number");
          prog.budget.maxCodeDiff = static_cast<int>(t[5].num);
          p = 6;
        }
      } else if (kw(3, "abs") || kw(3, "rel")) {
        prog.budget.kind = kw(3, "abs") ? Budget::Kind::Abs : Budget::Kind::Rel;
        if (t[4].kind != Tok::Num) err("expected tolerance");
        prog.budget.eps = t[4].num;
        p = 5;
      } else {
        err("budget kind must be exact, condition, temporal, depth, color8, color10, "
            "texcoord [px], abs <eps> or rel <eps>");
      }
      if (t[p].kind != Tok::End) err("trailing tokens");
      haveBudget = true;
    } else {
      err("unknown statement " + t[0].text);
    }
  }
  (void)haveBudget;
  if (prog.inputs.empty()) throw ParseError("no inputs declared");
  if (exprText.empty()) throw ParseError("no output declared");

  const auto toks = tokenize(exprText, exprLine);
  ExprBuilder b;
  ExprParser ep(toks, 0, prog.inputs, b, exprLine);
  const uint32_t root = ep.parse();
  if (toks[ep.pos()].kind != Tok::End)
    throw ParseError("line " + std::to_string(exprLine) + ": trailing tokens in expression");
  if (!isFloat(b.nodes()[root].type))
    throw ParseError("line " + std::to_string(exprLine) + ": output must be float or floatN");
  prog.target = b.finish(root);
  return prog;
}

std::string readExpect(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::string line;
  while (std::getline(f, line)) {
    const auto pos = line.find("# expect:");
    if (pos == std::string::npos) continue;
    std::string s = line.substr(pos + 9);
    const auto b = s.find_first_not_of(" \t");
    const auto e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
  }
  return {};
}

Program loadProgram(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw ParseError("cannot open " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return parseProgram(ss.str());
}

}  // namespace sopt
