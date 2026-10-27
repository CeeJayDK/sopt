#include "fx/frontend.hpp"

#include <algorithm>
#include <regex>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
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

// GLSL function prototypes (vec4 f(vec4 c);) with the definition further down, as WebRender's ps_quad declares
// pattern_fragment for the shader that includes it: ReShade FX has no prototypes. The prototype is dropped and every
// function between it and the definition that calls it (ps_quad's main) moves to just after the definition, with
// #line markers so its lines keep their numbers. Works on the preprocessed text (one function signature per line).
std::string glslMoveCallers(std::string text) {
  struct Fn {
    std::string name;
    size_t begin, end;  // the signature's line start, one past the closing '}' (or the ';' of a prototype)
    bool proto;
  };
  const auto scan = [&]() {
    std::vector<Fn> fns;
    static const std::regex sig(R"(^[ \t]*[A-Za-z_][\w\[\] \t]*[ \t]([A-Za-z_]\w*)[ \t]*\()");
    int depth = 0;
    for (size_t i = 0; i < text.size();) {
      const size_t eol = std::min(text.find('\n', i), text.size());
      std::smatch m;
      const std::string line = text.substr(i, eol - i);
      if (depth == 0 && std::regex_search(line, m, sig) && line.find('=') == std::string::npos) {
        // The parameter list, then ';' (a prototype) or '{' (a definition).
        size_t p = i + m.position(0) + m.length(0), paren = 1;
        for (; p < text.size() && paren > 0; ++p) paren += text[p] == '(' ? 1 : text[p] == ')' ? -1 : 0;
        while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
        if (p < text.size() && text[p] == ';') {
          fns.push_back({m[1], i, p + 1, true});
        } else if (p < text.size() && text[p] == '{') {
          int d = 0;
          size_t q = p;
          for (; q < text.size(); ++q)
            if (text[q] == '{') ++d;
            else if (text[q] == '}' && --d == 0) break;
          fns.push_back({m[1], i, q + 1, false});
          i = std::min(text.find('\n', q), text.size());
          if (i < text.size()) ++i;
          continue;
        }
      }
      for (char c : line) depth += c == '{' ? 1 : c == '}' ? -1 : 0;
      i = eol + 1;
    }
    return fns;
  };
  // "#line N \"file\"" for the text that starts at 'at'.
  const auto lineAt = [&](size_t at) {
    long line = 1;
    std::string file;
    static const std::regex dir(R"(^#line (\d+)( "[^"]*")?)");
    for (size_t i = 0; i < at;) {
      const size_t eol = std::min(text.find('\n', i), text.size());
      std::smatch m;
      const std::string l = text.substr(i, eol - i);
      if (std::regex_search(l, m, dir)) {
        line = std::stol(m[1]);
        if (m[2].matched) file = m[2];
      } else {
        ++line;
      }
      i = eol + 1;
    }
    return "#line " + std::to_string(line) + file + "\n";
  };
  for (int round = 0; round < 8; ++round) {
    const std::vector<Fn> fns = scan();
    bool moved = false;
    for (const Fn& proto : fns) {
      if (!proto.proto) continue;
      const auto def = std::find_if(fns.begin(), fns.end(), [&](const Fn& f) { return !f.proto && f.name == proto.name && f.begin > proto.begin; });
      if (def == fns.end()) continue;
      const std::regex call("\\b" + proto.name + "\\s*\\(");
      std::vector<const Fn*> callers;
      for (const Fn& f : fns)
        if (!f.proto && f.begin > proto.end && f.end <= def->begin && std::regex_search(text.begin() + f.begin, text.begin() + f.end, call))
          callers.push_back(&f);
      // After the definition (at the next line), each moved block with its own line numbers, then the old position.
      size_t after = std::min(text.find('\n', def->end), text.size());
      if (after < text.size()) ++after;
      std::string block;
      for (const Fn* f : callers) block += lineAt(f->begin) + text.substr(f->begin, f->end - f->begin) + "\n";
      if (!block.empty()) block += lineAt(after);
      text.insert(after, block);
      // Blank out the callers and the prototype (newlines kept, so the lines that follow keep their numbers).
      auto blank = [&](size_t b, size_t e) {
        for (size_t k = b; k < e; ++k)
          if (text[k] != '\n') text[k] = ' ';
      };
      for (const Fn* f : callers) blank(f->begin, f->end);
      blank(proto.begin, proto.end);
      moved = true;
      break;  // positions changed: scan again
    }
    if (!moved) break;
  }
  return text;
}

}  // namespace

std::vector<fs::path> folderIncludePaths(const fs::path& file) {
  std::vector<fs::path> dirs;
  // The folders above the file (owner, 2026-10-19: no setup for ReShade.fxh): an effect in
  // reshade-shaders\Shaders\SweetFX finds ReShade.fxh in Shaders. Up to a reshade-shaders folder or the drive root.
  fs::path shaders;  // the reshade-shaders\Shaders folder above the file, if any
  for (fs::path dir = fs::absolute(file).parent_path(); dir.has_relative_path() && dir.filename() != "reshade-shaders";) {
    if (dir.filename() == "Shaders" && dir.parent_path().filename() == "reshade-shaders") shaders = dir;
    dir = dir.parent_path();
    dirs.push_back(dir);
  }
  // ReShade's default search path is reshade-shaders\Shaders\** and it adds every folder below it as an include path
  // (runtime.cpp, sorted): BFBFX's AO.fx includes "random.fxh" from BFBFX\bfb_inc.
  if (!shaders.empty()) {
    std::error_code ec;
    std::set<fs::path> sub;
    for (auto it = fs::recursive_directory_iterator(shaders, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
      if (it->is_directory(ec)) sub.insert(it->path());
    dirs.insert(dirs.end(), sub.begin(), sub.end());
  }
  return dirs;
}

std::unique_ptr<Effect> loadEffect(const fs::path& path, const LoadOptions& opt,
                                   std::string& errors) {
  reshadefx::preprocessor pp;
  pp.add_include_path(path.parent_path());
  for (const auto& p : opt.includePaths) pp.add_include_path(p);
  for (const auto& p : folderIncludePaths(path)) pp.add_include_path(p);
  // Given macros first: the preprocessor keeps a name's first definition, so they override the defaults below
  // (e.g. __VENDOR__ / __RENDERER__ to check per-vendor rewrites).
  for (const auto& [k, v] : opt.macros) pp.add_macro_definition(k, v);
  // As ReShade 6 defines them (runtime.cpp), D3D11 renderer, SDR 8-bit back buffer (not
  // for plain HLSL).
  if (!opt.hlsl && !opt.glsl) {
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
  }
  pp.symbolic_macros.insert(opt.symbolic.begin(), opt.symbolic.end());
  std::string decls;
  for (const auto& m : opt.symbolic) decls += "uniform float " + std::string(kSymbolicPrefix) + m + ";\n";
  if (opt.bufferSymbolic) {
    // int, as the literals ReShade defines: BUFFER_WIDTH / 3 stays an integer division.
    for (const char* m : {"BUFFER_WIDTH", "BUFFER_HEIGHT"}) {
      pp.symbolic_macros.insert(m);
      decls += "uniform int " + std::string(kSymbolicPrefix) + m + ";\n";
    }
    pp.symbolic_exclude.insert(opt.symbolicExclude.begin(), opt.symbolicExclude.end());
  }
  if (!decls.empty()) pp.append_string(decls);
  pp.sopt_glsl = opt.glsl;
  // gl_FragCoord in functions other than main (WebRender's helpers): a global there; main has it as its parameter.
  if (opt.glsl) pp.append_string("static float4 gl_FragCoord;\n");
  if (!opt.hlsl && !opt.glsl)
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
  parser.sopt_named_expressions = opt.bufferSymbolic && opt.namedExpressions;
  parser.sopt_hlsl = opt.hlsl && !opt.glsl;
  parser.sopt_glsl = opt.glsl;
  parser.sopt_hlsl_entry = opt.entry;
  const std::string source = opt.glsl ? glslMoveCallers(pp.output()) : pp.output();
  if (!parser.parse(source, fx->cg.get())) {
    errors = pp.errors() + parser.errors();
    return nullptr;
  }
  mapLines(source, fx->ppLines);
  for (const auto& [k, v] : pp.used_macro_definitions()) fx->userMacros[k] = v;
  fx->symbolic = opt.symbolic;
  fx->width = opt.width;
  fx->height = opt.height;
  fx->bufferSymbolic = opt.bufferSymbolic;
  fx->hlsl = opt.hlsl || opt.glsl;
  fx->glsl = opt.glsl;
  fx->hlslFetchNames.insert(parser.sopt_hlsl_fetch_names.begin(), parser.sopt_hlsl_fetch_names.end());
  fx->hlslBuffers.insert(parser.sopt_hlsl_buffer_names.begin(), parser.sopt_hlsl_buffer_names.end());
  for (const auto& [name, m] : pp.sopt_macros()) {
    if (!m.is_function_like) {
      fx->objectMacros[name] = m.replacement_list;
      continue;
    }
    // Parameters are encoded as '\0' '\xFD' <index>; '\xFE' (#) and '\xFF' (##) are left out.
    std::string body;
    bool ok = !m.is_variadic;
    for (size_t i = 0; ok && i < m.replacement_list.size(); ++i) {
      if (m.replacement_list[i] != '\0') { body += m.replacement_list[i]; continue; }
      const size_t idx = i + 2 < m.replacement_list.size() ? static_cast<unsigned char>(m.replacement_list[i + 2]) : 0;
      ok = i + 2 < m.replacement_list.size() && m.replacement_list[i + 1] == '\xFD' && idx < m.parameters.size();
      if (ok) body += " " + m.parameters[idx] + " ";
      i += 2;
    }
    if (ok) fx->functionMacros[name] = {m.parameters, body};
  }
  fx->sourceFiles.push_back(pathString(path));
  for (const auto& f : pp.included_files()) fx->sourceFiles.push_back(pathString(f));
  return fx;
}

std::unique_ptr<Effect> loadEffectBufferSymbolic(const fs::path& path, LoadOptions& opt,
                                                 std::string& errors) {
  opt.bufferSymbolic = true;
  // With named expressions first; a named expression used where a constant is needed
  // (an array size) cannot be excluded line by line, so then once more without them.
  for (bool named : {true, false}) {
    opt.namedExpressions = named;
    opt.symbolicExclude.clear();
    for (int attempt = 0; attempt < 256; ++attempt) {
      errors.clear();
      if (auto fx = loadEffect(path, opt, errors)) return fx;
      // "file(line, col): error ..." lines: expand the macros there to their value.
      bool added = false;
      std::istringstream in(errors);
      for (std::string l; std::getline(in, l);) {
        const size_t e = l.find("): error");
        const size_t p = e == std::string::npos ? e : l.rfind('(', e);
        if (p == std::string::npos) continue;
        const unsigned long line = std::strtoul(l.c_str() + p + 1, nullptr, 10);
        added = opt.symbolicExclude.insert(l.substr(0, p) + '\n' + std::to_string(line)).second || added;
      }
      if (!added) break;
    }
  }
  opt.bufferSymbolic = false;
  opt.namedExpressions = false;
  opt.symbolicExclude.clear();
  return nullptr;
}

std::set<std::string> symbolicMacros(const fs::path& path, const LoadOptions& opt,
                                     const Effect& plain) {
  auto numeric = [](const std::string& v) {
    std::string t = v;
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    if (!t.empty() && (t.back() == 'f' || t.back() == 'F')) t.pop_back();
    char* end = nullptr;
    std::strtod(t.c_str(), &end);
    return !t.empty() && end && *end == 0;
  };
  // Each alone, then together (greedily, in name order).
  std::set<std::string> ok;
  for (const auto& [name, value] : plain.userMacros) {
    if (!numeric(value)) continue;
    LoadOptions o = opt;
    o.symbolic = ok;
    o.symbolic.insert(name);
    std::string err;
    if (loadEffect(path, o, err)) ok.insert(name);
  }
  return ok;
}

std::string textureFactKey(const Effect& fx, const std::string& texture) {
  // GLSL: sampler2D X is parsed as texture __sopt_tex_X plus sampler X; the key uses X.
  std::string name = texture;
  if (const size_t p = name.find("__sopt_tex_"); fx.glsl && p != std::string::npos) name.erase(0, p + 11);
  return fx.path.filename().string() + (fx.hlslBuffers.count(name) ? " buffer " : " texture ") + name;
}

// A global's source name from its unique name ("V__tile" -> "tile", "VNs__tile" -> "Ns::tile").
std::string globalSourceName(const std::string& unique) {
  std::string n = unique.rfind("V", 0) == 0 ? unique.substr(1) : unique;
  if (n.rfind("__", 0) == 0) n.erase(0, 2);
  std::string out;
  for (size_t i = 0; i < n.size(); ++i) {
    if (n[i] == '_' && i + 1 < n.size() && n[i + 1] == '_') { out += "::"; ++i; }
    else out += n[i];
  }
  return out;
}

std::string groupsharedFactKey(const Effect& fx, const std::string& name) {
  return fx.path.filename().string() + " groupshared " + name;
}

std::vector<std::string> groupsharedVariables(const Effect& fx) {
  std::set<std::string> out;
  for (const auto& [id, v] : fx.cg->variables)
    if (v.kind == Variable::Kind::Global && v.type.has(reshadefx::type::q_groupshared) && v.type.is_floating_point())
      out.insert(globalSourceName(v.uniqueName));
  return {out.begin(), out.end()};
}

std::vector<std::string> unrangedTextures(const Effect& fx) {
  using F = reshadefx::texture_format;
  std::set<std::string> out;
  for (const auto& [id, s] : fx.cg->samplers) {
    std::string sem = s.textureSemantic;
    for (char& c : sem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (sem == "COLOR" || sem == "SV_TARGET" || sem == "DEPTH") continue;
    switch (s.format) {
      case F::r8: case F::rg8: case F::rgba8: case F::rgb10a2:
      case F::r16: case F::rg16: case F::rgba16: continue;
      case F::r32i: case F::r32u: case F::rgba32i: case F::rgba32u: continue;  // integer reads end regions
      default: out.insert(s.textureName);
    }
  }
  // HLSL RW textures and buffers (storage objects named like the resource) with float elements.
  for (const auto& st : fx.cg->mod().storages)
    if (fx.hlslFetchNames.count(st.name) && st.type.is_floating_point()) out.insert(st.name);
  return {out.begin(), out.end()};
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
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {  // to the line's end: statements span lines
      const size_t e = s.find('\n', i);
      i = e == std::string::npos ? n : e;
      continue;
    }
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

// Tokens with member accesses split off ("c.rgb.x" -> "c" ".rgb" ".x"), so text with and without spaces before a
// swizzle compares equal (a macro argument followed by .x in its body).
std::vector<std::string> splitMembers(const std::vector<std::string>& in) {
  std::vector<std::string> out;
  for (const std::string& t : in) {
    if (std::isdigit(static_cast<unsigned char>(t[0])) || (t[0] == '.' && t.size() > 1 && std::isdigit(static_cast<unsigned char>(t[1])))) {
      out.push_back(t);  // a number
      continue;
    }
    size_t a = 0;
    for (size_t d = t.find('.', 1); d != std::string::npos; d = t.find('.', d + 1)) {
      out.push_back(t.substr(a, d - a));
      a = d;
    }
    out.push_back(t.substr(a));
  }
  return out;
}
std::vector<std::string> memberTokens(const std::string& s) { return splitMembers(tokens(s)); }

// Index of the bracket closing the one at `open` ('(' or '['), or npos.
size_t closeOf(const std::string& t, size_t open) {
  int depth = 0;
  for (size_t k = open; k < t.size(); ++k) {
    if (t[k] == '(' || t[k] == '[') ++depth;
    if ((t[k] == ')' || t[k] == ']') && --depth == 0) return k;
  }
  return std::string::npos;
}

// The '(' of a texture fetch call at i: ReShade FX "tex2D(", "tex2Dlod (", ... or an HLSL
// texture method call "tex.Sample(", "gColor.SampleLevel (", ...; npos if there is none.
// Plain HLSL resources whose elements are read as Name[index] (set while extracting from an
// HLSL effect, see Extractor): such reads are fetches too.
thread_local const std::set<std::string>* tlBracketFetches = nullptr;
// GLSL texture lookups (texture(s, uv), texelFetch(...), ...): set while extracting from GLSL.
thread_local bool tlGlslFetches = false;
// Function-like macros whose whole body is one texture fetch (getColor(uv) = tex2Dlod(...)): set while extracting.
thread_local const std::set<std::string>* tlMacroFetches = nullptr;

size_t fetchOpen(const std::string& t, size_t i) {
  if (i >= t.size() || (i && isIdent(t[i - 1])) || !isIdent(t[i]) || std::isdigit(static_cast<unsigned char>(t[i])))
    return std::string::npos;
  size_t j = i;
  while (j < t.size() && isIdent(t[j])) ++j;
  if (tlMacroFetches && tlMacroFetches->count(t.substr(i, j - i))) {
    size_t k = j;
    while (k < t.size() && (t[k] == ' ' || t[k] == '\t')) ++k;
    return k < t.size() && t[k] == '(' ? k : std::string::npos;
  }
  const bool fx = j - i >= 5 && t.compare(i, 3, "tex") == 0 && std::isdigit(static_cast<unsigned char>(t[i + 3])) &&
                  t[i + 4] == 'D' && t.compare(i + 5, 5, "store") != 0;
  if (!fx && tlGlslFetches) {
    static const std::set<std::string> kGlsl = {
        "texture", "texture2D", "texture3D", "textureLod", "textureGrad", "texelFetch", "textureOffset",
        "texelFetchOffset", "textureLodOffset", "textureGradOffset", "textureGather", "textureGatherOffset",
        "texture2DLod", "textureProj"};
    if (kGlsl.count(t.substr(i, j - i))) {
      size_t k = j;
      while (k < t.size() && (t[k] == ' ' || t[k] == '\t')) ++k;
      return k < t.size() && t[k] == '(' ? k : std::string::npos;
    }
  }
  if (!fx && tlBracketFetches && tlBracketFetches->count(t.substr(i, j - i))) {
    size_t k = j;
    while (k < t.size() && (t[k] == ' ' || t[k] == '\t')) ++k;
    if (k < t.size() && t[k] == '[') return k;
  }
  if (!fx) {
    if (j >= t.size() || t[j] != '.') return std::string::npos;
    size_t m = ++j;
    while (m < t.size() && isIdent(t[m])) ++m;
    static const std::set<std::string> kMethods = {
        "Sample", "SampleLevel", "SampleBias", "SampleGrad", "SampleCmp", "SampleCmpLevelZero",
        "Load", "Gather", "GatherRed", "GatherGreen", "GatherBlue", "GatherAlpha"};
    if (!kMethods.count(t.substr(j, m - j))) return std::string::npos;
    j = m;
  }
  while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
  return j < t.size() && t[j] == '(' ? j : std::string::npos;
}

bool fetchAt(const std::string& t, size_t i) { return fetchOpen(t, i) != std::string::npos; }

// The text with every outermost texture fetch call replaced by "tex_fetch": fetches
// are region inputs copied verbatim, so macros inside them do not matter.
std::string withoutFetches(const std::string& t) {
  std::string out;
  for (size_t i = 0; i < t.size(); ++i) {
    if (!fetchAt(t, i)) {
      out += t[i];
      continue;
    }
    size_t k = fetchOpen(t, i);
    int depth = 0;
    for (; k < t.size(); ++k) {
      if (t[k] == '(' || t[k] == '[') ++depth;
      if ((t[k] == ')' || t[k] == ']') && --depth == 0) break;
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
  std::string prefix, tail = ";";  // see Region
};

// The statement starting at column `start` of `line` (0-based; at most the first non-blank character), ending with the
// first ';' at bracket depth 0 (or ',' with `declarator`: one declarator of a declaration). Code before it on its
// first line and after it on its last line goes to st.prefix / st.tail.
bool statementAt(const std::vector<std::string>& lines, uint32_t line, StatementText& st, std::string& why,
                 size_t start = 0, bool declarator = false) {
  if (line == 0 || line > lines.size()) { why = "line out of range"; return false; }
  st.first = line;
  {
    const std::string& s = lines[line - 1];
    const size_t b = s.find_first_not_of(" \t");
    st.prefix = b != std::string::npos && start > b ? s.substr(b, start - b) : std::string();
    if (b == std::string::npos || start < b) start = 0;
  }
  int depth = 0;
  bool inBlock = false;
  std::string text;
  for (uint32_t l = line; l <= lines.size() && l < line + 24; ++l) {
    const std::string& s = lines[l - 1];
    for (size_t i = l == line ? start : 0; i < s.size(); ++i) {
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
      if ((c == ')' || c == ']') && --depth < 0) { why = "loop header"; return false; }  // in a for (...)
      if (c == '{' || c == '}') {
        const size_t h = text.find_first_not_of(" \t");
        const std::string head = h == std::string::npos ? std::string() : text.substr(h);
        why = head.rfind("for", 0) == 0 || head.rfind("while", 0) == 0 ? "loop header"
              : c == '{' && head.find('=') != std::string::npos      ? "array initializer"
                                                                     : "not a simple statement";
        return false;
      }
      if ((c == ';' || (declarator && c == ',')) && depth == 0) {
        // Blanks or a comment after it: the line is the statement's own; other code stays in the tail.
        const std::string rest = s.substr(i + 1);
        const size_t k = rest.find_first_not_of(" \t");
        bool own = k == std::string::npos || rest.compare(k, 2, "//") == 0;
        if (!own && rest.compare(k, 2, "/*") == 0) {
          const size_t e = rest.find("*/", k);
          own = e != std::string::npos && rest.find_first_not_of(" \t", e + 2) == std::string::npos;
        }
        // A block comment opened after it (or closed before it) would take the #if / #else lines of a variant with it.
        const size_t open = rest.rfind("/*");
        if ((open != std::string::npos && rest.find("*/", open + 2) == std::string::npos && rest.find("//") > open) ||
            st.prefix.find("*/") < st.prefix.find("/*")) {
          why = "comment";
          return false;
        }
        st.tail = own ? std::string(1, c) : s.substr(i);
        while (!st.tail.empty() && std::isspace(static_cast<unsigned char>(st.tail.back()))) st.tail.pop_back();
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
    if (p == '<' || p == '>' || p == '!' || p == '=' || p == '&' || p == '|' || p == '^')
      return std::string::npos;
    compound = (p == '+' || p == '-' || p == '*' || p == '/' || p == '%') ? p : 0;
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
  bool likely = false;   // depends on a likely range (a texture read of an unknown format) somewhere
  std::string likelyWhy;
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
  for (const Range* f : from) {
    r.assumed = r.assumed || f->assumed;
    if (f->likely && !r.likely) r.likely = true, r.likelyWhy = f->likelyWhy;
  }
  if (!r.known) return Range::unknown();
  return r;
}

Range unite(const Range& a, const Range& b) {
  if (!a.known || !b.known) return Range::unknown();
  Range r = Range::of(std::min(a.lo, b.lo), std::max(a.hi, b.hi),
                      a.why == b.why ? a.why : std::string("derived"));
  r.assumed = a.assumed || b.assumed;
  r.likely = a.likely || b.likely;
  r.likelyWhy = a.likely ? a.likelyWhy : b.likelyWhy;
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
Range semanticConvention(const std::string& semantic, double maxWidth);
Range computeInputRange(const Function& cs, const std::string& semantic, double maxWidth);

// A texture fetch call in current ReShade FX syntax (variants are written with it):
// the deprecated texNDoffset / texNDlodoffset / tex2Dgather(s, c, comp) /
// tex2Dgatheroffset(s, c, o, comp) become texND(s, c, o), texNDlod(s, c, o) and
// tex2DgatherR/G/B/A(s, c[, o]). Other calls are returned unchanged.
std::string modernFetch(const std::string& call) {
  const size_t open = call.find('(');
  if (open == std::string::npos || call.back() != ')') return call;
  const std::string name = trim(call.substr(0, open));
  std::vector<std::string> args;
  int depth = 0;
  size_t start = open + 1;
  for (size_t i = open + 1; i + 1 < call.size(); ++i) {
    const char c = call[i];
    if (c == '(' || c == '[') ++depth;
    if (c == ')' || c == ']') --depth;
    if (c == ',' && depth == 0) {
      args.push_back(trim(call.substr(start, i - start)));
      start = i + 1;
    }
  }
  args.push_back(trim(call.substr(start, call.size() - 1 - start)));
  auto join = [](const std::string& fn, const std::vector<std::string>& a) {
    std::string out = fn + "(";
    for (size_t k = 0; k < a.size(); ++k) out += (k ? ", " : "") + a[k];
    return out + ")";
  };
  static const char* rgba = "RGBA";
  auto comp = [&](const std::string& c) -> std::string {
    return c.size() == 1 && c[0] >= '0' && c[0] <= '3' ? std::string(1, rgba[c[0] - '0']) : std::string();
  };
  // tex1D/2D/3D: "texN" + "Dlodoffset" / "Doffset".
  const bool texN = name.size() > 4 && name.rfind("tex", 0) == 0 && name[3] >= '1' && name[3] <= '3';
  if (texN && name.substr(4) == "Dlodoffset" && args.size() == 3) return join(name.substr(0, 5) + "lod", args);
  if (texN && name.substr(4) == "Doffset" && args.size() == 3) return join(name.substr(0, 5), args);
  if (name == "tex2Dgather" && args.size() == 3 && !comp(args[2]).empty())
    return join("tex2Dgather" + comp(args[2]), {args[0], args[1]});
  if (name == "tex2Dgatheroffset" && args.size() == 4 && !comp(args[3]).empty())
    return join("tex2Dgather" + comp(args[3]), {args[0], args[1], args[2]});
  return call;
}

bool isTexFetch(const std::string& n) {
  return (n.rfind("tex1D", 0) == 0 || n.rfind("tex2D", 0) == 0 || n.rfind("tex3D", 0) == 0) &&
         n.find("store") == std::string::npos;
}

// Init and Store statements write a variable; Return and Write (into a resource) do not.
bool hasVar(const Statement& s) { return s.kind == Statement::Kind::Init || s.kind == Statement::Kind::Store; }


// ---------------------------------------------------------------------------
// Preprocessor conditionals around windows

// Directive of a source line: "if", "ifdef", "elif", ... and its argument.
bool directiveOf(const std::string& line, std::string& word, std::string& arg) {
  size_t i = line.find_first_not_of(" \t");
  if (i == std::string::npos || line[i] != '#') return false;
  i = line.find_first_not_of(" \t", i + 1);
  if (i == std::string::npos) return false;
  size_t j = i;
  while (j < line.size() && std::isalpha(static_cast<unsigned char>(line[j]))) ++j;
  word = line.substr(i, j - i);
  arg = line.substr(j);
  if (const size_t c = arg.find("//"); c != std::string::npos) arg.erase(c);
  for (size_t c = arg.find("/*"); c != std::string::npos; c = arg.find("/*")) {
    const size_t e = arg.find("*/", c + 2);
    arg.erase(c, e == std::string::npos ? std::string::npos : e + 2 - c);
  }
  arg = trim(arg);
  return true;
}

// The preprocessor condition under which the lines [first, last] of `file` compile as
// in this parse (a window's statements are all there or all not): for every #if group
// with a directive inside the span, the condition of the branch taken now. Empty when
// the span has no directives. False if a group's taken branch cannot be told.
// Whether lines first .. last - 1 open and close as many braces as they close and open (comments skipped): a
// statement on line `last` is then in the same block as one ending on line first - 1.
bool sameBlock(const std::string& file, uint32_t first, uint32_t last) {
  const std::vector<std::string>* lines = sourceLines(file);
  if (!lines) return false;
  int depth = 0;
  bool inComment = false;
  for (uint32_t l = first; l < last && l <= lines->size(); ++l) {
    const std::string& s = (*lines)[l - 1];
    for (size_t i = 0; i < s.size(); ++i) {
      if (inComment) {
        if (s.compare(i, 2, "*/") == 0) { inComment = false; ++i; }
      } else if (s.compare(i, 2, "//") == 0) {
        break;
      } else if (s.compare(i, 2, "/*") == 0) {
        inComment = true; ++i;
      } else if (s[i] == '{' || s[i] == '}') {
        depth += s[i] == '{' ? 1 : -1;
        if (depth < 0) return false;
      }
    }
  }
  return depth == 0;
}

bool spanGuard(const Effect& fx, const std::string& file, uint32_t first, uint32_t last,
               std::string& guard, std::string& why, bool enclosing = false, uint32_t takenLine = 0);

// Whether `name` appears as an identifier in the text from line `from` to the end of its block, outside the lines
// `removed` and [first, last] (code of every setting counts: the text is not preprocessed).
bool nameInScope(const std::string& file, const std::string& name, uint32_t from,
                 const std::vector<std::pair<uint32_t, uint32_t>>& removed, uint32_t first, uint32_t last) {
  const std::vector<std::string>* lines = sourceLines(file);
  if (!lines || name.empty()) return true;
  int depth = 0;
  for (uint32_t l = from; l <= lines->size(); ++l) {
    bool skip = l >= first && l <= last;
    for (const auto& [a, b] : removed) skip = skip || (l >= a && l <= b);
    for (const std::string& t : tokens((*lines)[l - 1])) {
      if (t == "{") ++depth;
      if (t == "}" && --depth < 0) return false;  // the end of the block the temporary was declared in
      if (!skip && (t == name || t.rfind(name + ".", 0) == 0)) return true;
    }
  }
  return false;
}

// Constants a statement names that are declared inside #if / #else (owner, 2026-10-25, SuperDepth3D: Luma_Dark is a
// static const = 0.15 in one build and a uniform slider in the other). ReShade's parser puts a static const's value in
// place, so a variant would carry 0.15 into the slider build: the region gets the declaration's condition as its guard
// (the variant only for this build; easy mode leaves guarded regions alone). False: the condition cannot be worked out.
bool constGuard(const Effect& fx, Region& r, std::string& why) {
  // Per file: each static const's name and line (one pass per file).
  static std::map<std::string, std::map<std::string, uint32_t>> declared;
  auto it = declared.find(r.file);
  if (it == declared.end()) {
    std::map<std::string, uint32_t> names;
    if (const std::vector<std::string>* lines = sourceLines(r.file)) {
      static const std::regex decl(R"(\bstatic\s+const\b)"), name(R"(([A-Za-z_]\w*)\s*=(?!=))");
      for (uint32_t l = 1; l <= lines->size(); ++l) {
        const std::string& s = (*lines)[l - 1];
        std::smatch m;
        if (!std::regex_search(s, m, decl)) continue;
        const std::string rest = m.suffix();
        for (auto n = std::sregex_iterator(rest.begin(), rest.end(), name); n != std::sregex_iterator(); ++n)
          names.emplace((*n)[1], l);
      }
    }
    it = declared.emplace(r.file, std::move(names)).first;
  }
  if (it->second.empty()) return true;
  static const std::regex ident(R"([A-Za-z_]\w*)");
  std::set<std::string> guards;
  for (auto n = std::sregex_iterator(r.original.begin(), r.original.end(), ident); n != std::sregex_iterator(); ++n) {
    const auto d = it->second.find(n->str());
    if (d == it->second.end()) continue;
    std::string g;
    if (!spanGuard(fx, r.file, d->second, d->second, g, why, true)) return false;
    if (!g.empty()) guards.insert(g);
  }
  for (const std::string& g : guards) r.guard = r.guard.empty() ? g : "(" + r.guard + ") && (" + g + ")";
  return true;
}

// enclosing: also the conditions of the #if groups around the span (the branch a declaration is in), not only of
// those with a directive inside it (a window across #if lines). takenLine: a line known to be compiled now (a #define
// in effect), whose branch is then the taken one.
bool spanGuard(const Effect& fx, const std::string& file, uint32_t first, uint32_t last,
               std::string& guard, std::string& why, bool enclosing, uint32_t takenLine) {
  guard.clear();
  const std::vector<std::string>* lines = sourceLines(file);
  if (!lines) { why = "no source text"; return false; }
  struct Branch {
    uint32_t line;
    std::string cond;  // empty: #else
  };
  struct Group {
    std::vector<Branch> branches;
    uint32_t end = 0;
  };
  std::vector<Group> groups, stack;
  for (uint32_t l = 1; l <= lines->size(); ++l) {
    std::string word, arg;
    if (!directiveOf((*lines)[l - 1], word, arg)) continue;
    if (!arg.empty() && arg.back() == '\\') {
      // A #define over several lines is no condition (2026-10-25: one in a file made every region there give up,
      // SuperDepth3D 317 -> 172 regions); a condition over several lines is joined.
      if (word != "if" && word != "elif") continue;
      while (!arg.empty() && arg.back() == '\\' && l < lines->size()) {
        arg.pop_back();
        arg += " " + (*lines)[l++];
        while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.back()))) arg.pop_back();
      }
    }
    if (word == "if" || word == "ifdef" || word == "ifndef") {
      const std::string c = word == "if" ? "(" + arg + ")"
                            : word == "ifdef" ? "defined(" + arg + ")" : "!defined(" + arg + ")";
      stack.push_back({{{l, c}}, 0});
    } else if ((word == "elif" || word == "else") && !stack.empty()) {
      stack.back().branches.push_back({l, word == "elif" ? "(" + arg + ")" : std::string()});
    } else if (word == "endif" && !stack.empty()) {
      stack.back().end = l;
      groups.push_back(std::move(stack.back()));
      stack.pop_back();
    }
  }
  // Whether a code line in (a, b) exists / is compiled now.
  auto lineIn = [&](uint32_t a, uint32_t b, bool compiled) {
    for (uint32_t l = a + 1; l < b; ++l) {
      std::string w, arg;
      if (directiveOf((*lines)[l - 1], w, arg) || tokens((*lines)[l - 1]).empty()) continue;
      if (!compiled) return true;
      const auto it = fx.ppLines.find({file, l});
      if (it != fx.ppLines.end() && !tokens(it->second).empty()) return true;
    }
    return false;
  };
  // An include guard (#ifndef X right before #define X) is no condition: X is defined wherever its code is.
  auto includeGuard = [&](const Group& g) {
    const std::string& c = g.branches[0].cond;
    if (g.branches.size() != 1 || c.rfind("!defined(", 0) != 0) return false;
    for (uint32_t l = g.branches[0].line + 1; l < g.end; ++l) {
      std::string w, a;
      if (directiveOf((*lines)[l - 1], w, a))
        return w == "define" && a.substr(0, a.find_first_of(" \t")) == c.substr(9, c.size() - 10);
      if (!tokens((*lines)[l - 1]).empty()) return false;
    }
    return false;
  };
  std::vector<std::string> terms;
  for (const Group& g : groups) {
    if (includeGuard(g)) continue;
    bool touched = g.end >= first && g.end <= last;
    for (const Branch& b : g.branches) touched = touched || (b.line >= first && b.line <= last);
    if (enclosing) touched = touched || (g.branches.front().line < first && last < g.end);
    if (!touched) continue;
    auto end = [&](size_t k) { return k + 1 < g.branches.size() ? g.branches[k + 1].line : g.end; };
    // Condition of taking branch k: the earlier ones false, its own true.
    auto takes = [&](size_t k) {
      std::vector<std::string> t;
      for (size_t j = 0; j < k; ++j) t.push_back("!" + g.branches[j].cond);
      if (!g.branches[k].cond.empty()) t.push_back(g.branches[k].cond);
      return t;
    };
    int taken = -1;
    for (size_t k = 0; k < g.branches.size() && taken < 0; ++k)
      if ((g.branches[k].line < takenLine && takenLine < end(k)) || lineIn(g.branches[k].line, end(k), true))
        taken = static_cast<int>(k);
    if (taken >= 0) {
      for (auto& t : takes(taken)) terms.push_back(t);
      continue;
    }
    // No branch with code is taken: none of them may be.
    for (size_t k = 0; k < g.branches.size(); ++k) {
      if (!lineIn(g.branches[k].line, end(k), false)) continue;
      const auto t = takes(k);
      if (t.empty()) { why = "preprocessor conditional"; return false; }  // #else with code
      std::string c;
      for (const auto& x : t) c += (c.empty() ? "" : " && ") + x;
      terms.push_back(t.size() == 1 ? "!" + c : "!(" + c + ")");
    }
  }
  for (const auto& t : terms) guard += (guard.empty() ? "" : " && ") + t;
  return true;
}

// ---------------------------------------------------------------------------
// Region extraction

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// The integer thread IDs a compute shader can take as inputs.
bool computeSemantic(const std::string& semantic) {
  const std::string s = upper(semantic);
  return s == "SV_DISPATCHTHREADID" || s == "SV_GROUPTHREADID" || s == "SV_GROUPID" || s == "SV_GROUPINDEX";
}

// Ops that are several instructions on the GPU (helpers and their like): one of them alone can get cheaper.
bool compoundOp(Op op) {
  switch (op) {
    case Op::Sign: case Op::Exp: case Op::Log: case Op::Div: case Op::Step: case Op::Pow: case Op::Lerp:
    case Op::Clamp: case Op::Smoothstep: case Op::Dot: case Op::Length: case Op::Normalize: case Op::Distance:
      return true;
    default:
      return false;
  }
}

// Set when a min16float value is read as float math (the region's original may run at half precision).
thread_local bool tlSawHalf = false;

bool isFloatType(const reshadefx::type& t) {
  const bool half = t.base == reshadefx::type::t_min16float;
  const bool ok = (t.base == reshadefx::type::t_float || half) && t.cols == 1 && t.rows >= 1 && t.rows <= 4 &&
                  !t.is_array();
  tlSawHalf = tlSawHalf || (ok && half);
  return ok;
}

// A skip reason with the type that is not float32 ("non-float arithmetic (int)"): corpus summaries tell integer
// math from min16float (fp16) from booleans.
std::string nonFloat(const char* what, const reshadefx::type& t) {
  using T = reshadefx::type;
  const char* base = t.base == T::t_bool ? "bool" : t.base == T::t_int || t.base == T::t_min16int ? "int"
                   : t.base == T::t_uint || t.base == T::t_min16uint ? "uint" : t.base == T::t_min16float ? "min16float"
                   : t.base == T::t_float ? (t.is_array() ? "float array" : "float matrix") : "other";
  return std::string(what) + " (" + base + ")";
}

struct Unsupported : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Extractor {
 public:
  ~Extractor() {
    tlBracketFetches = nullptr;
    tlGlslFetches = false;
    tlMacroFetches = nullptr;
  }
  // all(v) / any(v) of a float vector: the parser casts v to boolN first (!= 0, as all / any expand it); the float v.
  uint32_t floatBeforeBoolCast(uint32_t id) const {
    const Value& a = cg_.values.at(id);
    using O = reshadefx::expression::operation;
    if (a.kind != Value::Kind::Chain || a.chain.size() != 1 || a.chain[0].op != O::op_cast) return id;
    if (!a.chain[0].from.is_floating_point() || !a.chain[0].to.is_boolean() || !cg_.values.count(a.base)) return id;
    return a.base;
  }
  static bool isComparison(tokenid t) {
    return t == tokenid::less || t == tokenid::less_equal || t == tokenid::greater || t == tokenid::greater_equal ||
           t == tokenid::equal_equal || t == tokenid::exclaim_equal;
  }
  static Op comparisonOp(tokenid t) {
    return t == tokenid::less ? Op::Lt : t == tokenid::less_equal ? Op::Le : t == tokenid::greater ? Op::Gt
         : t == tokenid::greater_equal ? Op::Ge : t == tokenid::equal_equal ? Op::Eq : Op::Ne;
  }
  bool isScalarBool(uint32_t id) const {
    const auto& t = cg_.values.at(id).type;
    return t.is_boolean() && t.is_scalar();
  }
  Extractor(const Effect& fx, const RegionOptions& opt) : fx_(fx), cg_(*fx.cg), opt_(opt) {
    tlBracketFetches = fx.hlsl ? &fx.hlslFetchNames : nullptr;
    tlGlslFetches = fx.glsl;
    // A fixed macro that is one fetch (owner, 2026-10-26: getColor, GetLod, getColorSampler ...) reads as a fetch: the
    // input keeps the macro call (so one defined inside #if needs no guard). Also one that is such a macro
    // (SMAASamplePoint -> SMAASample -> tex2D, 2026-10-27).
    tlMacroFetches = &macroFetches_;
    for (size_t before = SIZE_MAX; before != macroFetches_.size();) {
      before = macroFetches_.size();
      for (const auto& [name, m] : fx.functionMacros) {
        std::string b = trim(m.second);
        while (b.size() > 1 && b.front() == '(' && closeOf(b, 0) + 1 == b.size()) b = trim(b.substr(1, b.size() - 2));
        const size_t open = fetchOpen(b, 0);
        std::set<std::string> unused;
        if (open != std::string::npos && closeOf(b, open) + 1 == b.size() && macroUsable(name, unused))
          macroFetches_.insert(name);
      }
    }
    for (const auto& [id, v] : cg_.values)
      for (uint32_t a : v.args) users_[a].push_back(id);
    for (const auto& [id, v] : cg_.values)
      if (v.kind == Value::Kind::Chain || v.kind == Value::Kind::Load) users_[v.base].push_back(id);
    for (const auto& f : cg_.functions) {
      for (const auto& s : f->stmts) {
        if (hasVar(s)) defs_[s.var].push_back(&s);
        stmtUses_[s.value].push_back(&s);
      }
      for (uint32_t p : f->params) paramOf_[p] = f.get(), varFunction_[p] = f.get();
      for (const auto& st : f->stmts)
        if (hasVar(st)) varFunction_.emplace(st.var, f.get());
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
  bool touchesPrecise(const Function& f, const Statement& s, const std::vector<const Statement*>& defs) const;
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
    bool call = false;           // ... or a helper call read as an input (fetch too)
    std::string semantic;        // of the struct member the prefix ends in, if any
    bool intSource = false;      // an integer variable, read converted to float
    bool uintLeaf = false;       // ... or read as an integer (intOps_): a Type::Uint input
  };
  Leaf& fetchLeaf(uint32_t id);

  // Pass 1 / 2 over the value tree.
  void collect(uint32_t id);
  uint32_t build(uint32_t id, ExprBuilder& b);
  // A select's condition as a scalar test (2026-10-26): HLSL widens a scalar condition to the branches' size, and a
  // bool / int / float can be the condition itself (then "!= 0"). Returns the value to use as is, or 0 for cond:
  // a copy of the load / chain without the widening cast, numeric when it is not a bool.
  uint32_t condOf(uint32_t id, Value& cond, bool& numeric);
  void collectCond(uint32_t id);
  uint32_t buildCond(uint32_t id, ExprBuilder& b);
  bool numberLeaf_ = false;  // leafOf: a bool / int variable read as a float (a condition)
  // Integer + - * converted to float (owner, 2026-10-26: float(i * 2 + 1)): searched as float math, exact while every
  // value is a whole number within 2^24 (intCast checks the ranges); > 0 while collecting / building one.
  int intMode_ = 0;
  // ... a scalar one as integer ops (owner, 2026-10-27: integers have their own costs): uint leaves, UAdd / USub /
  // UMul (two's complement: the same bits for int), IToF / UToF at the conversion; > 0 inside one.
  int intOps_ = 0;
  bool intScalar(const Value& v) const {
    return !fx_.glsl && v.chain[0].from.rows == 1 && v.chain[0].from.cols == 1;
  }
  bool intCast(const Value& v);
  bool intExact(uint32_t id, int depth = 0);
  // A select on a float vector whose condition compares float vectors (c <= 0.04045 ? ... : ...): the comparison.
  const Value* vectorCompare(const Value& v) const {
    if (v.kind != Value::Kind::Ternary || v.type.rows < 2 || !isFloatType(v.type)) return nullptr;
    const auto c = cg_.values.find(v.args[0]);
    if (c == cg_.values.end() || c->second.kind != Value::Kind::Binary || !isComparison(c->second.op) ||
        !c->second.type.is_boolean() || c->second.type.rows != v.type.rows)
      return nullptr;
    for (uint32_t a : c->second.args)
      if (const auto ai = cg_.values.find(a); ai == cg_.values.end() || !isFloatType(ai->second.type) ||
                                              (ai->second.type.rows != v.type.rows && ai->second.type.rows != 1))
        return nullptr;
    return &c->second;
  }
  struct MatMul {
    const Value* matrix = nullptr;  // a float matrix written out: a constant, or float3x3(...) in place
    uint32_t vector = 0;
    bool rowsFirst = false;         // mul(M, v): a dot per row; mul(v, M): per column
  };
  MatMul matMul(const Value& v) const;
  bool intCompare(const Value& v);
  // Splits a load into leaf prefix and the vector part of the chain.
  Leaf& leafOf(const Value& v, size_t& vecStart);
  uint32_t applyVectorChain(uint32_t node, const std::vector<reshadefx::expression::operation>& chain,
                            size_t start, ExprBuilder& b, const Leaf* leaf);
  // A variable's name as code writes it; relative = false keeps a uniform's full namespace (fact keys).
  std::string varText(uint32_t var, bool relative = true) const;
  std::string fnNs_;  // the current function's namespace as in unique names ("Ns__", "" at global scope)

  Range range(uint32_t valueId);
  Range varRange(uint32_t var, uint32_t seq, uint32_t block);
  Range varRangeRaw(uint32_t var, uint32_t seq, uint32_t block);
  Range loopRange(uint32_t var);
  std::string varKey(uint32_t var) const;  // UserRanges key of a variable
  std::unordered_map<uint32_t, const Function*> varFunction_;  // local/param -> function
  Range samplerRange(uint32_t valueId);
  // `object` (or the object a load of it reads) is a storage.
  bool isStorage(uint32_t object) const {
    if (const auto v = cg_.values.find(object); v != cg_.values.end()) object = v->second.base;
    for (const auto& st : cg_.mod().storages)
      if (st.id == object) return true;
    return false;
  }
  // Fact key of an HLSL RW texture / buffer read through `object` (the storage or a load of it); empty otherwise.
  std::string storageFactKey(uint32_t object) const {
    if (!fx_.hlsl) return {};
    if (const auto v = cg_.values.find(object); v != cg_.values.end()) object = v->second.base;
    for (const auto& st : cg_.mod().storages)
      if (st.id == object && fx_.hlslFetchNames.count(st.name)) return textureFactKey(fx_, st.name);
    return {};
  }
  Budget budgetFor(const Function& f, const Statement& s, std::string& reason);
  void useKinds(uint32_t valueId, bool& cmp, bool& coord, bool& other, int depth);
  static bool outOnly(const Variable& v);
  bool isSymbolic(uint32_t var) const {
    const auto it = cg_.variables.find(var);
    return it != cg_.variables.end() && it->second.kind == Variable::Kind::Uniform &&
           it->second.name.rfind(kSymbolicPrefix, 0) == 0;
  }
  void suggest(const Leaf& l, Fact& f) const;
  std::vector<std::string> sourceTokens(const std::string& src, std::set<std::string>& guards) const;
  bool fixedMacro(const std::string& name) const;
  bool macroUsable(const std::string& name, std::set<std::string>& guards) const;
  struct MacroDefs {
    int count = 0;
    bool inIf = false, undef = false;
    std::vector<std::pair<std::string, uint32_t>> at;  // file, line of each #define
  };
  mutable std::unordered_map<std::string, MacroDefs> macroDefs_;
  std::set<std::string> macroFetches_;  // see tlMacroFetches
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
  // Ranges with BUFFER_WIDTH / BUFFER_HEIGHT fixed to one of kBufferSizes (sizeFix_ >= 0):
  // interval arithmetic over the symbolic sizes loses correlations such as
  // trunc(S / b * t) * (b / S), so leaf ranges are the union over these sizes.
  int sizeFix_ = -1;
  std::vector<std::unordered_map<uint32_t, Range>> sizeMemo_;
  Range leafRange(uint32_t valueId);
  std::unordered_set<uint32_t> varBusy_;
  std::unordered_set<uint64_t> localBusy_;  // (variable, load seq)
  std::vector<const Statement*> reaching(uint32_t var, uint32_t seq, uint32_t block, bool& killed) const;
  int usesOf(const Statement& s);  // loads that read what s stores
  std::unordered_map<uint32_t, std::vector<uint32_t>> loadsByVar_;
  // The same for every pixel of a draw: a constant in ReShade's performance mode.
  bool perDraw(uint32_t valueId);
  bool chainPerDraw(const std::vector<reshadefx::expression::operation>& chain);
  std::unordered_map<uint32_t, bool> perDrawMemo_;
  std::unordered_set<uint32_t> perDrawBusy_;

  // current region
  std::vector<Leaf> leaves_;
  std::unordered_map<uint32_t, uint32_t> built_;
  std::unordered_set<uint32_t> collected_;  // collect(): values seen in this scope
  bool usesPow_ = false;
  std::unordered_map<uint32_t, uint32_t> inline_;  // temporary variable -> its value
  std::unordered_map<uint32_t, uint32_t> inlineLoad_;  // load -> value of its (chain) definition
  // Values the frontend makes itself, for a helper's variables (2026-10-26): after an if (Select: a when every one of
  // conds holds, else b; | kCondFalse: the false side) or after a partial store (Merge: b with a written into
  // chain's components). Ids from kSynthFirst up; collect() / build() look here first.
  struct Synth {
    enum class Kind { Select, Merge } kind = Kind::Select;
    std::vector<uint32_t> conds;
    uint32_t a = 0, b = 0;
    std::vector<reshadefx::expression::operation> chain;
    unsigned rows = 1;
  };
  static constexpr uint32_t kSynthFirst = 0x40000000;
  std::unordered_map<uint32_t, Synth> synth_;
  uint32_t nextSynth_ = kSynthFirst;
  uint32_t addSynth(Synth s) {
    synth_[nextSynth_] = std::move(s);
    return nextSynth_++;
  }
  void collectSynth(const Synth& s);
  uint32_t buildSynth(const Synth& s, ExprBuilder& b);
  // A call of a small pure helper, seen through (owner, 2026-10-26): its return value with the parameters bound to
  // the arguments and its variables' reads to their values (inline_ / inlineLoad_), while the scope lives.
  class CallScope {
   public:
    CallScope(Extractor& x, const Value& call);
    ~CallScope();
    uint32_t ret = 0;  // the callee's return value
   private:
    struct Saved {
      uint32_t key;
      std::optional<uint32_t> old;
    };
    Extractor& x_;
    std::vector<Saved> saved_, savedLoads_;  // inline_ / inlineLoad_ entries before the scope
  };
  bool findChain(const Statement& s, size_t length, std::vector<const Statement*>& defs);
  bool leavesUnchanged(const Statement& s, const std::vector<const Statement*>& defs) const;
  bool writesBetween(uint32_t var, uint32_t after, uint32_t before,
                     const std::vector<const Statement*>& window) const;
  std::unordered_map<uint32_t, std::string> fetchText_;  // fetch value -> call text
  std::vector<const Statement*> window_;  // statements inlined into the current region
  void mapFetches(const Statement& s, const std::string& text);
  // Helper calls of a statement, by their text (a call that cannot be inlined is then an input, 2026-10-26).
  void mapCalls(const Statement& s, const std::string& text);
  std::unordered_map<uint32_t, std::string> callText_;
  std::unordered_set<uint32_t> callLeaf_;  // calls read as inputs
  bool pureCall(const std::string& uniqueName, int depth = 0) const;
  std::unordered_set<std::string> callBusy_;  // range(): helpers being followed
};

std::string Extractor::varText(uint32_t var, bool relative) const {
  const auto it = cg_.variables.find(var);
  if (it == cg_.variables.end()) throw Unsupported("not a variable");
  const Variable& v = it->second;
  switch (v.kind) {
    case Variable::Kind::Local:
    case Variable::Kind::Param: return v.name;
    case Variable::Kind::Uniform: {
      if (v.name.rfind(kSymbolicPrefix, 0) == 0) return v.name.substr(std::strlen(kSymbolicPrefix));
      if (v.uniqueName == "V" + v.name) return v.name;
      std::string ns = v.uniqueName.substr(1, v.uniqueName.size() - 1 - v.name.size());
      // A uniform of the function's own namespace (or an enclosing one) by its plain name, as the source
      // writes it: a header included inside different namespaces (CobraFX's CobraUtility.fxh, COBRA_MSK and
      // others) must not get one effect's namespace in its variant text. Unless a nearer uniform of the
      // same name would take the plain name.
      if (relative && fnNs_.rfind(ns, 0) == 0) {
        bool shadowed = false;
        for (const auto& [id, o] : cg_.variables)
          if (&o != &v && o.kind == Variable::Kind::Uniform && o.name == v.name) {
            const std::string ons = o.uniqueName.substr(1, o.uniqueName.size() - 1 - o.name.size());
            shadowed = shadowed || (ons.size() > ns.size() && fnNs_.rfind(ons, 0) == 0);
          }
        if (!shadowed) return v.name;
      }
      // "VNs__name" -> "Ns::name"
      std::string out;
      for (size_t i = 0; i < ns.size(); ++i) {
        if (ns[i] == '_' && i + 1 < ns.size() && ns[i + 1] == '_') { out += "::"; ++i; }
        else out += ns[i];
      }
      if (out.rfind("::", 0) == 0) out.erase(0, 2);  // global scope
      return out + v.name;
    }
    case Variable::Kind::Global:
      // A static const (2026-10-27: weights read at a loop index), qualified by its namespace.
      if (v.type.has(reshadefx::type::q_const) && v.initializer) return globalSourceName(v.uniqueName);
      break;
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
    } else if (op.op == reshadefx::expression::operation::op_dynamic_index && op.from.is_array() &&
               window_.empty()) {
      // An element at a variable index (2026-10-27: offsets[i] in a loop): an input named by its text, with the
      // range of every element; not in windows (the index may change between the statements). The index must be
      // a variable of this function (a helper's parameter has no name at the call).
      const auto iv = cg_.values.find(op.index);
      bool plain = iv != cg_.values.end() && iv->second.kind == Value::Kind::Load && !inlineLoad_.count(op.index) &&
                   !inline_.count(iv->second.base) && cg_.variables.count(iv->second.base) &&
                   !isSymbolic(iv->second.base);
      for (size_t k = 0; plain && k < iv->second.chain.size(); ++k)
        plain = iv->second.chain[k].op == reshadefx::expression::operation::op_cast;
      if (!plain) throw Unsupported("access chain (index)");
      const std::string idx = varText(iv->second.base);
      if (text.empty() && i == 0 && var->kind == Variable::Kind::Local) {
        // ReShade reads a static const array at a run-time index through a nameless copy: the name the source line
        // uses with this index ("W[k]"), when there is one such name and it is no macro.
        const std::vector<std::string>* lines = sourceLines(var->loc.source);
        if (!lines || var->loc.line == 0 || var->loc.line > lines->size()) throw Unsupported("access chain (copy)");
        const std::string& l = (*lines)[var->loc.line - 1];
        const std::regex use("\\b([A-Za-z_]\\w*)\\s*\\[\\s*" + idx + "\\s*\\]");
        std::set<std::string> names;
        for (auto m = std::sregex_iterator(l.begin(), l.end(), use); m != std::sregex_iterator(); ++m)
          names.insert((*m)[1].str());
        if (names.size() != 1 || fx_.objectMacros.count(*names.begin()) || fx_.functionMacros.count(*names.begin()))
          throw Unsupported("access chain (copy)");
        text = *names.begin();
      }
      text += "[" + idx + "]";
    } else {
      throw Unsupported("access chain");
    }
    t = op.to;
  }
  vecStart = i;
  // BUFFER_WIDTH / BUFFER_HEIGHT (int) converted to float: a float compile-time input.
  if (var->kind == Variable::Kind::Uniform && isSymbolic(v.base) && t.is_integral() && t.is_scalar() &&
      i < v.chain.size() && v.chain[i].op == reshadefx::expression::operation::op_cast &&
      isFloatType(v.chain[i].to))
    t.base = reshadefx::type::t_float;
  // An integer variable (a compute shader's thread ID, a loop counter, an int slider; 2026-10-26: any) read through a
  // conversion to float: a float input written float(i) / float2(id.xy) (integer arithmetic on it stays unsupported).
  bool intSource = false;
  const size_t rest = v.chain.size() - i;  // [component pick,] cast
  if (t.is_integral() && !isSymbolic(v.base) && (rest == 1 || rest == 2)) {
    using O = reshadefx::expression::operation;
    const size_t c = v.chain.size() - 1;
    const bool picked = rest == 1 || v.chain[i].op == O::op_swizzle || v.chain[i].op == O::op_constant_index;
    if (picked && v.chain[c].op == O::op_cast && v.chain[c].from.is_integral() && isFloatType(v.chain[c].to) &&
        v.chain[c].to.rows == v.chain[c].from.rows) {
      t.base = reshadefx::type::t_float;
      intSource = true;
    }
  }
  // Inside integer + - * converted to float (intMode_): the variable is a float input too.
  if (intMode_ && !intOps_ && t.is_integral() && !t.is_boolean() &&
      (rest == 0 || (rest == 1 && (v.chain[i].op == reshadefx::expression::operation::op_swizzle ||
                                   v.chain[i].op == reshadefx::expression::operation::op_constant_index)))) {
    t.base = reshadefx::type::t_float;
    intSource = !isSymbolic(v.base);  // BUFFER_WIDTH: a compile-time input, variants convert it
  }
  if (numberLeaf_ && i == v.chain.size() && t.is_scalar() && (t.is_boolean() || t.is_integral())) {
    t.base = reshadefx::type::t_float;  // written float(x): a select's condition tests it against 0
    intSource = true;
  }
  const bool uintLeaf = intOps_ && rest == 0 && t.is_integral() && !t.is_boolean() && t.is_scalar();
  if (uintLeaf) intSource = false;
  if (!uintLeaf && !isFloatType(t)) throw Unsupported(nonFloat("non-float variable", t));
  for (auto& l : leaves_)
    if (l.var == v.base && l.prefix == text && l.uintLeaf == uintLeaf) return l;
  Leaf l;
  l.var = v.base;
  l.prefix = text;
  l.type = t;
  l.intSource = intSource;
  l.uintLeaf = uintLeaf;
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
  // int scalar -> float(N): only symbolic BUFFER_WIDTH / HEIGHT get here (leafOf rejects
  // other int variables, collect() int arithmetic).
  if (op.op == O::op_cast && op.from.is_integral() && isFloatType(op.to) &&
      (op.from.is_scalar() || op.to.rows == op.from.rows))
    return (1u << rows) - 1;
  throw Unsupported("access chain");
}

Extractor::CallScope::CallScope(Extractor& x, const Value& call) : x_(x) {
  const Function* f = x.cg_.function(call.name);
  if (!f) throw Unsupported("function call");
  if (f->params.size() != call.args.size()) throw Unsupported("function call: arguments");
  // ReShade passes each argument through a temporary variable (call.args): its last store before the call.
  std::vector<uint32_t> args;
  for (size_t k = 0; k < f->params.size(); ++k) {
    const auto pv = x.cg_.variables.find(f->params[k]);
    if (pv == x.cg_.variables.end() || pv->second.type.has(reshadefx::type::q_out))
      throw Unsupported("function call: out parameter");
    if (pv->second.type.is_object()) throw Unsupported("function call: texture or sampler argument");
    const Statement* st = nullptr;
    if (const auto d = x.defs_.find(call.args[k]); d != x.defs_.end())
      for (const Statement* s : d->second)
        if (s->seq < call.seq && (!st || s->seq > st->seq)) st = s;
    if (!st || !st->chain.empty()) throw Unsupported("function call: arguments");
    args.push_back(st->value);
  }
  // Stores to its locals and parameters (whole, or components: c.rgb = ...) and returns, also under ifs (2026-10-26),
  // not in loops or switches: each read is what the stores before it on its path leave, a select where an if decides.
  using O = reshadefx::expression::operation;
  auto condsOf = [&](uint32_t block) -> const std::vector<uint32_t>& {
    const auto c = x.cg_.blockConds.find(block);
    if (c == x.cg_.blockConds.end() || std::find(c->second.begin(), c->second.end(), 0u) != c->second.end())
      throw Unsupported("function call: switch");
    return c->second;
  };
  std::unordered_set<uint32_t> vars(f->params.begin(), f->params.end());
  std::unordered_map<uint32_t, std::vector<const Statement*>> stores;  // var -> stores in order
  std::vector<const Statement*> returns;
  for (const Statement& s : f->stmts) {
    condsOf(s.block);
    for (const auto& [lo, hi] : x.cg_.loops)
      if (lo <= s.seq && s.seq <= hi) throw Unsupported("function call: loop");
    if (s.kind == Statement::Kind::Return) {
      returns.push_back(&s);
    } else if (s.kind == Statement::Kind::Write) {
      throw Unsupported("function call: writes a resource");
    } else {
      const auto sv = x.cg_.variables.find(s.var);
      if (sv == x.cg_.variables.end() ||
          (sv->second.kind != Variable::Kind::Local && sv->second.kind != Variable::Kind::Param))
        throw Unsupported("function call: writes a global");
      // Components only: one swizzle or constant index of a vector.
      if (!s.chain.empty() && (s.chain.size() != 1 || sv->second.type.is_matrix() || sv->second.type.is_array() ||
                               (s.chain[0].op != O::op_swizzle && s.chain[0].op != O::op_constant_index)))
        throw Unsupported("function call: writes part of a variable");
      vars.insert(s.var);
      stores[s.var].push_back(&s);
    }
  }
  if (returns.empty()) throw Unsupported("function call: no return value");
  // The value a read sees: fold the stores before it; one on the other side of an if the read is in does not reach
  // it, one under conditions the read is not under is a select.
  auto valueAt = [&](uint32_t var, const Value& load, uint32_t entry) -> uint32_t {
    const std::vector<uint32_t>& lc = condsOf(load.block);
    uint32_t v = entry;
    const auto st = stores.find(var);
    if (st == stores.end()) return v;
    for (const Statement* d : st->second) {
      if (d->seq >= load.seq) break;
      const std::vector<uint32_t>& dc = condsOf(d->block);
      size_t k = 0;
      while (k < dc.size() && k < lc.size() && dc[k] == lc[k]) ++k;
      if (k < dc.size() && k < lc.size() && (dc[k] & ~kCondFalse) == (lc[k] & ~kCondFalse)) continue;
      uint32_t nv = d->value;
      if (!d->chain.empty()) {
        if (!v) throw Unsupported("function call: reads an unset variable");
        Synth m;
        m.kind = Synth::Kind::Merge;
        m.a = d->value;
        m.b = v;
        m.chain = d->chain;
        m.rows = x.cg_.variables.at(var).type.rows;
        nv = x.addSynth(std::move(m));
      }
      if (k == dc.size() || !v) {
        v = nv;  // on every path to the read (or the first value: an unset variable is read only where it was set)
      } else {
        Synth s;
        s.conds.assign(dc.begin() + static_cast<std::ptrdiff_t>(k), dc.end());
        s.a = nv;
        s.b = v;
        v = x.addSynth(std::move(s));
      }
    }
    return v;
  };
  // An earlier return wins where its conditions hold.
  ret = returns.back()->value;
  for (size_t i = returns.size() - 1; i-- > 0;) {
    const std::vector<uint32_t>& rc = condsOf(returns[i]->block);
    if (rc.empty()) {
      ret = returns[i]->value;
      continue;
    }
    Synth s;
    s.conds = rc;
    s.a = returns[i]->value;
    s.b = ret;
    ret = x.addSynth(std::move(s));
  }
  auto bind = [&](std::unordered_map<uint32_t, uint32_t>& m, std::vector<Saved>& saved, uint32_t key, uint32_t value) {
    const auto old = m.find(key);
    saved.push_back({key, old == m.end() ? std::nullopt : std::optional<uint32_t>(old->second)});
    m[key] = value;
  };
  if (x.loadsByVar_.empty())
    for (const auto& [id, v] : x.cg_.values)
      if (v.kind == Value::Kind::Load) x.loadsByVar_[v.base].push_back(id);
  std::vector<std::pair<uint32_t, uint32_t>> loads;  // load -> the value it reads
  for (const uint32_t var : vars) {
    const auto lv = x.loadsByVar_.find(var);
    if (lv == x.loadsByVar_.end()) continue;
    const auto pk = std::find(f->params.begin(), f->params.end(), var);
    const uint32_t entry = pk == f->params.end() ? 0 : args[static_cast<size_t>(pk - f->params.begin())];
    for (const uint32_t id : lv->second) {
      const uint32_t v = valueAt(var, x.cg_.values.at(id), entry);
      if (!v) throw Unsupported("function call: reads an unset variable");
      loads.emplace_back(id, v);
    }
  }
  for (size_t k = 0; k < f->params.size(); ++k) bind(x_.inline_, saved_, f->params[k], args[k]);
  for (const auto& [load, value] : loads) bind(x_.inlineLoad_, savedLoads_, load, value);
}

Extractor::CallScope::~CallScope() {
  auto restore = [](std::unordered_map<uint32_t, uint32_t>& m, const std::vector<Saved>& saved) {
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
      if (it->old) m[it->key] = *it->old;
      else m.erase(it->key);
    }
  };
  restore(x_.inline_, saved_);
  restore(x_.inlineLoad_, savedLoads_);
}

uint32_t Extractor::condOf(uint32_t id, Value& cond, bool& numeric) {
  const Value& c = cg_.values.at(id);
  using O = reshadefx::expression::operation;
  // reshadefx widens with a swizzle (.xx) and then casts to bool (int2 -> bool2): both come off.
  Value s = c;
  bool stripped = false;
  if ((s.kind == Value::Kind::Load || s.kind == Value::Kind::Chain) && !s.chain.empty() &&
      s.chain.back().op == O::op_cast && s.chain.back().to.is_boolean()) {
    numeric = numeric || !s.chain.back().from.is_boolean();
    s.type = s.chain.back().from;
    s.chain.pop_back();
    stripped = true;
  }
  if ((s.kind == Value::Kind::Load || s.kind == Value::Kind::Chain) && !s.chain.empty() &&
      s.chain.back().op == O::op_swizzle && s.chain.back().from.is_scalar() && (stripped || s.type.is_boolean())) {
    s.type = s.chain.back().from;
    s.chain.pop_back();
    stripped = true;
  }
  if (stripped) {
    const reshadefx::type from = s.type;
    if (!from.is_scalar()) throw Unsupported("vector select");
    cond = s;
    if (cond.kind == Value::Kind::Chain && cond.chain.empty()) {  // the widened value itself
      if (from.is_boolean()) return condOf(cond.base, cond, numeric);
      return cond.base;
    }
    return 0;
  }
  if (!c.type.is_scalar()) throw Unsupported("vector select");
  if (c.kind == Value::Kind::Load && c.type.is_boolean()) {  // a bool variable
    cond = c;
    numeric = true;
    return 0;
  }
  return id;
}

void Extractor::collectCond(uint32_t id) {
  Value c;
  bool numeric = false;
  if (const uint32_t use = condOf(id, c, numeric)) { collect(use); return; }
  if (c.kind == Value::Kind::Chain) {
    collect(c.base);
    chainMask(c.chain, 0, 4);  // validates
    return;
  }
  if (inlineLoad_.count(id) || inline_.count(c.base)) {
    collect(inlineLoad_.count(id) ? inlineLoad_.at(id) : inline_.at(c.base));
    return;
  }
  size_t vs = 0;
  numberLeaf_ = true;
  Leaf& l = leafOf(c, vs);
  numberLeaf_ = false;
  l.mask |= chainMask(c.chain, vs, l.type.rows);
  if (!l.loadValue) l.loadValue = id;
}

uint32_t Extractor::buildCond(uint32_t id, ExprBuilder& b) {
  Value c;
  bool numeric = false;
  const uint32_t use = condOf(id, c, numeric);
  uint32_t r = 0;
  if (use) {
    r = build(use, b);
  } else if (c.kind == Value::Kind::Chain) {
    r = applyVectorChain(build(c.base, b), c.chain, 0, b, nullptr);
  } else if (inlineLoad_.count(id) || inline_.count(c.base)) {
    r = applyVectorChain(build(inlineLoad_.count(id) ? inlineLoad_.at(id) : inline_.at(c.base), b), c.chain, 0, b,
                         nullptr);
  } else {
    size_t vs = 0;
    numberLeaf_ = true;
    const Leaf& l = leafOf(c, vs);
    numberLeaf_ = false;
    r = applyVectorChain(b.input(l.input, floatType(std::popcount(l.mask))), c.chain, vs, b, &l);
    numeric = true;  // a variable is read as a float, also a bool one
  }
  return numeric ? b.op(Op::Ne, r, b.constant(0.0f)) : r;
}

// A conversion to float of integer + - * over integer variables and constants, every value (also the operands) a whole
// number within 2^24 by its range (and not below 0 for uint, which wraps): float math gives the same.
bool Extractor::intCast(const Value& v) {
  using O = reshadefx::expression::operation;
  if (v.kind != Value::Kind::Chain || v.chain.size() != 1 || v.chain[0].op != O::op_cast ||
      !isFloatType(v.chain[0].to) || !v.chain[0].from.is_integral() || v.chain[0].from.is_boolean() ||
      v.chain[0].to.rows != v.chain[0].from.rows)
    return false;
  return intExact(v.base);
}

bool Extractor::intExact(uint32_t id, int depth) {
  const auto it = cg_.values.find(id);
  if (it == cg_.values.end() || depth > 32) return false;
  const Value& x = it->second;
  if (!x.type.is_integral() || x.type.is_boolean()) return false;
  const Range r = range(id);
  if (!r.known || std::max(std::abs(r.lo), std::abs(r.hi)) > 16777216.0 || (!x.type.is_signed() && r.lo < 0))
    return false;
  switch (x.kind) {
    case Value::Kind::Const:
    case Value::Kind::Load:
      return true;
    case Value::Kind::Unary:
      return (x.op == tokenid::minus || x.op == tokenid::plus) && intExact(x.args[0], depth + 1);
    case Value::Kind::Binary:
      return (x.op == tokenid::plus || x.op == tokenid::minus || x.op == tokenid::star ||
              x.op == tokenid::plus_equal || x.op == tokenid::minus_equal || x.op == tokenid::star_equal) &&
             intExact(x.args[0], depth + 1) && intExact(x.args[1], depth + 1);
    case Value::Kind::Construct:  // int2(BUFFER_WIDTH, BUFFER_HEIGHT)
      if (x.type.is_matrix()) return false;
      for (uint32_t a : x.args)
        if (!intExact(a, depth + 1)) return false;
      return true;
    default:
      return false;
  }
}

// mul(M, v) / mul(v, M) of a float matrix written out and a float vector (2026-10-27: color space conversions such
// as mul(float3x3(...), color)); a matrix variable is not one (its elements have no inputs yet).
Extractor::MatMul Extractor::matMul(const Value& v) const {
  MatMul mm;
  if (v.kind != Value::Kind::Intrinsic || v.name != "mul" || v.args.size() != 2 || !isFloatType(v.type)) return mm;
  const auto a = cg_.values.find(v.args[0]), c = cg_.values.find(v.args[1]);
  if (a == cg_.values.end() || c == cg_.values.end()) return mm;
  const bool first = a->second.type.is_matrix();
  const Value& mat = first ? a->second : c->second;
  const Value& vec = first ? c->second : a->second;
  const reshadefx::type& t = mat.type;
  if (!t.is_matrix() || t.base != reshadefx::type::t_float || t.rows > 4 || t.cols > 4 || !isFloatType(vec.type) ||
      vec.type.rows != (first ? t.cols : t.rows) || v.type.rows != (first ? t.rows : t.cols))
    return mm;
  if (mat.kind == Value::Kind::Construct) {
    if (mat.args.size() != t.rows * t.cols) return mm;
    for (uint32_t e : mat.args)
      if (const auto ei = cg_.values.find(e); ei == cg_.values.end() || !isFloatType(ei->second.type) ||
                                              ei->second.type.rows != 1)
        return mm;
  } else if (mat.kind != Value::Kind::Const) {
    return mm;
  }
  mm.matrix = &mat;
  mm.vector = first ? v.args[1] : v.args[0];
  mm.rowsFirst = first;
  return mm;
}

// A comparison of such integer values (mode == 2 in a select's condition): the same in float math.
bool Extractor::intCompare(const Value& v) {
  return v.kind == Value::Kind::Binary && v.args.size() == 2 && isComparison(v.op) && intExact(v.args[0]) &&
         intExact(v.args[1]);
}

void Extractor::collect(uint32_t id) {
  // Each value once per scope: a value read several times (a helper's parameter, a shared temporary) would otherwise
  // be walked again for every path to it, exponentially in nested helpers (CRT.fx, 2026-10-26).
  if (!collected_.insert(id).second) return;
  if (const auto s = synth_.find(id); s != synth_.end()) {
    collectSynth(s->second);
    return;
  }
  const auto it = cg_.values.find(id);
  if (it == cg_.values.end()) throw Unsupported("value");
  const Value& v = it->second;
  using K = Value::Kind;
  if (intOps_ && !(v.kind == K::Const || v.kind == K::Load || v.kind == K::Unary ||
                   (v.kind == K::Binary && (v.op == tokenid::plus || v.op == tokenid::minus || v.op == tokenid::star ||
                                            v.op == tokenid::plus_equal || v.op == tokenid::minus_equal ||
                                            v.op == tokenid::star_equal))))
    throw Unsupported("integer math (other)");
  switch (v.kind) {
    case K::Const:
      if (!isFloatType(v.type) && !(intMode_ && v.type.is_integral()))
        throw Unsupported(nonFloat("non-float constant", v.type));
      return;
    case K::Load: {
      if (const auto it = inlineLoad_.find(id); it != inlineLoad_.end()) {
        collect(it->second);
        chainMask(v.chain, 0, 4);  // validates
        return;
      }
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
      if (intCast(v)) {
        const bool ops = intScalar(v);
        ++intMode_;
        intOps_ += ops;
        collect(v.base);
        intOps_ -= ops;
        --intMode_;
        return;
      }
      collect(v.base);
      chainMask(v.chain, 0, 4);  // validates
      return;
    case K::Unary:
      if (v.op == tokenid::exclaim) {  // !c on a scalar condition
        if (!(v.type.is_boolean() && v.type.is_scalar() && isScalarBool(v.args[0]))) throw Unsupported("vector logic");
        collect(v.args[0]);
        return;
      }
      if (v.op != tokenid::minus && v.op != tokenid::plus) throw Unsupported("unary operator");
      if (!isFloatType(v.type) && !intMode_) throw Unsupported(nonFloat("non-float arithmetic", v.type));
      collect(v.args[0]);
      return;
    case K::Binary: {
      if (v.op == tokenid::ampersand_ampersand || v.op == tokenid::pipe_pipe) {  // a && b on scalar conditions
        if (!(v.type.is_boolean() && v.type.is_scalar() && isScalarBool(v.args[0]) && isScalarBool(v.args[1])))
          throw Unsupported("vector logic");
        collect(v.args[0]);
        collect(v.args[1]);
        return;
      }
      const bool cmp = v.op == tokenid::less || v.op == tokenid::less_equal ||
                       v.op == tokenid::greater || v.op == tokenid::greater_equal ||
                       v.op == tokenid::equal_equal || v.op == tokenid::exclaim_equal;
      const bool arith = v.op == tokenid::plus || v.op == tokenid::minus ||
                         v.op == tokenid::star || v.op == tokenid::slash ||
                         v.op == tokenid::plus_equal || v.op == tokenid::minus_equal ||
                         v.op == tokenid::star_equal || v.op == tokenid::slash_equal ||
                         v.op == tokenid::percent || v.op == tokenid::percent_equal;  // float %: fmod
      if (!cmp && !arith) throw Unsupported("binary operator");
      if (arith && !isFloatType(v.type) && !intMode_) throw Unsupported(nonFloat("non-float arithmetic", v.type));
      if (cmp && !(v.type.is_boolean() && v.type.is_scalar())) throw Unsupported("vector comparison");
      if (cmp && intCompare(v)) {
        ++intMode_;
        for (uint32_t a : v.args) collect(a);
        --intMode_;
        return;
      }
      for (uint32_t a : v.args) {
        const auto& av = cg_.values.at(a);
        if (!isFloatType(av.type) && !intMode_) throw Unsupported(nonFloat("non-float arithmetic", av.type));
        collect(a);
      }
      return;
    }
    case K::Ternary: {
      if (!isFloatType(v.type)) throw Unsupported(nonFloat("non-float select", v.type));
      if (const Value* cmp = vectorCompare(v)) {
        for (uint32_t a : cmp->args) collect(a);
        collect(v.args[1]);
        collect(v.args[2]);
        return;
      }
      collectCond(v.args[0]);
      collect(v.args[1]);
      collect(v.args[2]);
      return;
    }
    case K::Intrinsic: {
      if (fetchText_.count(id)) {
        Leaf& l = fetchLeaf(id);
        l.mask |= (1u << l.type.rows) - 1;
        return;
      }
      if ((v.name == "all" || v.name == "any") && v.args.size() == 1) {  // a bool from a float vector or comparison
        const uint32_t arg = floatBeforeBoolCast(v.args[0]);
        const Value& a = cg_.values.at(arg);
        if (a.kind == K::Binary && isComparison(a.op) && !a.type.is_scalar()) {
          for (uint32_t x : a.args) {
            if (!isFloatType(cg_.values.at(x).type)) throw Unsupported("intrinsic argument");
            collect(x);
          }
        } else if (isFloatType(a.type) || isScalarBool(arg)) {
          collect(arg);
        } else {
          throw Unsupported("intrinsic argument");
        }
        return;
      }
      if (const MatMul mm = matMul(v); mm.matrix) {
        collect(mm.vector);
        if (mm.matrix->kind == K::Construct)
          for (uint32_t a : mm.matrix->args) collect(a);
        return;
      }
      const auto op = opFromCall(v.name, static_cast<uint8_t>(v.args.size()));
      const bool sugar = !op && isSugarCall(v.name, v.args.size());
      if (!op && !sugar) throw Unsupported("intrinsic " + v.name);
      if (!isFloatType(v.type)) throw Unsupported(nonFloat("non-float intrinsic", v.type));
      if (sugar && v.name == "cross" && v.type.rows != 3) throw Unsupported("intrinsic cross");
      if (sugar && (v.name == "log10" || v.name == "tan")) usesPow_ = true;
      if (op && (*op == Op::Pow || *op == Op::Exp || *op == Op::Log || *op == Op::Exp2 || *op == Op::Log2 ||
          *op == Op::Sin || *op == Op::Cos))
        usesPow_ = true;
      for (uint32_t a : v.args) {
        const auto ai = cg_.values.find(a);
        if (ai == cg_.values.end() || !isFloatType(ai->second.type)) throw Unsupported("intrinsic argument");
        collect(a);
      }
      return;
    }
    case K::Construct:
      if (!isFloatType(v.type) && !(intMode_ && v.type.is_integral() && !v.type.is_matrix()))
        throw Unsupported("matrix/non-float constructor");
      for (uint32_t a : v.args) {
        const auto ai = cg_.values.find(a);
        if (ai == cg_.values.end() || (!isFloatType(ai->second.type) && !intMode_))
          throw Unsupported("constructor argument");
        collect(a);
      }
      return;
    case K::Call: {
      // Inlined if it can be; else a pure call written in the statement is an input by its text, like a texture read
      // (a helper reading a texture, with a loop, ...; 2026-10-26).
      const std::vector<Leaf> before = leaves_;
      auto outer = collected_;
      try {
        const CallScope scope(*this, v);
        collected_.clear();
        collect(scope.ret);
        collected_ = std::move(outer);
      } catch (const Unsupported&) {
        const auto t = callText_.find(id);
        if (t == callText_.end() || !isFloatType(v.type) || !pureCall(v.name)) throw;
        leaves_ = before;
        collected_ = std::move(outer);
        collected_.insert(id);
        callLeaf_.insert(id);
        Leaf* l = nullptr;
        for (auto& x : leaves_)
          if (x.fetch && x.prefix == t->second) l = &x;
        if (!l) {
          Leaf n;
          n.prefix = t->second;
          n.type = v.type;
          n.loadValue = id;
          n.fetch = true;  // named by its text, its range from the helper's returns
          n.call = true;
          leaves_.push_back(n);
          l = &leaves_.back();
        }
        l->mask |= (1u << l->type.rows) - 1;
      }
      return;
    }
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
      // float1 operands, so the IR keeps the scalar; any component of it (float4(a.xxx, 1): .y of a.xxx) is the
      // scalar too.
      const bool broadcast = width(nt) == 1;
      if (!identity && !broadcast) node = b.swizzle(node, sw, n);
    } else if (op.op == O::op_cast) {
      // float -> floatN broadcast: componentwise ops broadcast float1 operands. A bool read as a float is 1.0 or 0.0.
      if (op.from.is_boolean() && op.from.is_scalar() && op.to.is_floating_point() && b.nodes()[node].type == Type::Bool)
        node = b.op(Op::Select, node, b.constant(1.0f), b.constant(0.0f));
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

void Extractor::collectSynth(const Synth& s) {
  for (const uint32_t c : s.conds) collectCond(c & ~kCondFalse);
  collect(s.a);
  collect(s.b);
  if (s.kind == Synth::Kind::Merge) chainMask(s.chain, 0, 4);  // validates
}

uint32_t Extractor::buildSynth(const Synth& s, ExprBuilder& b) {
  if (s.kind == Synth::Kind::Select) {
    uint32_t c = 0;
    for (const uint32_t k : s.conds) {
      uint32_t n = buildCond(k & ~kCondFalse, b);
      if (k & kCondFalse) n = b.op(Op::LNot, n);
      c = c ? b.op(Op::LAnd, c, n) : n;
    }
    uint32_t x[2] = {build(s.a, b), build(s.b, b)};
    scalarizeConstants(x, 2, b);
    return b.op(Op::Select, c, x[0], x[1]);
  }
  // Merge: the old value's components, the written ones from the new value (a swizzle or one index).
  const uint32_t old = build(s.b, b), nw = build(s.a, b);
  const reshadefx::expression::operation& op = s.chain[0];
  int from[4] = {-1, -1, -1, -1};  // component -> component of the new value, -1: the old one
  if (op.op == reshadefx::expression::operation::op_constant_index) {
    if (op.index >= s.rows) throw Unsupported("function call: writes part of a variable");
    from[op.index] = 0;
  } else {
    for (int k = 0; k < 4 && op.swizzle[k] >= 0; ++k) {
      if (op.swizzle[k] >= static_cast<int>(s.rows)) throw Unsupported("function call: writes part of a variable");
      from[op.swizzle[k]] = k;
    }
  }
  const unsigned newRows = width(b.nodes()[nw].type), oldRows = width(b.nodes()[old].type);
  uint32_t parts[4];
  for (unsigned j = 0; j < s.rows; ++j) {
    const bool written = from[j] >= 0;
    const uint32_t src = written ? nw : old;
    const unsigned comp = written ? static_cast<unsigned>(from[j]) : j;
    if ((written ? newRows : oldRows) == 1) {
      parts[j] = src;  // a scalar (broadcast)
    } else {
      const uint8_t c = static_cast<uint8_t>(comp);
      parts[j] = b.swizzle(src, &c, 1);
    }
  }
  return s.rows == 1 ? parts[0] : b.construct(parts, s.rows);
}

uint32_t Extractor::build(uint32_t id, ExprBuilder& b) {
  if (auto it = built_.find(id); it != built_.end()) return it->second;
  if (const auto s = synth_.find(id); s != synth_.end()) {
    const uint32_t r = buildSynth(s->second, b);
    built_[id] = r;
    return r;
  }
  const Value& v = cg_.values.at(id);
  using K = Value::Kind;
  uint32_t r = 0;
  switch (v.kind) {
    case K::Const: {
      if (intOps_) {
        r = b.constantU(v.constant.as_uint[0]);  // an int's bits
        break;
      }
      float c[4];
      for (unsigned k = 0; k < v.type.rows; ++k)
        c[k] = v.type.is_floating_point() ? v.constant.as_float[k]
               : v.type.is_signed()       ? static_cast<float>(v.constant.as_int[k])
                                          : static_cast<float>(v.constant.as_uint[k]);  // intMode_
      r = b.constant(floatType(v.type.rows), c);
      break;
    }
    case K::Load: {
      if (const auto it = inlineLoad_.find(id); it != inlineLoad_.end()) {
        r = applyVectorChain(build(it->second, b), v.chain, 0, b, nullptr);
        break;
      }
      if (const auto it = inline_.find(v.base); it != inline_.end()) {
        r = applyVectorChain(build(it->second, b), v.chain, 0, b, nullptr);
        break;
      }
      size_t vs = 0;
      const Leaf& l = leafOf(v, vs);
      r = l.uintLeaf ? b.input(l.input, Type::Uint)
                     : applyVectorChain(b.input(l.input, floatType(std::popcount(l.mask))), v.chain, vs, b, &l);
      break;
    }
    case K::Chain:
      if (fetchText_.count(v.base)) {
        const Leaf& l = fetchLeaf(v.base);
        r = applyVectorChain(b.input(l.input, floatType(std::popcount(l.mask))), v.chain, 0, b, &l);
      } else if (intCast(v) && intScalar(v)) {
        ++intMode_;
        ++intOps_;
        const uint32_t n = build(v.base, b);
        --intOps_;
        --intMode_;
        r = b.op(v.chain[0].from.is_signed() ? Op::IToF : Op::UToF, n);
      } else if (intCast(v)) {
        ++intMode_;
        r = build(v.base, b);  // already float: the conversion is the identity
        --intMode_;
      } else {
        r = applyVectorChain(build(v.base, b), v.chain, 0, b, nullptr);
      }
      break;
    case K::Unary:
      if (intOps_) {
        r = v.op == tokenid::minus ? b.op(Op::USub, b.constantU(0), build(v.args[0], b)) : build(v.args[0], b);
        break;
      }
      r = v.op == tokenid::minus     ? b.op(Op::Neg, build(v.args[0], b))
          : v.op == tokenid::exclaim ? b.op(Op::LNot, build(v.args[0], b))
                                     : build(v.args[0], b);
      break;
    case K::Binary: {
      Op op = Op::Add;
      switch (v.op) {
        case tokenid::ampersand_ampersand: op = Op::LAnd; break;
        case tokenid::pipe_pipe: op = Op::LOr; break;
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
        case tokenid::percent: case tokenid::percent_equal: {  // float x % y: HLSL fmod, as fxc writes it
          uint32_t a[2] = {build(v.args[0], b), build(v.args[1], b)};
          scalarizeConstants(a, 2, b);
          r = buildSugarCall(b, "fmod", a, 2);
          built_[id] = r;
          return r;
        }
        default: throw Unsupported("binary operator");
      }
      if (intOps_) {
        op = op == Op::Add ? Op::UAdd : op == Op::Sub ? Op::USub : Op::UMul;  // collect allows + - * only
        r = b.op(op, build(v.args[0], b), build(v.args[1], b));
        break;
      }
      const bool ints = intCompare(v);
      intMode_ += ints;
      uint32_t a[2] = {build(v.args[0], b), build(v.args[1], b)};
      intMode_ -= ints;
      if (info(op).shape == Shape::Comp) scalarizeConstants(a, 2, b);
      r = b.op(op, a[0], a[1]);
      break;
    }
    case K::Ternary: {
      if (const Value* cmp = vectorCompare(v)) {
        // c < 0.5 ? x : y on vectors: a select per component (GPUs pick per component too).
        const uint32_t x = build(cmp->args[0], b), y = build(cmp->args[1], b), t = build(v.args[1], b),
                       f = build(v.args[2], b);        auto comp = [&](uint32_t n, unsigned k) {
          const uint8_t sw[1] = {static_cast<uint8_t>(k)};
          const Node& c = b.nodes()[n];
          if (width(c.type) == 1) return n;
          return c.op == Op::Const ? b.constant(c.value[k]) : b.swizzle(n, sw, 1);  // a constant's component
        };
        uint32_t parts[4];
        for (unsigned k = 0; k < v.type.rows; ++k) {
          uint32_t a[2] = {comp(t, k), comp(f, k)};
          scalarizeConstants(a, 2, b);
          parts[k] = b.op(Op::Select, b.op(comparisonOp(cmp->op), comp(x, k), comp(y, k)), a[0], a[1]);
        }
        r = b.construct(parts, v.type.rows);
        break;
      }
      uint32_t a[2] = {build(v.args[1], b), build(v.args[2], b)};
      scalarizeConstants(a, 2, b);
      r = b.op(Op::Select, buildCond(v.args[0], b), a[0], a[1]);
      break;
    }
    case K::Intrinsic: {
      if (fetchText_.count(id)) {
        const Leaf& l = fetchLeaf(id);
        r = b.input(l.input, floatType(std::popcount(l.mask)));
        break;
      }
      if ((v.name == "all" || v.name == "any") && v.args.size() == 1) {
        const uint32_t arg = floatBeforeBoolCast(v.args[0]);
        const Value& a = cg_.values.at(arg);
        if (a.kind == K::Binary && isComparison(a.op) && !a.type.is_scalar()) {  // all(x > y): per component
          const uint32_t x = build(a.args[0], b), y = build(a.args[1], b);
          const Op cmp = comparisonOp(a.op);
          auto comp = [&](uint32_t n, unsigned k) {
            const uint8_t sw[1] = {static_cast<uint8_t>(k)};
            return width(b.nodes()[n].type) == 1 ? n : b.swizzle(n, sw, 1);
          };
          for (unsigned k = 0; k < a.type.rows; ++k) {
            const uint32_t t = b.op(cmp, comp(x, k), comp(y, k));
            r = k ? b.op(v.name == "all" ? Op::LAnd : Op::LOr, r, t) : t;
          }
          break;
        }
        const uint32_t x = build(arg, b);
        r = buildSugarCall(b, v.name, &x, 1);
        break;
      }
      if (const MatMul mm = matMul(v); mm.matrix) {
        // One dot per result component: a row of M (mul(M, v)) or a column (mul(v, M)), as compilers emit it.
        const reshadefx::type& t = mm.matrix->type;
        const uint32_t x = build(mm.vector, b);
        const unsigned outs = mm.rowsFirst ? t.rows : t.cols, inner = mm.rowsFirst ? t.cols : t.rows;
        uint32_t res[4];
        for (unsigned o = 0; o < outs; ++o) {
          uint32_t e[4];
          float c[4];
          bool constant = mm.matrix->kind == K::Const;
          for (unsigned k = 0; k < inner; ++k) {
            const unsigned at = mm.rowsFirst ? o * t.cols + k : k * t.cols + o;
            if (constant) c[k] = mm.matrix->constant.as_float[at];
            else e[k] = build(mm.matrix->args[at], b);
          }
          const uint32_t line = constant ? b.constant(floatType(inner), c) : b.construct(e, inner);
          res[o] = b.op(Op::Dot, line, x);
        }
        r = outs == 1 ? res[0] : b.construct(res, outs);
        break;
      }
      if (isSugarCall(v.name, v.args.size())) {
        uint32_t a[2] = {0, 0};
        for (size_t k = 0; k < v.args.size(); ++k) a[k] = build(v.args[k], b);
        r = buildSugarCall(b, v.name, a, v.args.size());
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
    case K::Call: {
      // The callee's values are shared by every call: built with this call's bindings in a map of their own (the
      // DAG merges what the calls have in common).
      if (callLeaf_.count(id)) {
        const auto t = callText_.find(id);
        const Leaf* l = nullptr;
        for (const auto& x : leaves_)
          if (x.fetch && x.prefix == t->second) l = &x;
        r = b.input(l->input, floatType(std::popcount(l->mask)));
        break;
      }
      const CallScope scope(*this, v);
      auto outer = std::move(built_);
      built_.clear();
      r = build(scope.ret, b);
      built_ = std::move(outer);
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
  if (si == cg_.samplers.end()) {
    // A read of an HLSL RW texture / buffer (a storage object): the user's range for it.
    const std::string key = storageFactKey(valueId);
    const auto u = opt_.userRanges && !key.empty() ? opt_.userRanges->find(key) : UserRanges::const_iterator();
    if (!key.empty() && opt_.userRanges && u != opt_.userRanges->end())
      return Range::of(u->second.first, u->second.second, "user (facts file)");
    return Range::unknown();
  }
  const SamplerInfo& s = si->second;
  using F = reshadefx::texture_format;
  const std::string sem = upper(s.textureSemantic);
  if ((sem == "COLOR" || sem == "SV_TARGET") && opt_.hdrBackBuffer)
    return Range::of(kScRgbLo, kScRgbHi, "BackBuffer (scRGB)");
  if (sem == "COLOR" || sem == "SV_TARGET")
    return s.srgb ? Range::of(0, 1, "BackBuffer (sRGB sampler)")
                  : Range::of(0, 1, "BackBuffer (8-bit SDR assumed)", 255);
  if (sem == "DEPTH") return Range::of(0, 1, "depth buffer");
  switch (s.format) {
    case F::r8: case F::rg8: case F::rgba8:
      return s.srgb ? Range::of(0, 1, "8-bit texture (sRGB)") : Range::of(0, 1, "8-bit texture", 255);
    case F::rgb10a2: return Range::of(0, 1, "10-bit texture", 1023);
    case F::r16: case F::rg16: case F::rgba16: return Range::of(0, 1, "16-bit unorm texture");
    default: break;
  }
  // A float or unknown format (every HLSL texture): the user's range for the texture.
  if (opt_.userRanges) {
    const auto u = opt_.userRanges->find(textureFactKey(fx_, s.textureName));
    if (u != opt_.userRanges->end()) return Range::of(u->second.first, u->second.second, "user (facts file)");
  }
  // Plain HLSL / GLSL: the format is not in the shader. A likely [0, 1] (most textures are color or unorm data; see
  // Fact::likely). ReShade FX declares float formats, which keep no range.
  if (opt_.likelyRanges && (fx_.hlsl || fx_.glsl)) {
    Range r = Range::of(0, 1, "likely: texture read, format unknown");
    r.likely = true;
    r.likelyWhy = "texture read, format unknown";
    return r;
  }
  return Range::unknown();
}

Range Extractor::range(uint32_t id) {
  auto& memo = sizeFix_ < 0 ? rangeMemo_ : sizeMemo_[sizeFix_];
  if (auto it = memo.find(id); it != memo.end()) return it->second;
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
      // All components; for an array, of every element (an index may pick any).
      std::function<void(const reshadefx::constant&)> add = [&](const reshadefx::constant& c) {
        for (unsigned k = 0; k < v.type.components() && k < 16; ++k) {
          const double x = v.type.base == reshadefx::type::t_float ? c.as_float[k]
                           : v.type.base == reshadefx::type::t_int ? c.as_int[k]
                                                                   : c.as_uint[k];
          lo = std::min(lo, x);
          hi = std::max(hi, x);
        }
        for (const auto& e : c.array_data) add(e);
      };
      if (v.type.is_array() && !v.constant.array_data.empty()) {
        for (const auto& e : v.constant.array_data) add(e);
      } else {
        add(v.constant);
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
    case K::Call: {
      // What its returns can give (its parameters' ranges are the union over its call sites).
      const Function* f = cg_.function(v.name);
      if (!f || !callBusy_.insert(v.name).second) break;
      bool any = false;
      for (const Statement& s : f->stmts)
        if (s.kind == Statement::Kind::Return) {
          const Range x = range(s.value);
          r = any ? unite(r, x) : x;
          any = true;
        }
      callBusy_.erase(v.name);
      break;
    }
  }
  if (r.known && !(std::isfinite(r.lo) && std::isfinite(r.hi))) r = Range::unknown();
  // Integer values truncate (casts, integer division: int(1.5) = 3 / 2 = 1): widen to whole numbers.
  // A cast to int inside a chain counts too (float m = int(x): float(int(x)) in one chain).
  bool truncates = v.type.is_integral();
  for (const auto& op : v.chain)
    truncates = truncates || (op.op == reshadefx::expression::operation::op_cast && op.to.is_integral() &&
                              op.from.is_floating_point());
  if (r.known && truncates && (std::floor(r.lo) != r.lo || std::ceil(r.hi) != r.hi))
    r = derived(std::floor(r.lo), std::ceil(r.hi), {&r});
  memo[id] = r;
  return r;
}

// Common back buffer sizes (width, height), from small to 8K, ultrawide and portrait.
constexpr unsigned kBufferSizes[][2] = {{640, 480},   {800, 600},   {1280, 720},  {1366, 768},
                                        {1920, 1080}, {2560, 1080}, {2560, 1440}, {3440, 1440},
                                        {3840, 2160}, {5120, 1440}, {7680, 4320}, {1080, 1920}};

Range Extractor::leafRange(uint32_t id) {
  Range r = range(id);
  if (!fx_.bufferSymbolic) return r;
  constexpr size_t n = sizeof(kBufferSizes) / sizeof(kBufferSizes[0]);
  sizeMemo_.resize(n);
  Range u;
  for (size_t k = 0; k < n; ++k) {
    sizeFix_ = static_cast<int>(k);
    const Range rk = range(id);
    u = k == 0 ? rk : unite(u, rk);
  }
  sizeFix_ = -1;
  if (!u.known || (r.known && u.lo <= r.lo && u.hi >= r.hi)) return r;
  u.why = "derived (at common back buffer sizes)";
  return u;
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
  if (isSymbolic(var)) return "macro global " + varText(var);  // preprocessor definition
  return pathFrom(v.loc.source).filename().string() + " " + function + " " +
         (v.kind == Variable::Kind::Uniform ? varText(var, false) : v.name);
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

// A loop counter (owner, 2026-10-26: float(i) in a loop body): "for (int i = A; i < B; i++)" (<=, or counting down
// with > / >= and --; A and B numbers or uniforms with ranges, read from the preprocessed line), every store to it in
// the header. [A, B]: B is its value after the loop.
Range Extractor::loopRange(uint32_t var) {
  const Variable& v = cg_.variables.at(var);
  const auto d = defs_.find(var);
  if (v.kind != Variable::Kind::Local || !v.type.is_integral() || !v.type.is_scalar() || d == defs_.end() ||
      d->second.empty())
    return Range::unknown();
  for (const Statement* s : d->second)
    if (s->loc.line != v.loc.line || s->loc.source != v.loc.source) return Range::unknown();
  const auto pl = fx_.ppLines.find({v.loc.source, v.loc.line});
  if (pl == fx_.ppLines.end()) return Range::unknown();
  static const std::regex header(R"(\bfor\s*\(\s*(?:int|uint)\s+(\w+)\s*=\s*([^;]+);)"
                                 R"(\s*(\w+)\s*(<=|<|>=|>)\s*([^;]+);([^)]*)\))");
  std::smatch m;
  if (!std::regex_search(pl->second, m, header) || m[1] != v.name || m[3] != v.name) return Range::unknown();
  std::string step;
  for (const char c : m[6].str())
    if (!std::isspace(static_cast<unsigned char>(c))) step += c;
  const bool up = step == v.name + "++" || step == "++" + v.name || step == v.name + "+=1";
  const bool down = step == v.name + "--" || step == "--" + v.name || step == v.name + "-=1";
  const std::string cmp = m[4];
  if (!(up && cmp[0] == '<') && !(down && cmp[0] == '>')) return Range::unknown();
  auto value = [&](const std::string& text) {
    const std::string t = trim(text);
    char* end = nullptr;
    const double x = std::strtod(t.c_str(), &end);
    if (!t.empty() && (*end == '\0' || ((*end == 'u' || *end == 'U') && end[1] == '\0'))) return Range::of(x, x, "");
    for (const auto& [id, u] : cg_.variables)
      if (u.kind == Variable::Kind::Uniform && !isSymbolic(id) && varText(id) == t) return varRange(id, 0, 0);
    return Range::unknown();
  };
  const Range a = value(m[2]), b = value(m[5]);
  if (!a.known || !b.known) return Range::unknown();
  const double lo = up ? a.lo : std::min(a.lo, b.lo - (cmp == ">=" ? 1 : 0));
  const double hi = up ? std::max(a.hi, b.hi + (cmp == "<=" ? 1 : 0)) : a.hi;
  if (std::floor(lo) != lo || std::floor(hi) != hi || hi - lo > 1e6) return Range::unknown();
  return Range::of(lo, hi, "loop counter", static_cast<uint32_t>(hi - lo));
}

Range Extractor::varRangeRaw(uint32_t var, uint32_t seq, uint32_t block) {
  const auto vi = cg_.variables.find(var);
  if (vi == cg_.variables.end()) return Range::unknown();
  const Variable& v = vi->second;
  // BUFFER_WIDTH / BUFFER_HEIGHT: integers up to the --max-width limit.
  if (isSymbolic(var) && isBufferSizeMacro(varText(var))) {
    if (sizeFix_ >= 0) {
      const double x = kBufferSizes[sizeFix_][varText(var) == "BUFFER_WIDTH" ? 0 : 1];
      return Range::of(x, x, "back buffer size");
    }
    return Range::of(1, opt_.maxWidth, "back buffer size", static_cast<uint32_t>(opt_.maxWidth - 1));
  }
  // A preprocessor definition: the user can set any value, no fact.
  if (isSymbolic(var)) return Range::unknown();
  switch (v.kind) {
    case Variable::Kind::Uniform: {
      double lo = NAN, hi = NAN;
      std::string uiType, source, items;
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
        else if (a.name == "ui_items") items = a.value.string_data;
      }
      // An int combo / radio list: one value per item, 0 to items - 1 (items end with '\0').
      if (v.type.is_integral() && !v.type.is_boolean() && (uiType == "combo" || uiType == "radio") && !items.empty() &&
          !(std::isfinite(lo) && std::isfinite(hi))) {
        const double n = static_cast<double>(std::count(items.begin(), items.end(), '\0') + (items.back() != '\0'));
        return Range::of(0, n - 1, "ui_items", static_cast<uint32_t>(n - 1));
      }
      if (!source.empty()) {
        if (source == "timer") return Range::of(0, 1e7, "source = timer (ms)");
        if (source == "frametime") return Range::of(0, 1000, "source = frametime (ms)");
        if (source == "pingpong") return Range::of(lo == lo ? lo : 0, hi == hi ? hi : 1, "source = pingpong");
        return Range::unknown();
      }
      if (std::isfinite(lo) && std::isfinite(hi)) {
        const double steps = std::abs(hi - lo);  // an int slider takes whole numbers
        return Range::of(std::min(lo, hi), std::max(lo, hi), "ui_min/ui_max",
                         v.type.is_integral() && steps <= 1e6 ? static_cast<uint32_t>(steps) : 0);
      }
      if (uiType == "color") return Range::of(0, 1, "ui_type = color");
      if (v.type.is_boolean()) return Range::of(0, 1, "bool", 1);
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
          return Range::of(0, opt_.maxWidth, "SV_Position (pixels, up to --max-width)");
        if (f->type == reshadefx::shader_type::compute && computeSemantic(sem))
          return computeInputRange(*f, sem, opt_.maxWidth);
        // Pixel shader input: what the vertex shaders of its passes write, else the
        // semantic's convention.
        if (f->type == reshadefx::shader_type::pixel) {
          const Range r = pixelInputRange(*f, sem);
          return r.known ? r : semanticConvention(sem, opt_.maxWidth);
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
      if (const Range lr = loopRange(var); lr.known) return lr;
      bool killed = false;
      const std::vector<const Statement*> reach = reaching(var, seq, block, killed);
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
    case Variable::Kind::Global:
      // Groupshared memory: what the threads store there has no single statement to follow,
      // so a range only from the user (facts file key "<file> groupshared <name>").
      if (v.type.has(reshadefx::type::q_groupshared) && opt_.userRanges) {
        const auto u = opt_.userRanges->find(groupsharedFactKey(fx_, globalSourceName(v.uniqueName)));
        if (u != opt_.userRanges->end()) return Range::of(u->second.first, u->second.second, "user (facts file)");
      }
      // A static const: its initializer (an array: every element).
      if (v.type.has(reshadefx::type::q_const) && v.initializer) {
        Range r = range(v.initializer);
        if (r.known) r.why = "static const";
        return r;
      }
      break;
  }
  return Range::unknown();
}

// Reaching definitions of a load: the last one before it if it is in the load's block (it kills the others), else
// all earlier ones plus those later in a loop around the load (and the entry value, the caller's part).
std::vector<const Statement*> Extractor::reaching(uint32_t var, uint32_t seq, uint32_t block, bool& killed) const {
  std::vector<const Statement*> reach;
  killed = false;
  const auto di = defs_.find(var);
  if (di == defs_.end()) return reach;
  const Statement* last = nullptr;
  for (const Statement* d : di->second)
    if (d->seq < seq && (!last || d->seq > last->seq)) last = d;
  if (last && last->block == block && last->chain.empty()) {
    reach.push_back(last);
    killed = true;
    return reach;
  }
  for (const Statement* d : di->second) {
    bool inLoop = false;
    for (const auto& [lo, hi] : cg_.loops) inLoop = inLoop || (lo <= seq && seq <= hi && lo <= d->seq && d->seq <= hi);
    if (d->seq < seq || inLoop) reach.push_back(d);
  }
  return reach;
}

int Extractor::usesOf(const Statement& s) {
  if (loadsByVar_.empty())
    for (const auto& [id, v] : cg_.values)
      if (v.kind == Value::Kind::Load) loadsByVar_[v.base].push_back(id);
  int n = 0;
  if (const auto it = loadsByVar_.find(s.var); it != loadsByVar_.end())
    for (const uint32_t id : it->second) {
      const Value& v = cg_.values.at(id);
      bool killed = false;
      for (const Statement* d : reaching(s.var, v.seq, v.block, killed)) n += d == &s;
    }
  return n;
}

bool Extractor::chainPerDraw(const std::vector<reshadefx::expression::operation>& chain) {
  for (const auto& op : chain)
    if (op.op == reshadefx::expression::operation::op_dynamic_index && !perDraw(op.index)) return false;
  return true;
}

// Constants, uniforms without a source annotation, preprocessor values, static const globals, and math of them;
// a local counts when every store reaching the read is such a value under such conditions (LumaSharpen's
// sharp_strength_luma = CoefLuma * sharp_strength, scaled under if (pattern == 2); owner, 2026-10-25). Function
// parameters and calls do not (conservative).
bool Extractor::perDraw(uint32_t id) {
  if (const auto m = perDrawMemo_.find(id); m != perDrawMemo_.end()) return m->second;
  const auto it = cg_.values.find(id);
  if (it == cg_.values.end() || !perDrawBusy_.insert(id).second) return false;
  const Value& v = it->second;
  bool r = true;
  switch (v.kind) {
    case Value::Kind::Const: break;
    case Value::Kind::Call: r = false; break;
    case Value::Kind::Chain: r = perDraw(v.base) && chainPerDraw(v.chain); break;
    case Value::Kind::Load: {
      const auto vi = cg_.variables.find(v.base);
      r = vi != cg_.variables.end() && chainPerDraw(v.chain);
      if (!r || isSymbolic(v.base)) break;
      const Variable& var = vi->second;
      switch (var.kind) {
        case Variable::Kind::Uniform:
          for (const auto& a : var.annotations) r = r && a.name != "source";
          break;
        case Variable::Kind::Global:
          r = var.type.has(reshadefx::type::q_const) && var.initializer && perDraw(var.initializer);
          break;
        case Variable::Kind::Param: r = false; break;
        case Variable::Kind::Local: {
          bool killed = false;
          const std::vector<const Statement*> reach = reaching(v.base, v.seq, v.block, killed);
          bool full = false;
          for (const Statement* d : reach) {
            full = full || d->chain.empty();
            r = r && perDraw(d->value) && chainPerDraw(d->chain);
            if (const auto c = cg_.blockConds.find(d->block); c == cg_.blockConds.end()) r = false;
            else
              for (const uint32_t cond : c->second) r = r && perDraw(cond & ~kCondFalse);
          }
          r = r && full;
          break;
        }
      }
      break;
    }
    default:
      for (const uint32_t a : v.args) r = r && perDraw(a);
      break;
  }
  perDrawBusy_.erase(id);
  perDrawMemo_[id] = r;
  return r;
}

bool Extractor::outOnly(const Variable& v) {
  return v.type.has(reshadefx::type::q_out) && !v.type.has(reshadefx::type::q_in);
}

// Compute shader inputs: integer IDs, one interval for all components (ranges are per
// variable). Dispatch thread and group IDs are bounded like SV_Position (a dispatch covers at
// most a --max-width wide target); group thread IDs and the group index by the group size.
Range computeInputRange(const Function& cs, const std::string& semantic, double maxWidth) {
  const std::string sem = upper(semantic);
  const int x = std::max(cs.numThreads[0], 1), y = std::max(cs.numThreads[1], 1), z = std::max(cs.numThreads[2], 1);
  if (sem == "SV_DISPATCHTHREADID") return Range::of(0, maxWidth, "SV_DispatchThreadID (up to --max-width)", 1);
  if (sem == "SV_GROUPID") return Range::of(0, maxWidth, "SV_GroupID (up to --max-width)", 1);
  if (sem == "SV_GROUPTHREADID")
    return Range::of(0, std::max({x, y, z}) - 1, "SV_GroupThreadID (numthreads)", 1);
  if (sem == "SV_GROUPINDEX") return Range::of(0, double(x) * y * z - 1, "SV_GroupIndex (numthreads)", 1);
  return Range::unknown();
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
Range semanticConvention(const std::string& semantic, double maxWidth) {
  const std::string key = semanticKey(semantic);
  if (key == "SV_POSITION0" || key == "VPOS0") return Range::of(0, maxWidth, "SV_Position (pixels, up to --max-width)");
  if (key.rfind("TEXCOORD", 0) == 0) return Range::of(0, 1, "TEXCOORD semantic (convention)");
  return Range::unknown();
}

// How the value stored by a statement is used: in comparisons, as texture coordinates,
// otherwise.
void Extractor::useKinds(uint32_t id, bool& cmp, bool& coord, bool& other, int depth) {
  if (depth > 4) { other = true; return; }
  const auto ui = users_.find(id);
  const auto self = cg_.values.find(id);
  const bool isFloat = self != cg_.values.end() && self->second.type.is_floating_point();
  if (self != cg_.values.end() && self->second.kind == Value::Kind::Load && self->second.type.is_integral()) {
    const auto var = cg_.variables.find(self->second.base);  // a float variable read as int (implicit cast)
    if (var != cg_.variables.end() && var->second.type.is_floating_point()) {
      cmp = true;
      return;
    }
  }
  if (ui != users_.end())
    for (uint32_t u : ui->second) {
      const Value& uv = cg_.values.at(u);
      // Converted to an integer (int(x), int2(x, y), a tex2Dfetch coordinate): truncation is a threshold.
      if (isFloat && uv.type.is_integral() && (uv.kind == Value::Kind::Chain || uv.kind == Value::Kind::Load ||
                                               uv.kind == Value::Kind::Construct)) {
        cmp = true;
        continue;
      }
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
      if (!hasVar(*s)) { other = true; continue; }
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
  if (s.kind == Statement::Kind::Write) {
    // A compute shader's store: an 8-bit storage texture quantizes (and clamps) like a render target.
    using F = reshadefx::texture_format;
    uint32_t storage = s.storage;  // the storage object, or a load of it
    if (const auto lv = cg_.values.find(storage); lv != cg_.values.end() && lv->second.kind == Value::Kind::Load)
      storage = lv->second.base;
    for (const auto& st : cg_.mod().storages)
      if (st.id == storage)
        for (const auto& tex : cg_.mod().textures)
          if (tex.unique_name == st.texture_name &&
              (tex.format == F::rgba8 || tex.format == F::r8 || tex.format == F::rg8)) {
            b.kind = Budget::Kind::Color8;
            b.maxCodeDiff = 0;
            reason = "compute shader output, 8-bit storage texture";
            return b;
          }
    b.kind = Budget::Kind::Rel;
    b.eps = opt_.relEps;
    reason = "stored to a resource";
    return b;
  }
  if (s.kind == Statement::Kind::Return) {
    // A pixel shader's result written to an 8-bit target without blending.
    if (f.type == reshadefx::shader_type::pixel && !f.returnType.is_struct()) {
      bool eightBit = true, anyPass = false, backBuffer = false;
      for (const auto& t : cg_.mod().techniques)
        for (const auto& p : t.passes) {
          if (p.ps_entry_point != f.uniqueName) continue;
          anyPass = true;
          if (p.blend_enable[0] || p.srgb_write_enable) eightBit = false;
          if (p.render_target_names[0].empty()) backBuffer = true;
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
        // The back buffer is assumed 8-bit SDR here; sopt-fx also checks 10-bit and scRGB
        // (the reason names the back buffer).
        reason = backBuffer ? "pixel shader output, back buffer (8-bit SDR), no blending"
                            : "pixel shader output, 8-bit target, no blending";
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
    b.width = opt_.maxWidth;
    b.eps = opt_.texcoordPx / opt_.maxWidth;
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
  std::string name;
  if (hasVar(s)) {
    const auto vi = cg_.variables.find(s.var);
    if (vi == cg_.variables.end() || vi->second.kind == Variable::Kind::Global) {
      why = "store to a global";
      return false;
    }
    name = vi->second.name;
  }
  // Where the statement starts on its line (the location is the parsed text's: used when the source has the expected
  // word there): Init at its type before the name (or the name, after "," of a declarator list), Store at the
  // variable, Return at "return", Write at the call. 0: the line's first non-blank character.
  size_t start = 0;
  bool declarator = false;
  if (s.loc.line >= 1 && s.loc.line <= lines->size()) {
    const std::string& l = (*lines)[s.loc.line - 1];
    auto wordAt = [&](size_t at, const std::string& w) {
      return !w.empty() && l.compare(at, w.size(), w) == 0 && (at == 0 || !isIdent(l[at - 1])) &&
             (at + w.size() >= l.size() || !isIdent(l[at + w.size()]));
    };
    // The word w at or before the location (a declaration's location is its "="). The parsed line may be indented
    // differently (included files): the same text, shifted. Another parsed text (macros): the word if the line has
    // it once.
    const auto pl = fx_.ppLines.find({file, s.loc.line});
    const bool same = s.loc.column >= 1 && pl != fx_.ppLines.end() && trim(pl->second) == trim(l);
    const size_t col = !same ? std::string::npos
                             : s.loc.column - 1 + l.find_first_not_of(" \t") -
                                   std::min<size_t>(pl->second.find_first_not_of(" \t"), s.loc.column - 1);
    auto find = [&](const std::string& w) {
      size_t at = w.empty() ? std::string::npos : l.rfind(w, col);
      if (same) {
        while (at != std::string::npos && !wordAt(at, w)) at = at ? l.rfind(w, at - 1) : std::string::npos;
        return at;
      }
      size_t found = std::string::npos;
      for (; at != std::string::npos; at = at ? l.rfind(w, at - 1) : std::string::npos)
        if (wordAt(at, w)) {
          if (found != std::string::npos) return std::string::npos;  // twice
          found = at;
        }
      return found;
    };
    const size_t at = find(s.kind == Statement::Kind::Return ? "return" : name);
    if (at != std::string::npos && (s.kind == Statement::Kind::Store || s.kind == Statement::Kind::Return)) {
      start = at;
    } else if (at != std::string::npos && s.kind == Statement::Kind::Init) {
      size_t k = at;
      while (k > 0 && (isIdent(l[k - 1]) || l[k - 1] == ' ' || l[k - 1] == '\t')) --k;
      declarator = k > 0 && l[k - 1] == ',';
      start = l.find_first_not_of(" \t", k);
    } else if (same && s.kind == Statement::Kind::Write && !fx_.hlsl && l.compare(col, 3, "tex") == 0 &&
               (col == 0 || !isIdent(l[col - 1]))) {
      start = col;
    }
  }
  StatementText st;
  if (!statementAt(*lines, s.loc.line, st, why, start, s.kind == Statement::Kind::Init)) return false;
  reg.lastLine = st.last;
  reg.original = st.original;
  reg.text = st.text;
  reg.prefix = st.prefix;
  reg.tail = st.tail;
  const std::string& body = st.text;
  if (s.kind == Statement::Kind::Write && fx_.hlsl) {
    // HLSL: Name[index] = <value>; (or op=, then the region is Name[index] op (<value>)).
    char compound = 0;
    const size_t eq = assignmentOp(body, compound);
    const std::string lhs = eq == std::string::npos ? std::string() : trim(body.substr(0, eq));
    const size_t open = lhs.find('[');
    if (open == std::string::npos || lhs.back() != ']' || !fx_.hlslFetchNames.count(trim(lhs.substr(0, open)))) {
      why = "resource write shape";
      return false;
    }
    reg.kind = Region::Kind::Write;
    reg.lhs = lhs + " =";
    name = "r";
  } else if (s.kind == Statement::Kind::Write) {
    // texNDstore(storage, coord, <value>): the value is the third argument.
    const size_t open = body.find('(');
    const std::string callee = trim(body.substr(0, open == std::string::npos ? 0 : open));
    if (callee != "tex1Dstore" && callee != "tex2Dstore" && callee != "tex3Dstore") {
      why = "resource write not at statement start";
      return false;
    }
    int depth = 0, commas = 0;
    size_t valueStart = std::string::npos, close = std::string::npos;
    for (size_t i = open; i < body.size(); ++i) {
      const char c = body[i];
      if (c == '(' || c == '[') ++depth;
      else if (c == ')' || c == ']') {
        if (--depth == 0) { close = i; break; }
      } else if (c == ',' && depth == 1 && ++commas == 2) valueStart = i + 1;
    }
    if (valueStart == std::string::npos || close == std::string::npos || commas != 2 ||
        !trim(body.substr(close + 1)).empty()) {
      why = "resource write shape";
      return false;
    }
    reg.kind = Region::Kind::Write;
    reg.lhs = trim(body.substr(0, valueStart));
    reg.rhs = ")";
    name = "r";
  } else if (s.kind == Statement::Kind::Return) {
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
      // "type name", possibly with qualifiers; name last (only the name after "," in a declarator list).
      if (lhs.size() < name.size() || lhs.compare(lhs.size() - name.size(), name.size(), name) != 0 ||
          (declarator ? lhs.size() != name.size()
                      : lhs.size() == name.size() || isIdent(lhs[lhs.size() - name.size() - 1])) ||
          compound || lhs.find_first_of("[]:(") != std::string::npos) {
        why = "declaration shape";
        return false;
      }
      reg.kind = Region::Kind::Init;
    } else {
      // A one-line if / else in front stays as written (FXAA: "if(!doneP) posP.x += ..."; 2026-10-26): only the
      // value is rewritten, so it still runs under the same condition.
      std::string target = lhs;
      if (target.rfind("else", 0) == 0 && target.size() > 4 && !isIdent(target[4])) target = trim(target.substr(4));
      if (target.rfind("if", 0) == 0 && target.size() > 2 && !isIdent(target[2])) {
        const size_t open = target.find_first_not_of(" \t", 2);
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t i = open; open != std::string::npos && target[open] == '(' && i < target.size(); ++i) {
          if (target[i] == '(') ++depth;
          if (target[i] == ')' && --depth == 0) { close = i; break; }
        }
        if (close != std::string::npos) target = trim(target.substr(close + 1));
      }
      if (target.compare(0, name.size(), name) != 0 || (target.size() > name.size() && isIdent(target[name.size()])) ||
          target.find('(') != std::string::npos) {
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
  std::set<std::string> guards;
  std::vector<std::string> ppTokens = memberTokens(withoutFetches(pp));
  std::vector<std::string> srcTokens = sourceTokens(withoutFetches(src), guards);
  const size_t prefixLen = std::strlen(kSymbolicPrefix);
  for (auto& t : ppTokens)
    if (t.rfind(kSymbolicPrefix, 0) == 0) t.erase(0, prefixLen);  // symbolic definitions
  // Only the value is rewritten: a macro left of the assignment stays as written (FXAA's "FxaaFloat x = ...",
  // 2026-10-26).
  auto valueOnly = [&](std::vector<std::string>& t) {
    for (size_t i = 0; i < t.size(); ++i) {
      const bool assign = t[i] == "=" && (i == 0 || (t[i - 1] != "=" && t[i - 1] != "!" && t[i - 1] != "<" &&
                                                     t[i - 1] != ">")) &&
                          (i + 1 == t.size() || t[i + 1] != "=");
      if (assign || (reg.kind == Region::Kind::Return && t[i] == "return")) {
        t.erase(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        return;
      }
    }
  };
  if (reg.kind == Region::Kind::Init || reg.kind == Region::Kind::Store || reg.kind == Region::Kind::Return) {
    valueOnly(ppTokens);
    valueOnly(srcTokens);
  }
  if (srcTokens != ppTokens) {
    // Which macro and of what kind (corpus summaries group the reasons with the name taken out).
    size_t i = 0;
    while (i < srcTokens.size() && i < ppTokens.size() && srcTokens[i] == ppTokens[i]) ++i;
    const std::string m = i < srcTokens.size() ? srcTokens[i] : std::string();
    const char* kind = fx_.userMacros.count(m)                                 ? "a setting"
                       : fx_.objectMacros.count(m)                             ? "object-like"
                       : i + 1 < srcTokens.size() && srcTokens[i + 1] == "(" ? "function-like"
                                                                               : "other";
    why = std::string("uses a macro (") + kind + ") '" + m + "'";
    return false;
  }
  for (const std::string& g : guards) reg.guard = reg.guard.empty() ? g : "(" + reg.guard + ") && (" + g + ")";
  return true;
}

// Tokens of source text with macros expanded where a variant can spell them out: object-like ones such as
// BUFFER_SCREEN_SIZE or BUFFER_RCP_WIDTH made of symbolic definitions, numbers, float types and punctuation (the
// region's IR keeps them symbolic), and macros usable as written (macroUsable: fixed ones, or ones defined inside #if,
// whose condition joins `guards`), function-like ones too (owner, 2026-10-26: maxOf3, AF1_). Other macros stay as
// they are (the statement is then skipped).
std::vector<std::string> Extractor::sourceTokens(const std::string& src, std::set<std::string>& guards) const {
  auto symbolicName = [&](const std::string& t) {
    return fx_.symbolic.count(t) || (fx_.bufferSymbolic && isBufferSizeMacro(t));
  };
  auto plain = [](const std::string& t) {
    if (t.empty()) return false;
    if (!std::isalpha(static_cast<unsigned char>(t[0])) && t[0] != '_') return true;  // number, punctuation
    return t == "float" || t == "float2" || t == "float3" || t == "float4" || t == "int";
  };
  // Function-like macros, in the text: arguments substituted as written, the result scanned again.
  std::function<std::string(const std::string&, int)> calls = [&](const std::string& t, int depth) {
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
      if (!isIdent(t[i]) || (i && (isIdent(t[i - 1]) || t[i - 1] == '.'))) {
        out += t[i];
        continue;
      }
      size_t j = i;
      while (j < t.size() && isIdent(t[j])) ++j;
      const std::string name = t.substr(i, j - i);
      const auto m = fx_.functionMacros.find(name);
      const size_t open = t.find_first_not_of(" \t\n", j);
      const size_t close = open == std::string::npos || t[open] != '(' ? std::string::npos : closeOf(t, open);
      std::vector<std::string> args;
      if (m != fx_.functionMacros.end() && close != std::string::npos) {
        int d = 0;
        size_t a = open + 1;
        for (size_t k = open + 1; k < close; ++k) {
          if (t[k] == '(' || t[k] == '[') ++d;
          if (t[k] == ')' || t[k] == ']') --d;
          if (t[k] == ',' && d == 0) {
            args.push_back(t.substr(a, k - a));
            a = k + 1;
          }
        }
        args.push_back(t.substr(a, close - a));
        if (m->second.first.empty() && args.size() == 1 && trim(args[0]).empty()) args.clear();
      }
      std::set<std::string> g;
      if (m == fx_.functionMacros.end() || close == std::string::npos || depth > 16 ||
          args.size() != m->second.first.size() || !macroUsable(name, g)) {
        out += name;
        i = j - 1;
        continue;
      }
      guards.insert(g.begin(), g.end());
      const std::vector<std::string>& params = m->second.first;
      const std::string& b = m->second.second;
      std::string body;
      for (size_t k = 0; k < b.size(); ++k) {
        if (!isIdent(b[k]) || (k && (isIdent(b[k - 1]) || b[k - 1] == '.'))) {
          body += b[k];
          continue;
        }
        size_t e = k;
        while (e < b.size() && isIdent(b[e])) ++e;
        const auto p = std::find(params.begin(), params.end(), b.substr(k, e - k));
        body += p == params.end() ? b.substr(k, e - k) : " " + args[static_cast<size_t>(p - params.begin())] + " ";
        k = e - 1;
      }
      out += " " + calls(body, depth + 1) + " ";
      i = close;
    }
    return out;
  };
  std::function<bool(const std::string&, int, std::vector<std::string>&, bool&, bool&)> expand =
      [&](const std::string& t, int depth, std::vector<std::string>& out, bool& sym, bool& plainAll) {
        if (symbolicName(t)) {
          out.push_back(t);
          sym = true;
          return true;
        }
        const auto it = fx_.objectMacros.find(t);
        if (it == fx_.objectMacros.end()) {
          out.push_back(t);
          plainAll = plainAll && plain(t);
          return true;
        }
        if (depth > 16) return false;
        for (const auto& u : tokens(it->second))
          if (!expand(u, depth + 1, out, sym, plainAll)) return false;
        return true;
      };
  // Every macro in it usable as written (2026-10-26: FXAA_SUBPIX_CAP, SMAA_AREATEX_PIXEL_SIZE); a variant then
  // spells out the value.
  std::function<bool(const std::string&, int, std::set<std::string>&)> usableAll =
      [&](const std::string& t, int depth, std::set<std::string>& g) {
        if (symbolicName(t)) return true;
        const auto it = fx_.objectMacros.find(t);
        if (it == fx_.objectMacros.end()) return true;
        if (depth > 16 || !macroUsable(t, g)) return false;
        for (const auto& u : tokens(it->second))
          if (!usableAll(u, depth + 1, g)) return false;
        return true;
      };
  std::vector<std::string> out;
  for (const auto& t : memberTokens(withoutFetches(calls(src, 0)))) {
    std::vector<std::string> e;
    std::set<std::string> g;
    bool sym = false, plainAll = true;
    if (!symbolicName(t) && fx_.objectMacros.count(t) && expand(t, 0, e, sym, plainAll) &&
        ((sym && plainAll) || usableAll(t, 0, g))) {
      out.insert(out.end(), e.begin(), e.end());
      guards.insert(g.begin(), g.end());
    } else {
      out.push_back(t);
    }
  }
  return splitMembers(out);
}

// Not a setting the user can change (ReShade's preprocessor definitions are the ones the effect tests with #ifdef /
// #ifndef), and defined once, outside #if blocks (a header's include guard does not count), or not in the sources
// at all (ReShade's own).
bool Extractor::fixedMacro(const std::string& name) const {
  if (fx_.userMacros.count(name)) return false;
  if (macroDefs_.empty()) {
    macroDefs_[""] = {};  // scanned
    for (const auto& file : fx_.sourceFiles) {
      const std::vector<std::string>* lines = sourceLines(file);
      if (!lines) continue;
      std::vector<bool> conditional;  // per open #if: does it make code conditional?
      std::string guard;              // the first directive's #ifndef name (an include guard candidate)
      bool first = true;
      uint32_t lineNo = 0;
      for (const std::string& raw : *lines) {
        ++lineNo;
        const std::string l = trim(raw);
        if (l.empty() || l[0] != '#') continue;
        std::istringstream in(l.substr(1));
        std::string d, arg;
        in >> d >> arg;
        const std::string candidate = first && d == "ifndef" ? arg : std::string();
        if (d == "if" || d == "ifdef" || d == "ifndef") {
          conditional.push_back(true);
        } else if (d == "endif") {
          if (!conditional.empty()) conditional.pop_back();
        } else if (d == "define") {
          const std::string n = arg.substr(0, arg.find('('));
          if (!guard.empty() && n == guard && conditional.size() == 1) {
            conditional.back() = false;  // #ifndef X / #define X: the include guard
          } else {
            auto& def = macroDefs_[n];
            ++def.count;
            for (const bool c : conditional) def.inIf = def.inIf || c;
            def.at.emplace_back(file, lineNo);
          }
        } else if (d == "undef") {
          macroDefs_[arg].undef = true;
        }
        guard = candidate;  // only the directive right after it may complete the guard
        first = false;
      }
    }
  }
  const auto it = macroDefs_.find(name);
  return it == macroDefs_.end() || (it->second.count == 1 && !it->second.inIf && !it->second.undef);
}

// A macro whose expansion a variant may spell out: fixed, or defined inside #if blocks (owner, 2026-10-26:
// _NOISE_DITHERING), then the condition of the definition in effect joins `guards` (the variant only for that build).
// Never a setting the user changes (those are inputs or kept).
bool Extractor::macroUsable(const std::string& name, std::set<std::string>& guards) const {
  if (fixedMacro(name)) return true;
  const auto it = macroDefs_.find(name);
  if (fx_.userMacros.count(name) || it == macroDefs_.end() || it->second.undef) return false;
  // The definition in effect: the one whose replacement list is the current one (redefinitions are errors).
  const auto fm = fx_.functionMacros.find(name);
  const auto om = fx_.objectMacros.find(name);
  if (fm == fx_.functionMacros.end() && om == fx_.objectMacros.end()) return false;
  const std::vector<std::string> want = memberTokens(fm != fx_.functionMacros.end() ? fm->second.second : om->second);
  // Each definition with the condition it is compiled under; one outside #if with this text is in effect wherever no
  // definition with other text is (SweetFX's SMAA.fx defines SMAASamplePoint, SMAA.fxh again under SMAA_HLSL_3 ...).
  std::vector<std::string> same, other;
  for (const auto& [file, line] : it->second.at) {
    const std::vector<std::string>* lines = sourceLines(file);
    if (!lines || line > lines->size()) return false;
    const std::string& text = (*lines)[line - 1];
    if (!text.empty() && text.back() == '\\') {  // over several lines: not compared
      other.push_back("");
      continue;
    }
    size_t p = text.find(name, text.find("define"));
    if (p == std::string::npos) return false;
    p += name.size();
    if (fm != fx_.functionMacros.end() && (p = text.find(')', p)) != std::string::npos) ++p;
    std::string guard, why;
    if (!spanGuard(fx_, file, line, line, guard, why, true, line)) return false;
    (p != std::string::npos && memberTokens(text.substr(p)) == want ? same : other).push_back(guard);
  }
  if (std::find(same.begin(), same.end(), "") == same.end()) {
    if (same.size() != 1) return false;
    guards.insert(same[0]);
    return true;
  }
  for (const std::string& g : other) {
    if (g.empty()) return false;
    guards.insert("!(" + g + ")");
  }
  return true;
}

// IR of a statement's value (with the temporaries in inline_ replaced by their
// definitions), inputs and facts.
bool Extractor::buildRegion(const Statement& s, Region& reg, std::string& why) {
  tlSawHalf = false;
  intMode_ = intOps_ = 0;  // a skip thrown inside an integer conversion leaves them raised
  leaves_.clear();
  built_.clear();
  collected_.clear();
  usesPow_ = false;
  reg.prog.inputs.clear();
  reg.facts.clear();
  fetchText_.clear();
  callText_.clear();
  callLeaf_.clear();
  // A resource write's text starts with the call and its coordinate (HLSL: Name[index] =): only the
  // value is the region. An HLSL compound write (Name[index] += v) reads the element first, as its text does.
  const size_t lhsAt = reg.kind == Region::Kind::Write ? reg.text.find(reg.lhs) : std::string::npos;
  mapFetches(s, lhsAt != std::string::npos ? reg.text.substr(lhsAt + reg.lhs.size()) : reg.text);
  mapCalls(s, lhsAt != std::string::npos ? reg.text.substr(lhsAt + reg.lhs.size()) : reg.text);
  for (const Statement* d : window_) {
    Region shape;
    std::string w;
    if (shapeOf(*d, shape, w)) {
      mapFetches(*d, shape.text);
      mapCalls(*d, shape.text);
    }
  }
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
      // Integer thread IDs: the code reads them converted (unsigned arithmetic would wrap).
      if (l.intSource) d.name = "float" + (w > 1 ? std::to_string(w) : std::string()) + "(" + d.name + ")";
      d.type = l.uintLeaf ? Type::Uint : floatType(w);
      d.compileTime = !l.fetch && isSymbolic(l.var);
      // Rates (scheduling measures): user uniforms are constants in ReShade's performance mode
      // (plain HLSL has none), uniforms with a source annotation (timer, frame count, ...) stay
      // live; fetches in source order.
      if (l.fetch) {
        d.rate = InputDecl::Rate::Fetch;
        for (size_t j = 0; j < i; ++j) d.fetchOrder += leaves_[j].fetch ? 1 : 0;
      } else if (const auto uv = cg_.variables.find(l.var);
                 !fx_.hlsl && !d.compileTime && uv != cg_.variables.end() && uv->second.kind == Variable::Kind::Uniform) {
        bool source = false;
        for (const auto& a : uv->second.annotations) source = source || a.name == "source";
        if (!source) d.rate = InputDecl::Rate::Uniform;
      } else if (!fx_.hlsl && !d.compileTime && perDraw(l.loadValue)) {
        d.rate = InputDecl::Rate::Uniform;  // a local computed from uniforms only
      }
      Range r = d.compileTime ? range(l.loadValue) : leafRange(l.loadValue);
      Fact fact;
      fact.input = d.name;
      const bool plainVar = !l.fetch && cg_.variables.count(l.var) && l.prefix == varText(l.var);
      fact.key = plainVar ? varKey(l.var) : std::string();
      // Plain HLSL: a read of a texture is keyed by the texture (its range applies to every read).
      if (fx_.hlsl && l.fetch && !l.call) {
        const auto fv = cg_.values.find(l.loadValue);
        const auto sv = fv == cg_.values.end() || fv->second.args.empty() ? cg_.values.end()
                                                                           : cg_.values.find(fv->second.args[0]);
        const auto si = sv == cg_.values.end() ? cg_.samplers.end() : cg_.samplers.find(sv->second.base);
        if (si != cg_.samplers.end()) fact.key = textureFactKey(fx_, si->second.textureName);
        else if (!fv->second.args.empty()) fact.key = storageFactKey(fv->second.args[0]);
      }
      if (fact.key.empty())
        fact.key = pathFrom(reg.file).filename().string() + " " + reg.function + " " + l.prefix;
      // Ask for uniforms and parameters first: other values are often computed from them.
      const auto kv = cg_.variables.find(l.var);
      fact.order = l.fetch ? 2
                   : kv == cg_.variables.end() ? 3
                   : kv->second.kind == Variable::Kind::Uniform ? 0
                   : kv->second.kind == Variable::Kind::Param ? 1 : 3;
      if (!r.known && !l.semantic.empty()) r = semanticConvention(l.semantic, opt_.maxWidth);
      if (!r.known || r.assumed) {
        const auto u = opt_.userRanges ? opt_.userRanges->find(fact.key) : UserRanges::const_iterator();
        if (opt_.userRanges && u != opt_.userRanges->end()) {
          r = Range::of(u->second.first, u->second.second, "user (facts file)");
        } else if (!r.known) {
          r = Range::of(opt_.defaultLo, opt_.defaultHi, "assumed");
          r.assumed = true;
          // A suggestion with a reason is a likely range; "no guess" and a definition of 0 stay assumed.
          suggest(l, fact);
          if (opt_.likelyRanges && fact.suggestWhy.rfind("no guess", 0) != 0 && fact.suggestWhy.find("(value 0)") == std::string::npos &&
              fact.suggestLo < fact.suggestHi) {
            r = Range::of(fact.suggestLo, fact.suggestHi, "likely: " + fact.suggestWhy);
            fact.likely = true;
          }
        }
      }
      if (r.assumed) suggest(l, fact);
      if (r.known && r.likely && !fact.likely) {
        fact.likely = true;
        fact.suggestLo = r.lo;
        fact.suggestHi = r.hi;
        fact.suggestWhy = r.likelyWhy;
      }
      d.lo = r.lo;
      d.hi = r.hi;
      d.grid = r.grid;
      if (l.uintLeaf) {  // whole numbers only (intExact: a known range within 2^24)
        d.lo = std::ceil(d.lo);
        d.hi = std::floor(d.hi);
        d.grid = static_cast<uint32_t>(d.hi - d.lo);
      }
      fact.source = r.why;
      fact.assumed = r.assumed;
      fact.fetch = l.fetch;
      if (d.lo == d.hi) throw Unsupported("input is a constant");  // the compiler folds it
      if (d.compileTime) {
        // The definition's current value: the search specializes on it.
        const std::string name = varText(l.var);
        const auto m = fx_.userMacros.find(name);
        d.value = name == "BUFFER_WIDTH"             ? fx_.width
                  : name == "BUFFER_HEIGHT"          ? fx_.height
                  : m == fx_.userMacros.end()        ? 0.5 * (d.lo + d.hi)
                                                     : std::strtod(m->second.c_str(), nullptr);
        d.value = std::clamp(d.value, d.lo, d.hi);
      }
      reg.prog.inputs.push_back(d);
      reg.facts.push_back(fact);
    }
    ExprBuilder b;
    const uint32_t root = build(s.value, b);
    reg.prog.target = b.finish(root);
  } catch (const Unsupported& e) {
    why = e.what();
    return false;
  } catch (const std::invalid_argument& e) {
    why = std::string("types (") + e.what() + ")";
    return false;
  }
  const Type rt = reg.prog.target.nodes[reg.prog.target.root].type;
  if (!isFloat(rt)) { why = "boolean value"; return false; }
  uint32_t ops = 0;
  bool compound = false;
  for (const auto& n : reg.prog.target.nodes) {
    ops += n.op != Op::Input && n.op != Op::Const && n.op != Op::Swizzle && n.op != Op::Construct;  // those are free
    compound = compound || compoundOp(n.op);
  }
  // A single helper of several instructions (smoothstep, lerp, sign, pow, ...) can get cheaper alone (owner,
  // 2026-10-26); a single instruction cannot. Its uses tell whether a region with several results could help.
  if (ops < opt_.minOps && !(ops == 1 && compound)) {
    if (ops == 0) { why = "copy or constant"; return false; }
    const int uses = s.kind == Statement::Kind::Return ? -1 : usesOf(s);
    why = uses < 0    ? "one instruction (returned)"
          : uses == 0 ? "one instruction (value not used)"
          : uses == 1 ? "one instruction (value used once)"
                      : "one instruction (value used several times)";
    return false;
  }
  if (ops > opt_.maxOps) { why = "more than maxOps operations"; return false; }
  if (reg.prog.inputs.empty()) { why = "constant expression"; return false; }
  bool runtime = false;
  for (const auto& d : reg.prog.inputs) runtime = runtime || !d.compileTime;
  if (!runtime) { why = "compile-time constant expression"; return false; }
  reg.prog.half = tlSawHalf;
  return true;
}

// Texture fetches of a statement, matched to their call text in the statement: both
// are in source order when no fetch is nested in another's arguments.
void Extractor::mapFetches(const Statement& s, const std::string& text) {
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
  for (size_t i = 0; i < text.size(); ++i) {
    const size_t j = fetchOpen(text, i);
    if (j == std::string::npos) continue;
    int depth = 0;
    size_t k = j;
    for (; k < text.size(); ++k) {
      if (text[k] == '(' || text[k] == '[') ++depth;
      if ((text[k] == ')' || text[k] == ']') && --depth == 0) break;
    }
    if (k >= text.size()) return;
    for (size_t m = i + 1; m < k; ++m)
      if (fetchAt(text, m)) return;  // nested fetch
    calls.push_back(modernFetch(text.substr(i, k + 1 - i)));
    i = k;
  }
  if (calls.size() != fetches.size()) return;
  for (size_t k = 0; k < calls.size(); ++k) fetchText_[fetches[k].second] = calls[k];
}

void Extractor::mapCalls(const Statement& s, const std::string& text) {
  std::unordered_set<uint32_t> tree;
  treeValues(s.value, tree);
  std::vector<std::pair<uint32_t, uint32_t>> calls;  // seq, id
  for (uint32_t id : tree)
    if (const auto it = cg_.values.find(id); it != cg_.values.end() && it->second.kind == Value::Kind::Call)
      calls.emplace_back(it->second.seq, id);
  if (calls.empty()) return;
  std::sort(calls.begin(), calls.end());
  // The calls' names in the text in order, outermost only (an argument's calls are not in the tree: arguments are
  // passed through variables).
  std::vector<std::string> texts;
  size_t c = 0;
  for (size_t i = 0; i < text.size() && c < calls.size(); ++i) {
    const Function* f = cg_.function(cg_.values.at(calls[c].second).name);
    if (!f) return;
    const std::string& name = f->name;
    if (text.compare(i, name.size(), name) != 0 || (i > 0 && (isIdent(text[i - 1]) || text[i - 1] == '.'))) continue;
    size_t j = text.find_first_not_of(" \t", i + name.size());
    if (j == std::string::npos || text[j] != '(') continue;
    size_t start = i;  // a namespace in front belongs to the call (ReShade::GetLinearizedDepth)
    while (start >= 2 && text.compare(start - 2, 2, "::") == 0) {
      start -= 2;
      while (start > 0 && isIdent(text[start - 1])) --start;
    }
    int depth = 0;
    size_t k = j;
    for (; k < text.size(); ++k) {
      if (text[k] == '(' || text[k] == '[') ++depth;
      if ((text[k] == ')' || text[k] == ']') && --depth == 0) break;
    }
    if (k >= text.size()) return;
    texts.push_back(text.substr(start, k + 1 - start));
    ++c;
    i = k;
  }
  if (texts.size() != calls.size()) return;
  for (size_t k = 0; k < texts.size(); ++k) callText_[calls[k].second] = texts[k];
}

// No writes outside its own locals and parameters, no out parameters, in it and in the helpers it calls: the same
// value for the same arguments (texture reads are fine).
bool Extractor::pureCall(const std::string& uniqueName, int depth) const {
  const Function* f = cg_.function(uniqueName);
  if (!f || depth > 16) return false;
  for (const uint32_t p : f->params)
    if (const auto v = cg_.variables.find(p); v == cg_.variables.end() || v->second.type.has(reshadefx::type::q_out))
      return false;
  for (const Statement& s : f->stmts) {
    if (s.kind == Statement::Kind::Write) return false;
    if (hasVar(s)) {
      const auto v = cg_.variables.find(s.var);
      if (v == cg_.variables.end() ||
          (v->second.kind != Variable::Kind::Local && v->second.kind != Variable::Kind::Param))
        return false;
    }
  }
  for (const std::string& g : f->calls)
    if (!pureCall(g, depth + 1)) return false;
  return true;
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
  if (l.call) return;  // a helper's result: no guess
  if (l.fetch) return set(0, 1, "texture read (float format)");
  std::string semantic = l.semantic;
  if (const auto it = cg_.variables.find(l.var); semantic.empty() && it != cg_.variables.end())
    semantic = it->second.semantic;
  if (upper(semantic).rfind("COLOR", 0) == 0) return set(0, 1, "COLOR semantic (usually a color)");
  if (!l.fetch && isSymbolic(l.var)) {
    const auto m = fx_.userMacros.find(varText(l.var));
    const double d = m == fx_.userMacros.end() ? 0.0 : std::strtod(m->second.c_str(), nullptr);
    if (d > 0) return set(0, 2 * d, "preprocessor definition, twice its value");
    if (d < 0) return set(2 * d, 0, "preprocessor definition, twice its value");
    return set(-1, 1, "preprocessor definition (value 0)");
  }
  if (const auto it = cg_.variables.find(l.var); it != cg_.variables.end() &&
      it->second.kind == Variable::Kind::Uniform && it->second.hasDefault &&
      it->second.type.is_floating_point()) {
    const double d = it->second.defaultValue.as_float[0];
    if (d > 0) return set(0, 2 * d, "uniform, twice its default");
    if (d < 0) return set(2 * d, 0, "uniform, twice its default");
  }
  // Sizes and their inverses (OBS: base_dimension, base_dimension_i; owner, 2026-10-19: more automatic ranges).
  const bool size = has({"dimension", "size", "width", "height", "resolution"});
  if (size && (has({"inv", "rcp", "texel"}) || (last.size() > 2 && last.compare(last.size() - 2, 2, "_i") == 0)))
    return set(1.0 / opt_.maxWidth, 1, "name looks like an inverse size (1 / pixels)");
  if (size) return set(1, opt_.maxWidth, "name looks like a size in pixels");
  if (has({"alpha", "opacity"})) return set(0, 1, "name looks like alpha");
  if (has({"uv", "coord", "tex"})) return set(0, 1, "name looks like a texture coordinate");
  if (has({"col", "rgb", "luma", "lum"})) return set(0, 1, "name looks like a color");
  if (has({"depth"})) return set(0, 1, "name looks like depth");
  if (has({"pos", "pixel"})) return set(0, opt_.maxWidth, "name looks like a pixel position");
  set(opt_.defaultLo, opt_.defaultHi, "no guess (the default)");
}

// The region writes, reads or feeds (one assignment on) a `precise` variable: the author wants
// float rounding there (fxc keeps reassociations away from precise values), so the region is
// judged against the float32 original only, not exact math (owner, 2026-10-03).
bool Extractor::touchesPrecise(const Function& f, const Statement& s,
                               const std::vector<const Statement*>& defs) const {
  auto precise = [&](uint32_t var) {
    const auto it = cg_.variables.find(var);
    return it != cg_.variables.end() && it->second.type.has(reshadefx::type::q_precise);
  };
  std::vector<const Statement*> all = defs;
  all.push_back(&s);
  std::unordered_set<uint32_t> tree, written;
  for (const Statement* t : all) {
    if (hasVar(*t)) {
      if (precise(t->var)) return true;
      written.insert(t->var);
    }
    treeValues(t->value, tree);
  }
  for (const auto& [var, value] : inline_) {
    if (precise(var)) return true;
    treeValues(value, tree);
  }
  for (uint32_t id : tree) {
    const auto it = cg_.values.find(id);
    if (it != cg_.values.end() && it->second.kind == Value::Kind::Load && precise(it->second.base)) return true;
  }
  // Feeds a precise variable directly.
  for (const Statement& t : f.stmts) {
    if (!hasVar(t) || !precise(t.var)) continue;
    std::unordered_set<uint32_t> used;
    treeValues(t.value, used);
    for (uint32_t id : used) {
      const auto it = cg_.values.find(id);
      if (it != cg_.values.end() && it->second.kind == Value::Kind::Load && written.count(it->second.base)) return true;
    }
  }
  return false;
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
    if (!shapeOf(*d, shape, why) || shape.sharesLines()) continue;  // a variant removes its lines
    if (defs.size() >= opt_.maxStatements - 1) return;
    inline_[v.base] = d->value;
    defs.push_back(d);
    findTemps(*d, defs, depth + 1);
  }
}

// Same-variable chain (`x = 1.0 - x; x /= F - x;`): the `length` statements before `s`
// (a store to the whole variable) that compute the value of it `s` reads, in straight-
// line code (one block). Their loads of the variable are inlined (inlineLoad_). False if
// an intermediate value is read elsewhere or there are fewer statements.
bool Extractor::findChain(const Statement& s, size_t length, std::vector<const Statement*>& defs) {
  if (s.kind != Statement::Kind::Store || !s.chain.empty()) return false;
  const auto vi = cg_.variables.find(s.var);
  if (vi == cg_.variables.end() ||
      (vi->second.kind != Variable::Kind::Local && vi->second.kind != Variable::Kind::Param))
    return false;
  const auto di = defs_.find(s.var);
  if (di == defs_.end()) return false;
  const Statement* cur = &s;
  while (defs.size() < length) {
    if (cur->kind == Statement::Kind::Init) return false;
    std::unordered_set<uint32_t> tree;
    treeValues(cur->value, tree);
    std::vector<uint32_t> loads;
    uint32_t firstLoad = UINT32_MAX;
    for (uint32_t id : tree) {
      const auto it = cg_.values.find(id);
      if (it == cg_.values.end() || it->second.kind != Value::Kind::Load || it->second.base != s.var) continue;
      loads.push_back(id);
      firstLoad = std::min(firstLoad, it->second.seq);
    }
    if (loads.empty()) return false;
    const Statement* d = nullptr;  // the reaching definition
    for (const Statement* w : di->second)
      if (w->seq < firstLoad && (!d || w->seq > d->seq)) d = w;
    if (!d || !d->chain.empty() || d->block != s.block || d->loc.source != s.loc.source) return false;
    for (const Statement* w : di->second)
      if (w->seq > d->seq && w->seq < cur->seq) return false;
    for (const auto& [id, v] : cg_.values) {
      if (v.seq <= d->seq || v.seq >= cur->seq || v.kind == Value::Kind::Const) continue;
      if (v.block != s.block) return false;  // control flow in between
      if (v.kind == Value::Kind::Load && v.base == s.var && !tree.count(id)) return false;
      if (v.kind == Value::Kind::Call)
        for (uint32_t a : v.args)
          if (a == s.var) return false;  // out argument
    }
    Region shape;
    std::string why;
    if (!shapeOf(*d, shape, why) || shape.sharesLines()) return false;
    for (uint32_t l : loads) inlineLoad_[l] = d->value;
    defs.push_back(d);
    cur = d;
  }
  return true;
}

// Whether a window's leaves (variables read, not inlined) have at `s` the value they
// have where the original reads them: no write to them in between outside the window,
// and no window statement (removed in variants) writing them before the read.
bool Extractor::leavesUnchanged(const Statement& s, const std::vector<const Statement*>& defs) const {
  std::vector<const Statement*> window = defs;
  window.push_back(&s);
  for (const Statement* w : window) {
    std::unordered_set<uint32_t> tree;
    treeValues(w->value, tree);
    // A texture fetch is copied as its call text: its arguments must not read an
    // inlined (removed) value.
    for (uint32_t id : tree) {
      const auto it = cg_.values.find(id);
      if (it == cg_.values.end() || it->second.kind != Value::Kind::Intrinsic || !isTexFetch(it->second.name))
        continue;
      // A read of a storage (HLSL RW texture / buffer) moved past a write into a storage would
      // read the new value.
      if (!it->second.args.empty() && isStorage(it->second.args[0]))
        for (const auto& fn : cg_.functions)
          for (const Statement& x : fn->stmts)
            if (x.kind == Statement::Kind::Write && x.seq > it->second.seq && x.seq < s.seq) return false;
      if (!it->second.args.empty() && isStorage(it->second.args[0]))
        for (const auto& [cid, cv] : cg_.values)
          if (cv.kind == Value::Kind::Intrinsic && cv.name.rfind("atomic", 0) == 0 && cv.seq > it->second.seq &&
              cv.seq < s.seq)
            return false;
      std::unordered_set<uint32_t> args;
      for (uint32_t a : it->second.args) treeValues(a, args);
      for (uint32_t a : args) {
        const auto ai = cg_.values.find(a);
        if (ai != cg_.values.end() && ai->second.kind == Value::Kind::Load &&
            (inlineLoad_.count(a) || inline_.count(ai->second.base)))
          return false;
      }
    }
    for (uint32_t id : tree) {
      const auto it = cg_.values.find(id);
      if (it == cg_.values.end()) continue;
      const Value& v = it->second;
      if (v.kind != Value::Kind::Load || inlineLoad_.count(id) || inline_.count(v.base)) continue;
      if (const auto di = defs_.find(v.base); di != defs_.end())
        for (const Statement* x : di->second) {
          const bool removed = std::find(defs.begin(), defs.end(), x) != defs.end();
          if (removed && x->seq < v.seq) return false;
          if (!removed && x->seq > v.seq && x->seq < s.seq) return false;
        }
      for (const auto& [cid, cv] : cg_.values)
        if (cv.kind == Value::Kind::Call && cv.seq > v.seq && cv.seq < s.seq)
          for (uint32_t a : cv.args)
            if (a == v.base) return false;
    }
  }
  return true;
}

std::vector<Region> Extractor::run(SkipCount& skipped) {
#define SKIPADD(r) skipped.add((r), s.loc.source, s.loc.line)
  // Functions reachable from pixel and compute shaders.
  std::set<std::string> reach;
  std::vector<const Function*> work;
  for (const auto& f : cg_.functions)
    if ((f->type == reshadefx::shader_type::pixel || f->type == reshadefx::shader_type::compute) &&
        reach.insert(f->uniqueName).second)
      work.push_back(f.get());
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
    // "FNs__name" -> "Ns__"
    fnNs_.clear();
    if (f.uniqueName.size() > f.name.size() + 1 &&
        f.uniqueName.compare(f.uniqueName.size() - f.name.size(), f.name.size(), f.name) == 0)
      fnNs_ = f.uniqueName.substr(1, f.uniqueName.size() - 1 - f.name.size());
    for (const Statement& s : f.stmts) {
      // ReShade copies each argument of a call into a nameless temporary: not code of the effect's own.
      if (const auto tv = cg_.variables.find(s.var);
          s.kind != Statement::Kind::Return && s.kind != Statement::Kind::Write && tv != cg_.variables.end() &&
          tv->second.kind == Variable::Kind::Local && tv->second.name.empty()) {
        SKIPADD("ReShade's copy of a call argument");
        continue;
      }
      const auto val = cg_.values.find(s.value);
      if (val != cg_.values.end() &&
          (val->second.kind == Value::Kind::Load || val->second.kind == Value::Kind::Const)) {
        SKIPADD("copy or constant");
        continue;
      }
      Region reg;
      reg.function = f.name;
      reg.hlsl = fx_.hlsl;
      reg.glsl = fx_.glsl;
      std::string why;
      if (!shapeOf(s, reg, why)) { SKIPADD(why); continue; }
      Region single = reg;
      inline_.clear();
      if (buildRegion(s, single, why)) {
        single.prog.budget = budgetFor(f, s, single.budgetReason);
        if (touchesPrecise(f, s, {})) {
          single.prog.budget.vsExact = false;
        single.prog.budget.errorScale = false;  // the rounding is wanted, not noise
          single.budgetReason += "; precise: float math only";
        }
        if (constGuard(fx_, single, why)) out.push_back(single);
        else SKIPADD("constant: " + why);
      } else {
        SKIPADD(why);
      }
      // Windows: the statement with the single-use temporaries it reads, and with the
      // statements before it that compute its variable (chains of 1, 2, ...), each also
      // with the temporaries of the chain.
      if (opt_.maxStatements < 2 || reg.sharesLines()) continue;
      for (size_t length = 0; length < opt_.maxStatements; ++length) {
        std::vector<const Statement*> defs;
        inline_.clear();
        inlineLoad_.clear();
        if (length > 0 && !findChain(s, length, defs)) break;
        const std::vector<const Statement*> chainDefs = defs;
        findTemps(s, defs, 0);
        for (const Statement* c : chainDefs) findTemps(*c, defs, 0);
        if (defs.empty()) continue;
        std::sort(defs.begin(), defs.end(), [](const Statement* a, const Statement* b) { return a->seq < b->seq; });
        if (!leavesUnchanged(s, defs)) continue;
        Region win = reg;
        // A chain from the variable's declaration: the variant replaces the declaration, so the statement must be in
        // the declaration's block (dh_uber_rt.fx: "float2 weight = 1.0; { ... weight *= ...; }" declared weight
        // inside the inner block, undeclared after it).
        bool otherBlock = false;
        for (const Statement* c : chainDefs)
          if (c->kind == Statement::Kind::Init) {
            Region shape;
            shapeOf(*c, shape, why);
            win.kind = Region::Kind::Init;
            win.lhs = shape.lhs;
            otherBlock = otherBlock || !sameBlock(reg.file, shape.lastLine + 1, reg.line);
          }
        if (otherBlock) {
          SKIPADD("window: the declaration is in another block");
          continue;
        }
        std::string text;
        std::vector<std::string> macroGuards;  // of the macros its statements spell out (shapeOf)
        if (!reg.guard.empty()) macroGuards.push_back(reg.guard);
        for (const Statement* d : defs) {
          Region shape;
          shapeOf(*d, shape, why);
          win.removed.emplace_back(shape.line, shape.lastLine);
          text += shape.original + "\n";
          if (!shape.guard.empty()) macroGuards.push_back(shape.guard);
        }
        win.original = text + reg.original;
        // A temporary the variant removes must have no other use in its scope's text, whatever the settings: its
        // single use is counted in this parse only (ArcaneBloom.fx 2026-10-26: "exposure" read once more under
        // ARCANE_BLOOM_WHITE_POINT_FIX).
        bool usedElsewhere = false;
        for (const Statement* d : defs) {
          if (std::find(chainDefs.begin(), chainDefs.end(), d) != chainDefs.end()) continue;
          const auto dv = cg_.variables.find(d->var);
          if (dv != cg_.variables.end())
            usedElsewhere = usedElsewhere || nameInScope(reg.file, dv->second.name, d->loc.line, win.removed,
                                                         reg.line, reg.lastLine);
        }
        if (usedElsewhere) {
          SKIPADD("window: a temporary is used under other settings");
          continue;
        }
        if (!spanGuard(fx_, reg.file, win.removed.front().first, reg.lastLine, win.guard, why)) {
          SKIPADD("window: " + why);
          continue;
        }
        for (const std::string& g : macroGuards)
          if (win.guard.find(g) == std::string::npos)
            win.guard = win.guard.empty() ? g : "(" + win.guard + ") && (" + g + ")";
        window_ = defs;
        if (buildRegion(s, win, why)) {
          win.prog.budget = budgetFor(f, s, win.budgetReason);
          if (touchesPrecise(f, s, defs)) {
            win.prog.budget.vsExact = false;
          win.prog.budget.errorScale = false;  // the rounding is wanted, not noise
            win.budgetReason += "; precise: float math only";
          }
          if (constGuard(fx_, win, why)) out.push_back(std::move(win));
          else SKIPADD("window constant: " + why);
        } else {
          SKIPADD("window: " + why);
        }
        window_.clear();
      }
      inline_.clear();
      inlineLoad_.clear();
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
  using Key = std::tuple<std::string, uint32_t, int, std::vector<std::pair<uint32_t, uint32_t>>>;
  std::map<Key, std::string> texts;
  for (const auto& r : other)
    texts[{r.file, r.line, static_cast<int>(r.kind), r.removed}] = toString(r.prog.target, r.prog.inputs);
  std::vector<Region> kept;
  for (auto& r : regions) {
    const auto it = texts.find({r.file, r.line, static_cast<int>(r.kind), r.removed});
    if (it != texts.end() && it->second != toString(r.prog.target, r.prog.inputs)) {
      skipped.add("depends on BUFFER_WIDTH/HEIGHT", r.file, r.line);
      continue;
    }
    kept.push_back(std::move(r));
  }
  return kept;
}

bool underCondition(const Effect& fx, const std::string& file, uint32_t first, uint32_t last) {
  std::string g, why;
  return !spanGuard(fx, file, first, last, g, why, true) || !g.empty();
}

std::vector<std::pair<std::string, std::string>> settingValues(const Effect& fx) {
  static const std::regex cmp(R"(\b([A-Za-z_]\w*)\s*(==|!=|>=|<=|>|<)\s*(-?\d+(?:\.\d+)?)\b)"),
      rcmp(R"((-?\d+(?:\.\d+)?)\s*(==|!=|>=|<=|>|<)\s*([A-Za-z_]\w*)\b)"), ident(R"([A-Za-z_]\w*)");
  std::set<std::pair<std::string, double>> values;
  auto add = [&](const std::string& name, const std::string& op, double n, bool flipped) {
    if (!fx.userMacros.count(name)) return;
    std::string o = op;
    if (flipped) o = op == ">" ? "<" : op == "<" ? ">" : op == ">=" ? "<=" : op == "<=" ? ">=" : op;
    values.insert({name, o == ">" ? n + 1 : o == "<" ? n - 1 : n});  // a value that makes the test true
  };
  for (const std::string& file : fx.sourceFiles) {
    const std::vector<std::string>* lines = sourceLines(file);
    if (!lines) continue;
    for (const std::string& l : *lines) {
      std::string word, arg;
      if (!directiveOf(l, word, arg) || (word != "if" && word != "elif")) continue;
      std::set<std::string> compared;
      for (auto m = std::sregex_iterator(arg.begin(), arg.end(), cmp); m != std::sregex_iterator(); ++m) {
        add((*m)[1], (*m)[2], std::stod((*m)[3]), false);
        compared.insert((*m)[1]);
      }
      for (auto m = std::sregex_iterator(arg.begin(), arg.end(), rcmp); m != std::sregex_iterator(); ++m) {
        add((*m)[3], (*m)[2], std::stod((*m)[1]), true);
        compared.insert((*m)[3]);
      }
      // A setting tested as true / false: the other one.
      for (auto m = std::sregex_iterator(arg.begin(), arg.end(), ident); m != std::sregex_iterator(); ++m) {
        const std::string n = m->str();
        const auto u = fx.userMacros.find(n);
        if (u == fx.userMacros.end() || compared.count(n)) continue;
        values.insert({n, std::strtod(u->second.c_str(), nullptr) != 0.0 ? 0.0 : 1.0});
      }
    }
  }
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& [name, v] : values) {
    const std::string& now = fx.userMacros.at(name);
    char* end = nullptr;
    const double cur = std::strtod(now.c_str(), &end);
    if (end != now.c_str() && cur == v) continue;  // the current value
    std::ostringstream s;
    s << v;
    out.emplace_back(name, s.str());
    if (out.size() == 32) break;  // shortcut: a cap on extra parses per effect; raise if effects test more
  }
  return out;
}

std::vector<Region> settingRegions(const Effect& def, const Effect& cfg, const Effect* alt, const RegionOptions& opt) {
  // Every code line of the region (and of the temporaries it inlines) is one the default parse leaves out.
  auto off = [&](const std::string& file, uint32_t a, uint32_t b) {
    const std::vector<std::string>* lines = sourceLines(file);
    if (!lines) return false;
    for (uint32_t l = a; l <= b && l <= lines->size(); ++l) {
      if (tokens((*lines)[l - 1]).empty()) continue;
      const auto it = def.ppLines.find({file, l});
      if (it != def.ppLines.end() && !tokens(it->second).empty()) return false;
    }
    return true;
  };
  SkipCount ignored;  // the lines both parses have are counted with the default parse
  std::vector<Region> out;
  for (Region& r : extractRegions(cfg, alt, opt, ignored)) {
    bool all = off(r.file, r.line, r.lastLine);
    for (const auto& [a, b] : r.removed) all = all && off(r.file, a, b);
    if (!all) continue;
    std::string g, why;
    if (!spanGuard(cfg, r.file, r.line, r.lastLine, g, why, true) || g.empty()) continue;
    r.guard = r.guard.empty() ? g : "(" + r.guard + ") && (" + g + ")";
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace sopt::fx
