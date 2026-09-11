#include "verify/verify.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <thread>

#include "ir/eval.hpp"

namespace sopt {

const float* BlockEvaluator::eval(const Expr& e, const PointSet& ps, size_t begin, size_t count,
                                  const Profile& profile) {
  const size_t nn = e.nodes.size();
  scratch.resize(nn * count);
  ptr.assign(nn, nullptr);
  for (size_t i = 0; i < nn; ++i) {
    const Node& n = e.nodes[i];
    float* out = scratch.data() + i * count;
    if (n.op == Op::Input) {
      ptr[i] = ps.cols[n.input].data() + begin;
    } else if (n.op == Op::Const) {
      std::fill(out, out + count, n.value);
      ptr[i] = out;
    } else {
      const auto& oi = info(n.op);
      evalArray(n.op, ptr[n.args[0]], oi.arity > 1 ? ptr[n.args[1]] : nullptr,
                oi.arity > 2 ? ptr[n.args[2]] : nullptr, out, count, profile);
      ptr[i] = out;
    }
  }
  return ptr[e.root];
}

const char* klassName(Klass k) {
  switch (k) {
    case Klass::BitExact: return "bit-exact";
    case Klass::Identical8: return "8-bit identical";
    case Klass::Within: return "within budget";
  }
  return "?";
}

void Metrics::merge(const Metrics& o) {
  if (pass && !o.pass) failPoint = o.failPoint;
  pass = pass && o.pass;
  bitExact = bitExact && o.bitExact;
  maxCodeDiff = std::max(maxCodeDiff, o.maxCodeDiff);
  maxAbs = std::max(maxAbs, o.maxAbs);
  maxRel = std::max(maxRel, o.maxRel);
  checked += o.checked;
  valueHash += o.valueHash;
  codeChanged += o.codeChanged;
}

bool pointWithinBudget(const Budget& b, float t, float c) {
  if (!std::isfinite(c)) return false;
  switch (b.kind) {
    case Budget::Kind::Exact: return c == t;
    case Budget::Kind::Color8: return std::abs(code8(c) - code8(t)) <= b.maxCodeDiff;
    case Budget::Kind::Abs: return std::fabs(static_cast<double>(c) - t) <= b.eps;
    case Budget::Kind::Rel:
      return std::fabs(static_cast<double>(c) - t) <= b.eps * std::max(1.0, std::fabs(double(t)));
  }
  return false;
}

namespace {

Metrics compareRange(const Program& prog, const Expr& cand, const PointSet& ps, size_t begin,
                     size_t end, const Profile& profile, const std::vector<float>* targetVals) {
  constexpr size_t kBlock = 4096;
  BlockEvaluator et, ec;
  Metrics m;
  for (size_t b = begin; b < end; b += kBlock) {
    const size_t count = std::min(kBlock, end - b);
    const float* tv = targetVals ? targetVals->data() + b : et.eval(prog.target, ps, b, count, profile);
    const float* cv = ec.eval(cand, ps, b, count, profile);
    for (size_t i = 0; i < count; ++i) {
      const float t = tv[i], c = cv[i];
      if (!std::isfinite(t)) continue;
      {
        uint32_t bits = std::bit_cast<uint32_t>(c == 0.0f ? 0.0f : c);
        if (std::isnan(c)) bits = 0x7fc00000u;
        uint64_t z = ((b + i) << 32) ^ bits;  // splitmix64 finalizer
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        m.valueHash += z ^ (z >> 31);
      }
      ++m.checked;
      const bool ok = pointWithinBudget(prog.budget, t, c);
      if (!std::isfinite(c)) {
        m.bitExact = false;
        m.maxAbs = m.maxRel = INFINITY;
      } else {
        if (c != t) m.bitExact = false;
        const double err = std::fabs(static_cast<double>(c) - t);
        m.maxAbs = std::max(m.maxAbs, err);
        m.maxRel = std::max(m.maxRel, err / std::max(1e-30, std::fabs(double(t))));
        const int d = std::abs(code8(c) - code8(t));
        m.maxCodeDiff = std::max(m.maxCodeDiff, d);
        if (d) ++m.codeChanged;
      }
      if (!ok && m.pass) {
        m.pass = false;
        m.failPoint = ps.point(b + i);
      }
    }
  }
  return m;
}

}  // namespace

std::vector<float> evalAll(const Expr& e, const PointSet& ps, const Profile& profile) {
  constexpr size_t kBlock = 4096;
  std::vector<float> out(ps.size());
  BlockEvaluator ev;
  for (size_t b = 0; b < ps.size(); b += kBlock) {
    const size_t count = std::min(kBlock, ps.size() - b);
    const float* v = ev.eval(e, ps, b, count, profile);
    std::copy(v, v + count, out.begin() + static_cast<std::ptrdiff_t>(b));
  }
  return out;
}

Metrics compare(const Program& prog, const Expr& cand, const PointSet& ps, const Profile& profile,
                unsigned threads, const std::vector<float>* targetVals) {
  const size_t n = ps.size();
  if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
  if (n < 65536 || threads == 1) return compareRange(prog, cand, ps, 0, n, profile, targetVals);

  std::vector<Metrics> parts(threads);
  std::vector<std::thread> pool;
  const size_t chunk = (n + threads - 1) / threads;
  for (unsigned t = 0; t < threads; ++t) {
    const size_t b = t * chunk, e = std::min(n, b + chunk);
    if (b >= e) break;
    pool.emplace_back([&, t, b, e] { parts[t] = compareRange(prog, cand, ps, b, e, profile, targetVals); });
  }
  for (auto& th : pool) th.join();
  Metrics m;
  for (const auto& p : parts) m.merge(p);
  return m;
}

Klass classify(const Program& prog, const Expr& cand, const Metrics& worst) {
  if (worst.bitExact && !containsInexact(prog.target) && !containsInexact(cand))
    return Klass::BitExact;
  if (prog.budget.kind == Budget::Kind::Color8 && worst.maxCodeDiff == 0) return Klass::Identical8;
  return Klass::Within;
}

}  // namespace sopt
