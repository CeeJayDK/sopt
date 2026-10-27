#include "fx/guard.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "fx/codegen.hpp"
#include "verify/points.hpp"
#include "verify/verify.hpp"

namespace sopt::fx {
namespace {

using source::trim;

bool plainName(const std::string& s) {
  if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
  for (char c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) return false;
  return true;
}

std::optional<SourceRewrite> zeroGuard(const Effect& fx, const Function& ps, const Region& r,
                                        const std::vector<Region>& regions, uint64_t seed) {
  const Codegen& cg = *fx.cg;
  const std::vector<std::string>* lines = sourceLines(ps.loc.source);
  if (!lines || r.file != ps.loc.source || r.sharesLines() || !r.guard.empty()) return std::nullopt;
  size_t returns = 0;
  for (const auto& s : ps.stmts) returns += s.kind == Statement::Kind::Return;
  // The statement the color leaves by: the only return, or the last store to an out SV_Target / COLOR parameter (no
  // return), or the last store to a local that the next statement returns.
  const Statement* out = nullptr;
  size_t outIdx = 0;
  for (size_t i = 0; i < ps.stmts.size(); ++i)
    if (ps.stmts[i].loc.line >= r.line && ps.stmts[i].loc.line <= r.lastLine &&
        (ps.stmts[i].kind == Statement::Kind::Return || ps.stmts[i].kind == Statement::Kind::Store)) {
      out = &ps.stmts[i];
      outIdx = i;
    }
  if (!out) return std::nullopt;
  std::string tail;  // after the guard's store: "return;" or "return v;"
  uint32_t outVar = 0;
  if (out->kind == Statement::Kind::Return) {
    if (returns != 1 || r.kind != Region::Kind::Return) return std::nullopt;
  } else {
    const auto v = cg.variables.find(out->var);
    if (v == cg.variables.end() || r.kind != Region::Kind::Store) return std::nullopt;
    for (const auto& t : ps.stmts)
      if ((t.kind == Statement::Kind::Store || t.kind == Statement::Kind::Init) && t.var == out->var && t.seq > out->seq)
        return std::nullopt;
    std::string sem = v->second.semantic;
    for (auto& c : sem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (v->second.kind == Variable::Kind::Param && v->second.type.has(reshadefx::type::q_out) &&
        (sem.rfind("SV_TARGET", 0) == 0 || sem.rfind("COLOR", 0) == 0) && returns == 0) {
      tail = "return;";
    } else if (v->second.kind == Variable::Kind::Local && returns == 1 && outIdx + 1 < ps.stmts.size() &&
               ps.stmts[outIdx + 1].kind == Statement::Kind::Return && ps.stmts[outIdx + 1].block == out->block) {
      const Statement& ret = ps.stmts[outIdx + 1];
      if (ret.loc.line == 0 || ret.loc.line > lines->size()) return std::nullopt;
      std::string rl;
      for (char c : (*lines)[ret.loc.line - 1])
        if (!std::isspace(static_cast<unsigned char>(c))) rl += c;
      if (rl != "return" + v->second.name + ";") return std::nullopt;
      tail = "return " + v->second.name + ";";
    } else {
      return std::nullopt;
    }
    outVar = out->var;
  }
  // Zero sources: a scalar uniform S, or a local computed from uniforms only (coef = Balance * Vibrance) that its own
  // statement makes 0 wherever a scalar uniform S is 0. The guard tests S (known from the start).
  auto isUniform = [&](const std::string& nm) {
    for (const auto& [id, v] : cg.variables)
      if (v.kind == Variable::Kind::Uniform && v.name == nm) return true;
    return false;
  };
  auto scalarUniform = [&](const InputDecl& u) {
    return u.rate == InputDecl::Rate::Uniform && u.type == Type::Float && !u.compileTime && u.lo <= 0.0 && 0.0 <= u.hi &&
           plainName(u.name) && isUniform(u.name);
  };
  auto zeroSource = [&](const InputDecl& s) -> std::string {
    if (s.rate != InputDecl::Rate::Uniform || s.compileTime || !plainName(s.name)) return {};
    if (scalarUniform(s)) return s.name;
    for (const Region& d : regions) {
      const std::string lhs = trim(d.lhs);
      const std::string end = " " + s.name + " =";
      if (d.function != ps.name || d.kind != Region::Kind::Init || !d.removed.empty() || lhs.size() < end.size() ||
          lhs.compare(lhs.size() - end.size(), end.size(), end) != 0)
        continue;
      for (size_t u = 0; u < d.prog.inputs.size(); ++u) {
        if (!scalarUniform(d.prog.inputs[u])) continue;
        PointSet q = makeRandomPoints(d.prog, 256, seed, true);
        auto& col = q.cols[q.slotOf(static_cast<uint32_t>(u))];
        std::fill(col.begin(), col.end(), 0.0f);
        bool zero = true;
        for (const float v : evalAll(d.prog.target, q, kProfileRef)) zero = zero && v == 0.0f;
        if (zero) return d.prog.inputs[u].name;
      }
    }
    return {};
  };
  // An input A the output equals wherever S is 0 (the later values random), lerp both ways.
  const Program& p = r.prog;
  const unsigned ow = width(p.target.nodes[p.target.root].type);
  const PointSet base = makeRandomPoints(p, 1024, seed, true);
  for (size_t k = 0; k < p.inputs.size(); ++k) {
    const InputDecl& s = p.inputs[k];
    const std::string cond = zeroSource(s);
    if (cond.empty()) continue;
    PointSet ps0 = base;
    for (unsigned c = 0; c < width(s.type); ++c) {
      auto& col = ps0.cols[ps0.slotOf(static_cast<uint32_t>(k)) + c];
      std::fill(col.begin(), col.end(), 0.0f);
    }
    const std::vector<float> ref = evalAll(p.target, ps0, kProfileRef), mix = evalAll(p.target, ps0, kProfileMix);
    const size_t n = ps0.size();
    for (size_t j = 0; j < p.inputs.size(); ++j) {
      const InputDecl& a = p.inputs[j];
      if (j == k || width(a.type) != ow || !isFloat(a.type) || a.rate == InputDecl::Rate::Fetch || !plainName(a.name))
        continue;
      const uint32_t slot = ps0.slotOf(static_cast<uint32_t>(j));
      bool same = true, any = false;
      for (unsigned c = 0; c < ow && same; ++c)
        for (size_t i = 0; i < n && same; ++i) {
          const float t = ref[c * n + i];
          if (!std::isfinite(t)) continue;
          any = true;
          // Within a few float steps of the largest input there (lerp(a, b, 1) = (b - a) + a: b within a step of a).
          double big = std::fabs(t);
          for (const auto& col : ps0.cols) big = std::max(big, double(std::fabs(col[i])));
          const double tol = std::ldexp(big, -21);
          same = std::fabs(double(t) - ps0.cols[slot + c][i]) <= tol &&
                 std::fabs(double(mix[c * n + i]) - ps0.cols[slot + c][i]) <= tol;
        }
      if (!same || !any) continue;
      // A's variable (and the output's) stored for the last time before the output: the guard goes after that, before
      // the next statement of the same block, with only locals stored from there on.
      const std::string aVar = a.name.substr(0, a.name.find('.'));
      size_t anchor = SIZE_MAX;  // index of the last store to A's or the output's variable before the output
      for (size_t i = 0; i < outIdx; ++i) {
        const Statement& t = ps.stmts[i];
        if (t.kind != Statement::Kind::Init && t.kind != Statement::Kind::Store) continue;
        const auto tv = cg.variables.find(t.var);
        if (tv != cg.variables.end() && (tv->second.name == aVar || t.var == outVar)) anchor = i;
      }
      const size_t first = anchor == SIZE_MAX ? 0 : anchor + 1;
      if (first >= outIdx) continue;  // no work to skip
      const Statement& next = ps.stmts[first];
      if (next.block != out->block || (anchor != SIZE_MAX && ps.stmts[anchor].block != out->block)) continue;
      bool locals = true;
      for (size_t i = first; i < outIdx; ++i) {
        const Statement& t = ps.stmts[i];
        const auto tv = cg.variables.find(t.var);
        locals = locals && (t.kind == Statement::Kind::Init || t.kind == Statement::Kind::Store) &&
                 tv != cg.variables.end() && tv->second.kind == Variable::Kind::Local;
      }
      const uint32_t at = next.loc.line;
      if (!locals || at < 2 || at > lines->size() || (anchor != SIZE_MAX && ps.stmts[anchor].loc.line >= at)) continue;
      // The line before ends a statement or opens / closes a block; neither line is a preprocessor line.
      const std::string prev = trim((*lines)[at - 2]), here = trim((*lines)[at - 1]);
      if (here.empty() || here[0] == '#' || (!prev.empty() && prev[0] == '#')) continue;
      if (!prev.empty() && prev.back() != ';' && prev.back() != '{' && prev.back() != '}') continue;
      const std::string& ln = (*lines)[at - 1];
      const std::string indent = ln.substr(0, ln.find_first_not_of(" \t"));
      const std::string lhs = trim(r.lhs);
      const bool noStore = !lhs.empty() && trim(lhs.substr(0, lhs.size() - 1)) == a.name;  // color = color
      const std::string guard = tail.empty() ? "if (" + cond + " == 0.0) return " + a.name + ";"
                                : noStore  ? "if (" + cond + " == 0.0) " + tail
                                           : "if (" + cond + " == 0.0) { " + lhs + " " + a.name + "; " + tail + " }";
      SourceRewrite rw;
      rw.file = ps.loc.source;
      rw.line = at;
      rw.function = ps.name;
      rw.kind = "zero guard";
      rw.tag = "Z";
      rw.description = "skips the work after it when " + cond + " is 0 (performance mode off)";
      LineEdit e;
      e.first = e.last = at;
      e.lines = {indent + guard, ln};
      e.extra = " && !__RESHADE_PERFORMANCE_MODE__";
      rw.edits.push_back(std::move(e));
      return rw;
    }
  }
  return std::nullopt;
}

}  // namespace

std::vector<SourceRewrite> zeroGuardRewrites(const Effect& fx, const RegionOptions& opt, uint64_t seed) {
  std::vector<SourceRewrite> out;
  if (!fx.cg || fx.hlsl || fx.glsl) return out;
  const Codegen& cg = *fx.cg;
  std::set<std::string> called;
  for (const auto& f : cg.functions) called.insert(f->calls.begin(), f->calls.end());
  std::vector<const Function*> shaders;
  for (const auto& f : cg.functions) {
    const auto et = cg.entryPoints.find(f->uniqueName);
    if (et != cg.entryPoints.end() && et->second == reshadefx::shader_type::pixel && !called.count(f->uniqueName))
      shaders.push_back(f.get());
  }
  if (shaders.empty()) return out;
  // The output statement's region, down to one operation (lerp(color, effect, Strength)).
  RegionOptions ro = opt;
  ro.minOps = 1;
  SkipCount skipped;
  const std::vector<Region> regions = extractRegions(fx, nullptr, ro, skipped);
  for (const Function* f : shaders) {
    // The largest window first: it sees the uniform through the temporaries it inlines (coef = Balance * Vibrance).
    std::vector<const Region*> cands;
    for (const Region& r : regions)
      if (r.function == f->name && (r.kind == Region::Kind::Return || r.kind == Region::Kind::Store)) cands.push_back(&r);
    std::stable_sort(cands.begin(), cands.end(),
                     [](const Region* a, const Region* b) { return a->removed.size() > b->removed.size(); });
    for (const Region* r : cands)
      if (auto rw = zeroGuard(fx, *f, *r, regions, seed)) {
        out.push_back(std::move(*rw));
        break;
      }
  }
  return out;
}

}  // namespace sopt::fx
