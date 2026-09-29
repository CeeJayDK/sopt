#include "search/enumerator.hpp"

#include <algorithm>
#include <bit>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <functional>
#include <thread>
#include <unordered_map>

#include "ir/eval.hpp"
#include "search/subtrees.hpp"
#include "verify/exact.hpp"
#include "verify/verify.hpp"

#if defined(_MSC_VER)
#include <xmmintrin.h>
#define SOPT_PREFETCH(p) _mm_prefetch(reinterpret_cast<const char*>(p), _MM_HINT_T0)
#else
#define SOPT_PREFETCH(p) __builtin_prefetch(p)
#endif

namespace sopt {

double nowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

namespace {

constexpr uint32_t kEmpty = UINT32_MAX;
constexpr uint32_t kQuant = UINT32_MAX - 1;  // goal: quantized duplicate, checked only

uint32_t canonicalBits(uint32_t u) {
  const uint32_t a = u & 0x7fffffffu;
  return a > 0x7f800000u ? 0x7fc00000u : (a == 0 ? 0u : u);
}
void canonicalize(float* v, size_t n) {
  // One NaN; -0 as +0 (the sign of zero is don't-care). Branch-free on the bits, in
  // blocks of 8 so the compiler can vectorize the fixed-length inner loop.
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    for (size_t k = 0; k < 8; ++k) v[i + k] = std::bit_cast<float>(canonicalBits(std::bit_cast<uint32_t>(v[i + k])));
  for (; i < n; ++i) v[i] = std::bit_cast<float>(canonicalBits(std::bit_cast<uint32_t>(v[i])));
}

struct ConstPool {
  std::vector<float> scalars;
  std::vector<std::pair<Type, std::array<float, 4>>> vectors;  // vector constants of the target
};

// Scalars: a few basics plus, for every constant (component) c of the target, c, -c,
// c^2 and 1/c. Vector constants of the target are leaves as they are.
ConstPool constantPool(const Expr& target) {
  ConstPool p;
  std::vector<float> pool = {0.0f, 0.5f, 1.0f, 2.0f, -1.0f};
  for (const auto& n : target.nodes) {
    if (n.op != Op::Const) continue;
    const unsigned w = width(n.type);
    if (w > 1) {
      std::array<float, 4> v{};
      for (unsigned k = 0; k < w; ++k) v[k] = n.value[k];
      bool dup = false;
      for (const auto& o : p.vectors) dup = dup || (o.first == n.type && o.second == v);
      if (!dup) p.vectors.push_back({n.type, v});
    }
    for (unsigned k = 0; k < w; ++k) {
      const float c = n.value[k];
      pool.push_back(c);
      pool.push_back(-c);
      pool.push_back(c * c);
      if (c != 0.0f) pool.push_back(1.0f / c);
    }
  }
  for (float v : pool) {
    if (!std::isfinite(v)) continue;
    if (v == 0.0f) v = 0.0f;
    bool dup = false;
    for (float o : p.scalars) dup = dup || std::bit_cast<uint32_t>(o) == std::bit_cast<uint32_t>(v);
    if (!dup) p.scalars.push_back(v);
  }
  return p;
}

}  // namespace

Enumerator::Enumerator(const Program& prog, const PointSet& tests, const SearchConfig& cfg)
    : prog_(prog), tests_(tests), cfg_(cfg), n_(tests.size()) {
  targetType_ = prog.target.nodes[prog.target.root].type;
  tn_ = lenOf(targetType_);
  targetCost_ = dagCost(prog.target, *cfg.model, prog.inputs);
  slackCur_ = cfg.slack;
  objLimit_ = targetCost_;
  target_ = evalAll(prog.target, tests, kProfileRef);
  targetFinite_.resize(tn_);
  for (size_t i = 0; i < tn_; ++i) targetFinite_[i] = std::isfinite(target_[i]) ? 1 : 0;
  // Accuracy rule: hits may also be as close to the exact value as the target is, and
  // constants are fitted to the exact values.
  rule_ = accuracyRule(prog.budget);
  fit_.assign(target_.begin(), target_.end());
  const bool rel = prog.budget.scaledRel();
  scale_.assign(tn_, 0.0);  // the target's error scales (Rel budgets, see relBase)
  if (rule_ || rel) {
    std::vector<double> ex = evalExactAll(prog.target, tests, rel ? &scale_ : nullptr);
    if (rule_) {
      exact_ = std::move(ex);
      for (size_t i = 0; i < tn_; ++i)
        if (std::isfinite(exact_[i])) fit_[i] = exact_[i];
    }
  }

  // Ops are enumerated for float1 and the target's type only. Helpers (dot, length, ...)
  // are not enumerated, so another floatN could only reach the target through a
  // component, and a component of a componentwise op is cheaper as the scalar op on the
  // operands' components (which are leaves for vector inputs).
  types_.push_back(Type::Float);
  if (targetType_ != Type::Float) types_.push_back(targetType_);

  for (size_t i = 0; i < static_cast<size_t>(Op::Count); ++i) {
    const Op op = static_cast<Op>(i);
    if (op == Op::Input || op == Op::Const || op == Op::Swizzle || op == Op::Construct) continue;
    if (!cfg.helpers && isPureHelper(op)) continue;
    if (info(op).base || containsOp(prog.target, op)) ops_.push_back(op);
  }
  scratch_.resize(4 * n_);
  serialScratch_.fit.resize(tn_);
  // How far a hit may be from the target at each test point (budget, loose factor,
  // accuracy rule, a few ulps of rounding): the inner-fit prefilter's tolerance.
  {
    const Budget& b = prog.budget;
    const double scale = b.loose > 1.0 ? b.loose : 1.0;
    monoTol_.assign(tn_, 0.0);
    for (size_t i = 0; i < tn_; ++i) {
      const double t = target_[i];
      double a = 0.0;
      switch (b.kind) {
        case Budget::Kind::Exact: break;
        case Budget::Kind::Color8:
        case Budget::Kind::Color10:
          a = (b.maxCodeDiff + (b.loose > 1.0 ? 1.5 : 0.5)) / double((1 << b.codeBits()) - 1);
          break;
        case Budget::Kind::Texcoord:
        case Budget::Kind::Abs: a = scale * b.eps; break;
        case Budget::Kind::Rel: a = scale * b.eps * relBase(t, scale_[i]); break;
      }
      if (rule_ && std::isfinite(exact_[i])) a += 2.0 * scale * std::fabs(t - exact_[i]);
      monoTol_[i] = a + 4.0 * std::fabs(t) * 0x1p-23;
    }
  }
  table_.assign(1u << 16, kEmpty);
  // The bank grows to maxBank: reserve it (pages are only touched when used) so that
  // appending does not copy hundreds of MB, and size the table for it.
  {
    // Entries from the memory budget: each costs its Entry, offset, hit flag, level-list
    // slot, hash-table slots (load <= 1/2, rounded up to a power of two: <= 16 bytes) and a
    // scalar fingerprint; wider entries use more, so the bank also stops when the
    // fingerprints reach the budget (see bankFull).
    const size_t perEntry = sizeof(Packed) + sizeof(uint32_t) + sizeof(uint32_t) + 16 + 4 * n_;
    budget_ = bankBudget(cfg_);
    maxBank_ = std::max<size_t>(budget_ / perEntry, 4 * kBatch);
    if (cfg_.maxBank) maxBank_ = std::min(maxBank_, cfg_.maxBank);
    // Fingerprint offsets are 32-bit: at most 2^32 floats (widest entries are float4).
    maxBank_ = std::min<size_t>(maxBank_, (size_t{0xffffffffu} / (4 * std::max<size_t>(n_, 1))) - kBatch);
    maxBank_ = std::min<size_t>(maxBank_, (size_t{1} << kIndexBits) - 2 * kBatch);  // packed indices
    if (!cfg_.diskDir.empty()) {
      // Four resident tiles take at most an eighth of the budget.
      cfg_.diskTileFloats = std::min(cfg_.diskTileFloats, std::max<size_t>(size_t{1} << 16, budget_ / 128));
      cfg_.diskBlockFloats = std::min(cfg_.diskBlockFloats, cfg_.diskTileFloats);
      disk_ = std::make_unique<DiskFpStore>(cfg_.diskDir, cfg_.diskTileFloats, cfg_.diskBlockFloats);
      diskMode_ = disk_->ok();
      if (!diskMode_) disk_.reset();
    }
    if (diskMode_) {
      cfg_.quantBits = 0;
      // In RAM per entry: Packed, offset, both hashes, disk offset, level-list slot and
      // hash-table slots; fingerprints in RAM up to an eighth of the budget, the rest on disk.
      const size_t ramPerEntry = sizeof(Packed) + 4 + 16 + 8 + 4 + 16;
      maxBank_ = std::max<size_t>(budget_ / ramPerEntry, 4 * kBatch);
      if (cfg_.maxBank) maxBank_ = std::min(maxBank_, cfg_.maxBank);
      maxBank_ = std::min<size_t>(maxBank_, (size_t{1} << kIndexBits) - 2 * kBatch);
      diskBudget_ = cfg_.diskBudget;
      if (!diskBudget_) {
        std::error_code ec;
        const auto sp = std::filesystem::space(cfg_.diskDir, ec);
        const size_t reserve = std::max<size_t>(size_t{1} << 30, ec ? 0 : static_cast<size_t>(sp.capacity / 20));
        diskBudget_ = ec || sp.available < reserve + (size_t{1} << 28) ? size_t{1} << 28 : sp.available - reserve;
      }
      tileSlots_.resize(4);
    }
    // Bank and fingerprints grow in chunks; the hash table starts small and doubles as the
    // bank grows (growTable).

  }
}

size_t physicalMemory() {
#if defined(_WIN32)
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof(m);
  if (GlobalMemoryStatusEx(&m)) return static_cast<size_t>(m.ullTotalPhys);
#elif defined(__unix__) || defined(__APPLE__)
  const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page > 0) return static_cast<size_t>(pages) * static_cast<size_t>(page);
#endif
  return size_t{4} << 30;
}

size_t availableMemory() {
  static const size_t avail = [] {
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) return static_cast<size_t>(m.ullAvailPhys);
#elif defined(__linux__)
    size_t avail = 0;
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
      char line[256];
      unsigned long long kb = 0;
      while (std::fgets(line, sizeof(line), f))
        if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
      std::fclose(f);
      avail = static_cast<size_t>(kb) * 1024;
    }
    // A container / cgroup limit (v2 memory.max, v1 memory.limit_in_bytes) can be far below
    // the machine's RAM: what is left of it (usage minus reclaimable page cache).
    auto readNum = [](const std::string& path, unsigned long long& v) {
      FILE* f = std::fopen(path.c_str(), "r");
      if (!f) return false;
      const bool ok = std::fscanf(f, "%llu", &v) == 1;
      std::fclose(f);
      return ok;
    };
    auto statField = [](const std::string& path, const char* key) {
      unsigned long long v = 0;
      if (FILE* f = std::fopen(path.c_str(), "r")) {
        char name[128];
        unsigned long long x;
        while (std::fscanf(f, "%127s %llu", name, &x) == 2)
          if (std::strcmp(name, key) == 0) v = x;
        std::fclose(f);
      }
      return v;
    };
    std::string v1, v2;
    if (FILE* f = std::fopen("/proc/self/cgroup", "r")) {
      char line[512];
      while (std::fgets(line, sizeof(line), f)) {
        std::string l(line);
        if (!l.empty() && l.back() == '\n') l.pop_back();
        const size_t c1 = l.find(':'), c2 = l.find(':', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        const std::string ctrl = l.substr(c1 + 1, c2 - c1 - 1), path = l.substr(c2 + 1);
        if (ctrl == "memory") v1 = "/sys/fs/cgroup/memory" + path;
        if (ctrl.empty()) v2 = "/sys/fs/cgroup" + path;
      }
      std::fclose(f);
    }
    unsigned long long limit = 0, usage = 0;
    if (!v2.empty() && readNum(v2 + "/memory.max", limit) && readNum(v2 + "/memory.current", usage)) {
      const unsigned long long cache = statField(v2 + "/memory.stat", "inactive_file");
      usage = usage > cache ? usage - cache : 0;
    } else if (!v1.empty() && readNum(v1 + "/memory.limit_in_bytes", limit) &&
               readNum(v1 + "/memory.usage_in_bytes", usage)) {
      const unsigned long long cache = statField(v1 + "/memory.stat", "total_inactive_file");
      usage = usage > cache ? usage - cache : 0;
    } else {
      limit = 0;
    }
    if (limit && limit < (1ull << 62)) {
      const size_t left = limit > usage ? static_cast<size_t>(limit - usage) : 0;
      avail = avail ? std::min(avail, left) : left;
    }
    if (avail) return avail;
#endif
#if defined(__unix__) || defined(__APPLE__)
    const long pages = sysconf(_SC_AVPHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page > 0) return static_cast<size_t>(pages) * static_cast<size_t>(page);
#endif
    return physicalMemory() / 2;
  }();
  return avail;
}

size_t bankBudget(const SearchConfig& cfg) {
  if (cfg.memBudget) return cfg.memBudget;
  const size_t n = std::max(1u, cfg.concurrent);
  const size_t reserve = std::max<size_t>(size_t{1} << 30, physicalMemory() / 10) + n * (size_t{512} << 20);
  const size_t avail = availableMemory();
  const size_t total = avail > reserve + (size_t{64} << 20) * n ? avail - reserve : (size_t{64} << 20) * n;
  return total / n;
}

static_assert(static_cast<size_t>(Op::Count) * kNumTypes <= 256, "op/type codebook must fit one byte");

Enumerator::Packed Enumerator::pack(const Entry& e) {
  constexpr uint64_t m = (uint64_t{1} << kIndexBits) - 1;
  const uint64_t code = static_cast<uint64_t>(e.op) * kNumTypes + static_cast<uint64_t>(e.type);
  Packed p;
  p.w0 = (e.args[0] & m) | ((e.args[1] & m) << 28) | (code << 56);
  p.w1 = (e.args[2] & m) | (uint64_t{e.isConst} << 28) | (uint64_t{e.affine} << 29) | (uint64_t{e.ctime} << 30) |
         (uint64_t{e.cost} << 32) | (uint64_t{e.obj} << 48);
  return p;
}

Enumerator::Entry Enumerator::entry(uint32_t idx) const {
  constexpr uint64_t m = (uint64_t{1} << kIndexBits) - 1;
  const Packed& p = bank_[idx];
  const unsigned code = static_cast<unsigned>(p.w0 >> 56);
  Entry e{};
  e.args[0] = static_cast<uint32_t>(p.w0 & m);
  e.args[1] = static_cast<uint32_t>((p.w0 >> 28) & m);
  e.args[2] = static_cast<uint32_t>(p.w1 & m);
  e.op = static_cast<Op>(code / kNumTypes);
  e.type = static_cast<Type>(code % kNumTypes);
  e.isConst = (p.w1 >> 28) & 1u;
  e.affine = (p.w1 >> 29) & 1u;
  e.ctime = (p.w1 >> 30) & 1u;
  e.cost = static_cast<uint16_t>(p.w1 >> 32);
  e.obj = static_cast<uint16_t>(p.w1 >> 48);
  return e;
}

uint64_t Enumerator::hash2Fp(const float* fp, Type t) const {
  // An independent second hash (disk mode compares both instead of the fingerprints).
  uint64_t h = 0x2545F4914F6CDD1Dull + static_cast<uint64_t>(t) * 0x9E3779B97F4A7C15ull;
  const size_t len = lenOf(t);
  for (size_t i = 0; i < len; ++i) {
    h += std::bit_cast<uint32_t>(fp[i]) + 0x632BE59BD9B4E019ull;
    h ^= h >> 31;
    h *= 0xD6E8FEB86659FD93ull;
  }
  return h ^ (h >> 32);
}

bool Enumerator::sameAs(uint32_t idx, const float* fp, size_t len, uint64_t h1, uint64_t h2) const {
  if (diskMode_) return hashes_[idx][0] == h1 && hashes_[idx][1] == h2;
  return sameFp(fpOf(idx), fp, len);
}

void Enumerator::placeFp(uint32_t idx, const float* fp, size_t len, bool mayDisk, uint64_t h1) {
  if (!diskMode_) {
    off_.push_back(fp_.append(fp, len));
    return;
  }
  hashes_.push_back({h1, hash2Fp(fp, entry(idx).type)});
  // Fingerprints in RAM up to an eighth of the budget: the rest of it holds the entries
  // (~50 bytes each in disk mode) and the resident tiles.
  if (mayDisk && !spilling_ && fp_.floats() * sizeof(float) >= budget_ / 8) spilling_ = true;
  if (mayDisk && spilling_) {
    if (diskFrom_ == UINT32_MAX) diskFrom_ = idx;
    diskOff_.push_back(disk_->append(fp, len));
    off_.push_back(0);
    return;
  }
  off_.push_back(fp_.append(fp, len));
  if (idx >= diskFrom_) diskOff_.push_back(kNotOnDisk);
}

void Enumerator::dropLastFp(uint32_t idx, size_t len) {
  // Only RAM records are ever dropped (transient overflow entries).
  fp_.truncate(off_[off_.size() - 1], len);
  off_.pop_back();
  if (diskMode_) {
    hashes_.pop_back();
    if (idx >= diskFrom_) diskOff_.pop_back();
  }
}

bool Enumerator::bankFull() const {
  // Per entry: Packed, fingerprint offset, level-list slot (+ both hashes and the disk
  // offset in disk mode); the hash table counted 1.5x (it doubles when it grows, and the old
  // one lives until the new one is filled: grow before that would pass the budget).
  const size_t perEntry = sizeof(Packed) + 4 + 4 + (diskMode_ ? 16 + 8 : 0);
  size_t ram = bank_.size() * perEntry + table_.size() * 6 + fp_.floats() * sizeof(float);
  if (diskMode_) {
    ram += tileSlots_.size() * cfg_.diskTileFloats * sizeof(float);
    return bank_.size() >= maxBank_ || ram >= budget_ || disk_->bytesWritten() >= diskBudget_ || fp_.nearlyFull();
  }
  return bank_.size() >= maxBank_ || ram >= budget_ || fp_.nearlyFull();
}

uint32_t Enumerator::quant(float x) const {
  const uint32_t u = std::bit_cast<uint32_t>(x);
  const uint32_t k = cfg_.quantBits;
  if (k == 0 || !std::isfinite(x)) return u;
  // Round to nearest at bit k (a carry into the exponent is still monotonic).
  return (u + (1u << (k - 1))) & ~((1u << k) - 1u);
}

bool Enumerator::sameFp(const float* a, const float* b, size_t len) const {
  if (cfg_.quantBits == 0) return std::memcmp(a, b, len * sizeof(float)) == 0;
  for (size_t i = 0; i < len; ++i)
    if (quant(a[i]) != quant(b[i])) return false;
  return true;
}

uint64_t Enumerator::hashFp(const float* fp, Type t) const {
  const size_t len = lenOf(t);
  if (cfg_.quantBits) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(t);
    for (size_t i = 0; i < len; ++i) {
      h ^= quant(fp[i]);
      h *= 0xff51afd7ed558ccdull;
      h ^= h >> 32;
    }
    return h;
  }
  // Two floats per 64-bit step in two independent lanes (the serial multiply chain was
  // 13% of the search); only the table positions depend on it, not the results.
  uint64_t h0 = 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(t), h1 = 0xC2B2AE3D27D4EB4Full;
  size_t i = 0;
  auto word = [&](size_t k) {
    uint64_t w;
    std::memcpy(&w, fp + k, sizeof(w));
    return w;
  };
  for (; i + 4 <= len; i += 4) {
    h0 = (h0 ^ word(i)) * 0xff51afd7ed558ccdull;
    h1 = (h1 ^ word(i + 2)) * 0xc4ceb9fe1a85ec53ull;
    h0 ^= h0 >> 29;
    h1 ^= h1 >> 31;
  }
  for (; i < len; ++i) h0 = (h0 ^ std::bit_cast<uint32_t>(fp[i])) * 0xff51afd7ed558ccdull;
  uint64_t h = (h0 ^ (h1 * 0x9E3779B97F4A7C15ull));
  h ^= h >> 32;
  h *= 0xff51afd7ed558ccdull;
  return h ^ (h >> 29);
}

void Enumerator::growTable() {
  std::vector<uint32_t> old;
  old.swap(table_);
  table_.assign(old.size() * 2, kEmpty);
  const size_t mask = table_.size() - 1;
  for (uint32_t idx : old) {
    if (idx == kEmpty) continue;
    size_t pos = (diskMode_ ? hashes_[idx][0] : hashFp(fpOf(idx), entry(idx).type)) & mask;
    while (table_[pos] != kEmpty) pos = (pos + 1) & mask;
    table_[pos] = idx;
  }
}

bool Enumerator::insert(const Entry& e, const float* fp, SearchStats& stats) {
  const size_t mask = table_.size() - 1;
  const size_t len = lenOf(e.type);
  const uint64_t h1 = hashFp(fp, e.type), h2 = diskMode_ ? hash2Fp(fp, e.type) : 0;
  size_t pos = h1 & mask;
  while (table_[pos] != kEmpty) {
    const uint32_t idx = table_[pos];
    if (entry(idx).type == e.type && sameAs(idx, fp, len, h1, h2)) {
      ++stats.deduped;
      // Same fingerprint as a hit: not needed in the bank, but it may differ from the
      // hit outside the test points, so keep it as an alternative for verification.
      if (isHit(idx) && e.op != Op::Input && e.op != Op::Const && numHits() < cfg_.maxHits) {
        altHits_.push_back(e);
        ++stats.hits;
      }
      return false;
    }
    pos = (pos + 1) & mask;
  }
  const auto idx = static_cast<uint32_t>(bank_.size());
  // Bank full (overflow mode): the entry is only checked as a hit and dropped again,
  // so the search goes on with the stored entries as operands.
  const bool transient = cfg_.overflow && bankFull();
  const size_t hitsBefore = numHits();
  bank_.push_back(pack(e));
  placeFp(idx, fp, len, false, h1);
  table_[pos] = idx;
  if (!transient && bank_.size() * 2 > table_.size()) growTable();
  if (!transient) {
    byCost_[e.cost][static_cast<size_t>(e.type)].push_back(idx);
    if (!stats.levels.empty()) ++stats.levels.back().added;
  }

  // Goal check: does this value match the target within the budget on all test points?
  // Otherwise try solving an outer affine map / inner constant (fitted hits).
  fitOut_.clear();
  const bool direct = goalCheck(e, fp, idx, fitOut_, serialScratch_, stats);
  commitHits(idx, direct, fitOut_, stats);
  if (transient) {
    if (numHits() > hitsBefore) {
      ++stats.overflowKept;  // a hit (or fitted hit) refers to it: keep it
    } else {
      // Most recent insertion: removing it cannot break a probe chain.
      table_[pos] = kEmpty;
      dropLastFp(idx, len);
      bank_.pop_back();
    }
    ++stats.overflowChecked;
  }
  return true;
}

uint32_t Enumerator::addConst(Type t, const float* v, SearchStats& stats) {
  std::array<float, 4> val{};
  for (unsigned k = 0; k < width(t); ++k) val[k] = v[k];
  consts_.push_back(val);
  Entry e = Entry::make(Op::Const, t, 0, true, 0, 0, 0, static_cast<uint32_t>(consts_.size() - 1), false, 0, false);
  for (unsigned k = 0; k < width(t); ++k) std::fill(scratch_.begin() + k * n_, scratch_.begin() + (k + 1) * n_, val[k]);
  insert(e, scratch_.data(), stats);
  return static_cast<uint32_t>(bank_.size() - 1);
}

// target ~ p * v + q: least squares over the finite target points, then the budget
// check on the float result of the wrapper. Cheaper wrappers (v + q, v * p, q - v) are
// preferred when they also pass. top is v's op (a mul/div under an add/sub contracts)
// and baseObj its objective cost.
bool Enumerator::fitWrap(const float* v, Op top, uint32_t baseObj, AffineHit& out) const {
  const CostModel& model = *cfg_.model;
  const unsigned W = width(targetType_);  // the wrapper applies to every component
  auto wrapCost = [&](Op w) -> uint32_t {
    if ((w == Op::Add || w == Op::Sub) && model.fusesIntoAdd(top)) return W * model.fusedAdd;
    return model.opCost(w, W);
  };
  // No wrapper keeps it under the target's cost: skip the fit.
  if (baseObj + std::min({wrapCost(Op::Add), wrapCost(Op::Mul), wrapCost(Op::Sub), wrapCost(Op::Mad)}) >=
      objLimit_)
    return false;
  double mv = 0, mg = 0, svg0 = 0, svv0 = 0;
  size_t m = 0;
  for (size_t i = 0; i < tn_; ++i) {
    if (!targetFinite_[i]) continue;
    if (!std::isfinite(v[i])) return false;
    mv += v[i];
    mg += fit_[i];
    svv0 += double(v[i]) * v[i];
    svg0 += double(v[i]) * fit_[i];
    ++m;
  }
  if (m < 2) return false;
  mv /= static_cast<double>(m);
  mg /= static_cast<double>(m);
  double svv = 0, svg = 0;
  for (size_t i = 0; i < tn_; ++i) {
    if (!targetFinite_[i]) continue;
    const double dv = v[i] - mv;
    svv += dv * dv;
    svg += dv * (fit_[i] - mg);
  }
  if (!(svv > 0)) return false;  // constant fingerprint: nothing to scale
  const double pd = svg / svv;

  auto passes = [&](Op w, float p, float q) {
    if (!std::isfinite(p) || !std::isfinite(q)) return false;
    for (size_t i = 0; i < tn_; ++i) {
      if (!targetFinite_[i]) continue;
      float r;
      switch (w) {
        case Op::Add: r = v[i] + q; break;
        case Op::Mul: r = v[i] * p; break;
        case Op::Sub: r = q - v[i]; break;
        default: r = v[i] * p + q; break;  // mad, reference profile
      }
      if (!accepts(i, r)) return false;
    }
    return true;
  };
  // The full fit is the best any wrapper can do in the least-squares sense, but not always
  // under a relative budget: its offset q can be a tiny nonzero value where the target is 0,
  // while p * v (no offset) passes. So each wrapper is checked on its own.
  const AffineHit full{0, Op::Mad, static_cast<float>(pd), static_cast<float>(mg - pd * mv)};
  if (full.p == 0.0f) return false;

  // Cheaper wrappers, each with its own least-squares constant.
  const AffineHit tries[] = {
      {0, Op::Add, 1.0f, static_cast<float>(mg - mv)},
      {0, Op::Mul, static_cast<float>(svg0 / svv0), 0.0f},
      {0, Op::Sub, -1.0f, static_cast<float>(mg + mv)},
      full};
  const AffineHit* best = nullptr;
  uint32_t bestCost = objLimit_;
  for (const auto& h : tries) {
    const uint32_t c = baseObj + wrapCost(h.wrap);
    if (c < bestCost && passes(h.wrap, h.p, h.q)) {
      best = &h;
      bestCost = c;
    }
  }
  if (!best) return false;
  out.cost = bestCost;
  out.wrap = best->wrap;
  out.p = best->p;
  out.q = best->q;
  return true;
}

bool Enumerator::goalCheck(const Entry& e, const float* v, uint32_t idx, std::vector<AffineHit>& out,
                           FitScratch& s, SearchStats& stats) const {
  if (e.type != targetType_ || e.obj >= objLimit_) return false;
  bool ok = true;
  for (size_t i = 0; i < tn_ && ok; ++i) ok = !targetFinite_[i] || accepts(i, v[i]);
  if (ok) return true;
  // An affine step's base is in the bank and gets its own (cheaper) fit.
  if (cfg_.affine && !e.affine && !e.isConst && !e.ctime && numHits() < cfg_.maxHits)
    if (!affineFit(e, v, idx, out) && cfg_.inner) innerFit(e, v, idx, out, s, stats);
  return false;
}

void Enumerator::commitHits(uint32_t idx, bool direct, const std::vector<AffineHit>& fitted,
                            SearchStats& stats) {
  if (direct) {
    setHit(idx);
    hits_.push_back(idx);
    ++stats.hits;
    if (cfg_.bestBound && obj(idx) < bestHitObj_) boundBy(extract(entry(idx)), obj(idx));
  }
  if (cfg_.bestBound)
    for (const AffineHit& h : fitted) {
      AffineHit k = h;
      k.idx = idx;
      const uint32_t c = h.cost ? h.cost : obj(idx) + 1;
      if (c < bestHitObj_) boundBy(extract(k), c);
    }
  for (const AffineHit& h : fitted) {
    if (numHits() >= cfg_.maxHits) break;
    AffineHit k = h;
    k.idx = idx;
    affineHits_.push_back(k);
    ++stats.hits;
    ++(k.inner == Op::Count ? stats.affineHits : stats.innerHits);
  }
  if ((direct || !fitted.empty()) && stats.firstHitSec < 0) stats.firstHitSec = nowSeconds() - start_;
}

bool Enumerator::affineFit(const Entry& e, const float* v, uint32_t idx, std::vector<AffineHit>& out) const {
  AffineHit h{idx, Op::Mad, 0.0f, 0.0f};
  if (!fitWrap(v, e.op, e.obj, h)) return false;
  out.push_back(h);
  return true;
}

namespace {

// Least squares min |A x - b| for k <= 5 unknowns: column-scaled normal equations and
// Gaussian elimination with partial pivoting.
bool solveLsq(double ata[5][5], double atb[5], int k, double* x) {
  double d[5];
  for (int i = 0; i < k; ++i) {
    if (!(ata[i][i] > 0)) return false;
    d[i] = 1.0 / std::sqrt(ata[i][i]);
  }
  double m[5][6];
  for (int i = 0; i < k; ++i) {
    for (int j = 0; j < k; ++j) m[i][j] = ata[i][j] * d[i] * d[j];
    m[i][k] = atb[i] * d[i];
  }
  for (int c = 0; c < k; ++c) {
    int piv = c;
    for (int r = c + 1; r < k; ++r)
      if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
    if (std::fabs(m[piv][c]) < 1e-12) return false;
    for (int j = 0; j <= k; ++j) std::swap(m[c][j], m[piv][j]);
    for (int r = 0; r < k; ++r) {
      if (r == c) continue;
      const double f = m[r][c] / m[c][c];
      for (int j = c; j <= k; ++j) m[r][j] -= f * m[c][j];
    }
  }
  for (int i = 0; i < k; ++i) x[i] = m[i][k] / m[i][i] * d[i];
  return true;
}

}  // namespace

// target ~ p * u(v + c) + q with u in {rcp, sqrt, rsqrt}: c is solved in closed form,
// since each template is linear in a reparametrization (g = target):
//   rcp:   g v = -c g + q v + (p + q c)
//   sqrt:  g^2 = 2q g + p^2 v + (p^2 c - q^2)
//   rsqrt: g^2 v = -c g^2 + 2q g v + 2qc g - q^2 v + (p^2 - q^2 c)
// then p, q are refitted on w = u(v + c) by fitWrap.
int Enumerator::monotoneBreaks(const float* v, std::vector<uint32_t>& keysBuf) const {
  // Sort (value key, index) pairs as integers: no indirection in the comparisons. A
  // non-finite v fails the fit anyway (innerFit), so it counts as not monotonic.
  static_assert(sizeof(uint64_t) == 2 * sizeof(uint32_t));
  keysBuf.resize(2 * tn_);
  uint64_t* keys = reinterpret_cast<uint64_t*>(keysBuf.data());
  size_t n = 0;
  for (size_t i = 0; i < tn_; ++i) {
    if (!targetFinite_[i]) continue;
    if (!std::isfinite(v[i])) return 2;
    uint32_t u = std::bit_cast<uint32_t>(v[i] == 0.0f ? 0.0f : v[i]);  // -0 sorts as 0
    u = (u & 0x80000000u) ? ~u : (u | 0x80000000u);
    keys[n++] = (uint64_t{u} << 32) | i;
  }
  std::sort(keys, keys + n);
  int up = 0, down = 0;
  for (size_t k = 0; k + 1 < n; ++k) {
    const uint32_t a = static_cast<uint32_t>(keys[k]), b = static_cast<uint32_t>(keys[k + 1]);
    const double d = double(target_[b]) - target_[a];
    const double tol = monoTol_[a] + monoTol_[b];
    if (v[a] == v[b]) {
      if (std::fabs(d) > tol) return 2;  // one v, two targets: no function of v fits
    } else if (d > tol) {
      ++up;
    } else if (d < -tol) {
      ++down;
    }
    if (up > 1 && down > 1) return 2;  // the callers only distinguish 0, 1 and > 1
  }
  return std::min(up, down);
}

bool Enumerator::innerFit(const Entry& e, const float* v, uint32_t idx, std::vector<AffineHit>& out,
                          FitScratch& s, SearchStats& stats) const {
  const CostModel& model = *cfg_.model;
  const unsigned W0 = width(targetType_);
  const uint32_t addCost0 = model.fusesIntoAdd(e.op) ? W0 * model.fusedAdd : model.opCost(Op::Add, W0);
  // No u that is available and still under the target's cost: nothing to fit (checked
  // first: the monotonicity test below is the expensive part).
  bool any = false;
  for (Op u : {Op::Rcp, Op::Sqrt, Op::Rsqrt})
    any = any || (std::find(ops_.begin(), ops_.end(), u) != ops_.end() &&
                  e.obj + addCost0 + model.opCost(u, W0) + 1 < objLimit_);
  if (!any) return false;
  // p * u(v + c) + q is monotonic in v (rcp: on each side of its pole, one break), so
  // the target must be too, within the tolerance: cheap to check before fitting.
  const int breaks = cfg_.innerPrefilter ? monotoneBreaks(v, s.order) : 0;
  if (breaks > 1) {
    ++stats.innerPrefiltered;
    return false;
  }
  const unsigned W = width(targetType_);
  const uint32_t addCost = model.fusesIntoAdd(e.op) ? W * model.fusedAdd : model.opCost(Op::Add, W);
  bool found = false;
  for (Op u : {Op::Rcp, Op::Sqrt, Op::Rsqrt}) {
    if (std::find(ops_.begin(), ops_.end(), u) == ops_.end()) continue;
    if (breaks > 0 && u != Op::Rcp) continue;
    const uint32_t baseObj = e.obj + addCost + model.opCost(u, W);
    if (baseObj + 1 >= objLimit_) continue;
    const int k = u == Op::Rsqrt ? 5 : 3;
    double ata[5][5] = {}, atb[5] = {}, x[5];
    for (size_t i = 0; i < tn_; ++i) {
      if (!targetFinite_[i]) continue;
      if (!std::isfinite(v[i])) return false;
      const double g = fit_[i], vi = v[i];
      double col[5], rhs;
      if (u == Op::Rcp) {
        col[0] = -g; col[1] = vi; col[2] = 1.0; rhs = g * vi;
      } else if (u == Op::Sqrt) {
        col[0] = g; col[1] = vi; col[2] = 1.0; rhs = g * g;
      } else {
        col[0] = -g * g; col[1] = g * vi; col[2] = g; col[3] = -vi; col[4] = 1.0; rhs = g * g * vi;
      }
      for (int r = 0; r < k; ++r) {
        for (int c = 0; c < k; ++c) ata[r][c] += col[r] * col[c];
        atb[r] += col[r] * rhs;
      }
    }
    if (!solveLsq(ata, atb, k, x)) continue;
    {
      // Prefilter: a real template match fits the linear system almost exactly.
      double rr = 0, bb = 0;
      for (size_t i = 0; i < tn_; ++i) {
        if (!targetFinite_[i]) continue;
        const double g = fit_[i], vi = v[i];
        double r;
        if (u == Op::Rcp) r = -x[0] * g + x[1] * vi + x[2] - g * vi;
        else if (u == Op::Sqrt) r = x[0] * g + x[1] * vi + x[2] - g * g;
        else r = -x[0] * g * g + x[1] * g * vi + x[2] * g - x[3] * vi + x[4] - g * g * vi;
        const double b = u == Op::Rcp ? g * vi : (u == Op::Sqrt ? g * g : g * g * vi);
        rr += r * r;
        bb += b * b;
      }
      if (!(rr <= 1e-6 * bb)) continue;
    }
    double cd = x[0];
    if (u == Op::Sqrt) {
      if (!(x[1] > 0)) continue;
      cd = (x[2] + 0.25 * x[0] * x[0]) / x[1];
    }
    // The reparametrized fit is not the true least-squares fit (rsqrt's is only
    // approximately so); refine (p, q, c) with a few Gauss-Newton steps on
    // r = p * u(v + c) + q - g.
    {
      auto uf = [&](double z, double& du) {
        if (u == Op::Rcp) { du = -1.0 / (z * z); return 1.0 / z; }
        const double sq = std::sqrt(z);
        if (u == Op::Sqrt) { du = 0.5 / sq; return sq; }
        du = -0.5 / (z * sq);
        return 1.0 / sq;
      };
      double pp = 0, qq = 0;
      for (int it = 0; it < 6 && std::isfinite(cd); ++it) {
        double jtj[5][5] = {}, jtr[5] = {}, dx[5];
        bool ok = true;
        // Linear p, q for the current c (closed form), then one step in (p, q, c).
        double sw = 0, sg = 0, sww = 0, swg = 0;
        size_t m = 0;
        for (size_t i = 0; i < tn_ && ok; ++i) {
          if (!targetFinite_[i]) continue;
          double du;
          const double w = uf(double(v[i]) + cd, du);
          ok = std::isfinite(w) && std::isfinite(du);
          sw += w; sg += fit_[i]; sww += w * w; swg += w * fit_[i]; ++m;
        }
        const double den = double(m) * sww - sw * sw;
        if (!ok || !(den > 0)) break;
        pp = (double(m) * swg - sw * sg) / den;
        qq = (sg - pp * sw) / double(m);
        for (size_t i = 0; i < tn_; ++i) {
          if (!targetFinite_[i]) continue;
          double du;
          const double w = uf(double(v[i]) + cd, du);
          const double col[3] = {w, 1.0, pp * du};
          const double r = pp * w + qq - fit_[i];
          for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) jtj[a][b] += col[a] * col[b];
            jtr[a] -= col[a] * r;
          }
        }
        if (!solveLsq(jtj, jtr, 3, dx)) break;
        cd += dx[2];
        if (std::fabs(dx[2]) <= 1e-9 * std::max(1.0, std::fabs(cd))) break;
      }
    }
    const float c = static_cast<float>(cd);
    if (!std::isfinite(c) || c == 0.0f) continue;  // c == 0: u(v) is in the bank itself
    for (size_t i = 0; i < tn_; ++i) s.fit[i] = c;
    evalArray(Op::Add, v, s.fit.data(), nullptr, s.fit.data(), tn_, kProfileRef);
    evalArray(u, s.fit.data(), nullptr, nullptr, s.fit.data(), tn_, kProfileRef);
    if (u == Op::Rcp && cfg_.rational) {
      // p / (v + c) + q = q (v - r) / (v + c), r = -(c + p / q) = (v - r) * rcp(mad(v, 1/q, c/q)):
      // no cancellation near the zero r. p, q by least squares on w = rcp(v + c).
      double sw = 0, sg = 0, sww = 0, swg = 0;
      size_t m = 0;
      for (size_t i = 0; i < tn_; ++i) {
        if (!targetFinite_[i]) continue;
        const double w = s.fit[i];
        sw += w; sg += fit_[i]; sww += w * w; swg += w * fit_[i]; ++m;
      }
      const double den = double(m) * sww - sw * sw;
      const double p = den > 0 ? (double(m) * swg - sw * sg) / den : 0.0;
      const double q = m ? (sg - p * sw) / double(m) : 0.0;
      AffineHit rh{idx, Op::Mad, 0.0f, 0.0f, u, c};
      rh.rational = true;
      rh.r = static_cast<float>(-(double(c) + p / q));
      rh.a = static_cast<float>(1.0 / q);
      rh.b = static_cast<float>(double(c) / q);
      // Constants a float step or two from an integer are that integer (-0.99999994 = -1).
      for (float* k : {&rh.a, &rh.b}) {
        const float n = std::nearbyint(*k);
        if (n != 0.0f && std::fabs(*k - n) <= 4.0f * std::fabs(n) * 0x1p-23f) *k = n;
      }
      // The zero is exact where the target is exactly 0 (e.g. 1 - t at t = 1): snap r to it.
      for (size_t i = 0; i < tn_; ++i)
        if (targetFinite_[i] && target_[i] == 0.0f && std::fabs(double(v[i]) - rh.r) <= 1e-4 * std::max(1.0, std::fabs(double(rh.r))))
          rh.r = v[i];
      bool ok = q != 0.0 && std::isfinite(rh.r) && std::isfinite(rh.a) && std::isfinite(rh.b) && rh.a != 0.0f;
      for (size_t i = 0; i < tn_ && ok; ++i) {
        if (!targetFinite_[i]) continue;
        const float num = v[i] - rh.r;
        const float dn = v[i] * rh.a + rh.b;
        ok = accepts(i, num * (1.0f / dn));
      }
      rh.cost = e.obj + model.opCost(Op::Sub, W) + model.opCost(Op::Rcp, W) + model.opCost(Op::Mad, W) +
                model.opCost(Op::Mul, W);
      if (ok && rh.cost < objLimit_) {
        out.push_back(rh);
        found = true;
      }
    }
    AffineHit h{idx, Op::Mad, 0.0f, 0.0f, u, c};
    if (!fitWrap(s.fit.data(), u, baseObj, h)) continue;
    out.push_back(h);
    found = true;
  }
  return found;
}

void Enumerator::boundBy(const Expr& e, uint32_t objCost) {
  // The bound is the hit's real cost: shared leaves count 0 in objective costs, so a hit
  // using them is dearer than its obj (its DAG cost); otherwise obj (tree cost) is the
  // measure entries are pruned by.
  const uint32_t c = std::max(objCost, dagCost(e, *cfg_.model, prog_.inputs));
  if (c >= bestHitObj_ || !plausible(e)) return;
  bestHitObj_ = c;
  updateLimit();
}

void Enumerator::pruneHits() {
  // Hits above the current bound have no advantage any more; if that is not enough, the
  // cheapest half stays (also among hits of equal cost: the search going deeper matters
  // more than keeping every equivalent alternative).
  struct Ref {
    uint32_t cost;
    uint8_t kind;
    uint32_t i;
  };
  std::vector<Ref> refs;
  for (size_t i = 0; i < hits_.size(); ++i) refs.push_back({obj(hits_[i]), 0, static_cast<uint32_t>(i)});
  for (size_t i = 0; i < altHits_.size(); ++i) refs.push_back({altHits_[i].obj, 1, static_cast<uint32_t>(i)});
  for (size_t i = 0; i < affineHits_.size(); ++i) {
    const AffineHit& h = affineHits_[i];
    refs.push_back({h.cost ? h.cost : obj(h.idx) + 1, 2, static_cast<uint32_t>(i)});
  }
  // Over the bound, or false hits (they only fit the test points; checked once each,
  // on the extra points, loose budget so less accurate candidates stay).
  auto isNew = [&](const Ref& r) {
    return r.i >= (r.kind == 0 ? checkedHits_[0] : r.kind == 1 ? checkedHits_[1] : checkedHits_[2]);
  };
  auto exprOf = [&](const Ref& r) {
    if (r.kind == 0) return extract(entry(hits_[r.i]));
    if (r.kind == 1) return extract(altHits_[r.i]);
    return extract(affineHits_[r.i]);
  };
  // Real costs (shared leaves count 0 in objective costs, see boundBy).
  for (Ref& r : refs) r.cost = std::max(r.cost, dagCost(exprOf(r), *cfg_.model, prog_.inputs));
  refs.erase(std::remove_if(refs.begin(), refs.end(),
                            [&](const Ref& r) {
                              // The best cost itself stays, also when only strictly cheaper
                              // new hits are wanted (slack -1).
                              const uint32_t lim = std::max(objLimit_, bestHitObj_ == UINT32_MAX ? 0 : bestHitObj_ + 1);
                              return r.cost >= lim || (isNew(r) && !plausible(exprOf(r), true));
                            }),
             refs.end());
  const size_t keep = std::max<size_t>(cfg_.maxHits / 2, 1);
  if (refs.size() > keep) {
    std::stable_sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) { return a.cost < b.cost; });
    refs.resize(keep);
  }
  std::vector<char> k0(hits_.size(), 0), k1(altHits_.size(), 0), k2(affineHits_.size(), 0);
  for (const Ref& r : refs) (r.kind == 0 ? k0 : r.kind == 1 ? k1 : k2)[r.i] = 1;
  // Compact in order; every kept hit has now been checked.
  auto compact = [](auto& v, const std::vector<char>& k) {
    size_t n = 0;
    for (size_t i = 0; i < v.size(); ++i)
      if (k[i]) v[n++] = v[i];
    v.resize(n);
    return n;
  };
  checkedHits_[0] = compact(hits_, k0);
  checkedHits_[1] = compact(altHits_, k1);
  checkedHits_[2] = compact(affineHits_, k2);
}

bool Enumerator::plausible(const Expr& e, bool loose) {
  if (boundPts_.size() == 0) {
    boundPts_ = makeRandomPoints(prog_, 512, 0x5eed, true);
    boundTarget_ = evalAll(prog_.target, boundPts_, kProfileRef);
  }
  const Metrics m = compare(prog_, e, boundPts_, kProfileRef, 1, &boundTarget_);
  return loose ? m.loosePass : m.pass;
}

void Enumerator::checkLimits(SearchStats& stats) {
  // Pressed for depth: fewer alternatives, so the search reaches further (owner).
  if (cfg_.bestBound && bankFull()) {
    const int s = nowSeconds() - start_ > 0.5 * cfg_.timeLimitSec ? -1 : 0;
    if (s < slackCur_) {
      slackCur_ = s;
      updateLimit();
    }
  }
  // A full hit list does not end the search (owner): clear it out instead.
  if (numHits() >= cfg_.maxHits && cfg_.bestBound) pruneHits();
  if ((bankFull() && !cfg_.overflow) || numHits() >= cfg_.maxHits ||
      nowSeconds() - start_ > cfg_.timeLimitSec) {
    stop_ = true;
    stats.limitHit = true;
  }
}

// Candidates are generated in order into a batch; flush() evaluates them in parallel,
// inserts them in order (dedup), goal-checks the new ones in parallel and records hits
// in order: the result does not depend on the number of threads.
void Enumerator::tryAdd(Op op, uint16_t cost, uint32_t a, uint32_t b, uint32_t c,
                        SearchStats& stats, uint32_t aux) {
  batch_.push_back({op, cost, a, b, c, aux});
  if (batch_.size() >= kBatch) flush(stats);
}

Enumerator::Prep Enumerator::prepare(const Item& it, Entry& e, float* out) const {
  const Op op = it.op;
  const uint16_t cost = it.cost;
  const uint32_t a = it.a, b = it.b, c = it.c, aux = it.aux;
  const auto& oi = info(op);
  const uint32_t args[3] = {a, b, c};
  bool allConst = true;
  for (uint8_t k = 0; k < oi.arity; ++k) allConst = allConst && entry(args[k]).isConst;
  if (allConst) return Prep::ConstSkipped;
  // Result type: componentwise ops take the widest operand (float1 operands broadcast).
  Type type = Type::Float;
  if (oi.shape == Shape::Cmp) {
    type = Type::Bool;
  } else if (oi.shape == Shape::Comp || oi.shape == Shape::Select) {
    unsigned w = 1;
    for (uint8_t k = oi.shape == Shape::Select ? 1 : 0; k < oi.arity; ++k)
      w = std::max<unsigned>(w, width(entry(args[k]).type));
    type = floatType(w);
  }
  const unsigned w = width(type);

  bool affine = false;
  if (cfg_.affine && op != Op::Swizzle) {
    // An op with a single non-constant operand v that is affine in v. The outer affine
    // map is solved at the goal check, so only single steps (needed inside nonlinear
    // ops, e.g. rcp(t + c)) are kept: no chains, no two-constant mad/lerp.
    int nonConst = 0;
    uint32_t base = 0;
    for (uint8_t k = 0; k < oi.arity; ++k)
      if (!entry(args[k]).isConst) {
        ++nonConst;
        base = args[k];
      }
    const bool step = nonConst == 1 &&
                      (op == Op::Neg || op == Op::Add || op == Op::Sub || op == Op::Mul ||
                       op == Op::Mad || op == Op::Lerp || (op == Op::Div && entry(b).isConst));
    // A pure sign flip (-v, 0 - v, v * -1) is absorbed by the outer map and by the
    // consumer (sub for add, max for min, ...), so it is not stored either.
    auto isConst = [&](uint32_t i, float val) {
      return entry(i).isConst && entry(i).type == Type::Float && constValue(i) == val;
    };
    const bool flip = op == Op::Neg || (op == Op::Sub && isConst(a, 0.0f)) ||
                      (op == Op::Mul && (isConst(a, -1.0f) || isConst(b, -1.0f))) ||
                      (op == Op::Div && isConst(b, -1.0f));
    // c / v is a scaled 1 / v: keep only the reciprocal itself.
    if (op == Op::Div && nonConst == 1 && entry(a).isConst && !isConst(a, 1.0f)) return Prep::AffinePruned;
    if (step) {
      if (oi.arity == 3 || entry(base).affine || flip) return Prep::AffinePruned;
      affine = true;
    }
  }
  // Objective cost; an entry that already costs as much as the target can be neither a
  // hit nor part of one. A same-width mul/div under an add/sub contracts to fma.
  const CostModel& model = *cfg_.model;
  // Only constants and compile-time inputs: folded by the compiler, free.
  bool ctime = true;
  for (uint8_t k = 0; k < oi.arity; ++k) ctime = ctime && (entry(args[k]).isConst || entry(args[k]).ctime);
  uint32_t obj = 0;
  for (uint8_t k = 0; k < oi.arity; ++k) obj += entry(args[k]).obj;
  auto fuses = [&](uint32_t x) {
    return !entry(x).ctime && model.fusesIntoAdd(entry(x).op) && entry(x).type == type;
  };
  if (ctime)
    obj = 0;
  else if (model.fusedAdd && (op == Op::Add || op == Op::Sub) && (fuses(a) || fuses(b)))
    obj += w * model.fusedAdd;
  else
    obj += model.opCost(op, w);
  if (obj >= objLimit_) return Prep::ObjPruned;
  if (op == Op::Swizzle) {
    std::copy(fpOf(a) + aux * n_, fpOf(a) + (aux + 1) * n_, out);
  } else {
    for (unsigned comp = 0; comp < w; ++comp) {
      const float* p[3] = {nullptr, nullptr, nullptr};
      for (uint8_t k = 0; k < oi.arity; ++k)
        p[k] = fpOf(args[k]) + (width(entry(args[k]).type) == 1 ? 0 : comp * n_);
      evalArray(op, p[0], p[1], p[2], out + comp * n_, n_, kProfileRef);
    }
  }
  canonicalize(out, w * n_);
  e = Entry::make(op, type, cost, false, a, b, c, aux, affine, static_cast<uint16_t>(obj), ctime);
  return Prep::Ok;
}


namespace {

// fn(i) for i in [0, n) on up to `threads` threads (contiguous ranges).
void parallelRange(size_t n, unsigned threads, const std::function<void(size_t, size_t, unsigned)>& fn) {
  if (threads <= 1 || n < 256) {
    fn(0, n, 0);
    return;
  }
  threads = static_cast<unsigned>(std::min<size_t>(threads, n / 64));
  std::vector<std::thread> pool;
  const size_t chunk = (n + threads - 1) / threads;
  for (unsigned t = 1; t < threads; ++t) {
    const size_t b = t * chunk, e = std::min(n, b + chunk);
    if (b < e) pool.emplace_back(fn, b, e, t);
  }
  fn(0, std::min(n, chunk), 0);
  for (auto& th : pool) th.join();
}

}  // namespace

uint32_t Enumerator::storeEntry(const Entry& e, const float* fp, bool listed, SearchStats& stats) {
  const auto idx = static_cast<uint32_t>(bank_.size());
  bank_.push_back(pack(e));
  const uint64_t h1 = hashFp(fp, e.type);
  placeFp(idx, fp, lenOf(e.type), listed, h1);
  const size_t mask = table_.size() - 1;
  size_t pos = h1 & mask;
  while (table_[pos] != kEmpty) pos = (pos + 1) & mask;
  table_[pos] = idx;
  if (bank_.size() * 2 > table_.size()) growTable();
  if (listed) {
    byCost_[e.cost][static_cast<size_t>(e.type)].push_back(idx);
    if (!stats.levels.empty()) ++stats.levels.back().added;
  }
  return idx;
}

void Enumerator::flush(SearchStats& stats) {
  const size_t n = batch_.size();
  if (n == 0) return;
  const size_t stride = 4 * n_;
  prep_.resize(n);
  batchEntries_.resize(n);
  batchFp_.resize(n * stride);
  const unsigned threads = cfg_.threads ? cfg_.threads : std::max(1u, std::thread::hardware_concurrency());
  batchHash_.resize(n);
  if (diskMode_) batchH2_.resize(n);
  batchDup_.resize(n);
  // 1. Evaluate and look up among the entries stored before this batch (parallel; the
  // bank does not change here).
  parallelRange(n, threads, [&](size_t b, size_t e, unsigned) {
    const size_t mask = table_.size() - 1;
    for (size_t k = b; k < e; ++k) {
      float* fp = batchFp_.data() + k * stride;
      prep_[k] = prepare(batch_[k], batchEntries_[k], fp);
      if (prep_[k] != Prep::Ok) continue;
      const Type t = batchEntries_[k].type;
      const size_t len = lenOf(t);
      batchHash_[k] = hashFp(fp, t);
      if (diskMode_) batchH2_[k] = hash2Fp(fp, t);
      batchDup_[k] = kEmpty;
      for (size_t pos = batchHash_[k] & mask; table_[pos] != kEmpty; pos = (pos + 1) & mask) {
        const uint32_t idx = table_[pos];
        if (entry(idx).type == t && sameAs(idx, fp, len, batchHash_[k], diskMode_ ? batchH2_[k] : 0)) {
          batchDup_[k] = idx;
          break;
        }
      }
    }
  });
  // 2. Dedup and store in order (serial).
  goals_.clear();
  pendingDups_.clear();
  const auto batchStart = static_cast<uint32_t>(bank_.size());
  // Entries stored in this batch, by hash (small: stays in cache).
  localTable_.assign(4 * kBatch, kEmpty);
  const size_t localMask = localTable_.size() - 1;
  const size_t mainMask = table_.size() - 1;
  for (size_t k = 0; k < n && !stop_; ++k) {
    // The main-table slot a new entry will probe: fetch it ahead (random access).
    if (k + 16 < n && prep_[k + 16] == Prep::Ok && batchDup_[k + 16] == kEmpty)
      SOPT_PREFETCH(&table_[batchHash_[k + 16] & mainMask]);
    ++stats.generated;
    ++stats.levels.back().generated;
    if (++sinceCheck_ >= 4096) {
      sinceCheck_ = 0;
      checkLimits(stats);
    }
    switch (prep_[k]) {
      case Prep::ConstSkipped: ++stats.constSkipped; continue;
      case Prep::AffinePruned: ++stats.affinePruned; continue;
      case Prep::ObjPruned: ++stats.objPruned; continue;
      case Prep::Ok: break;
    }
    const Entry& e = batchEntries_[k];
    const float* fp = batchFp_.data() + k * stride;
    const size_t len = lenOf(e.type);
    // Among the entries stored before the batch (step 1), then those of this batch.
    uint32_t dupIdx = batchDup_[k];
    size_t lpos = batchHash_[k] & localMask;
    if (dupIdx == kEmpty)
      for (; localTable_[lpos] != kEmpty; lpos = (lpos + 1) & localMask) {
        const uint32_t idx = localTable_[lpos];
        if (entry(idx).type == e.type && sameAs(idx, fp, len, batchHash_[k], diskMode_ ? batchH2_[k] : 0)) {
          dupIdx = idx;
          break;
        }
      }
    const bool dup = dupIdx != kEmpty;
    if (dup && cfg_.quantBits && std::memcmp(fpOf(dupIdx), fp, len * sizeof(float)) != 0) {
      // Quantized match only: not an operand, but it may be a hit where the stored value
      // is not (bit-exact targets), so it is goal-checked like an overflow value.
      ++stats.deduped;
      ++stats.quantMerged;
      goals_.push_back({k, kQuant});
      continue;
    }
    if (dup) {
      ++stats.deduped;
      if (!shared_.empty()) upgradeShared(dupIdx, e);
      if (dupIdx >= batchStart) {
        pendingDups_.push_back({k, dupIdx});  // hit or not is known after step 3
      } else if (isHit(dupIdx) && numHits() < cfg_.maxHits) {
        altHits_.push_back(e);
        ++stats.hits;
      }
    }
    if (dup) continue;
    // Bank full (overflow mode): only checked as a hit, stored only if it is (part of) one.
    if (cfg_.overflow && bankFull()) {
      goals_.push_back({k, kEmpty});
    } else {
      const uint32_t idx = storeEntry(e, fp, true, stats);
      localTable_[lpos] = idx;
      goals_.push_back({k, idx});
    }
  }
  // 3. Goal checks and fits of the new values (parallel; no bank changes).
  const size_t ng = goals_.size();
  goalDirect_.assign(ng, 0);
  goalFits_.resize(ng);
  if (threadScratch_.size() < threads) threadScratch_.resize(threads);
  std::vector<SearchStats> tstats(threads);
  parallelRange(ng, threads, [&](size_t b, size_t e, unsigned t) {
    FitScratch& sc = threadScratch_[t];
    sc.fit.resize(tn_);
    for (size_t g = b; g < e; ++g) {
      goalFits_[g].clear();
      const size_t k = goals_[g].item;
      goalDirect_[g] = goalCheck(batchEntries_[k], batchFp_.data() + k * stride, goals_[g].idx, goalFits_[g],
                                 sc, tstats[t]);
    }
  });
  for (const auto& ts : tstats) stats.innerPrefiltered += ts.innerPrefiltered;
  // 4. Record hits in order (serial), with the duplicates of this batch's new entries
  // (alternatives of a hit) where they came.
  size_t pd = 0;
  auto flushDups = [&](size_t upTo) {
    for (; pd < pendingDups_.size() && pendingDups_[pd].item < upTo; ++pd)
      if (isHit(pendingDups_[pd].idx) && numHits() < cfg_.maxHits) {
        altHits_.push_back(batchEntries_[pendingDups_[pd].item]);
        ++stats.hits;
      }
  };
  for (size_t g = 0; g < ng; ++g) {
    flushDups(goals_[g].item);
    uint32_t idx = goals_[g].idx;
    const bool hit = goalDirect_[g] || !goalFits_[g].empty();
    if (idx == kEmpty || idx == kQuant) {
      if (idx == kEmpty) ++stats.overflowChecked;
      if (!hit) continue;
      if (idx == kEmpty) ++stats.overflowKept;
      const size_t k = goals_[g].item;
      idx = storeEntry(batchEntries_[k], batchFp_.data() + k * stride, false, stats);
    }
    if (hit) commitHits(idx, goalDirect_[g], goalFits_[g], stats);
  }
  flushDups(SIZE_MAX);
  batch_.clear();
}

std::vector<Candidate> Enumerator::run(SearchStats& stats) {
  start_ = nowSeconds();
  stats = SearchStats{};
  std::vector<Candidate> out;
  if (targetCost_ == 0) return out;
  // Level bound in order units. With a separate order model, an entry of objective
  // cost < target has order cost < R * target, R = max order/objective cost ratio.
  const CostModel& model = *cfg_.model;
  const CostModel& ord = order();
  double ratio = 1.0;
  for (Op op : ops_) ratio = std::max(ratio, double(ord[op]) / model[op]);
  if (model.fusedAdd)
    ratio = std::max(ratio, double(ord.fusedAdd ? ord.fusedAdd : std::max(ord[Op::Add], ord[Op::Sub])) /
                                model.fusedAdd);
  uint32_t maxCost = &ord == &model ? targetCost_ - 1
                                    : static_cast<uint32_t>(ratio * (targetCost_ - 1));
  if (cfg_.maxCost) maxCost = std::min(cfg_.maxCost, maxCost);
  byCost_.assign(maxCost + 1, {});
  stats.maxLevel = maxCost;

  // Level 0: inputs, the components of vector inputs (registers, no instruction) and
  // constants.
  stats.levels.push_back({0, 0, 0});
  for (uint32_t i = 0; i < prog_.inputs.size(); ++i) {
    const Type t = prog_.inputs[i].type;
    const uint32_t slot = tests_.slotOf(i);
    for (unsigned k = 0; k < width(t); ++k) {
      std::copy(tests_.cols[slot + k].begin(), tests_.cols[slot + k].end(), scratch_.begin() + k * n_);
    }
    canonicalize(scratch_.data(), width(t) * n_);
    const bool ct = prog_.inputs[i].compileTime;
    Entry e = Entry::make(Op::Input, t, 0, false, 0, 0, 0, i, false, 0, ct);
    if (!insert(e, scratch_.data(), stats) || width(t) == 1) continue;
    const auto vec = static_cast<uint32_t>(bank_.size() - 1);
    const auto swzCost = static_cast<uint16_t>(ct ? 0 : model.opCost(Op::Swizzle, 1));
    for (unsigned k = 0; k < width(t); ++k) {
      Entry s = Entry::make(Op::Swizzle, Type::Float, 0, false, vec, 0, 0, k, false, swzCost, ct);
      insert(s, fpOf(vec) + k * n_, stats);
    }
  }
  const ConstPool pool = constantPool(prog_.target);
  for (float cv : pool.scalars) addConst(Type::Float, &cv, stats);
  for (const auto& [t, v] : pool.vectors) addConst(t, v.data(), stats);
  if (cfg_.sharedLeaves) addSharedLeaves(stats);
  for (auto& list : byCost_[0])
    std::stable_sort(list.begin(), list.end(), [&](uint32_t x, uint32_t y) { return obj(x) < obj(y); });
  stats.completedCost = 0;

  const Type F = Type::Float;
  for (uint32_t cost = 1; cost <= maxCost && !stop_; ++cost) {
    stats.levels.push_back({cost, 0, 0});
    const auto c16 = static_cast<uint16_t>(cost);
    for (Op op : ops_) {
      if (stop_) break;
      const auto& oi = info(op);
      if (oi.shape == Shape::Cmp) {  // scalar comparisons
        if (ord.opCost(op, 1) <= cost) enumerateBinary(op, c16, cost - ord.opCost(op, 1), -1, F, F, stats);
        continue;
      }
      for (Type T : floatTypes()) {
        if (stop_) break;
        const unsigned w = width(T);
        const uint32_t opc = ord.opCost(op, w);
        if (opc > cost) continue;
        const uint32_t r = cost - opc;
        if (oi.shape == Shape::Select) {  // Bool condition, branches float1 or T
          if (w == 1) {
            enumerateTernary(op, c16, r, Type::Bool, F, F, stats);
          } else {
            enumerateTernary(op, c16, r, Type::Bool, T, T, stats);
            enumerateTernary(op, c16, r, Type::Bool, T, F, stats);
            enumerateTernary(op, c16, r, Type::Bool, F, T, stats);
          }
          continue;
        }
        if (oi.shape != Shape::Comp) continue;
        if (oi.arity == 1) {
          const auto& la = byCost_[r][static_cast<size_t>(T)];
          const uint32_t objOp = model.opCost(op, w);
          if (diskMode_ && hasDisk(r, T)) {
            for (const Seg& sg : listSegs(r, T)) {
              requireTiles({sg.tile}, stats);
              for (size_t i = sg.begin; i < sg.end && !stop_; ++i)
                if (obj(la[i]) + objOp < objLimit_) tryAdd(op, c16, la[i], 0, 0, stats);
            }
          } else {
            for (size_t i = 0; i < la.size() && !stop_ && obj(la[i]) + objOp < objLimit_; ++i)
              tryAdd(op, c16, la[i], 0, 0, stats);
          }
        } else if (oi.arity == 2) {
          // Operand widths: (T, T), (T, 1), (1, T); float1 x float1 when T is float1.
          std::vector<std::pair<Type, Type>> sigs = {{T, T}};
          if (w > 1) {
            sigs.push_back({T, F});
            if (!oi.commutative) sigs.push_back({F, T});
          }
          for (const auto& [ta, tb] : sigs) {
            if (ord.fusedAdd && (op == Op::Add || op == Op::Sub)) {
              // Contraction: an add/sub over a same-width mul costs fusedAdd per
              // component; the other pairs cost the full add (fusable pairs were
              // generated at the cheaper level).
              const uint32_t fc = w * ord.fusedAdd;
              if (fc <= cost) enumerateBinary(op, c16, cost - fc, 1, ta, tb, stats);
              enumerateBinary(op, c16, r, 0, ta, tb, stats);
            } else {
              enumerateBinary(op, c16, r, -1, ta, tb, stats);
            }
          }
        } else {
          // float1: one signature. floatN: every mix of float1 and T with at least one T
          // (mad(a, b, c) = mad(b, a, c): the mirrored (1, T, c) of (T, 1, c) is skipped).
          if (w == 1) {
            enumerateTernary(op, c16, r, F, F, F, stats);
          } else {
            for (int m = 1; m < 8; ++m) {
              const Type ta = (m & 1) ? T : F, tb = (m & 2) ? T : F, tc = (m & 4) ? T : F;
              if (op == Op::Mad && ta == F && tb == T) continue;
              enumerateTernary(op, c16, r, ta, tb, tc, stats);
            }
          }
        }
      }
    }
    flush(stats);
    for (auto& list : byCost_[cost])
      std::stable_sort(list.begin(), list.end(),
                       [&](uint32_t x, uint32_t y) { return obj(x) < obj(y); });
    if (diskMode_) finishLevelDisk(cost);
    if (cfg_.topDown && !diskMode_ && !stop_) topDownPass(cost, stats);
    if (!stop_ && stats.overflowChecked == 0) stats.completedCost = cost;
  }

  stats.bankSize = bank_.size();
  if (diskMode_) {
    stats.diskEntries = diskFrom_ == UINT32_MAX ? 0 : bank_.size() - diskFrom_;
    stats.diskBytes = disk_->bytesWritten();
    stats.diskRawBytes = disk_->rawBytes();
    stats.diskTilesRead = disk_->tilesRead();
  }
  stats.seconds = nowSeconds() - start_;
  out.reserve(numHits());
  // With shared leaves (free in the bank) a hit's real DAG cost can reach the target's:
  // not cheaper, so not a candidate. Without them tree cost >= DAG cost keeps all.
  auto push = [&](Expr x) {
    Candidate cand;
    cand.cost = dagCost(x, *cfg_.model, prog_.inputs);
    cand.expr = std::move(x);
    if (shared_.empty() || cand.cost < targetCost_) out.push_back(std::move(cand));
  };
  for (uint32_t idx : hits_) push(extract(entry(idx)));
  for (const Entry& e : altHits_) push(extract(e));
  for (const AffineHit& h : affineHits_) push(extract(h));
  return out;
}

// Binary op at this level with operand costs summing to r and operand types ta, tb.
// fuse: -1 = all pairs, 1 = only pairs with a same-width mul (or div) operand, 0 = only
// pairs without one.
void Enumerator::topDownPass(uint32_t cost, SearchStats& stats) {
  const Type T = targetType_;
  if (T == Type::Bool) return;
  // Lookup points: the last two test points (random ones) where the target is finite.
  if (tdP0_ == UINT32_MAX) {
    for (size_t i = n_; i-- > 0;)
      if (targetFinite_[i]) {
        if (tdP0_ == UINT32_MAX) tdP0_ = static_cast<uint32_t>(i);
        else if (tdP1_ == UINT32_MAX) tdP1_ = static_cast<uint32_t>(i);
      }
    if (tdP1_ == UINT32_MAX) return;
    for (uint32_t c = 0; c < cost; ++c)  // earlier levels (the pass starts at level 1)
      for (uint32_t x : byCost_[c][static_cast<size_t>(T)]) tdIndex_.push_back({fpOf(x)[tdP0_], x});
    std::sort(tdIndex_.begin(), tdIndex_.end());
  }
  const auto& fresh = byCost_[cost][static_cast<size_t>(T)];
  if (fresh.empty()) return;
  {
    const size_t mid = tdIndex_.size();
    for (uint32_t x : fresh) tdIndex_.push_back({fpOf(x)[tdP0_], x});
    std::sort(tdIndex_.begin() + static_cast<std::ptrdiff_t>(mid), tdIndex_.end());
    std::inplace_merge(tdIndex_.begin(), tdIndex_.begin() + static_cast<std::ptrdiff_t>(mid), tdIndex_.end());
  }
  const CostModel& ord = order();
  const unsigned w = width(T);
  const double t0 = target_[tdP0_], tol0 = monoTol_[tdP0_];
  const float t1 = target_[tdP1_];
  constexpr double kU = 0x1p-24;
  std::vector<float> fp(4 * n_);
  std::vector<AffineHit> fits;
  enum Kind { kAdd, kSub, kSubR, kMul, kDiv, kDivR };
  auto has = [&](Op op) { return std::find(ops_.begin(), ops_.end(), op) != ops_.end(); };
  const bool hasAdd = has(Op::Add), hasSub = has(Op::Sub), hasMul = has(Op::Mul), hasDiv = has(Op::Div);
  for (uint32_t a : fresh) {
    if (stop_) break;
    if (obj(a) + 1 >= objLimit_) continue;  // only hits within the best-so-far bound
    const float* va = fpOf(a);
    const double a0 = va[tdP0_];
    if (!std::isfinite(a0)) continue;
    for (int k = kAdd; k <= kDivR; ++k) {
      const Op op = k <= kSubR ? (k == kAdd ? Op::Add : Op::Sub) : (k == kMul ? Op::Mul : Op::Div);
      if ((op == Op::Add && !hasAdd) || (op == Op::Sub && !hasSub) || (op == Op::Mul && !hasMul) ||
          (op == Op::Div && !hasDiv))
        continue;
      if (obj(a) + cfg_.model->opCost(op, w) >= objLimit_) continue;
      // The operand b that makes op(a, b) = t at the lookup point, and how far off it may be.
      double b0, db;
      const double slack = tol0 + 4.0 * kU * (std::fabs(t0) + std::fabs(a0));
      switch (k) {
        case kAdd: b0 = t0 - a0; db = slack; break;
        case kSub: b0 = a0 - t0; db = slack; break;
        case kSubR: b0 = t0 + a0; db = slack; break;
        case kMul:
          if (std::fabs(a0) < 1e-30) continue;
          b0 = t0 / a0; db = slack / std::fabs(a0); break;
        case kDiv:
          if (std::fabs(t0) < 1e-30) continue;
          b0 = a0 / t0; db = std::fabs(a0) / (t0 * t0) * slack; break;
        default: b0 = t0 * a0; db = std::fabs(a0) * slack; break;
      }
      db += 8.0 * kU * std::fabs(b0) + 1e-37;
      if (!std::isfinite(b0) || !std::isfinite(db)) continue;
      auto lo = std::lower_bound(tdIndex_.begin(), tdIndex_.end(), std::make_pair(static_cast<float>(b0 - db), 0u));
      int budget = 64;  // at most this many candidates per (a, op): a flat range is not a lookup
      for (auto it = lo; it != tdIndex_.end() && it->first <= b0 + db && budget-- > 0; ++it) {
        const uint32_t b = it->second;
        const float* vb = fpOf(b);
        // Second point first (cheap), then the whole candidate.
        float r1;
        switch (k) {
          case kAdd: r1 = va[tdP1_] + vb[tdP1_]; break;
          case kSub: r1 = va[tdP1_] - vb[tdP1_]; break;
          case kSubR: r1 = vb[tdP1_] - va[tdP1_]; break;
          case kMul: r1 = va[tdP1_] * vb[tdP1_]; break;
          case kDiv: r1 = va[tdP1_] / vb[tdP1_]; break;
          default: r1 = vb[tdP1_] / va[tdP1_]; break;
        }
        if (!accepts(tdP1_, r1) && std::fabs(double(r1) - t1) > 4.0 * monoTol_[tdP1_]) continue;
        const bool swap = k == kSubR || k == kDivR;
        const Item item{op, static_cast<uint16_t>(entry(a).cost + entry(b).cost + ord.opCost(op, w)), swap ? b : a,
                        swap ? a : b, 0, 0};
        Entry e;
        // Top-down hits must be strictly cheaper than the best so far: it finds many
        // equivalent programs at the bound, and those only fill the hit list.
        if (prepare(item, e, fp.data()) != Prep::Ok || e.type != T || e.obj >= std::min(objLimit_, bestHitObj_))
          continue;
        ++stats.topDownChecked;
        if ((stats.topDownChecked & 4095) == 0) {
          checkLimits(stats);
          if (stop_) return;
        }
        // Already in the bank: it was goal-checked when it was stored.
        const size_t len = lenOf(e.type);
        const uint64_t h1 = hashFp(fp.data(), e.type);
        bool dup = false;
        for (size_t pos = h1 & (table_.size() - 1); table_[pos] != kEmpty && !dup; pos = (pos + 1) & (table_.size() - 1))
          dup = entry(table_[pos]).type == e.type && sameFp(fpOf(table_[pos]), fp.data(), len);
        if (dup) continue;
        fits.clear();
        const bool direct = goalCheck(e, fp.data(), kEmpty, fits, serialScratch_, stats);
        if (!direct && fits.empty()) continue;
        const uint32_t idx = storeEntry(e, fp.data(), false, stats);
        commitHits(idx, direct, fits, stats);
        ++stats.topDownHits;
      }
    }
  }
}

void Enumerator::finishLevelDisk(uint32_t cost) {
  // Everything appended so far becomes readable; the level's lists get their segments:
  // the entries with fingerprints in RAM first (still sorted by objective), then the disk
  // ones grouped by tile (bank order = stream order).
  if (spilling_) disk_->closeTile();
  tilePtr_.resize(disk_->tiles(), nullptr);
  for (const auto& slot : tileSlots_)
    if (slot.tile < tilePtr_.size()) tilePtr_[slot.tile] = slot.buf.get();
  if (segs_.size() <= cost) segs_.resize(cost + 1);
  for (size_t t = 0; t < kNumTypes; ++t) {
    auto& list = byCost_[cost][t];
    auto& sg = segs_[cost][t];
    sg.clear();
    if (list.empty()) continue;
    auto mid = std::stable_partition(list.begin(), list.end(), [&](uint32_t x) { return !onDisk(x); });
    std::sort(mid, list.end());
    const size_t ram = static_cast<size_t>(mid - list.begin());
    if (ram) sg.push_back({0, ram, -1});
    const size_t tf = cfg_.diskTileFloats;
    for (size_t i = ram; i < list.size();) {
      const int64_t tile = static_cast<int64_t>(diskOff_[list[i] - diskFrom_] / tf);
      size_t j = i;
      while (j < list.size() && static_cast<int64_t>(diskOff_[list[j] - diskFrom_] / tf) == tile) ++j;
      sg.push_back({i, j, tile});
      i = j;
    }
  }
}

const std::vector<Enumerator::Seg>& Enumerator::listSegs(uint32_t cost, Type t) {
  static const std::vector<Seg> none;
  if (cost >= segs_.size()) {
    // Levels finished before the bank spilled: all in RAM (a single sorted segment).
    segs_.resize(cost + 1);
  }
  auto& sg = segs_[cost][static_cast<size_t>(t)];
  if (sg.empty() && !byCost_[cost][static_cast<size_t>(t)].empty())
    sg.push_back({0, byCost_[cost][static_cast<size_t>(t)].size(), -1});
  return sg.empty() ? none : sg;
}

void Enumerator::requireTiles(std::initializer_list<int64_t> tiles, SearchStats& stats) {
  bool missing = false;
  for (int64_t t : tiles)
    if (t >= 0 && !tilePtr_[static_cast<size_t>(t)]) missing = true;
  for (int64_t t : tiles)
    if (t >= 0)
      for (auto& slot : tileSlots_)
        if (slot.tile == static_cast<uint64_t>(t)) slot.used = ++tileClock_;
  if (!missing) return;
  flush(stats);  // pending candidates may use the tiles about to be replaced
  for (int64_t t : tiles) {
    if (t < 0 || tilePtr_[static_cast<size_t>(t)]) continue;
    // The least recently used slot that is not needed now.
    TileSlot* victim = nullptr;
    for (auto& slot : tileSlots_) {
      bool needed = false;
      for (int64_t u : tiles) needed = needed || (u >= 0 && slot.tile == static_cast<uint64_t>(u));
      if (!needed && (!victim || slot.used < victim->used)) victim = &slot;
    }
    if (!victim->buf) victim->buf.reset(new float[cfg_.diskTileFloats]);
    if (victim->tile < tilePtr_.size()) tilePtr_[victim->tile] = nullptr;
    disk_->readTile(static_cast<uint64_t>(t), victim->buf.get());
    victim->tile = static_cast<uint64_t>(t);
    victim->used = ++tileClock_;
    tilePtr_[static_cast<size_t>(t)] = victim->buf.get();
  }
}

void Enumerator::enumerateBinary(Op op, uint16_t level, uint32_t r, int fuse, Type ta, Type tb,
                                 SearchStats& stats) {
  const auto& oi = info(op);
  const CostModel& model = order();
  const Type rt = oi.shape == Shape::Cmp ? Type::Bool : floatType(std::max(width(ta), width(tb)));
  // Lowest objective cost the op can add (a fused add/sub is cheaper).
  const CostModel& objective = *cfg_.model;
  const unsigned w = width(rt);
  uint32_t minOp = objective.opCost(op, w);
  if (objective.fusedAdd && (op == Op::Add || op == Op::Sub)) minOp = w * objective.fusedAdd;
  const bool sym = oi.commutative && ta == tb;
  auto fusable = [&](uint32_t x) { return model.fusesIntoAdd(entry(x).op) && entry(x).type == rt; };
  for (uint32_t c1 = 0; c1 <= r && !stop_; ++c1) {
    const uint32_t c2 = r - c1;
    if (sym && c1 > c2) continue;
    const auto& la = byCost_[c1][static_cast<size_t>(ta)];
    const auto& lb = byCost_[c2][static_cast<size_t>(tb)];
    if (lb.empty()) continue;
    if (diskMode_ && (hasDisk(c1, ta) || hasDisk(c2, tb))) {
      // Tile by tile: every pair of segments once, with both tiles resident.
      const std::vector<Seg> sa = listSegs(c1, ta), sb = listSegs(c2, tb);
      const bool same = sym && c1 == c2;
      for (size_t ia = 0; ia < sa.size() && !stop_; ++ia)
        for (size_t ib = same ? ia : 0; ib < sb.size() && !stop_; ++ib) {
          requireTiles({sa[ia].tile, sb[ib].tile}, stats);
          for (size_t i = sa[ia].begin; i < sa[ia].end && !stop_; ++i) {
            const bool fa = fusable(la[i]);
            for (size_t j = (same && ia == ib) ? i : sb[ib].begin; j < sb[ib].end && !stop_; ++j) {
              if (obj(la[i]) + obj(lb[j]) + minOp >= objLimit_) {
                if (sb[ib].tile < 0) break;  // the RAM segment is sorted by objective
                continue;
              }
              if (fuse >= 0 && (fa || fusable(lb[j])) != (fuse == 1)) continue;
              tryAdd(op, level, la[i], lb[j], 0, stats);
            }
          }
        }
      continue;
    }
    for (size_t i = 0; i < la.size() && !stop_; ++i) {
      if (obj(la[i]) + obj(lb[0]) + minOp >= objLimit_) break;
      const bool fa = fusable(la[i]);
      size_t j0 = (sym && c1 == c2) ? i : 0;
      for (size_t j = j0; j < lb.size() && !stop_; ++j) {
        if (obj(la[i]) + obj(lb[j]) + minOp >= objLimit_) break;
        if (fuse >= 0 && (fa || fusable(lb[j])) != (fuse == 1)) continue;
        tryAdd(op, level, la[i], lb[j], 0, stats);
      }
    }
  }
}

void Enumerator::enumerateTernary(Op op, uint16_t level, uint32_t r, Type ta, Type tb, Type tc,
                                  SearchStats& stats) {
  const bool sym01 = op == Op::Mad && ta == tb;  // mad(a, b, c) == mad(b, a, c)
  const unsigned w = std::max({width(ta == Type::Bool ? Type::Float : ta), width(tb), width(tc)});
  const uint32_t opc = cfg_.model->opCost(op, w);
  for (uint32_t c1 = 0; c1 <= r && !stop_; ++c1) {
    for (uint32_t c2 = 0; c1 + c2 <= r && !stop_; ++c2) {
      const uint32_t c3 = r - c1 - c2;
      if (sym01 && c1 > c2) continue;
      const auto& la = byCost_[c1][static_cast<size_t>(ta)];
      const auto& lb = byCost_[c2][static_cast<size_t>(tb)];
      const auto& lc = byCost_[c3][static_cast<size_t>(tc)];
      if (lb.empty() || lc.empty()) continue;
      if (diskMode_ && (hasDisk(c1, ta) || hasDisk(c2, tb) || hasDisk(c3, tc))) {
        const std::vector<Seg> sa = listSegs(c1, ta), sb = listSegs(c2, tb), sc = listSegs(c3, tc);
        const bool same = sym01 && c1 == c2;
        for (size_t ia = 0; ia < sa.size() && !stop_; ++ia)
          for (size_t ib = same ? ia : 0; ib < sb.size() && !stop_; ++ib)
            for (size_t ic = 0; ic < sc.size() && !stop_; ++ic) {
              requireTiles({sa[ia].tile, sb[ib].tile, sc[ic].tile}, stats);
              for (size_t i = sa[ia].begin; i < sa[ia].end && !stop_; ++i)
                for (size_t j = (same && ia == ib) ? i : sb[ib].begin; j < sb[ib].end && !stop_; ++j) {
                  const uint32_t ab = obj(la[i]) + obj(lb[j]) + opc;
                  if (ab >= objLimit_) continue;
                  for (size_t k = sc[ic].begin; k < sc[ic].end && !stop_; ++k) {
                    if (ab + obj(lc[k]) >= objLimit_) {
                      if (sc[ic].tile < 0) break;
                      continue;
                    }
                    if ((op == Op::Select || op == Op::Lerp) && lb[j] == lc[k]) continue;
                    tryAdd(op, level, la[i], lb[j], lc[k], stats);
                  }
                }
            }
        continue;
      }
      const uint32_t minC = obj(lc[0]);
      for (size_t i = 0; i < la.size() && !stop_; ++i) {
        if (obj(la[i]) + obj(lb[0]) + minC + opc >= objLimit_) break;
        size_t j0 = (sym01 && c1 == c2) ? i : 0;
        for (size_t j = j0; j < lb.size() && !stop_; ++j) {
          const uint32_t ab = obj(la[i]) + obj(lb[j]) + opc;
          if (ab + minC >= objLimit_) break;
          for (size_t k = 0; k < lc.size() && !stop_ && ab + obj(lc[k]) < objLimit_; ++k) {
            if ((op == Op::Select || op == Op::Lerp) && lb[j] == lc[k]) continue;
            tryAdd(op, level, la[i], lb[j], lc[k], stats);
          }
        }
      }
    }
  }
}

void Enumerator::addSharedLeaves(SearchStats& stats) {
  const Expr& t = prog_.target;
  const std::vector<bool> ct = compileTimeNodes(t, prog_.inputs);
  const Type tt = t.nodes[t.root].type;
  std::vector<std::pair<uint32_t, uint32_t>> subs;  // (DAG cost, node)
  for (uint32_t i = 0; i < t.nodes.size(); ++i) {
    const Node& nd = t.nodes[i];
    if (i == t.root || ct[i] || nd.op == Op::Input || nd.op == Op::Const || nd.op == Op::Swizzle ||
        nd.op == Op::Construct)
      continue;
    if (nd.type != Type::Float && nd.type != tt) continue;
    subs.emplace_back(dagCost(subexpr(t, i), *cfg_.model, prog_.inputs), i);
  }
  std::stable_sort(subs.begin(), subs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  if (subs.size() > cfg_.maxShared) subs.resize(cfg_.maxShared);
  for (const auto& [cost, node] : subs) {
    Expr s = subexpr(t, node);
    const Type ty = s.nodes[s.root].type;
    const std::vector<float> v = evalAll(s, tests_, kProfileRef);
    std::copy(v.begin(), v.begin() + width(ty) * n_, scratch_.begin());
    canonicalize(scratch_.data(), width(ty) * n_);
    const auto aux = kShared + static_cast<uint32_t>(shared_.size());
    shared_.push_back(std::move(s));
    sharedCost_.push_back(cost);
    Entry e = Entry::make(Op::Input, ty, 0, false, 0, 0, 0, aux, false, 0, false);
    insert(e, scratch_.data(), stats);
  }
}

// A program with the value of shared leaf idx that is cheaper than the leaf's current
// form (the original's subexpression) becomes its form: dedup keeps the first program
// of a value, which is the free leaf, so candidates would otherwise inherit the
// original's more expensive form.
void Enumerator::upgradeShared(uint32_t idx, const Entry& e) {
  const Entry leaf = entry(idx);
  if (leaf.op != Op::Input || leaf.aux() < kShared || e.op == Op::Input || e.op == Op::Const) return;
  const uint32_t k = leaf.aux() - kShared;
  if (e.obj >= sharedCost_[k]) return;
  Expr x = extract(e);
  const uint32_t c = dagCost(x, *cfg_.model, prog_.inputs);
  if (c >= sharedCost_[k]) return;
  shared_[k] = std::move(x);
  sharedCost_[k] = c;
}

uint32_t Enumerator::build(ExprBuilder& b, const Entry& rootEntry) const {
  std::unordered_map<uint32_t, uint32_t> memo;
  auto rec = [&](auto&& self, const Entry& e) -> uint32_t {
    if (e.op == Op::Input)
      return e.aux() >= kShared ? insertExpr(shared_[e.aux() - kShared], b) : b.input(e.aux(), e.type);
    if (e.op == Op::Const) return b.constant(e.type, consts_[e.aux()].data());
    const auto& oi = info(e.op);
    uint32_t a[3] = {0, 0, 0};
    for (uint8_t k = 0; k < oi.arity; ++k) {
      const uint32_t idx = e.args[k];
      if (auto it = memo.find(idx); it != memo.end()) {
        a[k] = it->second;
      } else {
        a[k] = self(self, entry(idx));
        memo[idx] = a[k];
      }
    }
    if (e.op == Op::Swizzle) {
      const auto comp = static_cast<uint8_t>(e.aux());
      return b.swizzle(a[0], &comp, 1);
    }
    return b.op(e.op, a[0], a[1], a[2]);
  };
  return rec(rec, rootEntry);
}

Expr Enumerator::extract(const Entry& rootEntry) const {
  ExprBuilder b;
  return b.finish(build(b, rootEntry));
}

Expr Enumerator::extract(const AffineHit& h) const {
  ExprBuilder b;
  uint32_t v = build(b, entry(h.idx));
  auto k = [&](float x) { return b.constant(x); };  // scalars broadcast to a vector v
  if (h.rational) {
    const uint32_t num = h.r < 0.0f ? b.op(Op::Add, v, k(-h.r)) : b.op(Op::Sub, v, k(h.r));
    const uint32_t den = b.op(Op::Mad, v, k(h.a), k(h.b));
    return b.finish(b.op(Op::Mul, num, b.op(Op::Rcp, den)));
  }
  if (h.inner != Op::Count) {
    v = h.c < 0.0f ? b.op(Op::Sub, v, k(-h.c)) : b.op(Op::Add, v, k(h.c));
    v = b.op(h.inner, v);
  }
  uint32_t r;
  switch (h.wrap) {
    case Op::Add: r = b.op(Op::Add, v, k(h.q)); break;
    case Op::Mul: r = b.op(Op::Mul, v, k(h.p)); break;
    case Op::Sub: r = b.op(Op::Sub, k(h.q), v); break;
    default: r = b.op(Op::Mad, v, k(h.p), k(h.q)); break;
  }
  return b.finish(r);
}

}  // namespace sopt
