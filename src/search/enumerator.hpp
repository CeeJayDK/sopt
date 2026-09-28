#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ir/expr.hpp"
#include "verify/points.hpp"
#include "verify/verify.hpp"
#include "search/diskstore.hpp"

namespace sopt {

struct SearchConfig {
  uint32_t maxCost = 0;         // inclusive, in order-model units; 0 = derived from the target
  // Bank size: by memory (owner, 2026-09-28: use what the machine has, keep a little for
  // the system). memBudget bytes (0 = the RAM available when the run started, minus
  // max(1 GB, 5% of the RAM) for the system and 256 MB per concurrent search for its other
  // data, shared by `concurrent` searches running at the same time); maxBank additionally
  // caps the number of entries (0 = no cap).
  size_t maxBank = 0;
  size_t memBudget = 0;
  unsigned concurrent = 1;
  // Disk-backed bank (owner, 2026-09-28: an option for long runs on single regions, not a
  // default): when the fingerprints in RAM reach an eighth of the memory budget, the rest go to
  // zstd-compressed tiles in diskDir (a temporary file, removed at the end); dedup then
  // compares 128-bit hashes of the fingerprints, and operands on disk are enumerated tile
  // by tile. diskBudget bytes (0 = the free space of diskDir minus a reserve).
  // Top-down split (B6 in docs/performance-ideas.md, flag --top-down): after each level,
  // for every new entry a of the target's type and every invertible binary op the missing
  // operand b (t - a, a - t, t + a, t / a, a / t, t * a) is looked up among all stored
  // entries (sorted by their value at one test point, within the target's tolerance) and
  // op(a, b) is goal-checked: pairs of any two stored entries, up to twice the depth for
  // the top operation, in one pass instead of all pairs. Not in disk mode.
  bool topDown = false;
  std::string diskDir;
  size_t diskBudget = 0;
  size_t diskTileFloats = size_t{1} << 24;  // 64 MB of fingerprints per tile
  size_t diskBlockFloats = size_t{1} << 18; // 1 MB per zstd block
  size_t maxHits = 10'000;
  double timeLimitSec = 60.0;
  const CostModel* model = &defaultCostModel();  // objective: hits and ranking
  // Enumeration levels (null = defaultOrderFor(model)). A cheap, uniform order model (generic) reaches
  // deeper than an objective with expensive ops (rdna3 transcendentals); the objective
  // still decides what is a hit. Dedup keeps the first program of a value in order.
  const CostModel* order = nullptr;
  // Symbolic constants, first step: the outer affine map of a candidate is solved, not
  // enumerated. Every entry v is fitted as target ~ p * v + q (least squares), and
  // entries that are only an affine map of another (chains such as (v*c1 + c2)*c3,
  // two-constant mad/lerp) are not added to the bank.
  bool affine = true;
  // Also solve an inner constant: target ~ p * u(v + c) + q, u in {rcp, sqrt, rsqrt}
  // (needs affine).
  bool inner = true;
  // With an inner rcp fit p / (v + c) + q, also emit (v - r) * rcp(mad(v, 1/q, c/q)): the
  // same function without the final cancellation (accuracy variants, owner 2026-09-26).
  bool rational = true;
  // Skip inner fits for entries the target is not monotonic in (see innerFit).
  bool innerPrefilter = true;
  // When the bank is full, keep enumerating with the stored entries as operands and only
  // check the new values as hits (not stored): one more level of reach, no more memory.
  // Runs until the time limit. Default (owner): bench +2 found, none lost.
  bool overflow = true;
  // Threads for evaluating and goal-checking candidates (0 = hardware concurrency).
  // Results do not depend on it.
  unsigned threads = 0;
  // Enumerate pure helper intrinsics (lerp, step). Off: they are only shorthand for
  // their expansions (lerp = mad(t, b - a, a), step = x >= e ? 1 : 0), which the search
  // builds anyway, so trying both wastes time. Single-instruction intrinsics (mad = fma,
  // clamp = med3, saturate = modifier, rcp, rsqrt) are always enumerated.
  bool helpers = false;
  // Shared leaves (M7, flag): the target's own subexpressions (up to maxShared, most
  // expensive first) are extra level-0 leaves at no cost, so rewrites that use one of the
  // original's intermediate values twice (u * u for pow(abs(u), 2.0)) are reached although
  // the bank prices trees. Candidates are still ranked by their real DAG cost.
  bool sharedLeaves = true;  // default (owner, 2026-09-27); --no-shared-leaves
  uint32_t maxShared = 16;
  // Quantized observational equivalence (M7, flag --quant-oe N): values whose
  // fingerprints agree after rounding away the low N mantissa bits count as one value, so
  // rounding variants of the same function (a + b + c vs a + (b + c)) take one bank slot.
  // A merged value that is not bitwise equal is still goal-checked (a hit is kept), it
  // only does not become an operand. 0 = bit-exact dedup.
  uint32_t quantBits = 0;
};

// Physical RAM of this machine in bytes (4 GB if unknown), and the part available (not
// used by other processes; measured once, at the first call).
size_t physicalMemory();
size_t availableMemory();
// The bank's memory budget for cfg (SearchConfig::memBudget / concurrent).
size_t bankBudget(const SearchConfig& cfg);

struct LevelStats {
  uint32_t cost = 0;
  uint64_t generated = 0;
  uint64_t added = 0;
};

struct SearchStats {
  uint64_t generated = 0;
  uint64_t deduped = 0;
  uint64_t constSkipped = 0;
  uint64_t bankSize = 0;
  uint64_t hits = 0;
  uint64_t affinePruned = 0;
  uint64_t innerHits = 0;
  uint64_t innerPrefiltered = 0;  // entries the monotonicity check ruled out
  uint64_t overflowChecked = 0;   // overflow mode: values checked after the bank was full
  uint64_t overflowKept = 0;      // ... kept because they are (part of) hits
  uint64_t objPruned = 0;  // objective cost already >= target
  uint64_t quantMerged = 0;  // SearchConfig::quantBits: dedups that were not bitwise equal
  uint64_t affineHits = 0;
  uint64_t topDownChecked = 0;  // SearchConfig::topDown: candidate pairs fully checked
  uint64_t topDownHits = 0;     // ... that were hits
  uint64_t diskEntries = 0;   // disk-backed bank: entries whose fingerprints went to disk
  uint64_t diskBytes = 0;     // ... compressed bytes written
  uint64_t diskRawBytes = 0;  // ... before compression
  uint64_t diskTilesRead = 0;
  uint32_t completedCost = 0;  // all levels <= this were fully enumerated
  uint32_t maxLevel = 0;       // the last level the search needed (order-model units)
  bool limitHit = false;
  double seconds = 0.0;
  double firstHitSec = -1.0;
  std::vector<LevelStats> levels;
};

struct Candidate {
  Expr expr;
  uint32_t cost = 0;  // DAG cost
};

// Grows in fixed chunks: no reallocation (no copying of GBs, no 2x peak) and no large
// up-front reservation (which Windows would commit).
template <class T, unsigned Shift = 20>
class Chunked {
 public:
  void push_back(const T& v) {
    if ((n_ >> Shift) == chunks_.size()) chunks_.emplace_back(new T[size_t{1} << Shift]);
    chunks_[n_ >> Shift][n_ & kMask] = v;
    ++n_;
  }
  void pop_back() { --n_; }
  size_t size() const { return n_; }
  T& operator[](size_t i) { return chunks_[i >> Shift][i & kMask]; }
  const T& operator[](size_t i) const { return chunks_[i >> Shift][i & kMask]; }

 private:
  static constexpr size_t kMask = (size_t{1} << Shift) - 1;
  std::vector<std::unique_ptr<T[]>> chunks_;
  size_t n_ = 0;
};

// Fingerprints: records in 64 MB chunks (a record never straddles two); 32-bit offsets =
// chunk << 24 | position, so at most 256 chunks (16 GB).
class FpArena {
 public:
  static constexpr unsigned kShift = 24;
  uint32_t append(const float* v, size_t len) {
    if (chunks_.empty() || pos_ + len > kSize) {
      if (!chunks_.empty()) ++cur_;
      if (cur_ == chunks_.size()) chunks_.emplace_back(new float[kSize]);
      pos_ = 0;
    }
    const uint32_t off = static_cast<uint32_t>((cur_ << kShift) | pos_);
    std::copy(v, v + len, chunks_[cur_].get() + pos_);
    pos_ += len;
    used_ += len;
    return off;
  }
  // Drops everything from offset off on (the most recent records).
  void truncate(uint32_t off, size_t len) {
    cur_ = off >> kShift;
    pos_ = off & (kSize - 1);
    used_ -= len;
  }
  const float* data(uint32_t off) const { return chunks_[off >> kShift].get() + (off & (kSize - 1)); }
  size_t floats() const { return used_; }
  bool nearlyFull() const { return chunks_.size() >= 255; }

 private:
  static constexpr size_t kSize = size_t{1} << kShift;
  std::vector<std::unique_ptr<float[]>> chunks_;
  size_t cur_ = 0, pos_ = 0, used_ = 0;
};

// Bottom-up enumeration by increasing cost with observational-equivalence dedup:
// one bank entry per distinct fingerprint (output on the test points).
class Enumerator {
 public:
  Enumerator(const Program& prog, const PointSet& tests, const SearchConfig& cfg);
  std::vector<Candidate> run(SearchStats& stats);

 private:
  // 20 bytes: the bank holds millions (memory per entry = this + the fingerprint).
  struct Entry {
    uint32_t args[3];     // operands; Input: args[2] = input index; Const: index into
                          // consts_; Swizzle: args[0] = vector, args[2] = component
    uint16_t cost;        // order-model level
    uint16_t obj;         // objective (model) tree cost
    Op op;
    Type type;
    bool isConst : 1;
    bool affine : 1;      // single affine step (v + c, v * c, -v, ...) of a non-constant entry
    // Computed only from constants and compile-time inputs (not constants themselves):
    // the compiler folds it, so its objective cost is 0.
    bool ctime : 1;
    uint32_t aux() const { return args[2]; }
    static Entry make(Op op, Type type, uint16_t cost, bool isConst, uint32_t a, uint32_t b, uint32_t c,
                      uint32_t aux, bool affine, uint16_t obj, bool ctime) {
      Entry e{};
      e.args[0] = a;
      e.args[1] = b;
      e.args[2] = (op == Op::Input || op == Op::Const || op == Op::Swizzle) ? aux : c;
      e.cost = cost;
      e.obj = obj;
      e.op = op;
      e.type = type;
      e.isConst = isConst;
      e.affine = affine;
      e.ctime = ctime;
      return e;
    }
  };
  static_assert(sizeof(Entry) == 20);
  // Hit through a solved outer affine map: wrap(x) with op Add (x + q), Mul (x * p),
  // Sub (q - x) or Mad (mad(x, p, q)), where x = entry(idx), or inner(entry(idx) + c)
  // with a solved inner constant.
  struct AffineHit {
    uint32_t idx;
    Op wrap;
    float p, q;
    Op inner = Op::Count;
    float c = 0.0f;
    // SearchConfig::rational: (v - r) * rcp(mad(v, a, b)) instead of the wrapper.
    bool rational = false;
    float r = 0.0f, a = 0.0f, b = 0.0f;
  };

  void tryAdd(Op op, uint16_t cost, uint32_t a, uint32_t b, uint32_t c, SearchStats& stats,
              uint32_t aux = 0);
  // One generated candidate: op over bank entries a, b, c (aux: swizzle component).
  struct Item {
    Op op;
    uint16_t cost;
    uint32_t a, b, c, aux;
  };
  enum class Prep : uint8_t { Ok, ConstSkipped, AffinePruned, ObjPruned };
  // Checks and evaluates a candidate into e and out (4 * n_ floats); no bank changes.
  Prep prepare(const Item& it, Entry& e, float* out) const;
  void flush(SearchStats& stats);
  uint32_t storeEntry(const Entry& e, const float* fp, bool listed, SearchStats& stats);
  static constexpr size_t kBatch = 16384;
  std::vector<Item> batch_;
  std::vector<Prep> prep_;
  std::vector<Entry> batchEntries_;
  std::vector<float> batchFp_;
  std::vector<uint64_t> batchHash_;
  std::vector<uint32_t> batchDup_;    // entry stored before the batch with the same value
  std::vector<uint32_t> localTable_;  // entries stored in this batch, by hash
  struct Goal {
    size_t item;
    uint32_t idx;  // bank index, kEmpty = not stored (overflow)
  };
  std::vector<Goal> goals_;
  std::vector<Goal> pendingDups_;  // duplicates of entries stored in the same batch
  std::vector<char> goalDirect_;
  std::vector<std::vector<AffineHit>> goalFits_;
  bool insert(const Entry& e, const float* fp, SearchStats& stats);
  void enumerateBinary(Op op, uint16_t level, uint32_t r, int fuse, Type ta, Type tb,
                       SearchStats& stats);
  void enumerateTernary(Op op, uint16_t level, uint32_t r, Type ta, Type tb, Type tc,
                        SearchStats& stats);
  uint32_t addConst(Type t, const float* v, SearchStats& stats);
  // Per-thread scratch of the fits.
  struct FitScratch {
    std::vector<float> fit;
    std::vector<uint32_t> order;
  };
  // Goal check of a new value v (entry e, bank index idx): true if it is a hit; else
  // fitted hits (outer affine map, inner constant) are appended to out. No bank changes.
  bool goalCheck(const Entry& e, const float* v, uint32_t idx, std::vector<AffineHit>& out,
                 FitScratch& s, SearchStats& stats) const;
  // Records the hits of entry idx found by goalCheck.
  void commitHits(uint32_t idx, bool direct, const std::vector<AffineHit>& fitted, SearchStats& stats);
  bool affineFit(const Entry& e, const float* v, uint32_t idx, std::vector<AffineHit>& out) const;
  bool innerFit(const Entry& e, const float* v, uint32_t idx, std::vector<AffineHit>& out,
                FitScratch& s, SearchStats& stats) const;
  // Direction changes of the target along v (sorted), beyond monoTol_; 2 = none fits.
  int monotoneBreaks(const float* v, std::vector<uint32_t>& keysBuf) const;
  std::vector<double> monoTol_;
  FitScratch serialScratch_;
  std::vector<FitScratch> threadScratch_;
  std::vector<AffineHit> fitOut_;
  bool fitWrap(const float* v, Op top, uint32_t baseObj, AffineHit& out) const;
  uint32_t obj(uint32_t idx) const { return static_cast<uint32_t>(bank_[idx].w1 >> 48); }
  const CostModel& order() const { return cfg_.order ? *cfg_.order : defaultOrderFor(*cfg_.model); }
  size_t numHits() const { return hits_.size() + altHits_.size() + affineHits_.size(); }
  uint64_t hashFp(const float* fp, Type t) const;
  // Fingerprint equality for dedup (quantized with SearchConfig::quantBits).
  bool sameFp(const float* a, const float* b, size_t len) const;
  uint32_t quant(float x) const;
  void growTable();
  const float* fpOf(uint32_t idx) const {
    if (idx >= diskFrom_) {
      const uint64_t d = diskOff_[idx - diskFrom_];
      if (d != kNotOnDisk) {
        const size_t tf = cfg_.diskTileFloats;
        return tilePtr_[d / tf] + d % tf;  // the tile must be resident (requireTiles)
      }
    }
    return fp_.data(off_[idx]);
  }
  size_t lenOf(Type t) const { return width(t) * n_; }
  float constValue(uint32_t idx) const { return consts_[entry(idx).aux()][0]; }
  const std::vector<Type>& floatTypes() const { return types_; }
  Expr extract(const Entry& e) const;
  void addSharedLeaves(SearchStats& stats);
  void upgradeShared(uint32_t idx, const Entry& e);
  Expr extract(const AffineHit& h) const;
  uint32_t build(ExprBuilder& b, const Entry& e) const;
  void checkLimits(SearchStats& stats);

  const Program& prog_;
  const PointSet& tests_;
  SearchConfig cfg_;
  size_t n_;       // test points
  Type targetType_;
  size_t tn_;      // target fingerprint length: width * n_ (component-major)
  uint32_t targetCost_;
  std::vector<float> target_;
  std::vector<char> targetFinite_;
  bool rule_ = false;           // accuracy rule (accuracyRule(budget))
  std::vector<double> exact_;   // exact target values on the test points (rule_)
  std::vector<double> fit_;     // values constants are fitted to: exact_ where finite, else target_
  std::vector<double> scale_;   // the target's error scales on the test points (Rel budgets)
  // Hit test at test point i: the budget, the accuracy rule, or the loose budget (less
  // accurate candidates are classified by the driver).
  bool accepts(size_t i, float v) const {
    const double* x = rule_ ? &exact_[i] : nullptr;
    return pointAcceptable(prog_.budget, target_[i], x, v, scale_[i]) ||
           pointLoose(prog_.budget, target_[i], x, v, scale_[i]);
  }
  std::vector<Op> ops_;
  std::vector<Type> types_;  // float types ops are enumerated for: float1, then the target's

  // The bank: 16 bytes per entry (owner's idea: op and type as one codebook byte, flags as
  // bits): w0 = a (28 bits) | b (28) | code (8, op * kNumTypes + type), w1 = c (28) |
  // isConst, affine, ctime, isHit (4 bits) | cost (16) | obj (16). Entry is its unpacked
  // form; entry() and pack() convert.
  struct Packed {
    uint64_t w0, w1;
  };
  static_assert(sizeof(Packed) == 16);
  static constexpr uint32_t kIndexBits = 28;  // bank indices, inputs, constants, components
  Chunked<Packed> bank_;
  static Packed pack(const Entry& e);
  Entry entry(uint32_t idx) const;
  bool isHit(uint32_t idx) const { return (bank_[idx].w1 >> 31) & 1u; }
  void setHit(uint32_t idx) { bank_[idx].w1 |= uint64_t{1} << 31; }
  FpArena fp_;
  // Disk-backed bank (SearchConfig::diskDir): entries from diskFrom_ on have their
  // fingerprint at stream offset diskOff_[idx - diskFrom_] (kNotOnDisk: in fp_ after all,
  // e.g. hits kept after the bank was full); tiles are read into tileBuf_ (tilePtr_[t]).
  static constexpr uint64_t kNotOnDisk = ~uint64_t{0};
  std::unique_ptr<DiskFpStore> disk_;
  bool diskMode_ = false, spilling_ = false;
  uint32_t diskFrom_ = UINT32_MAX;
  size_t diskBudget_ = 0;
  Chunked<uint64_t> diskOff_;
  Chunked<std::array<uint64_t, 2>> hashes_;  // disk mode: both fingerprint hashes per entry
  std::vector<uint64_t> batchH2_;
  std::vector<float*> tilePtr_;
  struct TileSlot {
    std::unique_ptr<float[]> buf;
    uint64_t tile = ~uint64_t{0};
    uint64_t used = 0;
  };
  std::vector<TileSlot> tileSlots_;
  uint64_t tileClock_ = 0;
  // Per level list: [begin, end) ranges of list positions, tile (-1 = fingerprints in RAM;
  // the RAM range comes first and stays sorted by objective).
  struct Seg {
    size_t begin, end;
    int64_t tile;
  };
  std::vector<std::array<std::vector<Seg>, kNumTypes>> segs_;
  uint64_t hash2Fp(const float* fp, Type t) const;
  bool sameAs(uint32_t idx, const float* fp, size_t len, uint64_t h1, uint64_t h2) const;
  void placeFp(uint32_t idx, const float* fp, size_t len, bool mayDisk, uint64_t h1);
  void dropLastFp(uint32_t idx, size_t len);
  bool onDisk(uint32_t idx) const { return idx >= diskFrom_ && diskOff_[idx - diskFrom_] != kNotOnDisk; }
  void finishLevelDisk(uint32_t cost);
  const std::vector<Seg>& listSegs(uint32_t cost, Type t);
  bool hasDisk(uint32_t cost, Type t) { return listSegs(cost, t).size() > 1 || (!listSegs(cost, t).empty() && listSegs(cost, t)[0].tile >= 0); }
  void requireTiles(std::initializer_list<int64_t> tiles, SearchStats& stats);
  // Top-down split: (value at test point tdP0_, index) of the target-type entries, sorted.
  std::vector<std::pair<float, uint32_t>> tdIndex_;
  uint32_t tdP0_ = UINT32_MAX, tdP1_ = UINT32_MAX;
  uint32_t bestHitObj_ = UINT32_MAX;  // objective cost of the cheapest hit so far
  void topDownPass(uint32_t cost, SearchStats& stats);
  size_t maxBank_ = 0;         // entries: from the memory budget, cfg_.maxBank, 32-bit offsets
  size_t budget_ = 0;          // bank memory budget in bytes (bankBudget)
  bool bankFull() const;
  Chunked<uint32_t> off_;  // fingerprint offset of each entry in fp_
  std::vector<std::array<float, 4>> consts_;
  std::vector<Expr> shared_;  // SearchConfig::sharedLeaves: Input entries with aux >= kShared
  std::vector<uint32_t> sharedCost_;  // objective DAG cost of shared_[k]'s current form
  static constexpr uint32_t kShared = 0x08000000u;  // < 2^kIndexBits
  std::vector<std::array<std::vector<uint32_t>, kNumTypes>> byCost_;
  std::vector<uint32_t> table_;
  std::vector<float> scratch_;
  std::vector<uint32_t> hits_;
  std::vector<Entry> altHits_;  // programs whose fingerprint equals an existing hit
  std::vector<AffineHit> affineHits_;
  bool stop_ = false;
  uint64_t sinceCheck_ = 0;
  double start_ = 0.0;
};

double nowSeconds();

}  // namespace sopt
