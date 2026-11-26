#include "verify/points.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

#include "verify/verify.hpp"

namespace sopt {
namespace {

float snap(const InputDecl& d, double v) {
  v = std::clamp(v, d.lo, d.hi);
  if (d.grid > 0 && d.hi > d.lo) {
    const double step = (d.hi - d.lo) / d.grid;
    v = d.lo + std::round((v - d.lo) / step) * step;
  }
  // An integer input holds its 32 bits (an int's two's complement), as uint ops read them.
  if (d.type == Type::Uint) return std::bit_cast<float>(static_cast<uint32_t>(std::llround(v)));
  return static_cast<float>(v);
}

std::vector<float> specialValues(const InputDecl& d, const Expr& target) {
  std::vector<float> v = {snap(d, d.lo), snap(d, d.hi), snap(d, 0.5 * (d.lo + d.hi))};
  auto addIfInside = [&](double x) {
    if (d.lo <= x && x <= d.hi) v.push_back(snap(d, x));
  };
  addIfInside(0.0);
  addIfInside(1.0);
  addIfInside(-1.0);
  // Tiny magnitudes (the smallest normal float up): uniform samples of a range around 0 never
  // reach them, and rounding tricks fail there (a ceil that gave 0 for x in (0, 6e-8)).
  // Not for compile-time constants (preprocessor definitions, library `const` variables).
  if (!d.compileTime)
    for (double m : {1.1754943508222875e-38, 1e-30, 1e-20, 1e-10, 1e-7, 1e-4}) {
      addIfInside(m);
      addIfInside(-m);
    }
  // Constants from the target are likely thresholds (step, comparisons, clamp):
  // test exactly at them and next to them.
  for (const auto& n : target.nodes) {
    if (n.op != Op::Const) continue;
    for (unsigned k = 0; k < width(n.type); ++k) {
    const float c = n.value[k];
    addIfInside(c);
    if (d.grid > 0 && d.hi > d.lo) {
      const double step = (d.hi - d.lo) / d.grid;
      addIfInside(c - step);
      addIfInside(c + step);
    } else {
      addIfInside(std::nextafter(c, -INFINITY));
      addIfInside(std::nextafter(c, INFINITY));
    }
    }
  }
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

// A value next to v in the domain (adjacent grid point or a few ulps away).
float neighbour(const InputDecl& d, float v, Rng& rng) {
  const bool up = (rng.next() & 1) != 0;
  if (d.grid > 0 && d.hi > d.lo) {
    const double step = (d.hi - d.lo) / d.grid;
    const double x = d.type == Type::Uint ? std::bit_cast<int32_t>(v) : v;  // an integer input's bits (snap)
    return snap(d, x + (up ? step : -step));
  }
  float r = v;
  const int ulps = 1 + static_cast<int>(rng.next() % 4);
  for (int i = 0; i < ulps; ++i)
    r = std::nextafter(r, up ? static_cast<float>(d.hi) : static_cast<float>(d.lo));
  return r;
}

// Vector inputs are sampled per component (slots, see slotDecls).
std::vector<std::vector<float>> specialPoints(const Program& prog, Rng& rng) {
  const std::vector<InputDecl> slots = slotDecls(prog.inputs);
  const size_t n = slots.size();
  std::vector<std::vector<float>> specials(n);
  for (size_t i = 0; i < n; ++i) specials[i] = specialValues(slots[i], prog.target);

  std::vector<std::vector<float>> pts;
  // All inputs at the k-th special value (lo, hi, mid, 0, 1, ...).
  size_t maxK = 0;
  for (const auto& s : specials) maxK = std::max(maxK, s.size());
  for (size_t k = 0; k < maxK; ++k) {
    std::vector<float> p(n);
    for (size_t i = 0; i < n; ++i) p[i] = specials[i][k % specials[i].size()];
    pts.push_back(p);
  }
  // One input at lo/hi, the rest at the opposite edge.
  if (n > 1) {
    for (size_t i = 0; i < n; ++i) {
      for (int edge = 0; edge < 2; ++edge) {
        std::vector<float> p(n);
        for (size_t j = 0; j < n; ++j) {
          const auto& d = slots[j];
          const bool lo = (j == i) == (edge == 0);
          p[j] = snap(d, lo ? d.lo : d.hi);
        }
        pts.push_back(p);
      }
    }
  }
  // Each special value of one input, others random (catches thresholds such as
  // x == 0.5 that the combined points above would only test with equal inputs).
  for (size_t i = 0; i < n; ++i) {
    for (float sv : specials[i]) {
      std::vector<float> p(n);
      for (size_t k = 0; k < n; ++k) p[k] = sampleInput(slots[k], rng);
      p[i] = sv;
      pts.push_back(p);
    }
  }
  // Near-cancellation: input j is next to input i, others random.
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      std::vector<float> p(n);
      for (size_t k = 0; k < n; ++k) p[k] = sampleInput(slots[k], rng);
      p[j] = snap(slots[j], neighbour(slots[i], p[i], rng));
      pts.push_back(p);
    }
  }
  return pts;
}

// Thresholds that depend on other inputs (gray < 4.0 * quant, step(e, x), min / max /
// clamp bounds): where one operand is an input component, points with that input at the
// other operand's value and its neighbours (at random values of the rest). A candidate
// that moves such a threshold (gray < 0.25) differs only in a window that uniform samples
// of a wide range rarely hit.
std::vector<std::vector<float>> thresholdPoints(const Program& prog, Rng& rng) {
  const Expr& e = prog.target;
  const std::vector<InputDecl> slots = slotDecls(prog.inputs);
  const std::vector<uint32_t> first = inputSlots(prog.inputs);
  auto inputSlot = [&](uint32_t a, unsigned c) -> int {
    const Node& n = e.nodes[a];
    if (n.op == Op::Input) return static_cast<int>(first[n.input] + (width(n.type) == 1 ? 0 : c));
    if (n.op == Op::Swizzle && e.nodes[n.args[0]].op == Op::Input) {
      const Node& in = e.nodes[n.args[0]];
      return static_cast<int>(first[in.input] + (width(in.type) == 1 ? 0 : n.swz[width(n.type) == 1 ? 0 : c]));
    }
    return -1;
  };
  struct Pair { uint32_t node, in, other; };
  std::vector<Pair> pairs;
  for (uint32_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    const Op o = n.op;
    if (o == Op::Lt || o == Op::Le || o == Op::Gt || o == Op::Ge || o == Op::Eq || o == Op::Ne ||
        o == Op::Step || o == Op::Min || o == Op::Max) {
      pairs.push_back({i, 0, 1});
      pairs.push_back({i, 1, 0});
    } else if (o == Op::Clamp) {
      pairs.push_back({i, 0, 1});
      pairs.push_back({i, 0, 2});
      pairs.push_back({i, 1, 0});
      pairs.push_back({i, 2, 0});
    }
  }
  std::vector<std::vector<float>> pts;
  if (pairs.empty()) return pts;
  PointSet one;
  one.cols.resize(slots.size());
  one.slot = first;
  BlockEvaluator ev;
  const size_t maxPoints = 512;
  for (const Pair& p : pairs) {
    const Node& n = e.nodes[p.node];
    const uint32_t a = n.args[p.in], b = n.args[p.other];
    const unsigned w = std::max(width(e.nodes[a].type), width(e.nodes[b].type));
    for (unsigned c = 0; c < w; ++c) {
      const int s = inputSlot(a, c);
      if (s < 0) continue;
      const InputDecl& d = slots[s];
      for (int base = 0; base < 4 && pts.size() < maxPoints; ++base) {
        std::vector<float> pt(slots.size());
        for (size_t k = 0; k < pt.size(); ++k) pt[k] = sampleInput(slots[k], rng);
        for (size_t k = 0; k < pt.size(); ++k) one.cols[k].assign(1, pt[k]);
        ev.eval(e, one, 0, 1, kProfileRef);
        const float v = ev.ptr[b][width(e.nodes[b].type) == 1 ? 0 : c][0];
        if (!std::isfinite(v) || v < d.lo || v > d.hi) continue;
        float vals[3] = {v, std::nextafter(v, -INFINITY), std::nextafter(v, INFINITY)};
        if (d.grid > 0 && d.hi > d.lo) {
          const double step = (d.hi - d.lo) / d.grid;
          vals[0] = snap(d, v);
          vals[1] = snap(d, v - step);
          vals[2] = snap(d, v + step);
        }
        for (float x : vals) {
          if (x < d.lo || x > d.hi) continue;
          pt[s] = x;
          pts.push_back(pt);
        }
      }
    }
  }
  // Several conditions at once (a == 0 && b == 0, nested selects): points with every compared input at its
  // threshold together (all comparisons, or a random half), then each one moved to its neighbours. One input
  // at a time almost never makes all of them hold.
  struct Cmp { uint32_t node, in, other; int slot; };
  std::vector<Cmp> cmps;
  for (const Pair& p : pairs) {
    const Node& n = e.nodes[p.node];
    if (n.op < Op::Lt || n.op > Op::Ne) continue;
    const int s = inputSlot(n.args[p.in], 0);
    if (s >= 0) cmps.push_back({p.node, n.args[p.in], n.args[p.other], s});
  }
  if (cmps.size() < 2) return pts;
  auto valueAt = [&](const std::vector<float>& pt, uint32_t node) {
    for (size_t k = 0; k < pt.size(); ++k) one.cols[k].assign(1, pt[k]);
    ev.eval(e, one, 0, 1, kProfileRef);
    return ev.ptr[node][0][0];
  };
  for (int base = 0; base < 8 && pts.size() < 2 * maxPoints; ++base) {
    std::vector<float> pt(slots.size());
    for (size_t k = 0; k < pt.size(); ++k) pt[k] = sampleInput(slots[k], rng);
    std::vector<const Cmp*> used;
    for (const Cmp& c : cmps) {
      if (base >= 4 && (rng.next() & 1)) continue;
      const InputDecl& d = slots[c.slot];
      const float v = valueAt(pt, c.other);
      if (!std::isfinite(v) || v < d.lo || v > d.hi) continue;
      pt[c.slot] = d.grid > 0 && d.hi > d.lo ? snap(d, v) : v;
      used.push_back(&c);
    }
    if (used.empty()) continue;
    pts.push_back(pt);
    for (const Cmp* c : used) {
      const InputDecl& d = slots[c->slot];
      const double step = d.grid > 0 && d.hi > d.lo ? (d.hi - d.lo) / d.grid : 0.0;
      for (int dir : {-1, 1}) {
        std::vector<float> q = pt;
        const float x = q[c->slot];
        q[c->slot] = step > 0.0 ? snap(d, x + dir * step) : std::nextafter(x, dir < 0 ? -INFINITY : INFINITY);
        if (q[c->slot] >= d.lo && q[c->slot] <= d.hi) pts.push_back(q);
      }
    }
  }
  return pts;
}

// Comparison boundaries (2026-11-08): a comparison of expressions (abs(uv.x - pos.x) < 0.003, a character box
// step(0.0, min(p.x, p.y))) can hold only in a window that random points never hit, so a candidate without it (0.0 for
// a marker, Out without its character) passed as bit-exact. From random starts, each comparison component in turn is
// flipped by a search along one input it depends on (a scan of the range, zoomed in on the closest approach when no
// sample flips it), and the point stays there for the next comparison, so several conditions hold together
// (all(abs(d) < r)). Kept: the point inside each flip and the two floats on either side of its boundary.
// The same search, other flips (owner, 2026-11-10: verdicts there decided by the seed): Jumps, where floor / ceil /
// round / trunc / frac change their integer part (step(0.024, y % 0.025)); Cancels, where an add / sub / mad / dot
// crosses zero (dot(c, w) + 0.5 * hash ~ 0: the relative error of a near-cancellation).
enum class Flip { Compare, Jump, Cancel };
std::vector<std::vector<float>> boundaryPoints(const Program& prog, Rng& rng, Flip kind = Flip::Compare) {
  const Expr& e = prog.target;
  const std::vector<InputDecl> slots = slotDecls(prog.inputs);
  const std::vector<uint32_t> first = inputSlots(prog.inputs);
  std::vector<std::vector<float>> pts;
  // The inputs each node depends on (bit i: input i; inputs past 63 count as every input).
  std::vector<uint64_t> deps(e.nodes.size(), 0);
  for (size_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    if (n.op == Op::Input) deps[i] = n.input < 64 ? uint64_t{1} << n.input : ~uint64_t{0};
    for (unsigned a = 0; a < n.nargs; ++a) deps[i] |= deps[n.args[a]];
  }
  struct Cmp {
    uint32_t node;
    unsigned c;
    std::vector<size_t> slots;  // with a finite range, that the comparison depends on
  };
  std::vector<Cmp> cmps;
  for (uint32_t i = 0; i < e.nodes.size() && cmps.size() < 64; ++i) {
    const Node& n = e.nodes[i];
    const Op o = n.op;
    const bool take = kind == Flip::Compare ? (o >= Op::Lt && o <= Op::Ne) || o == Op::Step
                      : kind == Flip::Jump  ? o == Op::Floor || o == Op::Ceil || o == Op::Round || o == Op::Trunc ||
                                                 o == Op::Frac
                                            : o == Op::Add || o == Op::Sub || o == Op::Mad || o == Op::Dot;
    if (!take) continue;
    std::vector<size_t> on;
    for (size_t in = 0; in < prog.inputs.size(); ++in) {
      if (!(deps[i] >> std::min<size_t>(in, 63) & 1)) continue;
      const size_t end = in + 1 < first.size() ? first[in + 1] : slots.size();
      for (size_t s = first[in]; s < end; ++s)
        if (std::isfinite(slots[s].lo) && std::isfinite(slots[s].hi) && slots[s].hi > slots[s].lo) on.push_back(s);
    }
    if (on.empty()) continue;
    const unsigned w = width(n.type);
    for (unsigned c = 0; c < w; ++c) cmps.push_back({i, c, on});
  }
  if (cmps.empty()) return pts;
  constexpr size_t kScan = 128;
  PointSet line;
  line.cols.resize(slots.size());
  line.slot = first;
  BlockEvaluator ev;
  auto evalAt = [&](const std::vector<float>& pt, size_t s, const std::vector<float>& xs) {
    for (size_t k = 0; k < slots.size(); ++k) line.cols[k].assign(xs.size(), pt[k]);
    line.cols[s] = xs;
    ev.eval(e, line, 0, xs.size(), kProfileRef);
  };
  auto at = [&](uint32_t a, unsigned c, size_t j) { return double(ev.ptr[a][width(e.nodes[a].type) == 1 ? 0 : c][j]); };
  auto out = [&](const Cmp& k, size_t j) -> double {  // what flips: the result, frac's integer part, a sum's sign
    const double v = at(k.node, k.c, j);
    if (kind == Flip::Cancel) return v > 0.0 ? 1.0 : v < 0.0 ? -1.0 : 0.0;
    if (e.nodes[k.node].op == Op::Frac) return std::floor(at(e.nodes[k.node].args[0], k.c, j));
    return v;
  };
  auto gap = [&](const Cmp& k, size_t j) {  // how close it is to flipping
    const Node& n = e.nodes[k.node];
    if (kind == Flip::Cancel) return std::fabs(at(k.node, k.c, j));
    if (kind == Flip::Jump) {  // distance to the next integer (round: to the next half)
      const double x = at(n.args[0], k.c, j) + (n.op == Op::Round ? 0.5 : 0.0);
      return std::fabs(x - std::nearbyint(x));
    }
    return std::fabs(at(n.args[0], k.c, j) - at(n.args[1], k.c, j));
  };
  size_t work = 0;  // line evaluations: bounds the time on regions with many comparisons that never flip
  for (int base = 0; base < 8 && pts.size() < 1024 && work < 4000; ++base) {
    std::vector<float> pt(slots.size());
    for (size_t k = 0; k < pt.size(); ++k) pt[k] = sampleInput(slots[k], rng);
    std::vector<size_t> order(cmps.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = k;
    for (size_t k = order.size(); k > 1; --k) std::swap(order[k - 1], order[rng.next() % k]);
    // Per input moved: the window it was moved into. Points spread over all of them together reach what lies
    // inside the windows (a character's lit pixels inside its box: floor steps, no comparison), 2026-11-08.
    std::vector<std::pair<float, float>> window(slots.size(), {NAN, NAN});
    for (size_t ci : order) {
      const Cmp& k = cmps[ci];
      bool moved = false;
      for (size_t t = 0, off = rng.next() % k.slots.size(); t < k.slots.size() && !moved && work < 4000; ++t) {
        const size_t s = k.slots[(t + off) % k.slots.size()];
        const InputDecl& d = slots[s];
        evalAt(pt, s, {pt[s]});
        const double cur = out(k, 0);
        double lo = d.lo, hi = d.hi;
        std::vector<float> xs(kScan);
        bool rejected = false;
        float bestX = pt[s];  // Cancel: the closest approach to zero so far
        double bestG = gap(cmps[ci], 0);
        for (int zoom = 0; zoom < 6 && !moved && !rejected && hi > lo; ++zoom, ++work) {
          for (size_t j = 0; j < kScan; ++j) xs[j] = snap(d, lo + (hi - lo) * double(j) / double(kScan - 1));
          evalAt(pt, s, xs);
          size_t hit = kScan, best = kScan;
          double maxGap = 0.0;
          for (size_t j = 0; j < kScan; ++j) {
            const double g = gap(k, j);
            if (!std::isfinite(g)) continue;
            maxGap = std::max(maxGap, g);
            if (out(k, j) != cur && (hit == kScan || std::fabs(xs[j] - pt[s]) < std::fabs(xs[hit] - pt[s]))) hit = j;
            if (best == kScan || g < gap(k, best)) best = j;
          }
          if (best < kScan && gap(k, best) < bestG) bestG = gap(k, best), bestX = xs[best];
          if (hit < kScan) {
            // The flipped stretch of the scan (and one sample past each end): spread points go there below.
            size_t a = hit, b = hit;
            while (a > 0 && out(k, a - 1) != cur) --a;
            while (b + 1 < kScan && out(k, b + 1) != cur) ++b;
            const std::pair<float, float> win = {xs[a > 0 ? a - 1 : a], xs[b + 1 < kScan ? b + 1 : b]};
            const float inside = xs[hit];
            // The boundary between the flipped sample and its neighbour towards the start, by bisection.
            float in = xs[hit], other = hit > 0 && xs[hit - 1] != in ? xs[hit - 1] : xs[std::min(hit + 1, kScan - 1)];
            evalAt(pt, s, {other});
            if (out(k, 0) == cur)
              for (int it = 0; it < 40; ++it) {
                const float mid = snap(d, 0.5 * (double(in) + double(other)));
                if (mid == in || mid == other) break;
                evalAt(pt, s, {mid});
                (out(k, 0) != cur ? in : other) = mid;
              }
            if (kind == Flip::Cancel) {  // a sign flip across a jump (a hash's frac) is no cancellation: next input
              evalAt(pt, s, {in, other});
              if (!(gap(k, 0) <= 1e-4 * maxGap && gap(k, 1) <= 1e-4 * maxGap)) {
                rejected = true;
                continue;
              }
            }
            window[s] = win;
            std::vector<float> q = pt;
            for (float x : {in, other}) q[s] = x, pts.push_back(q);
            pt[s] = inside;  // well inside the flip: stays true while the next comparisons move
            pts.push_back(pt);
            moved = true;
          } else if (best < kScan) {
            const size_t a = best > 0 ? best - 1 : 0, b = std::min(best + 1, kScan - 1);
            lo = xs[a];
            hi = xs[b];
          }
        }
        // Cancel: no zero crossing along this input: keep its closest approach, the next input goes on from there
        // (a sum that reaches zero only with two inputs at once: dot(c, w) + 0.5 * hash, NightVision.fx).
        if (kind == Flip::Cancel && !moved) pt[s] = bestX;
      }
    }
    for (int j = 0; j < 32; ++j) {
      std::vector<float> q = pt;
      bool any = false;
      for (size_t s = 0; s < slots.size(); ++s)
        if (!std::isnan(window[s].first)) {
          const double u = double(rng.next() >> 11) * 0x1p-53;
          q[s] = snap(slots[s], window[s].first + (double(window[s].second) - window[s].first) * u);
          any = true;
        }
      if (!any) break;
      pts.push_back(q);
    }
  }
  return pts;
}

}  // namespace

void PointSet::add(const std::vector<float>& point) {
  if (cols.size() < point.size()) cols.resize(point.size());
  for (size_t i = 0; i < point.size(); ++i) cols[i].push_back(point[i]);
}

std::vector<float> PointSet::point(size_t idx) const {
  std::vector<float> p(cols.size());
  for (size_t i = 0; i < cols.size(); ++i) p[i] = cols[i][idx];
  return p;
}

float sampleInput(const InputDecl& d, Rng& rng) {
  if (d.hi <= d.lo) return snap(d, d.lo);
  if (d.grid > 0) {
    const uint64_t k = rng.next() % (static_cast<uint64_t>(d.grid) + 1);
    return snap(d, d.lo + (d.hi - d.lo) * static_cast<double>(k) / d.grid);
  }
  // Wide positive ranges: half the samples log-uniform so small values are covered.
  if (d.lo > 0.0 && d.hi / d.lo > 1000.0 && (rng.next() & 1)) {
    const double l = std::log(d.lo), h = std::log(d.hi);
    return snap(d, std::exp(l + (h - l) * rng.uniform()));
  }
  return snap(d, d.lo + (d.hi - d.lo) * rng.uniform());
}

namespace {

// Monotone map of float32 to uint32 (for enumerating every float in an interval).
uint32_t orderedBits(float f) {
  const uint32_t b = std::bit_cast<uint32_t>(f);
  return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}
float fromOrdered(uint32_t o) {
  return std::bit_cast<float>((o & 0x80000000u) ? (o & 0x7fffffffu) : ~o);
}

}  // namespace

uint64_t domainCount(const InputDecl& d) {
  if (d.hi <= d.lo) return 1;
  if (d.grid > 0) return static_cast<uint64_t>(d.grid) + 1;
  return uint64_t{orderedBits(static_cast<float>(d.hi))} - orderedBits(static_cast<float>(d.lo)) + 1;
}

float domainValue(const InputDecl& d, uint64_t k) {
  if (d.hi <= d.lo) return snap(d, d.lo);
  if (d.grid > 0) return snap(d, d.lo + (d.hi - d.lo) * static_cast<double>(k) / d.grid);
  return fromOrdered(orderedBits(static_cast<float>(d.lo)) + static_cast<uint32_t>(k));
}

PointSet makeTestPoints(const Program& prog, uint32_t count, uint64_t seed, bool coincidences, bool more) {
  Rng rng(seed);
  const std::vector<InputDecl> slots = slotDecls(prog.inputs);
  PointSet ps;
  ps.cols.resize(slots.size());
  ps.slot = inputSlots(prog.inputs);
  auto specials = specialPoints(prog, rng);
  const size_t maxSpecial = std::max<size_t>(1, (count * 2) / 3);
  for (size_t i = 0; i < specials.size() && i < maxSpecial; ++i) ps.add(specials[i]);
  // Extra points (the flags) come on top of the random ones, before them and from their own generator: the random
  // points stay what they are without the flags, and stay last (the top-down lookup points). They used to replace
  // the random points, which left the lookups on flat zero points (2026-11-05).
  Rng rx(seed + 77);
  const size_t before = ps.size();
  std::vector<float> p(slots.size());
  auto random = [&]() {
    for (size_t i = 0; i < p.size(); ++i) p[i] = sampleInput(slots[i], rx);
  };
  if (coincidences) {
    // 0 where the domain holds it (else its nearest end); the others random. At most count / 2.
    const size_t limit = ps.size() + count / 2;
    for (size_t i = 0; i < p.size(); ++i) p[i] = snap(slots[i], 0.0);
    if (ps.size() < limit) ps.add(p);
    for (size_t i = 0; i < slots.size() && ps.size() < limit; ++i)
      for (size_t j = i + 1; j < slots.size() && ps.size() < limit; ++j) {
        random();
        p[i] = snap(slots[i], 0.0);
        p[j] = snap(slots[j], 0.0);
        ps.add(p);
      }
  }
  if (more) {
    // --fp-more: the special points that did not fit, threshold points (an input at a comparison / min / max / step
    // operand's value) and every pair of inputs equal. At most count / 2.
    const size_t limit = ps.size() + count / 2;
    for (size_t i = maxSpecial; i < specials.size() && ps.size() < limit; ++i) ps.add(specials[i]);
    for (const auto& q : thresholdPoints(prog, rx))
      if (ps.size() < limit) ps.add(q);
    for (size_t i = 0; i < slots.size() && ps.size() < limit; ++i)
      for (size_t j = i + 1; j < slots.size() && ps.size() < limit; ++j) {
        random();
        p[j] = snap(slots[j], p[i]);
        ps.add(p);
      }
  }
  const size_t extra = ps.size() - before;
  while (ps.size() < count + extra) {
    for (size_t i = 0; i < p.size(); ++i) p[i] = sampleInput(slots[i], rng);
    ps.add(p);
  }
  return ps;
}

namespace {

// Coincidence points (owner, 2026-10-10): every input on a coarse grid (its range in eighths, and 0), where sums and
// differences of inputs meet exactly (x + y == z). Random samples never hit those, so a candidate with == / != that is
// wrong only there passed as bit-exact (any(x, y, z) for x, y, z >= 0 as `-y != x - z ? 1.0 : z`, wrong at 0.25, 0.25,
// 0.5). All combinations when there are at most `limit`, else `limit` random ones.
std::vector<std::vector<float>> coincidencePoints(const std::vector<InputDecl>& slots, Rng& rng, size_t limit) {
  std::vector<std::vector<float>> vals(slots.size());
  size_t combos = 1;
  for (size_t i = 0; i < slots.size(); ++i) {
    const InputDecl& d = slots[i];
    if (!(d.hi > d.lo) || !std::isfinite(d.lo) || !std::isfinite(d.hi)) {
      vals[i] = {snap(d, d.lo)};
      continue;
    }
    for (int k = 0; k <= 8; ++k) {
      double v = d.lo + (d.hi - d.lo) * k / 8.0;
      if (d.type == Type::Uint) v = std::round(v);
      vals[i].push_back(snap(d, v));
    }
    if (d.lo < 0.0 && d.hi > 0.0) vals[i].push_back(snap(d, 0.0));
    std::sort(vals[i].begin(), vals[i].end());
    vals[i].erase(std::unique(vals[i].begin(), vals[i].end()), vals[i].end());
    combos = std::min(combos * vals[i].size(), limit + 1);
  }
  std::vector<std::vector<float>> out;
  std::vector<float> p(slots.size());
  if (combos <= limit) {
    std::vector<size_t> k(slots.size(), 0);
    for (size_t n = 0; n < combos; ++n) {
      for (size_t i = 0; i < slots.size(); ++i) p[i] = vals[i][k[i]];
      out.push_back(p);
      for (size_t i = 0; i < slots.size() && ++k[i] == vals[i].size(); ++i) k[i] = 0;
    }
  } else {
    for (size_t n = 0; n < limit; ++n) {
      for (size_t i = 0; i < slots.size(); ++i) p[i] = vals[i][rng.next() % vals[i].size()];
      out.push_back(p);
    }
  }
  return out;
}

}  // namespace

PointSet makeRandomPoints(const Program& prog, size_t count, uint64_t seed, bool withSpecials) {
  Rng rng(seed);
  const std::vector<InputDecl> slots = slotDecls(prog.inputs);
  PointSet ps;
  ps.cols.resize(slots.size());
  ps.slot = inputSlots(prog.inputs);
  for (auto& c : ps.cols) c.reserve(count);
  if (withSpecials) {
    for (const auto& p : specialPoints(prog, rng)) {
      if (ps.size() >= count) break;
      ps.add(p);
    }
    for (const auto& p : thresholdPoints(prog, rng)) {
      if (ps.size() >= count) break;
      ps.add(p);
    }
    Rng rb(seed + 99);  // its own generator: the points after these stay what they were
    for (const auto& p : boundaryPoints(prog, rb)) {
      if (ps.size() >= count) break;
      ps.add(p);
    }
    Rng rj(0x7a3b), rc(0xca9c);  // fixed sets (owner, 2026-11-10): the same points for every seed
    if (prog.budget.jumpPoints)
      for (const auto& p : boundaryPoints(prog, rj, Flip::Jump)) {
        if (ps.size() >= count) break;
        ps.add(p);
      }
    if (prog.budget.cancelPoints)
      for (const auto& p : boundaryPoints(prog, rc, Flip::Cancel)) {
        if (ps.size() >= count) break;
        ps.add(p);
      }
    for (const auto& p : coincidencePoints(slots, rng, std::min<size_t>(4096, count / 8))) {
      if (ps.size() >= count) break;
      ps.add(p);
    }
  }
  while (ps.size() < count) {
    for (size_t i = 0; i < slots.size(); ++i) ps.cols[i].push_back(sampleInput(slots[i], rng));
  }
  return ps;
}

}  // namespace sopt
