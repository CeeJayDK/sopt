# Making the superoptimizer faster

Owner (2026-09-24): all worth exploring; order A1-A3, A4, then B6/B7; B8 is the
"moving pivot" idea from the design phase; also use less RAM per bank entry.

Status: A1 (prefilter), A2 (verification stops at the first failure; inline budget
check; sopt-fx verifies 20 alternatives instead of 50), A3 (distinct regions searched
once) done. Overflow is the default since the owner's go (`--no-overflow` to disable).
A4 (multithreaded enumeration) done: batches of 16384 candidates; evaluation, lookup
among earlier entries, goal checks and fits run in parallel; dedup/store and hit
recording stay serial and in order, so results match the single-threaded search.
sqrt_product: 5.7 s -> 2.7 s on 4 cores; the serial store (random hash-table access)
is now the limit.

Bench after A1-A3 (rdna3, 14 examples + 12 planted, same machine): same problems found,
same time to first hit. Search instructions halved on sqrt_product, wall time ~15%
lower (the rest is memory bound). Bench verification did not get faster: it is
dominated by accepted candidates (up to 50, each on 1M points x 4 profiles, plus V2),
not by rejected ones. sopt-fx now verifies 20.

`--overflow` on the same bench: 2 more problems solved and none lost: length_squared
(first hit 6.4 s; never before) and planted_1 (a needs-sharing one, 5.7 s). Cost: the
search always runs to the time limit instead of stopping when the bank is full.

Measured 2026-09-24 in the cloud sandbox (4 cores), `examples/sqrt_product.sopt`
(rdna3) and a full sopt-fx run over 53 effects.

## 2026-09-28: profile after M7, and what was done

sopt-fx over SweetFX (239 searched regions, --time 3): enumeration 568 s, subtree
searches 197 s, cut searches 56 s, verification 30 s, other 26 s. Enumeration runs to the
time limit, so faster means deeper, not shorter. Inside it (callgrind, bank-bounded
normalize_x): candidate evaluation ~22%, fingerprint hash 13.5% (a serial multiply chain
per float), inner-fit monotonicity prefilter 23% (std::sort through an index
indirection), affine fits 21%, canonicalize 8%. Done, identical results on 8 examples
with a fixed bank: two-lane 64-bit hash; integer (key, index) sort with early exit; cost
checks before the monotonicity test and the affine sums; the full affine fit only when it
can win; canonicalize branch-free: -14% instructions on normalize_x, -8% on rational.

Memory per bank entry (scalar target, 32 test points): Entry 32 B, offset 8 B, hit flag
1 B, level list 4 B, hash table 8 B, fingerprint 128 B (float3: 384 B) = ~181 B. Done:
Entry packed to 20 B, offsets 32-bit, and the main bank freed before the subtree / cut
searches (they had their own full banks on top): normalize_x at a full 2M bank 414 ->
383 MB alone, 619 -> 383 MB with the default part searches. The fingerprint (70-80%)
must stay exact (quantized OE lost variants); open: fewer test points (24 instead of 32,
measure), recomputing the top levels' fingerprints (flag), and a disk-backed bank for
long single-region runs (option, not default; owner 2026-09-28).

## 2026-10-06: where candidates fail, operand unpacking, affine pre-check

Owner: early outs to get through the haystack. Measured (instrumented build): of the affine fits that fail, ~99.5%
fail at the first test point, so the point order does not matter; the cost is the fit itself (sums over all points
before the check). Callgrind (rational, bank 150k, one thread): prepare 39% inclusive with 17% in entry() (bank
entries unpacked 6-8 times per candidate), goalCheck 20% with fitWrap 15%, innerFit 2%. Done, identical results
(generated / deduped / bank / hits equal on 7 examples with a fixed bank): prepare() unpacks each operand once, dedup
reads only the type byte (typeOf); affine pre-check (SearchConfig::affinePrecheck, default, `--no-affine-precheck`):
three test points with a finite interval of accepted values (acceptHull: budget, accuracy rule, loose budget; color
budgets open-ended at the code edges), and every wrapper is some p * v + q, so if the pairwise slope intervals (with
a generous rounding allowance, u = 1e-6) do not overlap, no wrapper can pass and the sums are skipped. Instructions
(callgrind, bank 100k): rational -4%, sqrt_product -10.5%; prepare -37%; wall time 3-17% less search time.

## Where the time went (2026-09-24)

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
   Done (`SearchConfig::topDown`): sorted index at one test point, tolerance lookup,
   second-point filter, goal check; default since 2026-09-29 (owner), `--no-top-down`.
   Corpus 74 -> 79 regions with variants, +10% time.
7. **Shared leaves (M7, planned).** Fixes the `needs-sharing` misses in the bench.
8. **Stochastic search for large regions** (STOKE-like: random rewrites of the
   original, accepting cheaper verified ones). Complements enumeration above cost ~16,
   where the bank cannot reach.
9. **Owner's idea: equivalence classes per input domain** (only one representative
   per class enumerated). Related to 6: both avoid building values the domain cannot
   tell apart.

Suggested order: A1-A3 (small, safe, measurable), then A4, then B6 or B7.
