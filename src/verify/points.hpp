#pragma once
#include <cstdint>
#include <vector>

#include "ir/expr.hpp"

namespace sopt {

struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed) {}
  uint64_t next() {  // splitmix64
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
};

// Structure of arrays: cols[input][point].
struct PointSet {
  std::vector<std::vector<float>> cols;
  size_t size() const { return cols.empty() ? 0 : cols[0].size(); }
  void add(const std::vector<float>& point);
  std::vector<float> point(size_t i) const;
};

// Small, deliberately chosen set for fingerprints: interval edges, 0, 1, midpoints,
// near-cancellation pairs and random values from the declared domain.
PointSet makeTestPoints(const Program& prog, uint32_t count, uint64_t seed);

// Large random sample from the domain (optionally starting with the special points).
PointSet makeRandomPoints(const Program& prog, size_t count, uint64_t seed, bool withSpecials);

float sampleInput(const InputDecl& d, Rng& rng);

}  // namespace sopt
