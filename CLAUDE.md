# CLAUDE.md

Shader superoptimizer for ReShade FX shaders. Finds cheaper, verified alternatives
to small pure arithmetic regions and presents them as user-selectable variants.
Full design and milestones: `docs/design.md` (Danish). Status: M0, M1, M2 done (CI green on
MSVC/GCC/Clang, golden hashes match); M3 implemented (`sopt-fx`: FX front end, regions,
facts, budgets, variant .fx), waiting for the owner's manual test in ReShade; plus RDNA3 cost model, `gpu` semantic profile, ISA
ranking via fxstat + RGA, solved outer and inner constants (affine + inner, default),
a separate enumeration order model (`--order-model`; rdna3 and nvidia default to
`search`), no pure helper intrinsics (lerp, step) during search (default), an `nvidia`
cost model and NVIDIA SASS ranking (`--sass`, ptxas + nvdisasm). Default cost model: rdna3.

## Working with the owner
- Christian (CeeJay, SweetFX/ReShade). Communicates in Danish; prefers brief, direct answers.
- Do not implement your own improvisations or design changes without asking first.
  Implementing the agreed milestone plan is fine; flag anything beyond it.

## Commands
- Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`
- Tests: `ctest --test-dir build --output-on-failure` (or `build/sopt-tests [filter]`)
- CLI: `build/sopt examples/screen.sopt --stats`
- FX: `build/sopt-fx -I <reshade-shaders>/Shaders -o out <dir or .fx>... [--isa --sass]`
  (`--list --skips` shows regions, facts and why statements were skipped)
- Bench: `build/sopt-bench --examples examples [--cost-model rdna3]` and
  `build/sopt-bench --planted 12 --size 3 --inputs 3 --time 30`
- ISA ranking: `SOPT_FXSTAT=... SOPT_RGA=... build/sopt examples/factor.sopt --isa`,
  NVIDIA: `SOPT_PTXAS=... SOPT_NVDISASM=... build/sopt ... --sass` (pip:
  nvidia-cuda-nvcc-cu12 for ptxas, nvidia-cuda-nvdisasm for nvdisasm).
  Tools: ReShade-Testing-Initiative (`build_reshade_testing_initiative.sh`, needs
  spirv-tools, flex, bison) and RGA 2.14 (`rga-linux-2.14.tgz` from GitHub releases).

## Layout
- `src/ir`: op table (`ops.cpp`: exactness, base set, Shape, cost models generic/rdna3/
  nvidia/search, `CostModel::opCost` per width), float32 evaluator (`evalNode` is the one
  vector-aware node evaluation), hash-consed Expr DAG (Node: type, swizzle, up to 4
  operands, vector constants; `inferType`), `.sopt` parser (floatN inputs, swizzles,
  constructors), printer.
- `src/verify`: test/sample point generation (vector inputs as scalar slots, see
  `slotDecls`/`PointSet::slotOf`), block evaluation (component columns), metrics and
  budget checks per component, V2 exhaustive verification (`compareExhaustive`).
- `src/search`: `enumerator` (bottom-up by cost, observational equivalence on
  fingerprints; affine (default, `--no-affine`): outer p * v + q solved by least squares
  at the goal check, affine chains / two-constant mad, lerp / sign flips / c / v not
  stored; `--order-model`: levels by one cost model, hits/ranking by the objective,
  entries with objective cost >= target not stored, level lists sorted by objective;
  inner (default, `--no-inner`): target ~ p * u(v + c) + q for u = rcp/sqrt/rsqrt, c
  from a linear reparametrization + Gauss-Newton, then the affine fit; pure helpers
  lerp/step not enumerated unless `--helpers`, see `isPureHelper` in ops.hpp) and `driver` (CEGIS loop, stage-2 filter, V1 verification, grouping).
- `src/measure`: `isa` emits a candidate as a ReShade FX effect, runs fxstat + RGA and
  parses the pixel shader's ISA cost; `sass` emits a PTX kernel (inputs loaded and
  stored back so they live in registers), runs ptxas + nvdisasm and counts SASS.
- `third_party/reshadefx`: ReShade 6.8.0 FX lexer/preprocessor/parser, unmodified
  (built as C++17). `src/fx/codegen`: its codegen interface recorded as a dataflow graph
  (values with seq/block, statements Init/Store/Return, loops, samplers, uniforms).
- `src/fx/frontend`: `loadEffect` (ReShade's predefined macros; `ppLines` maps source
  lines to preprocessed text), `extractRegions`: pixel-reachable functions, statement
  text checks (alone on its lines, no macros outside fetch calls), IR building (leaves =
  variable + member chain with used components, or texture fetch call text), windows
  (single-use temporaries inlined), ranges (`Range`, reaching definitions, loops),
  budget from use, and a second parse at 2560x1440 to drop resolution-dependent ones.
- `src/fx/variants`: `compiledCost` (contraction, modifiers, swizzles free), variant
  files (switch per region, `SOPT_ALL`, overlap resolution), Markdown report.
  `src/cli/fx_main.cpp`: sopt-fx (parallel search, filters, --isa/--sass, re-parse).
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
- Accuracy rule (owner): besides the budget vs the float32 original, a candidate passes
  a point if at least as close to the exact value as the original (or within the budget
  of it). Exact values: `verify/exact` (double, only for metrics; never mixed into the
  float32 evaluation). Not for Exact budgets. Less accurate candidates (`--loose`, owner:
  "list them with their accuracy, the user decides") are Klass::LessAccurate; the
  enumerator accepts them as hits, the driver caps them at maxLoose.
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
- One output (float1..4). Vector ops are enumerated only at the target's width (and
  float1); no constructors or swizzles of computed vectors are enumerated. dot/length/
  normalize/distance are pure helpers (not enumerated): their expansions over input
  components are, so e.g. length(v) * length(v) -> dot expansion needs --max-bank 5000000.
- M2 done criteria changed (owner's helper rule): c.r*a + c.g*b + c.b*c -> dot(...) and
  sqrt(dot(v, v)) -> length(v) cost the same on GPUs; they are readability rewrites
  (optional post-search step), not search results. Real vector wins are the examples
  (normalize_length, length_squared).
- V2 checks the cheapest 20 alternatives when the domain has <= 2^24 points; continuous
  multi-input domains are sampled only (V3 in M7).
- `generic` costs are placeholders. `rdna3` is calibrated per op on gfx1100 but misses
  context effects (min(max()) -> med3, extra v_mov for some constants); `--isa` covers them.
- `rdna3` and `nvidia` enumerate in `search` order by default (rdna3's cheap ops,
  transcendentals at half cost). Bench (rdna3 objective, 11 examples + 36 planted): search 38 found,
  rdna3 order 37, generic order 36 (loses cheap-op planted problems). `rdna3` is the default
  objective. With a separate order, dedup keeps the order-cheapest program
  of a value, not the objective-cheapest.
- normalize_x (x * rsqrt(x*x + y*y), rdna3 cost 28, two inputs) is not reached by any
  order: the bank fills first.
- NVIDIA data is the CUDA compiler (ptxas), not the graphics driver's; the MUFU weight
  (8x on sm_86+, 4x on sm_75/80) is from memory of the CUDA guide's throughput table and
  still to be verified (docs.nvidia.com is blocked from the cloud sandbox).
- Inner fitting covers u(v + c) for u = rcp/sqrt/rsqrt only (one inner shift, no inner
  scale: exp/sin/log need one); it costs up to ~40% generation speed on planted problems.
- Affine and inner fitting use the fingerprint points, so exact budgets rarely fit.
  With `--no-inner`, inner constants (the c in rcp(t + c)) must come from the constant pool.

- sopt-fx: statements need ops >= 2 and <= 24, <= 4 inputs / 8 components; returns only
  when `return` starts the line. Windows: single-use temporaries declared once in the
  same block, and same-variable chains (`findChain`: the root is a whole-variable store,
  chain members in its block with no other reads of intermediate values;
  `leavesUnchanged` checks every leaf reads the same value at the root). Windows across
  #if lines get `Region::guard` (`spanGuard`: the taken branch of every #if group with a
  directive in the span; variants use `#if SW >= k && guard`, removed statements
  `#if SW < 1 || !(guard)`). --max-statements / --max-ops bound regions. Fetches
  nested in another fetch's arguments, user function calls and control flow end a
  region. Ranges are per variable (one interval for all components), unions over
  branches (no path sensitivity); back buffer assumed 8-bit SDR; pixel shader inputs
  come from the passes' vertex shaders (PostProcessVS texcoord = [0, 1] by name), else
  semantic conventions (owner): TEXCOORD0..9 = [0, 1] as a fact, SV_Position = pixels [0, 7680] (8K, `--max-width`; hardware limit 16384; the texcoord
  budget is 0.01 px at the same width),
  COLOR only a suggestion [0, 1] (often abused); struct input members by their semantic. Variants of regions with assumed ranges are not written
  (sampling misses rare-event differences, e.g. CRT.fx corner()). The static cost
  model gains only survive `compiledCost`; with --isa/--sass most remaining
  single-statement gains in SweetFX turn out to be compiler-done already.
- Result on reshade-shaders + legacy + SweetFX (chain windows, accuracy rule, loose 100):
  53 effects, 0 parse failures, 731 regions, 22 with gains (new: FilmicPass.fx:87-90
  sigmoid 1 / (1 + exp(a / 2)) as a rational, less accurate 6.5e-5, amd 10 -> 7,
  nv 19 -> 13); all variants compile (231 HLSL/SPIR-V builds). With the facts file
  (depth [0, 1], FAR_PLANE [100, 10000]) sopt-fx finds ReShade.fxh's reversed depth
  rewrite itself: as accurate (1.3e-7 vs exact, original 2.3e-4), amd 7 -> 6, nv 12 -> 11.
  Earlier, slim + SweetFX: 33 effects, 283 regions, 11 with measured gains (Daltonize 0*x terms: NVIDIA only; Vignette XOR dot: AMD 3 -> 2,
  NVIDIA 4 -> 3); all variants compile to HLSL and SPIR-V (spirv-val) for every switch.

- Owner's decisions (M3 review): variants are kept if faster for any measured vendor
  (per-vendor code: `SOPT_AUTO = 1` in variant files picks per `__VENDOR__` the
  variant measured fastest, `vendorPick`; default 0 = unchanged; `__DEVICE__` later, M5); `SOPT_ALL = k` uses the
  last variant where a region has fewer; constant-input regions are skipped; assumed
  ranges: track facts further (done for vertex shaders) and ask the user for missing
  ranges (done: sopt-facts.txt + `--facts`, interactive `--ask`; user ranges apply in
  the range propagation, key "<file> <function|global> <variable>"); dataflow cut
  points to split windows: try in M7.
- Preprocessor definitions (owner: compile-time constants): user-changeable numeric
  ones (`used_macro_definitions`) stay symbolic via a small hook in the vendored
  preprocessor (`symbolic_macros`: code uses become the identifier `__sopt_<name>`, a
  uniform in the parse; `#if` keeps the value). `InputDecl::compileTime` inputs: nodes of
  only constants/compile-time inputs cost 0 (`dagCost(e, m, inputs)`, enumerator
  `Entry::ctime`, `compiledCost`). Search specializes (`Options::specialize`,
  `search/generalize.cpp`): compile-time inputs set to `InputDecl::value` (the macro's
  value), normal search, then each candidate's constants are replaced by <= 4-op
  expressions of the inputs (tolerance 2e-5 rel, 12 matches per constant, <= 1024
  combos) and V1-verified over the full range (examples/depth_far.sopt). A definition
  used as a literal (uniform initializer, DisplayDepth.fx) stays a number (owner: fine,
  DisplayDepth is a setup/debug effect). ReShade.fxh: the reversed-depth chain (lines
  108-111) is a window (guard RESHADE_DEPTH_INPUT_IS_REVERSED); owner: FAR_PLANE is almost
  always 1000, realistic range [100, 10000]; RESHADE_DEPTH_MULTIPLIER stays 1.

## Next (per docs/design.md)
0. M3 done criteria left: owner's manual test of variants in ReShade (DX11 + Vulkan).
1. Optional (owner: "could"): after search, try re-writing the best candidates with pure
   helpers (mad(t, b - a, a) -> lerp(a, b, t)) for readability only. When M2 adds
   smoothstep/length/distance/normalize, treat them as pure helpers too.
2. M7 search scaling continues (e.g. shared leaves for needs-sharing, reaching
   normalize_x) — ask first.
3. M3: reshadefx front end, region extraction, facts, variant `.fx` output.

## Under discussion (not decided — ask before implementing)
- Library of small verified snippets/rewrites that humans, AI or the tool can reuse.
- Precomputing equivalent instruction forms per input domain to prune the search
  (only one representative per equivalence class needs to be enumerated).
