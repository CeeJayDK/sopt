#include "verify/points.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace sopt {
namespace {

float snap(const InputDecl& d, double v) {
  v = std::clamp(v, d.lo, d.hi);
  if (d.grid > 0 && d.hi > d.lo) {
    const double step = (d.hi - d.lo) / d.grid;
    v = d.lo + std::round((v - d.lo) / step) * step;
  }
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
    return snap(d, v + (up ? step : -step));
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
  if (d.hi <= d.lo) return static_cast<float>(d.lo);
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
  if (d.hi <= d.lo) return static_cast<float>(d.lo);
  if (d.grid > 0) return snap(d, d.lo + (d.hi - d.lo) * static_cast<double>(k) / d.grid);
  return fromOrdered(orderedBits(static_cast<float>(d.lo)) + static_cast<uint32_t>(k));
}

PointSet makeTestPoints(const Program& prog, uint32_t count, uint64_t seed) {
  Rng rng(seed);
  const std::vector<InputDecl> slots = slotDecls(prog.inputs);
  PointSet ps;
  ps.cols.resize(slots.size());
  ps.slot = inputSlots(prog.inputs);
  auto specials = specialPoints(prog, rng);
  const size_t maxSpecial = std::max<size_t>(1, (count * 2) / 3);
  for (size_t i = 0; i < specials.size() && i < maxSpecial; ++i) ps.add(specials[i]);
  while (ps.size() < count) {
    std::vector<float> p(slots.size());
    for (size_t i = 0; i < p.size(); ++i) p[i] = sampleInput(slots[i], rng);
    ps.add(p);
  }
  return ps;
}

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
  }
  while (ps.size() < count) {
    for (size_t i = 0; i < slots.size(); ++i) ps.cols[i].push_back(sampleInput(slots[i], rng));
  }
  return ps;
}

}  // namespace sopt
