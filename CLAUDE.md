# CLAUDE.md

Shader superoptimizer for ReShade FX shaders. Finds cheaper, verified alternatives
to small pure arithmetic regions and presents them as user-selectable variants.
Full design and milestones: `docs/design.md` (Danish). Status: M0 + M1 done (CI green on
MSVC/GCC/Clang, golden hashes match), plus RDNA3 cost model, `gpu` semantic profile, ISA
ranking via fxstat + RGA, solved outer and inner constants (affine + inner, default),
a separate enumeration order model (`--order-model`; rdna3 defaults to `rdna3-search`),
and no pure helper intrinsics (lerp, step) during search (default). Default cost model:
rdna3.

## Working with the owner
- Christian (CeeJay, SweetFX/ReShade). Communicates in Danish; prefers brief, direct answers.
- Do not implement your own improvisations or design changes without asking first.
  Implementing the agreed milestone plan is fine; flag anything beyond it.

## Commands
- Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`
- Tests: `ctest --test-dir build --output-on-failure` (or `build/sopt-tests [filter]`)
- CLI: `build/sopt examples/screen.sopt --stats`
- Bench: `build/sopt-bench --examples examples [--cost-model rdna3]` and
  `build/sopt-bench --planted 12 --size 3 --inputs 3 --time 30`
- ISA ranking: `SOPT_FXSTAT=... SOPT_RGA=... build/sopt examples/factor.sopt --isa`.
  Tools: ReShade-Testing-Initiative (`build_reshade_testing_initiative.sh`, needs
  spirv-tools, flex, bison) and RGA 2.14 (`rga-linux-2.14.tgz` from GitHub releases).

## Layout
- `src/ir`: op table (`ops.cpp`: exactness, base set, cost models generic/rdna3),
  float32 evaluator,
  hash-consed Expr DAG, `.sopt` parser, printer.
- `src/verify`: test/sample point generation, block evaluation, metrics, budget checks.
- `src/search`: `enumerator` (bottom-up by cost, observational equivalence on
  fingerprints; affine (default, `--no-affine`): outer p * v + q solved by least squares
  at the goal check, affine chains / two-constant mad, lerp / sign flips / c / v not
  stored; `--order-model`: levels by one cost model, hits/ranking by the objective,
  entries with objective cost >= target not stored, level lists sorted by objective;
  inner (default, `--no-inner`): target ~ p * u(v + c) + q for u = rcp/sqrt/rsqrt, c
  from a linear reparametrization + Gauss-Newton, then the affine fit; pure helpers
  lerp/step not enumerated unless `--helpers`, see `isPureHelper` in ops.hpp) and `driver` (CEGIS loop, stage-2 filter, V1 verification, grouping).
- `src/measure`: emits a candidate as a ReShade FX effect, runs fxstat + RGA, parses
  the pixel shader's ISA cost.
- `bench/bench.cpp`: example suite + planted problems. `examples/*.sopt` with `# expect:`.

## Invariants (do not break)
- CPU evaluation is the float32 reference. Never enable FP contraction or fast-math
  (`-ffp-contract=off`, MSVC `/fp:precise`). Use `f` suffixes; never promote to double
  inside evaluation.
- `eval_golden_exact_ops` hashes must match on MSVC, GCC and Clang. If an exact op's
  semantics change intentionally, regenerate the hashes and say so.
- All non-leaf op costs >= 1 in every cost model, fusedAdd included (levels are
  well-founded). Bank entries are appended in
  cost order; operands always have lower index.
- Undefined inputs are don't-care: points where the target is not finite are skipped.
- Inexact ops (rsqrt, rcp, div, pow, exp, log, sin, cos) are never classified bit-exact.
  Div is inexact because GPUs lower it to a * rcp(b) with an approximate rcp.
- Contraction (profile `gpu`, cost model `fusedAdd`) uses one rule, `fusedArg` in
  `expr.cpp`: an add/sub over a single-use mul (or div) is one fma.
- Pure helper intrinsics (lerp, step, later smoothstep/length/...) are not enumerated
  during search (owner's rule: their expansions are tried anyway). Single-instruction
  intrinsics and modifiers (mad, clamp, saturate, rcp, rsqrt, ...) are.
- Every new search technique goes behind a flag and must improve time-to-best on the
  bench (section 6 of the design) before becoming default.

## Known limitations (v1, by design)
- Bank cost is tree cost: solutions that need a shared intermediate value are missed
  (bench marks them `needs-sharing`). Planned fix: shared leaves (M7).
- Bank limit (2M entries) is reached around cost 8 with 3 inputs; ternary ops dominate.
- Scalar float only; one output; verification by sampling only (V2/V3 in M2/M7).
- `generic` costs are placeholders. `rdna3` is calibrated per op on gfx1100 but misses
  context effects (min(max()) -> med3, extra v_mov for some constants); `--isa` covers them.
- `rdna3` enumerates in `rdna3-search` order by default (same cheap ops, transcendentals
  at half cost). Bench (rdna3 objective, 11 examples + 36 planted): rdna3-search 38 found,
  rdna3 order 37, generic order 36 (loses cheap-op planted problems). `rdna3` is the default
  objective. With a separate order, dedup keeps the order-cheapest program
  of a value, not the objective-cheapest.
- normalize_x (x * rsqrt(x*x + y*y), rdna3 cost 28, two inputs) is not reached by any
  order: the bank fills first.
- Inner fitting covers u(v + c) for u = rcp/sqrt/rsqrt only (one inner shift, no inner
  scale: exp/sin/log need one); it costs up to ~40% generation speed on planted problems.
- Affine and inner fitting use the fingerprint points, so exact budgets rarely fit.
  With `--no-inner`, inner constants (the c in rcp(t + c)) must come from the constant pool.

## Next (per docs/design.md)
1. Optional (owner: "could"): after search, try re-writing the best candidates with pure
   helpers (mad(t, b - a, a) -> lerp(a, b, t)) for readability only. When M2 adds
   smoothstep/length/distance/normalize, treat them as pure helpers too.
2. M7 search scaling continues (e.g. shared leaves for needs-sharing, reaching
   normalize_x) — ask first.
3. M2: float2–4, dot/length/normalize, component access; V2 exhaustive verification on
   8-bit grids and unary float inputs; error-budget classes.
4. M3: reshadefx front end, region extraction, facts, variant `.fx` output.

## Under discussion (not decided — ask before implementing)
- Library of small verified snippets/rewrites that humans, AI or the tool can reuse.
- Precomputing equivalent instruction forms per input domain to prune the search
  (only one representative per equivalence class needs to be enumerated).
