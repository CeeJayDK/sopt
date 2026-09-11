# CLAUDE.md

Shader superoptimizer for ReShade FX shaders. Finds cheaper, verified alternatives
to small pure arithmetic regions and presents them as user-selectable variants.
Full design and milestones: `docs/design.md` (Danish). Status: M0 + M1 done.

## Working with the owner
- Christian (CeeJay, SweetFX/ReShade). Communicates in Danish; prefers brief, direct answers.
- Do not implement your own improvisations or design changes without asking first.
  Implementing the agreed milestone plan is fine; flag anything beyond it.

## Commands
- Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`
- Tests: `ctest --test-dir build --output-on-failure` (or `build/sopt-tests [filter]`)
- CLI: `build/sopt examples/screen.sopt --stats`
- Bench: `build/sopt-bench --examples examples` and
  `build/sopt-bench --planted 12 --size 3 --inputs 3 --time 30`

## Layout
- `src/ir`: op table (`ops.cpp`: costs, exactness, base set), float32 evaluator,
  hash-consed Expr DAG, `.sopt` parser, printer.
- `src/verify`: test/sample point generation, block evaluation, metrics, budget checks.
- `src/search`: `enumerator` (bottom-up by cost, observational equivalence on
  fingerprints) and `driver` (CEGIS loop, stage-2 filter, V1 verification, grouping).
- `bench/bench.cpp`: example suite + planted problems. `examples/*.sopt` with `# expect:`.

## Invariants (do not break)
- CPU evaluation is the float32 reference. Never enable FP contraction or fast-math
  (`-ffp-contract=off`, MSVC `/fp:precise`). Use `f` suffixes; never promote to double
  inside evaluation.
- `eval_golden_exact_ops` hashes must match on MSVC, GCC and Clang. If an exact op's
  semantics change intentionally, regenerate the hashes and say so.
- All non-leaf op costs >= 1 (levels are well-founded). Bank entries are appended in
  cost order; operands always have lower index.
- Undefined inputs are don't-care: points where the target is not finite are skipped.
- Inexact ops (rsqrt, pow, exp, log, sin, cos) are never classified bit-exact.
- Every new search technique goes behind a flag and must improve time-to-best on the
  bench (section 6 of the design) before becoming default.

## Known limitations (v1, by design)
- Bank cost is tree cost: solutions that need a shared intermediate value are missed
  (bench marks them `needs-sharing`). Planned fix: shared leaves (M7).
- Bank limit (2M entries) is reached around cost 8 with 3 inputs; ternary ops dominate.
- Scalar float only; one output; verification by sampling only (V2/V3 in M2/M7).
- Cost weights in `ops.cpp` are placeholders (calibration in M6).

## Next (per docs/design.md)
1. Confirm CI green on MSVC (golden hashes) — M0 criterion not yet verified on Windows.
2. M2: float2–4, dot/length/normalize, component access; V2 exhaustive verification on
   8-bit grids and unary float inputs; error-budget classes.
3. M3: reshadefx front end, region extraction, facts, variant `.fx` output.

## Under discussion (not decided — ask before implementing)
- Library of small verified snippets/rewrites that humans, AI or the tool can reuse.
- Precomputing equivalent instruction forms per input domain to prune the search
  (only one representative per equivalence class needs to be enumerated).
