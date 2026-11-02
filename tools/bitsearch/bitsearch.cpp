// bitsearch: the cheapest integer formula for a small table read by pixel position (a dither decoder). sopt searches
// float math only; this searches uint math over x and y.
//
//   sopt-bitsearch --bayer 8 [--card nvidia|intel|flat] [--top 10] [--time 300]
//   sopt-bitsearch --table 4 v0,v1,...        (a tile of 4 x 4 values, row by row)
//   sopt-bitsearch --check                    (self-check: finds Bayer 4 x 4)
//
// Bottom-up by operation count: every function of x and y reachable with constants (and, multiply, shifts) and with
// each other, equal functions merged. A result is one such function, or two with no bit in common added (meet in the
// middle, so the reach is about twice the bank's depth). The value may also come out shifted up: float(v) * scale then
// undoes it, so no right shift is needed. Also accepted: x and y swapped (the same pattern on its side).
// Costs are from OpBench (tput, fma per op; GTX 1660 and UHD 630). Each result is checked again on random inputs.
//
// Not searched: three or more pieces, constants outside the lists below, xor with a constant.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

enum Op : uint8_t { kLeaf, kAnd, kMul, kShl, kShr, kAddC, kAdd, kSub, kXor, kOr, kAndV, kMulV };
struct Weights {
  double andOp, mul, shl, shr, add, logic;  // logic: or, xor
};
// Above 0 everywhere, so a useless operation never ties.
const std::map<std::string, Weights> kCards = {
    {"nvidia", {0.25, 0.25, 0.25, 4.0, 0.25, 4.0}},
    {"intel", {3.7, 7.2, 7.2, 7.2, 3.7, 7.2}},
    {"flat", {1.0, 1.0, 1.0, 1.0, 1.0, 1.0}},
};

struct Entry {
  Op op = kLeaf;
  uint32_t a = 0, b = 0;  // operands (entries), or a and a constant; kLeaf: a is the leaf
  double cost = 0.0;      // the tree's operations (shared ones counted twice, until the final count)
};

struct Search {
  Weights w;
  size_t points = 0, cap = 0;
  uint32_t tile = 0;
  std::vector<uint32_t> xs, ys, vals;  // vals: entry * points
  std::vector<Entry> entries;
  std::unordered_map<uint64_t, uint32_t> seen;
  std::vector<size_t> levelEnd;
  bool full = false;

  static uint64_t hashOf(const uint32_t* v, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ v[i]) * 0xFF51AFD7ED558CCDull, h ^= h >> 29;
    return h;
  }
  const uint32_t* at(uint32_t e) const { return vals.data() + size_t(e) * points; }
  double opCost(Op op) const {
    switch (op) {
      case kAnd: case kAndV: return w.andOp;
      case kMul: case kMulV: return w.mul;
      case kShl: return w.shl;
      case kShr: return w.shr;
      case kAddC: case kAdd: case kSub: return w.add;
      case kXor: case kOr: return w.logic;
      default: return 0.0;
    }
  }
  // Keeps a function not seen yet, or the cheaper way to an old one.
  void add(const uint32_t* v, Op op, uint32_t a, uint32_t b, double cost) {
    bool same = true;
    for (size_t i = 1; i < points && same; ++i) same = v[i] == v[0];
    if (same) return;  // a constant
    const uint64_t h = hashOf(v, points);
    const auto it = seen.find(h);
    if (it != seen.end()) {
      Entry& e = entries[it->second];
      if (e.op != kLeaf && cost < e.cost) e.op = op, e.a = a, e.b = b, e.cost = cost;
      return;
    }
    if (entries.size() >= cap) {
      full = true;
      return;
    }
    seen.emplace(h, uint32_t(entries.size()));
    Entry e;
    e.op = op, e.a = a, e.b = b, e.cost = cost;
    entries.push_back(e);
    vals.insert(vals.end(), v, v + points);
  }
};

constexpr int kLeaves = 6;
const char* const kLeafNames[kLeaves] = {"x", "y", "x & M", "y & M", "x ^ y", "(x ^ y) & M"};

uint32_t leafValue(int leaf, uint32_t x, uint32_t y, uint32_t mask) {
  switch (leaf) {
    case 0: return x;
    case 1: return y;
    case 2: return x & mask;
    case 3: return y & mask;
    case 4: return x ^ y;
    default: return (x ^ y) & mask;
  }
}

uint32_t apply(Op op, uint32_t a, uint32_t b) {
  switch (op) {
    case kAnd: case kAndV: return a & b;
    case kMul: case kMulV: return a * b;
    case kShl: return a << (b & 31);
    case kShr: return a >> (b & 31);
    case kAddC: case kAdd: return a + b;
    case kSub: return a - b;
    case kXor: return a ^ b;
    case kOr: return a | b;
    default: return 0;
  }
}

std::vector<std::pair<Op, uint32_t>> constantOps() {
  std::vector<std::pair<Op, uint32_t>> c;
  auto bits = [](uint32_t k) {
    int n = 0;
    for (; k; k &= k - 1) ++n;
    return n;
  };
  for (uint32_t m = 1; m < 256; ++m) c.push_back({kAnd, m});
  for (int k = 0; k < 6; ++k)
    for (const uint32_t m : {0x555u, 0xAAAu, 0x249u, 0x492u, 0x924u}) c.push_back({kAnd, m << k});
  for (uint32_t k = 3; k < 1024; ++k)  // up to 3 bits set; a power of two is a shift
    if (bits(k) >= 2 && bits(k) <= 3) c.push_back({kMul, k});
  for (uint32_t s = 1; s <= 10; ++s) {
    c.push_back({kShl, s});
    c.push_back({kShr, s});
  }
  return c;
}

// One more level: constants on the last level, and pairs whose operations add up to it.
void grow(Search& s, size_t level, const std::vector<std::pair<Op, uint32_t>>& constants, double deadline,
          const std::chrono::steady_clock::time_point start) {
  auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
  const size_t n = s.points;
  std::vector<uint32_t> v(n);
  const size_t from = level >= 2 ? s.levelEnd[level - 2] : 0, to = s.levelEnd[level - 1];
  for (size_t e = from; e < to && !s.full; ++e) {
    if ((e & 1023) == 0 && seconds() > deadline) return;
    const double cost = s.entries[e].cost;
    for (const auto& [op, k] : constants) {
      const uint32_t* a = s.at(uint32_t(e));  // vals may move
      switch (op) {
        case kAnd: for (size_t i = 0; i < n; ++i) v[i] = a[i] & k; break;
        case kMul: for (size_t i = 0; i < n; ++i) v[i] = a[i] * k; break;
        case kShl: for (size_t i = 0; i < n; ++i) v[i] = a[i] << k; break;
        default: for (size_t i = 0; i < n; ++i) v[i] = a[i] >> k; break;
      }
      s.add(v.data(), op, uint32_t(e), k, cost + s.opCost(op));
    }
  }
  // Pairs: operations i + j + 1 = level + 1. Only while the pairs stay few.
  for (size_t i = 0; i * 2 <= level; ++i) {
    const size_t j = level - 1 - i;
    if (j >= level || i > j) continue;
    const size_t a0 = i ? s.levelEnd[i - 1] : 0, a1 = s.levelEnd[i];
    const size_t b0 = j ? s.levelEnd[j - 1] : 0, b1 = s.levelEnd[j];
    if ((a1 - a0) * (b1 - b0) > 40000000) continue;
    for (size_t a = a0; a < a1 && !s.full; ++a) {
      if (seconds() > deadline) return;
      for (size_t b = b0; b < b1; ++b)
        for (const Op op : {kAdd, kSub, kXor, kOr, kAndV, kMulV}) {
          const uint32_t* pa = s.at(uint32_t(a));
          const uint32_t* pb = s.at(uint32_t(b));
          for (size_t q = 0; q < n; ++q) v[q] = apply(op, pa[q], pb[q]);
          s.add(v.data(), op, uint32_t(a), uint32_t(b), s.entries[a].cost + s.entries[b].cost + s.opCost(op));
        }
    }
  }
}

std::string print(const Search& s, uint32_t e) {
  const Entry& n = s.entries[e];
  auto hex = [](uint32_t k) { return std::to_string(k) + "u"; };
  switch (n.op) {
    case kLeaf: return std::string("(") + kLeafNames[n.a] + ")";
    case kAnd: return "(" + print(s, n.a) + " & " + hex(n.b) + ")";
    case kMul: return "(" + print(s, n.a) + " * " + hex(n.b) + ")";
    case kShl: return "(" + print(s, n.a) + " << " + std::to_string(n.b) + ")";
    case kShr: return "(" + print(s, n.a) + " >> " + std::to_string(n.b) + ")";
    case kAdd: return "(" + print(s, n.a) + " + " + print(s, n.b) + ")";
    case kSub: return "(" + print(s, n.a) + " - " + print(s, n.b) + ")";
    case kXor: return "(" + print(s, n.a) + " ^ " + print(s, n.b) + ")";
    case kOr: return "(" + print(s, n.a) + " | " + print(s, n.b) + ")";
    case kAndV: return "(" + print(s, n.a) + " & " + print(s, n.b) + ")";
    case kMulV: return "(" + print(s, n.a) + " * " + print(s, n.b) + ")";
    default: return "?";
  }
}

uint32_t evaluate(const Search& s, uint32_t e, uint32_t x, uint32_t y) {
  const Entry& n = s.entries[e];
  if (n.op == kLeaf) return leafValue(int(n.a), x, y, s.tile - 1);
  const uint32_t a = evaluate(s, n.a, x, y);
  const bool withConstant = n.op == kAnd || n.op == kMul || n.op == kShl || n.op == kShr;
  return apply(n.op, a, withConstant ? n.b : evaluate(s, n.b, x, y));
}

// The real cost: each node once, leaves' setup included (x & M, y & M, x ^ y are shared).
double dagCost(const Search& s, const std::vector<uint32_t>& roots) {
  std::set<uint32_t> done;
  std::set<int> leaves;
  double cost = 0.0;
  std::vector<uint32_t> todo = roots;
  while (!todo.empty()) {
    const uint32_t e = todo.back();
    todo.pop_back();
    if (!done.insert(e).second) continue;
    const Entry& n = s.entries[e];
    if (n.op == kLeaf) {
      leaves.insert(int(n.a));
      continue;
    }
    cost += s.opCost(n.op);
    todo.push_back(n.a);
    if (n.op != kAnd && n.op != kMul && n.op != kShl && n.op != kShr) todo.push_back(n.b);
  }
  const bool mx = leaves.count(2), my = leaves.count(3), xr = leaves.count(4), xm = leaves.count(5);
  if (mx || xm) cost += s.w.andOp;
  if (my || xm) cost += s.w.andOp;
  if (xr) cost += s.w.logic;
  if (xm) cost += s.w.logic;
  return cost;
}

struct Found {
  double cost;
  std::string text;
  int scale;
};

// tile x tile values, row by row. Returns the results, cheapest first.
std::vector<Found> run(const std::vector<uint32_t>& table, uint32_t tile, const std::string& card, size_t top,
                       double limit) {
  Search s;
  s.w = kCards.at(card);
  s.tile = tile;
  const uint32_t mask = tile - 1;
  std::mt19937 rng(12345);
  for (uint32_t y = 0; y < tile; ++y)
    for (uint32_t x = 0; x < tile; ++x) s.xs.push_back(x), s.ys.push_back(y);
  for (int k = 0; k < 16; ++k) s.xs.push_back(rng()), s.ys.push_back(rng());  // wrong for high bits: out
  s.points = s.xs.size();
  s.cap = std::min<size_t>(800000000 / (s.points * 4), 8000000);  // 800 MB at most
  s.vals.reserve(std::min<size_t>(s.cap, 200000) * s.points);
  std::vector<uint32_t> v(s.points);
  for (int leaf = 0; leaf < kLeaves; ++leaf) {
    for (size_t i = 0; i < s.points; ++i) v[i] = leafValue(leaf, s.xs[i], s.ys[i], mask);
    s.add(v.data(), kLeaf, uint32_t(leaf), 0, 0.0);
  }
  s.levelEnd.push_back(s.entries.size());
  // Targets: the value shifted up by 0 to 6 bits, x and y also swapped.
  struct Target {
    std::vector<uint32_t> v;
    int scale;
    bool swapped;
  };
  std::vector<Target> targets;
  for (const bool swapped : {false, true})
    for (int scale = 0; scale <= 6; ++scale) {
      Target t;
      t.scale = scale, t.swapped = swapped;
      t.v.resize(s.points);
      for (size_t i = 0; i < s.points; ++i) {
        const uint32_t x = s.xs[i] & mask, y = s.ys[i] & mask;
        t.v[i] = table[swapped ? x * tile + y : y * tile + x] << scale;
      }
      targets.push_back(std::move(t));
    }
  const auto start = std::chrono::steady_clock::now();
  const auto constants = constantOps();
  std::map<std::string, Found> results;
  auto check = [&](const std::vector<uint32_t>& roots, const Target& t) {
    std::mt19937 r(777);
    for (int k = 0; k < 20000; ++k) {
      const uint32_t x = r(), y = r();
      uint32_t sum = 0;
      for (const uint32_t e : roots) sum += evaluate(s, e, x, y);
      const uint32_t cx = x & mask, cy = y & mask;
      if (sum != table[t.swapped ? cx * tile + cy : cy * tile + cx] << t.scale) return;
    }
    std::string text;
    for (const uint32_t e : roots) text += (text.empty() ? "" : " + ") + print(s, e);
    const double cost = dagCost(s, roots) + (roots.size() > 1 ? s.w.add : 0.0);
    results[text] = {cost, text + (t.swapped ? "  [x, y swapped]" : ""), t.scale};
  };
  size_t searched = 0;
  auto look = [&] {
    for (size_t e = searched; e < s.entries.size(); ++e)
      for (const Target& t : targets) {
        const uint32_t* a = s.at(uint32_t(e));
        bool inside = true;
        for (size_t i = 0; i < s.points && inside; ++i) inside = (a[i] & ~t.v[i]) == 0;
        if (!inside) continue;
        for (size_t i = 0; i < s.points; ++i) v[i] = t.v[i] - a[i];
        if (std::all_of(v.begin(), v.end(), [](uint32_t q) { return q == 0; })) {
          check({uint32_t(e)}, t);
          continue;
        }
        const auto it = s.seen.find(Search::hashOf(v.data(), s.points));
        if (it != s.seen.end() && it->second >= e) check({uint32_t(e), it->second}, t);
      }
    searched = s.entries.size();
  };
  look();
  for (size_t level = 1; level <= 5; ++level) {
    const size_t before = s.entries.size();
    grow(s, level, constants, limit, start);
    s.levelEnd.push_back(s.entries.size());
    searched = 0;  // an old piece may pair with a new one
    look();
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::fprintf(stderr, "level %zu: %zu functions (%zu new), %zu results, %.0f s%s\n", level, s.entries.size(),
                 s.entries.size() - before, results.size(), took, s.full ? ", bank full: this level is incomplete" : "");
    if (s.full || took > limit) break;
  }
  std::vector<Found> out;
  for (const auto& [k, f] : results) out.push_back(f);
  std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.cost < b.cost; });
  if (out.size() > top) out.resize(top);
  return out;
}

std::vector<uint32_t> bayer(uint32_t tile) {
  int levels = 0;
  while ((1u << levels) < tile) ++levels;
  std::vector<uint32_t> t;
  for (uint32_t y = 0; y < tile; ++y)
    for (uint32_t x = 0; x < tile; ++x) {
      uint32_t v = 0;
      for (int l = 0; l < levels; ++l) v = v << 2 | ((x ^ y) >> l & 1) << 1 | (y >> l & 1);
      t.push_back(v);
    }
  return t;
}

}  // namespace

int main(int argc, char** argv) {
  std::string card = "nvidia";
  std::vector<uint32_t> table;
  uint32_t tile = 0;
  size_t top = 10;
  double limit = 300.0;
  bool selfCheck = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--bayer") tile = uint32_t(std::atoi(next())), table = bayer(tile);
    else if (a == "--table") {
      tile = uint32_t(std::atoi(next()));
      table.clear();
      for (const char* p = next(); *p; ++p) {
        table.push_back(uint32_t(std::strtoul(p, const_cast<char**>(&p), 10)));
        if (!*p) break;
      }
    } else if (a == "--card") card = next();
    else if (a == "--top") top = size_t(std::atoi(next()));
    else if (a == "--time") limit = std::atof(next());
    else if (a == "--check") selfCheck = true;
    else {
      std::fprintf(stderr, "usage: sopt-bitsearch --bayer N | --table N v0,v1,... [--card nvidia|intel|flat] [--top N] [--time S] | --check\n");
      return a == "-h" || a == "--help" ? 0 : 2;
    }
  }
  if (selfCheck) {
    const auto f = run(bayer(4), 4, "flat", 1, 60.0);
    if (f.empty() || f[0].cost > 8.0) {
      std::fprintf(stderr, "bitsearch self-check failed: no Bayer 4 x 4 at 8 operations or fewer\n");
      return 1;
    }
    std::printf("ok: %s\n", f[0].text.c_str());
    return 0;
  }
  if (!kCards.count(card) || tile < 2 || (tile & (tile - 1)) || table.size() != size_t(tile) * tile) {
    std::fprintf(stderr, "needs a tile of a power of two, its values and a known card\n");
    return 2;
  }
  uint32_t most = 0;
  for (const uint32_t v : table) most = std::max(most, v);
  const std::vector<Found> found = run(table, tile, card, top, limit);
  std::printf("M = %uu; divide by %u%s; cost in fma (%s)\n", tile - 1, most + 1, " << scale", card.c_str());
  for (const Found& f : found) std::printf("%7.2f  scale %d  %s\n", f.cost, f.scale, f.text.c_str());
  if (found.empty()) std::printf("none found\n");
  return 0;
}
