#include "fx/classic.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>

#include "fx/codegen.hpp"

namespace sopt::fx {
namespace source {

bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

size_t skipSpace(const std::string& s, size_t p) {
  for (;;) {
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
    if (s.compare(p, 2, "//") == 0) {
      while (p < s.size() && s[p] != '\n') ++p;
    } else if (s.compare(p, 2, "/*") == 0) {
      const size_t e = s.find("*/", p + 2);
      p = e == std::string::npos ? s.size() : e + 2;
    } else {
      return p;
    }
  }
}

size_t matching(const std::string& s, size_t p) {
  int depth = 0;
  for (size_t i = p; i < s.size(); ++i) {
    if (s.compare(i, 2, "//") == 0 || s.compare(i, 2, "/*") == 0) {
      i = skipSpace(s, i) - 1;
      continue;
    }
    const char c = s[i];
    if (c == '(' || c == '[' || c == '{') ++depth;
    else if (c == ')' || c == ']' || c == '}') {
      if (--depth == 0) return i;
    }
  }
  return std::string::npos;
}

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

size_t nameEnd(const std::string& s, size_t p) {
  while (p < s.size() && (identChar(s[p]) || (s[p] == ':' && p + 1 < s.size() && s[p + 1] == ':')))
    p += s[p] == ':' ? 2 : 1;
  return p;
}

Text joinLines(const std::vector<std::string>& lines) {
  Text t;
  for (const auto& l : lines) {
    t.starts.push_back(t.s.size());
    t.s += l;
    t.s += '\n';
  }
  return t;
}

}  // namespace source

namespace {

using source::identChar;
using source::joinLines;
using source::matching;
using source::skipSpace;
using source::Text;
using source::trim;

std::string zeroOf(const reshadefx::type& t, const std::string& typeName) {
  const char* z = t.is_floating_point() ? "0.0" : t.is_boolean() ? "false" : t.is_signed() ? "0" : "0u";
  if (t.rows <= 1) return z;
  std::string s = typeName + "(";
  for (unsigned k = 0; k < t.rows; ++k) s += (k ? ", " : "") + std::string(z);
  return s + ")";
}

std::optional<SourceRewrite> tableRewrite(const Codegen& cg, const Function& f, const Statement& init) {
  const auto vit = cg.variables.find(init.var);
  if (vit == cg.variables.end()) return std::nullopt;
  const Variable& var = vit->second;
  const reshadefx::type& t = var.type;
  if (var.kind != Variable::Kind::Local || !t.is_bounded_array() || t.is_struct() || t.cols > 1 || !t.is_numeric())
    return std::nullopt;
  const auto ival = cg.values.find(init.value);
  if (ival == cg.values.end()) return std::nullopt;
  // All constants: the parser folds the initializer into one constant.
  const bool allConst = ival->second.kind == Value::Kind::Const;
  if (!allConst && (ival->second.kind != Value::Kind::Construct || ival->second.args.size() != t.array_length ||
                    ival->second.argConst.size() != t.array_length))
    return std::nullopt;
  const std::vector<bool> isConst = allConst ? std::vector<bool>(t.array_length, true) : ival->second.argConst;
  const size_t consts = static_cast<size_t>(std::count(isConst.begin(), isConst.end(), true));
  const size_t others = isConst.size() - consts;
  if (consts < 2 || others > 2) return std::nullopt;

  // Read only by index, at least once at run time; never written after the initializer.
  size_t loads = 0;
  bool dynamic = false;
  for (const auto& [id, v] : cg.values) {
    if (v.kind != Value::Kind::Load || v.base != init.var) continue;
    if (v.chain.empty() || (v.chain[0].op != reshadefx::expression::operation::op_dynamic_index &&
                            v.chain[0].op != reshadefx::expression::operation::op_constant_index))
      return std::nullopt;
    dynamic = dynamic || v.chain[0].op == reshadefx::expression::operation::op_dynamic_index;
    ++loads;
  }
  for (const auto& s : f.stmts)
    if (s.kind == Statement::Kind::Store && s.var == init.var) return std::nullopt;
  if (!dynamic) return std::nullopt;  // constant indices: the compiler folds them anyway

  const std::string file = var.loc.source;
  if (file.empty() || f.loc.source != file) return std::nullopt;
  const std::vector<std::string>* lines = sourceLines(file);
  if (!lines || var.loc.line == 0 || var.loc.line > lines->size() || f.loc.line == 0 || f.loc.line >= var.loc.line)
    return std::nullopt;
  const Text text = joinLines(*lines);
  const std::string& s = text.s;

  // The declaration: "<type> name[N] = { ... };" alone on its lines.
  const std::string& declText = (*lines)[var.loc.line - 1];
  const size_t col = declText.find(var.name);
  if (col == std::string::npos) return std::nullopt;
  std::string typeName = trim(declText.substr(0, col));
  if (typeName.rfind("const ", 0) == 0) typeName = trim(typeName.substr(6));  // a local const array
  if (typeName.empty() || !std::all_of(typeName.begin(), typeName.end(), identChar)) return std::nullopt;
  const std::string indent = declText.substr(0, declText.find_first_not_of(" \t"));
  size_t p = text.starts[var.loc.line - 1] + col + var.name.size();
  p = skipSpace(s, p);
  if (p >= s.size() || s[p] != '[') return std::nullopt;
  const size_t lenEnd = matching(s, p);
  if (lenEnd == std::string::npos) return std::nullopt;
  const std::string lenText = s.substr(p, lenEnd + 1 - p);  // "[18]"
  p = skipSpace(s, lenEnd + 1);
  if (p >= s.size() || s[p] != '=') return std::nullopt;
  const size_t open = skipSpace(s, p + 1);
  if (open >= s.size() || s[open] != '{') return std::nullopt;
  const size_t close = matching(s, open);
  if (close == std::string::npos) return std::nullopt;
  const size_t semi = skipSpace(s, close + 1);
  if (semi >= s.size() || s[semi] != ';') return std::nullopt;
  const uint32_t declLine = var.loc.line, endLine = text.lineOf(semi);
  {
    const size_t after = skipSpace(s, semi + 1);
    if (after < s.size() && text.lineOf(after) == endLine) return std::nullopt;  // more code on the line
  }

  // The elements: [start, end) of each expression in the braces.
  std::vector<std::pair<size_t, size_t>> elems;
  {
    size_t q = open + 1;
    while (q < close) {
      const size_t a = skipSpace(s, q);
      if (a >= close) break;
      size_t b = a;
      int depth = 0;
      while (b < close) {
        if (s.compare(b, 2, "//") == 0 || s.compare(b, 2, "/*") == 0) {
          b = skipSpace(s, b);
          continue;
        }
        const char c = s[b];
        if (c == '(' || c == '[' || c == '{') ++depth;
        else if (c == ')' || c == ']' || c == '}') --depth;
        else if (c == ',' && depth == 0) break;
        ++b;
      }
      size_t e = b;  // trim trailing space / comments of the element
      while (e > a && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
      elems.push_back({a, e});
      q = b + 1;
    }
  }
  if (elems.size() != t.array_length) return std::nullopt;

  // The scope: up to the brace that closes the block holding the declaration.
  size_t scopeEnd = std::string::npos;
  {
    int depth = 0;
    for (size_t i = semi + 1; i < s.size(); ++i) {
      if (s.compare(i, 2, "//") == 0 || s.compare(i, 2, "/*") == 0) {
        i = skipSpace(s, i) - 1;
        continue;
      }
      if (s[i] == '{') ++depth;
      else if (s[i] == '}' && depth-- == 0) {
        scopeEnd = i;
        break;
      }
    }
  }
  if (scopeEnd == std::string::npos) return std::nullopt;

  SourceRewrite r;
  r.file = file;
  r.line = declLine;
  r.function = f.name;
  r.kind = "static const table";
  const std::string table = "sopt_" + f.name + "_" + var.name;
  std::vector<std::pair<size_t, std::string>> otherEntries;  // (index, expression)
  for (size_t k = 0; k < elems.size(); ++k)
    if (!isConst[k]) otherEntries.push_back({k, s.substr(elems[k].first, elems[k].second - elems[k].first)});

  // Uses name[i] in the scope (the same number as the loads), rewritten line by line.
  std::map<uint32_t, std::string> useLines;
  size_t uses = 0;
  for (size_t i = semi + 1; i < scopeEnd; ++i) {
    if (s.compare(i, 2, "//") == 0 || s.compare(i, 2, "/*") == 0) {
      i = skipSpace(s, i) - 1;
      continue;
    }
    if (s.compare(i, var.name.size(), var.name) != 0 || (i > 0 && identChar(s[i - 1])) ||
        identChar(s[i + var.name.size()]))
      continue;
    const size_t b = skipSpace(s, i + var.name.size());
    if (s[b] != '[') return std::nullopt;  // the array used whole
    const size_t e = matching(s, b);
    if (e == std::string::npos || text.lineOf(e) != text.lineOf(i)) return std::nullopt;
    const std::string idx = trim(s.substr(b + 1, e - b - 1));
    for (size_t c = 0; c < idx.size(); ++c) {  // side effects would run twice in the selects
      const bool assign = idx[c] == '=' && (c == 0 || std::string("=<>!").find(idx[c - 1]) == std::string::npos) &&
                          (c + 1 >= idx.size() || idx[c + 1] != '=');
      if (assign || idx.compare(c, 2, "++") == 0 || idx.compare(c, 2, "--") == 0) return std::nullopt;
    }
    std::string repl = table + "[" + idx + "]";
    for (auto it = otherEntries.rbegin(); it != otherEntries.rend(); ++it)
      repl = "(" + idx + " == " + std::to_string(it->first) + " ? " + it->second + " : " + repl + ")";
    const uint32_t ln = text.lineOf(i);
    std::string& line = useLines.try_emplace(ln, (*lines)[ln - 1]).first->second;
    // Positions shift as earlier uses on the line are replaced: search from the end of the line.
    const size_t lineStart = text.starts[ln - 1];
    const size_t offFromEnd = (lineStart + (*lines)[ln - 1].size()) - (e + 1);
    const size_t endInLine = line.size() - offFromEnd;
    const size_t startInLine = endInLine - (e + 1 - i);
    line.replace(startInLine, e + 1 - i, repl);
    ++uses;
    i = e;
  }
  if (uses != loads) return std::nullopt;

  // The function's first line holds its return type and name: the table goes before it.
  const std::string& head = (*lines)[f.loc.line - 1];
  const size_t fn = head.find(f.name);
  if (fn == std::string::npos || trim(head.substr(0, fn)).empty()) return std::nullopt;

  std::string tableText = "static const " + typeName + " " + table + lenText + " =";
  {
    std::string body = s.substr(open, close + 1 - open);
    for (auto it = otherEntries.rbegin(); it != otherEntries.rend(); ++it) {
      const auto [a, e] = elems[it->first];
      body.replace(a - open, e - a, zeroOf(t, typeName) + " /* " + it->second + ": picked at the use */");
    }
    // The braces may start on the next line.
    tableText += (s.find('\n', p) < open ? "\n" + indent : " ") + body + ";";
  }
  LineEdit insert{f.loc.line, f.loc.line, {}};
  for (size_t a = 0; a <= tableText.size();) {
    size_t b = tableText.find('\n', a);
    if (b == std::string::npos) b = tableText.size();
    std::string l = tableText.substr(a, b - a);
    if (l.compare(0, indent.size(), indent) == 0) l = l.substr(indent.size());
    insert.lines.push_back(l);
    a = b + 1;
  }
  insert.lines.push_back("");
  insert.lines.push_back(head);
  r.edits.push_back(std::move(insert));
  r.edits.push_back({declLine, endLine, {}});
  for (auto& [ln, l] : useLines) r.edits.push_back({ln, ln, {l}});
  r.otherEntries = static_cast<uint32_t>(others);
  r.description = "local array " + var.name + lenText + " (" + std::to_string(consts) + " constants" +
                  (others ? ", " + std::to_string(others) + " other" : "") + ") as a static const table";
  return r;
}

}  // namespace

std::string rewriteSwitch(const SourceRewrite& r) {
  std::string stem = pathFrom(r.file).stem().string();
  for (auto& c : stem)
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  return "SOPT_" + stem + "_" + r.tag + std::to_string(r.line);
}

std::vector<SourceRewrite> tableRewrites(const Effect& fx) {
  std::vector<SourceRewrite> out;
  if (!fx.cg) return out;
  for (const auto& f : fx.cg->functions)
    for (const auto& s : f->stmts)
      if (s.kind == Statement::Kind::Init)
        if (auto r = tableRewrite(*fx.cg, *f, s)) out.push_back(std::move(*r));
  return out;
}

}  // namespace sopt::fx
