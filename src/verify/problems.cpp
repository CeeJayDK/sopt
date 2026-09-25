#include "verify/problems.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "verify/points.hpp"
#include "verify/verify.hpp"

namespace sopt {

namespace {

// Where an operand makes its op fail: rcp(x) and a / x at x = 0; rsqrt and log for
// x <= 0; sqrt and pow's base for x < 0.
enum class Side { Zero, NonPos, Neg };

constexpr uint32_t kCheckPoints = 256;  // samples of the other inputs per checked value
constexpr int kMaxProbes = 256;         // full checks per candidate value
constexpr size_t kMaxCandidates = 32;   // candidate values per operand

struct Analyzer {
  const Program& prog;
  const Expr& cand;
  bool loose;
  std::vector<InputDecl> slots = slotDecls(prog.inputs);
  std::vector<uint32_t> first = inputSlots(prog.inputs);

  // Values of the scalar subexpression `root` of cand, with input `in` at vals.
  std::vector<float> operand(uint32_t root, uint32_t in, const std::vector<float>& vals) const {
    PointSet ps;
    ps.slot = first;
    ps.cols.resize(slots.size());
    for (size_t s = 0; s < slots.size(); ++s) ps.cols[s].assign(vals.size(), static_cast<float>(slots[s].lo));
    ps.cols[first[in]] = vals;
    Expr sub = cand;
    sub.root = root;
    std::vector<float> out = evalAll(sub, ps, kProfileRef);
    out.resize(vals.size());
    return out;
  }

  // Whether the candidate fails the budget somewhere with input `in` fixed at x.
  bool failsAt(uint32_t in, float x, bool* notFinite) const {
    Program p = prog;
    InputDecl& d = p.inputs[in];
    d.lo = d.hi = x;
    d.grid = 0;
    const PointSet ps = makeRandomPoints(p, kCheckPoints, 0x5eed + in, true);
    for (const auto& prof : kAllProfiles) {
      const Metrics m = compare(p, cand, ps, prof, 1);
      if (loose ? m.loosePass : m.pass) continue;
      if (notFinite) {
        const std::vector<float>& fp = loose ? m.looseFailPoint : m.failPoint;
        *notFinite = false;
        if (!fp.empty()) {
          PointSet one;
          one.slot = ps.slot;
          one.cols.resize(fp.size());
          one.add(fp);
          for (float v : evalAll(cand, one, prof)) *notFinite = *notFinite || !std::isfinite(v);
        }
      }
      return true;
    }
    return false;
  }
};

// Index of the first domain value >= x (domainValue is increasing in k).
uint64_t keyOf(const InputDecl& d, double x) {
  uint64_t lo = 0, hi = domainCount(d) - 1;
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo) / 2;
    if (domainValue(d, mid) < x) lo = mid + 1; else hi = mid;
  }
  return lo;
}

int signOf(float v) { return std::isnan(v) ? 2 : (v > 0.0f) - (v < 0.0f); }

bool badSide(Side s, float v) {
  if (!std::isfinite(v)) return true;
  switch (s) {
    case Side::Zero: return v == 0.0f;
    case Side::NonPos: return v <= 0.0f;
    case Side::Neg: return v < 0.0f;
  }
  return false;
}

}  // namespace

std::vector<ProblemRange> findProblemRanges(const Program& prog, const Expr& cand, bool loose) {
  std::vector<ProblemRange> out;
  if (prog.inputs.size() > 64) return out;
  Analyzer an{prog, cand, loose};
  // Inputs each node depends on.
  std::vector<uint64_t> deps(cand.nodes.size(), 0);
  for (size_t k = 0; k < cand.nodes.size(); ++k) {
    const Node& n = cand.nodes[k];
    if (n.op == Op::Input) deps[k] = uint64_t{1} << n.input;
    for (unsigned a = 0; a < operandCount(n); ++a) deps[k] |= deps[n.args[a]];
  }
  auto inside = [&](uint32_t in, double x) {
    for (const auto& p : out)
      if (p.input == in && x >= p.lo && x <= p.hi) return true;
    return false;
  };
  for (size_t k = 0; k < cand.nodes.size(); ++k) {
    const Node& n = cand.nodes[k];
    uint32_t arg;
    Side side;
    switch (n.op) {
      case Op::Rcp: arg = n.args[0]; side = Side::Zero; break;
      case Op::Div: arg = n.args[1]; side = Side::Zero; break;
      case Op::Rsqrt: arg = n.args[0]; side = Side::NonPos; break;
      case Op::Log: arg = n.args[0]; side = Side::NonPos; break;
      case Op::Sqrt: arg = n.args[0]; side = Side::Neg; break;
      case Op::Pow: arg = n.args[0]; side = Side::Neg; break;
      default: continue;
    }
    const uint64_t dm = deps[arg];
    if (dm == 0 || (dm & (dm - 1)) != 0 || nodeType(cand, arg) != Type::Float) continue;
    uint32_t in = 0;
    while (!(dm >> in & 1)) ++in;
    const InputDecl& d = prog.inputs[in];
    if (d.type != Type::Float || !(d.hi > d.lo)) continue;
    const uint64_t count = domainCount(d);

    // Scan: uniform in value and uniform in domain index (float bits: log-like).
    std::vector<uint64_t> keys;
    const uint64_t n1 = std::min<uint64_t>(count, 4097);
    for (uint64_t j = 0; j < n1; ++j) {
      keys.push_back(n1 > 1 ? (count - 1) * j / (n1 - 1) : 0);
      keys.push_back(keyOf(d, d.lo + (d.hi - d.lo) * static_cast<double>(j) / static_cast<double>(n1 - 1 ? n1 - 1 : 1)));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<float> vals(keys.size());
    for (size_t j = 0; j < keys.size(); ++j) vals[j] = domainValue(d, keys[j]);
    const std::vector<float> g = an.operand(static_cast<uint32_t>(arg), in, vals);
    auto gAt = [&](uint64_t key) { return an.operand(static_cast<uint32_t>(arg), in, {domainValue(d, key)})[0]; };

    std::vector<uint64_t> cands;
    for (size_t j = 0; j < keys.size() && cands.size() < kMaxCandidates; ++j) {
      if (badSide(side, g[j]) && (j == 0 || !badSide(side, g[j - 1]))) cands.push_back(keys[j]);
      if (j + 1 < keys.size() && signOf(g[j]) != signOf(g[j + 1]) && signOf(g[j]) != 2 && signOf(g[j + 1]) != 2) {
        // Sign change: bisect to adjacent domain values.
        uint64_t a = keys[j], b = keys[j + 1];
        const int sa = signOf(g[j]);
        while (b - a > 1) {
          const uint64_t mid = a + (b - a) / 2;
          const int sm = signOf(gAt(mid));
          if (sm == 0 || sm == 2) { a = b = mid; break; }
          (sm == sa ? a : b) = mid;
        }
        cands.push_back(a);
        if (b != a) cands.push_back(b);
      }
    }

    for (uint64_t k0 : cands) {
      const float x0 = domainValue(d, k0);
      if (inside(in, x0)) continue;
      bool notFinite = false;
      if (!an.failsAt(in, x0, &notFinite)) continue;
      // Widen to the contiguous failing values around x0.
      int probes = 0;
      auto edge = [&](int dir) {
        uint64_t bad = k0, step = 1;
        while (probes < kMaxProbes) {
          const uint64_t limit = dir < 0 ? bad : count - 1 - bad;
          if (limit == 0) return bad;
          const uint64_t probe = dir < 0 ? bad - std::min(step, limit) : bad + std::min(step, limit);
          ++probes;
          if (an.failsAt(in, domainValue(d, probe), nullptr)) {
            bad = probe;
            step *= 2;
            continue;
          }
          uint64_t good = probe;
          while ((good > bad ? good - bad : bad - good) > 1 && probes < kMaxProbes) {
            const uint64_t mid = good > bad ? bad + (good - bad) / 2 : bad - (bad - good) / 2;
            ++probes;
            (an.failsAt(in, domainValue(d, mid), nullptr) ? bad : good) = mid;
          }
          return bad;
        }
        return bad;
      };
      ProblemRange p;
      p.input = in;
      p.lo = domainValue(d, edge(-1));
      p.hi = domainValue(d, edge(+1));
      p.notFinite = notFinite;
      out.push_back(p);
    }
  }
  std::sort(out.begin(), out.end(), [](const ProblemRange& a, const ProblemRange& b) {
    return a.input != b.input ? a.input < b.input : a.lo < b.lo;
  });
  return out;
}

std::string describeProblems(const Program& prog, const std::vector<ProblemRange>& problems) {
  std::string s;
  auto num = [](double v) {  // shortest text that reads back as the same float
    const float f = static_cast<float>(v);
    char buf[32];
    const double a = std::fabs(static_cast<double>(f));
    if (a == 0.0 || (a >= 1e-4 && a < 1e9)) {
      for (int dec = 0; dec <= 12; ++dec) {
        std::snprintf(buf, sizeof(buf), "%.*f", dec, static_cast<double>(f));
        if (std::strtof(buf, nullptr) == f) return std::string(buf);
      }
    }
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(f));
    return std::string(buf);
  };
  for (size_t i = 0; i < problems.size();) {
    const uint32_t in = problems[i].input;
    const InputDecl& d = prog.inputs[in];
    std::string where, fine;
    bool notFinite = false;
    double from = d.lo;
    bool fromOpen = false;
    for (; i < problems.size() && problems[i].input == in; ++i) {
      const ProblemRange& p = problems[i];
      notFinite = notFinite || p.notFinite;
      if (!where.empty()) where += ", ";
      where += p.lo == p.hi ? num(p.lo) : "[" + num(p.lo) + ", " + num(p.hi) + "]";
      if (p.lo > from) fine += std::string(fine.empty() ? "" : " and ") + (fromOpen ? "(" : "[") + num(from) + ", " + num(p.lo) + ")";
      from = p.hi;
      fromOpen = true;
    }
    if (from < d.hi) fine += std::string(fine.empty() ? "" : " and ") + (fromOpen ? "(" : "[") + num(from) + ", " + num(d.hi) + "]";
    if (!s.empty()) s += "; ";
    s += "fails at " + d.name + " = " + where + (notFinite ? " (NaN/inf at some)" : " (outside the budget)");
    if (!fine.empty()) s += ", fine on " + fine;
  }
  return s;
}

}  // namespace sopt
