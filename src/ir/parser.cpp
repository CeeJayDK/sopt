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
    if (std::string_view("+-*/()<>?:,=[]").find(ch) != std::string_view::npos) {
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

  Type typeOf(uint32_t n) const { return info(b_.nodes()[n].op).result; }
  void want(uint32_t n, Type ty, const char* what) const {
    if (typeOf(n) != ty)
      err(std::string(what) + (ty == Type::Bool ? " must be a comparison" : " must be float"));
  }

  // Builds an op node, folding it if all operands are constants.
  uint32_t make(Op op, uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    const auto& oi = info(op);
    uint32_t args[3] = {a, b, c};
    for (uint8_t k = 0; k < oi.arity; ++k) want(args[k], oi.args[k], "operand");
    bool allConst = true;
    for (uint8_t k = 0; k < oi.arity; ++k)
      allConst = allConst && b_.nodes()[args[k]].op == Op::Const;
    if (allConst && oi.result == Type::Float) {
      const auto& ns = b_.nodes();
      const float v = evalScalar(op, ns[a].value, oi.arity > 1 ? ns[b].value : 0.0f,
                                 oi.arity > 2 ? ns[c].value : 0.0f, kProfileRef);
      return b_.constant(v);
    }
    return b_.op(op, a, b, c);
  }

  uint32_t ternary() {
    const uint32_t cond = comparison();
    if (!isPunct("?")) return cond;
    ++p_;
    const uint32_t x = ternary();
    expect(":");
    const uint32_t y = ternary();
    want(cond, Type::Bool, "condition");
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
      const auto op = opFromCall(tok.text, static_cast<uint8_t>(args.size()));
      if (!op)
        err("unknown function " + tok.text + " with " + std::to_string(args.size()) + " arguments");
      return make(*op, args[0], args.size() > 1 ? args[1] : 0, args.size() > 2 ? args[2] : 0);
    }
    for (uint32_t i = 0; i < inputs_.size(); ++i)
      if (inputs_[i].name == tok.text) return b_.input(i);
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
      if (t[1].kind != Tok::Ident || !kw(2, ":") || !kw(3, "float") || !kw(4, "in") || !kw(5, "["))
        err("expected: input <name> : float in [lo, hi] [grid N]");
      InputDecl d;
      d.name = t[1].text;
      size_t p = 6;
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
      if (kw(3, "exact")) {
        prog.budget.kind = Budget::Kind::Exact;
      } else if (kw(3, "color8")) {
        prog.budget.kind = Budget::Kind::Color8;
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
        err("budget kind must be exact, color8, abs <eps> or rel <eps>");
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
  if (info(b.nodes()[root].op).result != Type::Float)
    throw ParseError("line " + std::to_string(exprLine) + ": output must be float");
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
