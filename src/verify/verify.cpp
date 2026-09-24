#include "verify/verify.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <thread>

#include "ir/eval.hpp"
#include "verify/exact.hpp"

namespace sopt {

const BlockEvaluator::Cols& BlockEvaluator::eval(const Expr& e, const PointSet& ps, size_t begin,
                                                 size_t count, const Profile& profile) {
  const size_t nn = e.nodes.size();
  size_t total = 0;
  for (const auto& n : e.nodes) total += width(n.type);
  scratch.resize(total * count);
  ptr.assign(nn, Cols{});
  const auto uses = profile.contract ? useCounts(e) : std::vector<uint32_t>();
  // Component c of node j (a float1 node is broadcast to every component).
  auto col = [&](uint32_t j, unsigned c) { return ptr[j][width(e.nodes[j].type) == 1 ? 0 : c]; };
  float* next = scratch.data();
  for (size_t i = 0; i < nn; ++i) {
    const Node& n = e.nodes[i];
    const unsigned w = width(n.type);
    float* out[4] = {nullptr, nullptr, nullptr, nullptr};
    if (n.op != Op::Input)
      for (unsigned c = 0; c < w; ++c, next += count) {
        out[c] = next;
        ptr[i][c] = next;
      }
    const int k = profile.contract ? fusedArg(e, static_cast<uint32_t>(i), uses, profile.divRcp) : -1;
    if (k >= 0) {
      // a*b + c as one fma; a / b + c as fma(a, rcp(b), c), per component.
      const Node& m = e.nodes[n.args[k]];
      const bool div = m.op == Op::Div;
      const float sa = (n.op == Op::Sub && k == 1) ? -1.0f : 1.0f;  // o - a*b = fma(-a, b, o)
      const float so = (n.op == Op::Sub && k == 0) ? -1.0f : 1.0f;  // a*b - o = fma(a, b, -o)
      for (unsigned c = 0; c < w; ++c) {
        const float* p = col(m.args[0], c);
        const float* q = col(m.args[1], c);
        const float* o = col(n.args[1 - k], c);
        for (size_t j = 0; j < count; ++j)
          out[c][j] = std::fma(sa * p[j], div ? 1.0f / q[j] : q[j], so * o[j]);
      }
    } else if (n.op == Op::Input) {
      const uint32_t slot = ps.slotOf(n.input);
      for (unsigned c = 0; c < w; ++c) ptr[i][c] = ps.cols[slot + c].data() + begin;
    } else if (n.op == Op::Const) {
      for (unsigned c = 0; c < w; ++c) std::fill(out[c], out[c] + count, n.value[c]);
    } else {
      Type ts[4];
      const float* args[4][4] = {};
      for (unsigned a = 0; a < n.nargs; ++a) {
        ts[a] = e.nodes[n.args[a]].type;
        for (unsigned c = 0; c < width(ts[a]); ++c) args[a][c] = ptr[n.args[a]][c];
      }
      evalNode(n, ts, args, out, count, profile, tmp);
    }
  }
  return ptr[e.root];
}

const char* klassName(Klass k, int codeBits) {
  switch (k) {
    case Klass::BitExact: return "bit-exact";
    case Klass::Identical8: return codeBits == 10 ? "10-bit identical" : "8-bit identical";
    case Klass::Within: return "within budget";
    case Klass::Accurate: return "as accurate";
    case Klass::LessAccurate: return "less accurate";
  }
  return "?";
}

void Metrics::merge(const Metrics& o) {
  if (pass && !o.pass) failPoint = o.failPoint;
  pass = pass && o.pass;
  if (loosePass && !o.loosePass) looseFailPoint = o.looseFailPoint;
  loosePass = loosePass && o.loosePass;
  bitExact = bitExact && o.bitExact;
  maxCodeDiff = std::max(maxCodeDiff, o.maxCodeDiff);
  maxAbs = std::max(maxAbs, o.maxAbs);
  maxRel = std::max(maxRel, o.maxRel);
  checked += o.checked;
  valueHash += o.valueHash;
  codeChanged += o.codeChanged;
  viaExact += o.viaExact;
  exactAbs = std::max(exactAbs, o.exactAbs);
  exactRel = std::max(exactRel, o.exactRel);
}


Budget looseBudget(const Budget& b) {
  Budget l = b;
  l.eps *= b.loose;
  l.maxCodeDiff += 1;
  return l;
}

bool pointAccurate(const Budget& b, float t, double x, float c, double scale) {
  if (!std::isfinite(c) || !std::isfinite(x)) return false;
  switch (b.kind) {
    case Budget::Kind::Exact: return false;
    case Budget::Kind::Color8:
    case Budget::Kind::Color10: {
      const int bits = b.codeBits();
      const double cx = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
      const int codeX = static_cast<int>(cx * ((1 << bits) - 1) + 0.5);
      const int orig = std::abs(codeN(t, bits) - codeX) + (scale > 1.0 ? 1 : 0);
      return std::abs(codeN(c, bits) - codeX) <= std::max(b.maxCodeDiff, orig);
    }
    case Budget::Kind::Texcoord:
    case Budget::Kind::Abs: return std::fabs(c - x) <= std::max(b.eps, scale * std::fabs(t - x));
    case Budget::Kind::Rel:
      return std::fabs(c - x) <= std::max(b.eps * std::max(1.0, std::fabs(x)), scale * std::fabs(t - x));
  }
  return false;
}

namespace {

Metrics compareRange(const Program& prog, const Expr& cand, const PointSet& ps, size_t begin,
                     size_t end, const Profile& profile, const std::vector<float>* targetVals,
                     const std::vector<double>* exactVals) {
  constexpr size_t kBlock = 4096;
  BlockEvaluator et, ec;
  ExactEvaluator ex;
  const bool rule = accuracyRule(prog.budget);
  Metrics m;
  const unsigned w = width(prog.target.nodes[prog.target.root].type);
  const int bits = prog.budget.codeBits();
  if (cand.nodes[cand.root].type != prog.target.nodes[prog.target.root].type) {
    m.pass = m.loosePass = false;  // a candidate must have the target's type
    m.failPoint = m.looseFailPoint = ps.point(begin);
    return m;
  }
  const size_t total = ps.size();
  for (size_t b = begin; b < end; b += kBlock) {
    const size_t count = std::min(kBlock, end - b);
    BlockEvaluator::Cols tv{};
    if (targetVals) {
      for (unsigned c = 0; c < w; ++c) tv[c] = targetVals->data() + c * total + b;
    } else {
      tv = et.eval(prog.target, ps, b, count, profile);
    }
    const BlockEvaluator::Cols cv = ec.eval(cand, ps, b, count, profile);
    ExactEvaluator::Cols xv{};
    if (rule && exactVals) {
      for (unsigned c = 0; c < w; ++c) xv[c] = exactVals->data() + c * total + b;
    } else if (rule) {
      xv = ex.eval(prog.target, ps, b, count);
    }
    for (size_t i = 0; i < count; ++i) {
      bool pointOk = true, looseOk = true;
      for (unsigned comp = 0; comp < w; ++comp) {
        const float t = tv[comp][i], c = cv[comp][i];
        if (!std::isfinite(t)) continue;
        {
          uint32_t bits = std::bit_cast<uint32_t>(c == 0.0f ? 0.0f : c);
          if (std::isnan(c)) bits = 0x7fc00000u;
          uint64_t z = (((b + i) * 4 + comp) << 32) ^ bits;  // splitmix64 finalizer
          z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
          z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
          m.valueHash += z ^ (z >> 31);
        }
        ++m.checked;
        if (!pointWithinBudget(prog.budget, t, c)) {
          if (rule && pointAccurate(prog.budget, t, xv[comp][i], c)) {
            ++m.viaExact;
          } else {
            pointOk = false;
            looseOk = looseOk && pointLoose(prog.budget, t, rule ? &xv[comp][i] : nullptr, c);
          }
        }
        if (rule && std::isfinite(xv[comp][i])) {
          const double x = xv[comp][i];
          const double d = std::isfinite(c) ? std::fabs(c - x) : INFINITY;
          m.exactAbs = std::max(m.exactAbs, d);
          m.exactRel = std::max(m.exactRel, d / std::max(1.0, std::fabs(x)));
        }
        if (!std::isfinite(c)) {
          m.bitExact = false;
          m.maxAbs = m.maxRel = INFINITY;
        } else {
          if (c != t) m.bitExact = false;
          const double err = std::fabs(static_cast<double>(c) - t);
          m.maxAbs = std::max(m.maxAbs, err);
          m.maxRel = std::max(m.maxRel, err / std::max(1e-30, std::fabs(double(t))));
          const int d = std::abs(codeN(c, bits) - codeN(t, bits));
          m.maxCodeDiff = std::max(m.maxCodeDiff, d);
          if (d) ++m.codeChanged;
        }
      }
      if (!pointOk && m.pass) {
        m.pass = false;
        m.failPoint = ps.point(b + i);
      }
      if (!looseOk && m.loosePass) {
        m.loosePass = false;
        m.looseFailPoint = ps.point(b + i);
        // Rejected: callers only use the failing point, so the remaining points are not
        // compared (each thread's range stops at its own first failure: deterministic).
        return m;
      }
    }
  }
  return m;
}

}  // namespace

std::vector<float> evalAll(const Expr& e, const PointSet& ps, const Profile& profile) {
  constexpr size_t kBlock = 4096;
  const unsigned w = width(e.nodes[e.root].type);
  const size_t total = ps.size();
  std::vector<float> out(w * total);
  BlockEvaluator ev;
  for (size_t b = 0; b < total; b += kBlock) {
    const size_t count = std::min(kBlock, total - b);
    const BlockEvaluator::Cols& v = ev.eval(e, ps, b, count, profile);
    for (unsigned c = 0; c < w; ++c)
      std::copy(v[c], v[c] + count, out.begin() + static_cast<std::ptrdiff_t>(c * total + b));
  }
  return out;
}

Metrics compare(const Program& prog, const Expr& cand, const PointSet& ps, const Profile& profile,
                unsigned threads, const std::vector<float>* targetVals,
                const std::vector<double>* exactVals) {
  const size_t n = ps.size();
  if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
  if (n < 65536 || threads == 1) return compareRange(prog, cand, ps, 0, n, profile, targetVals, exactVals);

  std::vector<Metrics> parts(threads);
  std::vector<std::thread> pool;
  const size_t chunk = (n + threads - 1) / threads;
  for (unsigned t = 0; t < threads; ++t) {
    const size_t b = t * chunk, e = std::min(n, b + chunk);
    if (b >= e) break;
    pool.emplace_back([&, t, b, e] {
      parts[t] = compareRange(prog, cand, ps, b, e, profile, targetVals, exactVals);
    });
  }
  for (auto& th : pool) th.join();
  Metrics m;
  for (const auto& p : parts) m.merge(p);
  return m;
}

uint64_t domainSize(const Program& prog, uint64_t cap) {
  uint64_t total = 1;
  for (const auto& d : slotDecls(prog.inputs)) {
    const uint64_t n = domainCount(d);
    if (n > cap || total > cap / n) return 0;
    total *= n;
  }
  return total;
}

Metrics compareExhaustive(const Program& prog, const Expr& cand, const Profile& profile,
                          unsigned threads) {
  const std::vector<InputDecl> slots = slotDecls(prog.inputs);
  std::vector<uint64_t> radix;
  uint64_t total = 1;
  for (const auto& d : slots) {
    radix.push_back(domainCount(d));
    total *= radix.back();
  }
  constexpr uint64_t kBlock = 1u << 20;
  Metrics m;
  PointSet ps;
  ps.slot = inputSlots(prog.inputs);
  ps.cols.resize(slots.size());
  std::vector<uint64_t> digit(slots.size(), 0);  // mixed-radix index of the next point
  for (uint64_t start = 0; start < total && m.loosePass; start += kBlock) {
    const uint64_t count = std::min(kBlock, total - start);
    for (auto& c : ps.cols) c.resize(count);
    for (uint64_t i = 0; i < count; ++i) {
      for (size_t k = 0; k < slots.size(); ++k) ps.cols[k][i] = domainValue(slots[k], digit[k]);
      for (size_t k = 0; k < slots.size() && ++digit[k] == radix[k]; ++k) digit[k] = 0;
    }
    m.merge(compare(prog, cand, ps, profile, threads));
  }
  return m;
}

Klass classify(const Program& prog, const Expr& cand, const Metrics& worst) {
  if (worst.bitExact && !containsInexact(prog.target) && !containsInexact(cand))
    return Klass::BitExact;
  if (!worst.pass) return Klass::LessAccurate;
  if (worst.viaExact) return Klass::Accurate;
  if ((prog.budget.kind == Budget::Kind::Color8 || prog.budget.kind == Budget::Kind::Color10) &&
      worst.maxCodeDiff == 0)
    return Klass::Identical8;
  return Klass::Within;
}

}  // namespace sopt
