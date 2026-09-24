#include "search/enumerator.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "ir/eval.hpp"
#include "verify/exact.hpp"
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
  target_ = evalAll(prog.target, tests, kProfileRef);
  targetFinite_.resize(tn_);
  for (size_t i = 0; i < tn_; ++i) targetFinite_[i] = std::isfinite(target_[i]) ? 1 : 0;
  // Accuracy rule: hits may also be as close to the exact value as the target is, and
  // constants are fitted to the exact values.
  rule_ = accuracyRule(prog.budget);
  fit_.assign(target_.begin(), target_.end());
  if (rule_) {
    exact_ = evalExactAll(prog.target, tests);
    for (size_t i = 0; i < tn_; ++i)
      if (std::isfinite(exact_[i])) fit_[i] = exact_[i];
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
  fitScratch_.resize(tn_);
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
        case Budget::Kind::Rel: a = scale * b.eps * std::max(1.0, std::fabs(t)); break;
      }
      if (rule_ && std::isfinite(exact_[i])) a += 2.0 * scale * std::fabs(t - exact_[i]);
      monoTol_[i] = a + 4.0 * std::fabs(t) * 0x1p-23;
    }
  }
  table_.assign(1u << 16, kEmpty);
}

uint64_t Enumerator::hashFp(const float* fp, Type t) const {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(t);
  const size_t len = lenOf(t);
  for (size_t i = 0; i < len; ++i) {
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
  const size_t len = lenOf(e.type);
  size_t pos = hashFp(fp, e.type) & mask;
  while (table_[pos] != kEmpty) {
    const uint32_t idx = table_[pos];
    if (entries_[idx].type == e.type && std::memcmp(fpOf(idx), fp, len * sizeof(float)) == 0) {
      ++stats.deduped;
      // Same fingerprint as a hit: not needed in the bank, but it may differ from the
      // hit outside the test points, so keep it as an alternative for verification.
      if (isHit_[idx] && e.op != Op::Input && e.op != Op::Const && numHits() < cfg_.maxHits) {
        altHits_.push_back(e);
        ++stats.hits;
      }
      return false;
    }
    pos = (pos + 1) & mask;
  }
  const auto idx = static_cast<uint32_t>(entries_.size());
  // Bank full (overflow mode): the entry is only checked as a hit and dropped again,
  // so the search goes on with the stored entries as operands.
  const bool transient = cfg_.overflow && entries_.size() >= cfg_.maxBank;
  const size_t hitsBefore = numHits();
  entries_.push_back(e);
  isHit_.push_back(0);
  off_.push_back(fp_.size());
  fp_.insert(fp_.end(), fp, fp + len);
  table_[pos] = idx;
  if (!transient && entries_.size() * 2 > table_.size()) growTable();
  if (!transient) {
    byCost_[e.cost][static_cast<size_t>(e.type)].push_back(idx);
    if (!stats.levels.empty()) ++stats.levels.back().added;
  }

  // Goal check: does this value match the target within the budget on all test points?
  if (e.type == targetType_ && e.obj < targetCost_) {
    const float* v = fpOf(idx);
    bool ok = true;
    for (size_t i = 0; i < tn_ && ok; ++i)
      ok = !targetFinite_[i] || accepts(i, v[i]);
    if (ok) {
      isHit_[idx] = 1;
      hits_.push_back(idx);
      ++stats.hits;
      if (stats.firstHitSec < 0) stats.firstHitSec = nowSeconds() - start_;
    } else if (cfg_.affine && !e.affine && !e.isConst && !e.ctime && numHits() < cfg_.maxHits) {
      // An affine step's base is in the bank and gets its own (cheaper) fit.
      if (!affineFit(idx, stats) && cfg_.inner) innerFit(idx, stats);
    }
  }
  if (transient) {
    if (numHits() > hitsBefore) {
      ++stats.overflowKept;  // a hit (or fitted hit) refers to it: keep it
    } else {
      // Most recent insertion: removing it cannot break a probe chain.
      table_[pos] = kEmpty;
      fp_.resize(off_.back());
      off_.pop_back();
      isHit_.pop_back();
      entries_.pop_back();
    }
    ++stats.overflowChecked;
  }
  return true;
}

uint32_t Enumerator::addConst(Type t, const float* v, SearchStats& stats) {
  std::array<float, 4> val{};
  for (unsigned k = 0; k < width(t); ++k) val[k] = v[k];
  consts_.push_back(val);
  Entry e{Op::Const, t, 0, true, {0, 0, 0}, static_cast<uint32_t>(consts_.size() - 1), false, 0};
  for (unsigned k = 0; k < width(t); ++k) std::fill(scratch_.begin() + k * n_, scratch_.begin() + (k + 1) * n_, val[k]);
  insert(e, scratch_.data(), stats);
  return static_cast<uint32_t>(entries_.size() - 1);
}

// target ~ p * v + q: least squares over the finite target points, then the budget
// check on the float result of the wrapper. Cheaper wrappers (v + q, v * p, q - v) are
// preferred when they also pass. top is v's op (a mul/div under an add/sub contracts)
// and baseObj its objective cost.
bool Enumerator::fitWrap(const float* v, Op top, uint32_t baseObj, AffineHit& out) const {
  const CostModel& model = *cfg_.model;
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
  // The full fit is the best any wrapper can do (in the least-squares sense).
  const AffineHit full{0, Op::Mad, static_cast<float>(pd), static_cast<float>(mg - pd * mv)};
  if (full.p == 0.0f || !passes(Op::Mad, full.p, full.q)) return false;

  // Cheaper wrappers, each with its own least-squares constant.
  const AffineHit tries[] = {
      {0, Op::Add, 1.0f, static_cast<float>(mg - mv)},
      {0, Op::Mul, static_cast<float>(svg0 / svv0), 0.0f},
      {0, Op::Sub, -1.0f, static_cast<float>(mg + mv)},
      full};
  const unsigned W = width(targetType_);  // the wrapper applies to every component
  auto wrapCost = [&](Op w) -> uint32_t {
    if ((w == Op::Add || w == Op::Sub) && model.fusesIntoAdd(top)) return W * model.fusedAdd;
    return model.opCost(w, W);
  };
  const AffineHit* best = nullptr;
  uint32_t bestCost = targetCost_;
  for (const auto& h : tries) {
    const uint32_t c = baseObj + wrapCost(h.wrap);
    if (c < bestCost && (&h == &tries[3] || passes(h.wrap, h.p, h.q))) {
      best = &h;
      bestCost = c;
    }
  }
  if (!best) return false;
  out.wrap = best->wrap;
  out.p = best->p;
  out.q = best->q;
  return true;
}

bool Enumerator::affineFit(uint32_t idx, SearchStats& stats) {
  const Entry& e = entries_[idx];
  AffineHit h{idx, Op::Mad, 0.0f, 0.0f};
  if (!fitWrap(fpOf(idx), e.op, e.obj, h)) return false;
  affineHits_.push_back(h);
  ++stats.hits;
  ++stats.affineHits;
  if (stats.firstHitSec < 0) stats.firstHitSec = nowSeconds() - start_;
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
int Enumerator::monotoneBreaks(const float* v) {
  monoOrder_.clear();
  for (size_t i = 0; i < tn_; ++i)
    if (targetFinite_[i]) monoOrder_.push_back(static_cast<uint32_t>(i));
  std::sort(monoOrder_.begin(), monoOrder_.end(), [&](uint32_t a, uint32_t b) { return v[a] < v[b]; });
  int up = 0, down = 0;
  for (size_t k = 0; k + 1 < monoOrder_.size(); ++k) {
    const uint32_t a = monoOrder_[k], b = monoOrder_[k + 1];
    const double d = double(target_[b]) - target_[a];
    const double tol = monoTol_[a] + monoTol_[b];
    if (v[a] == v[b]) {
      if (std::fabs(d) > tol) return 2;  // one v, two targets: no function of v fits
    } else if (d > tol) {
      ++up;
    } else if (d < -tol) {
      ++down;
    }
  }
  return std::min(up, down);
}

bool Enumerator::innerFit(uint32_t idx, SearchStats& stats) {
  const Entry& e = entries_[idx];
  const CostModel& model = *cfg_.model;
  const float* v = fpOf(idx);
  // p * u(v + c) + q is monotonic in v (rcp: on each side of its pole, one break), so
  // the target must be too, within the tolerance: cheap to check before fitting.
  const int breaks = cfg_.innerPrefilter ? monotoneBreaks(v) : 0;
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
    if (baseObj + 1 >= targetCost_) continue;
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
    for (size_t i = 0; i < tn_; ++i) fitScratch_[i] = c;
    evalArray(Op::Add, v, fitScratch_.data(), nullptr, fitScratch_.data(), tn_, kProfileRef);
    evalArray(u, fitScratch_.data(), nullptr, nullptr, fitScratch_.data(), tn_, kProfileRef);
    AffineHit h{idx, Op::Mad, 0.0f, 0.0f, u, c};
    if (!fitWrap(fitScratch_.data(), u, baseObj, h)) continue;
    affineHits_.push_back(h);
    ++stats.hits;
    ++stats.innerHits;
    if (stats.firstHitSec < 0) stats.firstHitSec = nowSeconds() - start_;
    found = true;
  }
  return found;
}

void Enumerator::checkLimits(SearchStats& stats) {
  if ((entries_.size() >= cfg_.maxBank && !cfg_.overflow) || numHits() >= cfg_.maxHits ||
      nowSeconds() - start_ > cfg_.timeLimitSec) {
    stop_ = true;
    stats.limitHit = true;
  }
}

void Enumerator::tryAdd(Op op, uint16_t cost, uint32_t a, uint32_t b, uint32_t c,
                        SearchStats& stats, uint32_t aux) {
  ++stats.generated;
  ++stats.levels.back().generated;
  if (++sinceCheck_ >= 4096) {
    sinceCheck_ = 0;
    checkLimits(stats);
  }
  const auto& oi = info(op);
  const uint32_t args[3] = {a, b, c};
  bool allConst = true;
  for (uint8_t k = 0; k < oi.arity; ++k) allConst = allConst && entries_[args[k]].isConst;
  if (allConst) {
    ++stats.constSkipped;
    return;
  }
  // Result type: componentwise ops take the widest operand (float1 operands broadcast).
  Type type = Type::Float;
  if (oi.shape == Shape::Cmp) {
    type = Type::Bool;
  } else if (oi.shape == Shape::Comp || oi.shape == Shape::Select) {
    unsigned w = 1;
    for (uint8_t k = oi.shape == Shape::Select ? 1 : 0; k < oi.arity; ++k)
      w = std::max<unsigned>(w, width(entries_[args[k]].type));
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
      if (!entries_[args[k]].isConst) {
        ++nonConst;
        base = args[k];
      }
    const bool step = nonConst == 1 &&
                      (op == Op::Neg || op == Op::Add || op == Op::Sub || op == Op::Mul ||
                       op == Op::Mad || op == Op::Lerp || (op == Op::Div && entries_[b].isConst));
    // A pure sign flip (-v, 0 - v, v * -1) is absorbed by the outer map and by the
    // consumer (sub for add, max for min, ...), so it is not stored either.
    auto isConst = [&](uint32_t i, float val) {
      return entries_[i].isConst && entries_[i].type == Type::Float && constValue(i) == val;
    };
    const bool flip = op == Op::Neg || (op == Op::Sub && isConst(a, 0.0f)) ||
                      (op == Op::Mul && (isConst(a, -1.0f) || isConst(b, -1.0f))) ||
                      (op == Op::Div && isConst(b, -1.0f));
    // c / v is a scaled 1 / v: keep only the reciprocal itself.
    if (op == Op::Div && nonConst == 1 && entries_[a].isConst && !isConst(a, 1.0f)) {
      ++stats.affinePruned;
      return;
    }
    if (step) {
      if (oi.arity == 3 || entries_[base].affine || flip) {
        ++stats.affinePruned;
        return;
      }
      affine = true;
    }
  }
  // Objective cost; an entry that already costs as much as the target can be neither a
  // hit nor part of one. A same-width mul/div under an add/sub contracts to fma.
  const CostModel& model = *cfg_.model;
  // Only constants and compile-time inputs: folded by the compiler, free.
  bool ctime = true;
  for (uint8_t k = 0; k < oi.arity; ++k) ctime = ctime && (entries_[args[k]].isConst || entries_[args[k]].ctime);
  uint32_t obj = 0;
  for (uint8_t k = 0; k < oi.arity; ++k) obj += entries_[args[k]].obj;
  auto fuses = [&](uint32_t x) {
    return !entries_[x].ctime && model.fusesIntoAdd(entries_[x].op) && entries_[x].type == type;
  };
  if (ctime)
    obj = 0;
  else if (model.fusedAdd && (op == Op::Add || op == Op::Sub) && (fuses(a) || fuses(b)))
    obj += w * model.fusedAdd;
  else
    obj += model.opCost(op, w);
  if (obj >= targetCost_) {
    ++stats.objPruned;
    return;
  }
  float* out = scratch_.data();
  if (op == Op::Swizzle) {
    std::copy(fpOf(a) + aux * n_, fpOf(a) + (aux + 1) * n_, out);
  } else {
    for (unsigned comp = 0; comp < w; ++comp) {
      const float* p[3] = {nullptr, nullptr, nullptr};
      for (uint8_t k = 0; k < oi.arity; ++k)
        p[k] = fpOf(args[k]) + (width(entries_[args[k]].type) == 1 ? 0 : comp * n_);
      evalArray(op, p[0], p[1], p[2], out + comp * n_, n_, kProfileRef);
    }
  }
  canonicalize(out, w * n_);
  Entry e{op, type, cost, false, {a, b, c}, aux, affine, static_cast<uint16_t>(obj), ctime};
  insert(e, out, stats);
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
    Entry e{Op::Input, t, 0, false, {0, 0, 0}, i, false, 0, ct};
    if (!insert(e, scratch_.data(), stats) || width(t) == 1) continue;
    const auto vec = static_cast<uint32_t>(entries_.size() - 1);
    const auto swzCost = static_cast<uint16_t>(ct ? 0 : model.opCost(Op::Swizzle, 1));
    for (unsigned k = 0; k < width(t); ++k) {
      Entry s{Op::Swizzle, Type::Float, 0, false, {vec, 0, 0}, k, false, swzCost, ct};
      insert(s, fpOf(vec) + k * n_, stats);
    }
  }
  const ConstPool pool = constantPool(prog_.target);
  for (float cv : pool.scalars) addConst(Type::Float, &cv, stats);
  for (const auto& [t, v] : pool.vectors) addConst(t, v.data(), stats);
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
          for (size_t i = 0; i < la.size() && !stop_ && obj(la[i]) + objOp < targetCost_; ++i)
            tryAdd(op, c16, la[i], 0, 0, stats);
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
    for (auto& list : byCost_[cost])
      std::stable_sort(list.begin(), list.end(),
                       [&](uint32_t x, uint32_t y) { return obj(x) < obj(y); });
    if (!stop_ && stats.overflowChecked == 0) stats.completedCost = cost;
  }

  stats.bankSize = entries_.size();
  stats.seconds = nowSeconds() - start_;
  out.reserve(numHits());
  auto emit = [&](const Entry& e) {
    Candidate cand;
    cand.expr = extract(e);
    cand.cost = dagCost(cand.expr, *cfg_.model, prog_.inputs);
    out.push_back(std::move(cand));
  };
  for (uint32_t idx : hits_) emit(entries_[idx]);
  for (const Entry& e : altHits_) emit(e);
  for (const AffineHit& h : affineHits_) {
    Candidate cand;
    cand.expr = extract(h);
    cand.cost = dagCost(cand.expr, *cfg_.model, prog_.inputs);
    out.push_back(std::move(cand));
  }
  return out;
}

// Binary op at this level with operand costs summing to r and operand types ta, tb.
// fuse: -1 = all pairs, 1 = only pairs with a same-width mul (or div) operand, 0 = only
// pairs without one.
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
  auto fusable = [&](uint32_t x) { return model.fusesIntoAdd(entries_[x].op) && entries_[x].type == rt; };
  for (uint32_t c1 = 0; c1 <= r && !stop_; ++c1) {
    const uint32_t c2 = r - c1;
    if (sym && c1 > c2) continue;
    const auto& la = byCost_[c1][static_cast<size_t>(ta)];
    const auto& lb = byCost_[c2][static_cast<size_t>(tb)];
    if (lb.empty()) continue;
    for (size_t i = 0; i < la.size() && !stop_; ++i) {
      if (obj(la[i]) + obj(lb[0]) + minOp >= targetCost_) break;
      const bool fa = fusable(la[i]);
      size_t j0 = (sym && c1 == c2) ? i : 0;
      for (size_t j = j0; j < lb.size() && !stop_; ++j) {
        if (obj(la[i]) + obj(lb[j]) + minOp >= targetCost_) break;
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
      const uint32_t minC = obj(lc[0]);
      for (size_t i = 0; i < la.size() && !stop_; ++i) {
        if (obj(la[i]) + obj(lb[0]) + minC + opc >= targetCost_) break;
        size_t j0 = (sym01 && c1 == c2) ? i : 0;
        for (size_t j = j0; j < lb.size() && !stop_; ++j) {
          const uint32_t ab = obj(la[i]) + obj(lb[j]) + opc;
          if (ab + minC >= targetCost_) break;
          for (size_t k = 0; k < lc.size() && !stop_ && ab + obj(lc[k]) < targetCost_; ++k) {
            if ((op == Op::Select || op == Op::Lerp) && lb[j] == lc[k]) continue;
            tryAdd(op, level, la[i], lb[j], lc[k], stats);
          }
        }
      }
    }
  }
}

uint32_t Enumerator::build(ExprBuilder& b, const Entry& rootEntry) const {
  std::unordered_map<uint32_t, uint32_t> memo;
  auto rec = [&](auto&& self, const Entry& e) -> uint32_t {
    if (e.op == Op::Input) return b.input(e.aux, e.type);
    if (e.op == Op::Const) return b.constant(e.type, consts_[e.aux].data());
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
    if (e.op == Op::Swizzle) {
      const auto comp = static_cast<uint8_t>(e.aux);
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
  uint32_t v = build(b, entries_[h.idx]);
  if (h.inner != Op::Count) {
    v = h.c < 0.0f ? b.op(Op::Sub, v, b.constant(-h.c)) : b.op(Op::Add, v, b.constant(h.c));
    v = b.op(h.inner, v);
  }
  uint32_t r;
  switch (h.wrap) {
    case Op::Add: r = b.op(Op::Add, v, b.constant(h.q)); break;
    case Op::Mul: r = b.op(Op::Mul, v, b.constant(h.p)); break;
    case Op::Sub: r = b.op(Op::Sub, b.constant(h.q), v); break;
    default: r = b.op(Op::Mad, v, b.constant(h.p), b.constant(h.q)); break;
  }
  return b.finish(r);
}

}  // namespace sopt
