#include "search/enumerator.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "ir/eval.hpp"
#include "verify/verify.hpp"

namespace sopt {

double nowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

namespace {

constexpr uint32_t kEmpty = UINT32_MAX;

void canonicalize(float* v, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (std::isnan(v[i])) v[i] = std::bit_cast<float>(uint32_t{0x7fc00000});
    else if (v[i] == 0.0f) v[i] = 0.0f;  // treat -0 as +0 (sign of zero is don't-care)
  }
}

std::vector<float> constantPool(const Expr& target) {
  std::vector<float> pool = {0.0f, 0.5f, 1.0f, 2.0f, -1.0f};
  for (const auto& n : target.nodes) {
    if (n.op != Op::Const) continue;
    const float c = n.value;
    pool.push_back(c);
    pool.push_back(-c);
    pool.push_back(c * c);
    if (c != 0.0f) pool.push_back(1.0f / c);
  }
  std::vector<float> out;
  for (float v : pool) {
    if (!std::isfinite(v)) continue;
    if (v == 0.0f) v = 0.0f;
    bool dup = false;
    for (float o : out) dup = dup || std::bit_cast<uint32_t>(o) == std::bit_cast<uint32_t>(v);
    if (!dup) out.push_back(v);
  }
  return out;
}

}  // namespace

Enumerator::Enumerator(const Program& prog, const PointSet& tests, const SearchConfig& cfg)
    : prog_(prog), tests_(tests), cfg_(cfg), n_(tests.size()) {
  targetCost_ = dagCost(prog.target, *cfg.model);
  target_ = evalAll(prog.target, tests, kProfileRef);
  targetFinite_.resize(n_);
  for (size_t i = 0; i < n_; ++i) targetFinite_[i] = std::isfinite(target_[i]) ? 1 : 0;

  for (size_t i = 0; i < static_cast<size_t>(Op::Count); ++i) {
    const Op op = static_cast<Op>(i);
    if (op == Op::Input || op == Op::Const) continue;
    if (info(op).base || containsOp(prog.target, op)) ops_.push_back(op);
  }
  scratch_.resize(n_);
  table_.assign(1u << 16, kEmpty);
}

uint64_t Enumerator::hashFp(const float* fp, Type t) const {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(t);
  for (size_t i = 0; i < n_; ++i) {
    h ^= std::bit_cast<uint32_t>(fp[i]);
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 32;
  }
  return h;
}

void Enumerator::growTable() {
  std::vector<uint32_t> old;
  old.swap(table_);
  table_.assign(old.size() * 2, kEmpty);
  const size_t mask = table_.size() - 1;
  for (uint32_t idx : old) {
    if (idx == kEmpty) continue;
    size_t pos = hashFp(fpOf(idx), entries_[idx].type) & mask;
    while (table_[pos] != kEmpty) pos = (pos + 1) & mask;
    table_[pos] = idx;
  }
}

bool Enumerator::insert(const Entry& e, const float* fp, SearchStats& stats) {
  const size_t mask = table_.size() - 1;
  size_t pos = hashFp(fp, e.type) & mask;
  while (table_[pos] != kEmpty) {
    const uint32_t idx = table_[pos];
    if (entries_[idx].type == e.type && std::memcmp(fpOf(idx), fp, n_ * sizeof(float)) == 0) {
      ++stats.deduped;
      // Same fingerprint as a hit: not needed in the bank, but it may differ from the
      // hit outside the test points, so keep it as an alternative for verification.
      if (isHit_[idx] && e.op != Op::Input && e.op != Op::Const &&
          hits_.size() + altHits_.size() < cfg_.maxHits) {
        altHits_.push_back(e);
        ++stats.hits;
      }
      return false;
    }
    pos = (pos + 1) & mask;
  }
  const auto idx = static_cast<uint32_t>(entries_.size());
  entries_.push_back(e);
  isHit_.push_back(0);
  fp_.insert(fp_.end(), fp, fp + n_);
  table_[pos] = idx;
  if (entries_.size() * 2 > table_.size()) growTable();
  byCost_[e.cost][static_cast<size_t>(e.type)].push_back(idx);
  if (!stats.levels.empty()) ++stats.levels.back().added;

  // Goal check: does this value match the target within the budget on all test points?
  if (e.type == Type::Float && e.cost < targetCost_) {
    const float* v = fpOf(idx);
    bool ok = true;
    for (size_t i = 0; i < n_ && ok; ++i)
      ok = !targetFinite_[i] || pointWithinBudget(prog_.budget, target_[i], v[i]);
    if (ok) {
      isHit_[idx] = 1;
      hits_.push_back(idx);
      ++stats.hits;
      if (stats.firstHitSec < 0) stats.firstHitSec = nowSeconds() - start_;
    }
  }
  return true;
}

void Enumerator::checkLimits(SearchStats& stats) {
  if (entries_.size() >= cfg_.maxBank || hits_.size() + altHits_.size() >= cfg_.maxHits ||
      nowSeconds() - start_ > cfg_.timeLimitSec) {
    stop_ = true;
    stats.limitHit = true;
  }
}

void Enumerator::tryAdd(Op op, uint16_t cost, uint32_t a, uint32_t b, uint32_t c,
                        SearchStats& stats) {
  ++stats.generated;
  ++stats.levels.back().generated;
  if (++sinceCheck_ >= 4096) {
    sinceCheck_ = 0;
    checkLimits(stats);
  }
  const auto& oi = info(op);
  bool allConst = entries_[a].isConst;
  if (oi.arity > 1) allConst = allConst && entries_[b].isConst;
  if (oi.arity > 2) allConst = allConst && entries_[c].isConst;
  if (allConst) {
    ++stats.constSkipped;
    return;
  }
  evalArray(op, fpOf(a), oi.arity > 1 ? fpOf(b) : nullptr, oi.arity > 2 ? fpOf(c) : nullptr,
            scratch_.data(), n_, kProfileRef);
  canonicalize(scratch_.data(), n_);
  Entry e{op, oi.result, cost, false, {a, b, c}, 0.0f, 0};
  insert(e, scratch_.data(), stats);
}

std::vector<Candidate> Enumerator::run(SearchStats& stats) {
  start_ = nowSeconds();
  stats = SearchStats{};
  std::vector<Candidate> out;
  if (targetCost_ == 0) return out;
  const uint32_t maxCost = std::min(cfg_.maxCost, targetCost_ - 1);
  byCost_.assign(maxCost + 1, {});

  // Level 0: inputs and constants.
  stats.levels.push_back({0, 0, 0});
  for (uint32_t i = 0; i < prog_.inputs.size(); ++i) {
    Entry e{Op::Input, Type::Float, 0, false, {0, 0, 0}, 0.0f, i};
    std::vector<float> v(tests_.cols[i]);
    canonicalize(v.data(), n_);
    insert(e, v.data(), stats);
  }
  for (float cv : constantPool(prog_.target)) {
    Entry e{Op::Const, Type::Float, 0, true, {0, 0, 0}, cv, 0};
    std::vector<float> v(n_, cv);
    insert(e, v.data(), stats);
  }
  stats.completedCost = 0;

  const CostModel& model = *cfg_.model;
  for (uint32_t cost = 1; cost <= maxCost && !stop_; ++cost) {
    stats.levels.push_back({cost, 0, 0});
    const auto c16 = static_cast<uint16_t>(cost);
    for (Op op : ops_) {
      if (stop_) break;
      const auto& oi = info(op);
      if (model[op] > cost) continue;
      const uint32_t r = cost - model[op];
      const auto t0 = static_cast<size_t>(oi.args[0]);
      const auto t1 = static_cast<size_t>(oi.args[1]);
      const auto t2 = static_cast<size_t>(oi.args[2]);

      if (oi.arity == 1) {
        const auto& la = byCost_[r][t0];
        for (size_t i = 0; i < la.size() && !stop_; ++i) tryAdd(op, c16, la[i], 0, 0, stats);
      } else if (oi.arity == 2) {
        if (model.fusedAdd && (op == Op::Add || op == Op::Sub)) {
          // Contraction: an add/sub over a mul costs fusedAdd; the other pairs cost the
          // full add (fusable pairs were already generated at the cheaper level).
          if (model.fusedAdd <= cost) enumerateBinary(op, c16, cost - model.fusedAdd, 1, stats);
          enumerateBinary(op, c16, r, 0, stats);
        } else {
          enumerateBinary(op, c16, r, -1, stats);
        }
      } else {
        const bool sym01 = op == Op::Mad;  // mad(a, b, c) == mad(b, a, c)
        for (uint32_t c1 = 0; c1 <= r && !stop_; ++c1) {
          for (uint32_t c2 = 0; c1 + c2 <= r && !stop_; ++c2) {
            const uint32_t c3 = r - c1 - c2;
            if (sym01 && c1 > c2) continue;
            const auto& la = byCost_[c1][t0];
            const auto& lb = byCost_[c2][t1];
            const auto& lc = byCost_[c3][t2];
            for (size_t i = 0; i < la.size() && !stop_; ++i) {
              size_t j0 = (sym01 && c1 == c2) ? i : 0;
              for (size_t j = j0; j < lb.size() && !stop_; ++j) {
                for (size_t k = 0; k < lc.size() && !stop_; ++k) {
                  if ((op == Op::Select || op == Op::Lerp) && lb[j] == lc[k]) continue;
                  tryAdd(op, c16, la[i], lb[j], lc[k], stats);
                }
              }
            }
          }
        }
      }
    }
    if (!stop_) stats.completedCost = cost;
  }

  stats.bankSize = entries_.size();
  stats.seconds = nowSeconds() - start_;
  out.reserve(hits_.size() + altHits_.size());
  auto emit = [&](const Entry& e) {
    Candidate cand;
    cand.expr = extract(e);
    cand.cost = dagCost(cand.expr, *cfg_.model);
    out.push_back(std::move(cand));
  };
  for (uint32_t idx : hits_) emit(entries_[idx]);
  for (const Entry& e : altHits_) emit(e);
  return out;
}

// Binary op at this level with operand costs summing to r. fuse: -1 = all pairs,
// 1 = only pairs with a mul (or div) operand, 0 = only pairs without one.
void Enumerator::enumerateBinary(Op op, uint16_t level, uint32_t r, int fuse, SearchStats& stats) {
  const auto& oi = info(op);
  const CostModel& model = *cfg_.model;
  const auto t0 = static_cast<size_t>(oi.args[0]);
  const auto t1 = static_cast<size_t>(oi.args[1]);
  for (uint32_t c1 = 0; c1 <= r && !stop_; ++c1) {
    const uint32_t c2 = r - c1;
    if (oi.commutative && c1 > c2) continue;
    const auto& la = byCost_[c1][t0];
    const auto& lb = byCost_[c2][t1];
    for (size_t i = 0; i < la.size() && !stop_; ++i) {
      const bool fa = model.fusesIntoAdd(entries_[la[i]].op);
      size_t j0 = (oi.commutative && c1 == c2) ? i : 0;
      for (size_t j = j0; j < lb.size() && !stop_; ++j) {
        if (fuse >= 0 && (fa || model.fusesIntoAdd(entries_[lb[j]].op)) != (fuse == 1)) continue;
        tryAdd(op, level, la[i], lb[j], 0, stats);
      }
    }
  }
}

Expr Enumerator::extract(const Entry& rootEntry) const {
  ExprBuilder b;
  std::unordered_map<uint32_t, uint32_t> memo;
  auto build = [&](auto&& self, const Entry& e) -> uint32_t {
    if (e.op == Op::Input) return b.input(e.input);
    if (e.op == Op::Const) return b.constant(e.value);
    const auto& oi = info(e.op);
    uint32_t a[3] = {0, 0, 0};
    for (uint8_t k = 0; k < oi.arity; ++k) {
      const uint32_t idx = e.args[k];
      if (auto it = memo.find(idx); it != memo.end()) {
        a[k] = it->second;
      } else {
        a[k] = self(self, entries_[idx]);
        memo[idx] = a[k];
      }
    }
    return b.op(e.op, a[0], a[1], a[2]);
  };
  return b.finish(build(build, rootEntry));
}

}  // namespace sopt
