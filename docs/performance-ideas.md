# Making the superoptimizer faster

Owner (2026-09-24): all worth exploring; order A1-A3, A4, then B6/B7; B8 is the
"moving pivot" idea from the design phase; also use less RAM per bank entry.

Status: A1 (prefilter), A2 (verification stops at the first failure; inline budget
check; sopt-fx verifies 20 alternatives instead of 50), A3 (distinct regions searched
once) done. New, behind a flag: `--overflow` (see A5).

Measured 2026-09-24 in the cloud sandbox (4 cores), `examples/sqrt_product.sopt`
(rdna3) and a full sopt-fx run over 53 effects.

## Where the time goes today

Search (one thread):
- 1.6 M candidates/s generated; 64% are duplicates of values already in the bank.
- The bank limit (2M entries, about 320 MB) is reached at cost level 16. Most searches
  stop there ("limit hit"), not at the time limit.
- Profile (callgrind, verification made small): `innerFit` 58%, insert/tryAdd 22%,
  `fitWrap` 5%, evaluation 3%.

Verification (multithreaded):
- V1 compares each candidate on 1M points under 4 profiles. With a small bank this is
  70% of all work, most of it in the per-point loop (hash, 8-bit code metrics, budget
  check), not in evaluating the expressions.

sopt-fx over the corpus: 731 regions, but only 585 distinct once input names are
ignored (same ranges and budget), e.g. Daltonize's 9 statements, FilmicPass 87-90.

## Proposals, best value first

### A. Faster without changing results (engineering; bench must show same results)

1. **Prefilter for inner fitting.** `u(v + c)` is monotonic in v for rcp/sqrt/rsqrt
   on each side of the pole, so the target must be monotonic in v over the test points
   (within the budget). Sorting 32 values is far cheaper than three least-squares fits
   plus Gauss-Newton. Expected: search up to ~2x faster.
2. **Leaner verification loop.** Stop at the first failing point when only pass/fail
   is needed (stage 2, rejected V1 candidates); compute 8-bit code metrics only for
   color budgets; inline the budget check; check 64k points before the full 1M.
   Expected: verification 2-4x faster.
3. **Search each distinct region once.** Cache results by the region's expression with
   inputs renamed, plus ranges and budget; map variants back to the real names.
   About 20% fewer searches on the corpus.
4. **Multithreaded enumeration.** Split each cost level's operand pairs across
   threads, then merge and deduplicate. About 3x on 4 cores, more on a desktop CPU.
5. **Bigger bank by default.** 2M entries is about 320 MB. Size the limit from free
   memory (e.g. 10-20M entries on a 16-32 GB machine) to reach 1-2 levels deeper.
   Less RAM per entry: the 32 fingerprint floats (128 of ~180 bytes) must stay exact;
   the rest (entry 32 bytes, 8-byte offset, hash slots) can shrink by ~20 bytes.
   `--overflow` (flag): when the bank is full, keep enumerating with the stored entries
   as operands and only check new values as hits (not stored) until the time limit:
   one more level of reach for the top operation at no memory cost.

### B. Search changes (each behind a flag, bench decides; design section 6)

6. **Top-down split (meet in the middle).** For a target t and a bank value a, the
   missing operand is b = t - a (or t / a, a - t, ...). Look b up in the bank instead
   of combining all pairs: one pass over the bank instead of all pairs, so roughly
   double the reachable depth for the top operation. Needs a tolerant lookup
   (quantized fingerprints), because b is computed in float.
7. **Shared leaves (M7, planned).** Fixes the `needs-sharing` misses in the bench.
8. **Stochastic search for large regions** (STOKE-like: random rewrites of the
   original, accepting cheaper verified ones). Complements enumeration above cost ~16,
   where the bank cannot reach.
9. **Owner's idea: equivalence classes per input domain** (only one representative
   per class enumerated). Related to 6: both avoid building values the domain cannot
   tell apart.

Suggested order: A1-A3 (small, safe, measurable), then A4, then B6 or B7.
