#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "ir/expr.hpp"
#include "verify/points.hpp"
#include "verify/verify.hpp"

namespace sopt {

struct SearchConfig {
  uint32_t maxCost = 0;         // inclusive, in order-model units; 0 = derived from the target
  size_t maxBank = 2'000'000;   // entries (memory: ~(4*tests + 32) bytes each)
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

// Bottom-up enumeration by increasing cost with observational-equivalence dedup:
// one bank entry per distinct fingerprint (output on the test points).
class Enumerator {
 public:
  Enumerator(const Program& prog, const PointSet& tests, const SearchConfig& cfg);
  std::vector<Candidate> run(SearchStats& stats);

 private:
  struct Entry {
    Op op;
    Type type;
    uint16_t cost;        // order-model level
    bool isConst;
    uint32_t args[3];
    uint32_t aux;         // Input: input index; Const: index into consts_; Swizzle: component
    bool affine = false;  // single affine step (v + c, v * c, -v, ...) of a non-constant entry
    uint16_t obj = 0;     // objective (model) tree cost
    // Computed only from constants and compile-time inputs (not constants themselves):
    // the compiler folds it, so its objective cost is 0.
    bool ctime = false;
  };
  // Hit through a solved outer affine map: wrap(x) with op Add (x + q), Mul (x * p),
  // Sub (q - x) or Mad (mad(x, p, q)), where x = entries_[idx], or inner(entries_[idx] + c)
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
  int monotoneBreaks(const float* v, std::vector<uint32_t>& order) const;
  std::vector<double> monoTol_;
  FitScratch serialScratch_;
  std::vector<FitScratch> threadScratch_;
  std::vector<AffineHit> fitOut_;
  bool fitWrap(const float* v, Op top, uint32_t baseObj, AffineHit& out) const;
  uint32_t obj(uint32_t idx) const { return entries_[idx].obj; }
  const CostModel& order() const { return cfg_.order ? *cfg_.order : defaultOrderFor(*cfg_.model); }
  size_t numHits() const { return hits_.size() + altHits_.size() + affineHits_.size(); }
  uint64_t hashFp(const float* fp, Type t) const;
  // Fingerprint equality for dedup (quantized with SearchConfig::quantBits).
  bool sameFp(const float* a, const float* b, size_t len) const;
  uint32_t quant(float x) const;
  void growTable();
  const float* fpOf(uint32_t idx) const { return fp_.data() + off_[idx]; }
  size_t lenOf(Type t) const { return width(t) * n_; }
  float constValue(uint32_t idx) const { return consts_[entries_[idx].aux][0]; }
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

  std::vector<Entry> entries_;
  std::vector<float> fp_;
  std::vector<uint64_t> off_;  // fingerprint offset of each entry in fp_
  std::vector<std::array<float, 4>> consts_;
  std::vector<Expr> shared_;  // SearchConfig::sharedLeaves: Input entries with aux >= kShared
  std::vector<uint32_t> sharedCost_;  // objective DAG cost of shared_[k]'s current form
  static constexpr uint32_t kShared = 0x40000000u;
  std::vector<std::array<std::vector<uint32_t>, kNumTypes>> byCost_;
  std::vector<uint32_t> table_;
  std::vector<float> scratch_;
  std::vector<uint32_t> hits_;
  std::vector<char> isHit_;
  std::vector<Entry> altHits_;  // programs whose fingerprint equals an existing hit
  std::vector<AffineHit> affineHits_;
  bool stop_ = false;
  uint64_t sinceCheck_ = 0;
  double start_ = 0.0;
};

double nowSeconds();

}  // namespace sopt
