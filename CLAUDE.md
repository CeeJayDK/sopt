# CLAUDE.md

Shader superoptimizer for ReShade FX shaders. Finds cheaper, verified alternatives
to small pure arithmetic regions and presents them as user-selectable variants.
Full design and milestones: `docs/design.md` (Danish). Status: M0, M1, M2 done (CI green on
MSVC/GCC/Clang, golden hashes match); M3 done (`sopt-fx`: FX front end, regions,
facts, budgets, variant .fx; owner's ReShade test passed on DX11 and Vulkan); plus RDNA3 cost model, `gpu` semantic profile, ISA
ranking via fxstat + RGA, solved outer and inner constants (affine + inner, default),
a separate enumeration order model (`--order-model`; rdna3 and the nvidia / intel models default to
`search`), no pure helper intrinsics (lerp, step) during search (default), an `nvidia`
cost model and NVIDIA SASS ranking (`--sass`, ptxas + nvdisasm), `intel-gen9`, `nvidia-turing`, `nvidia-ampere` and
`nvidia-blackwell` cost models (sopt-opbench timings). Default cost model: rdna3.

## Working with the owner
- Owner's principle (2026-09-26): fewer instructions at equal measured speed are still
  better (less power; faster once the bottleneck moves). Timings (M4 harness) inform, they
  do not veto such variants. ReShade's own performance statistics need a look too (owner is
  not sure they are consistent).
- Christian (CeeJay, SweetFX/ReShade). Communicates in Danish; prefers brief, direct answers.
  Hardware (owner, 2026-10-01): NVIDIA GTX 1660 and an Intel NUC; no AMD card (AMD numbers come
  from RGA / ACO only).
- When asking the owner to do or download something, repeat the links/files in that message
  (resend packages, give the CI run link) so nothing has to be searched for in the thread.
- Do not implement your own improvisations or design changes without asking first.
  Implementing the agreed milestone plan is fine; flag anything beyond it.

## Commands
- Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`
- Tests: `ctest --test-dir build --output-on-failure` (or `build/sopt-tests [filter]`)
- CLI: `build/sopt examples/screen.sopt --stats`
- Library: `build/sopt --check-library [--library-file F]` checks every rule (used by default, `--no-library`)
- FX: `build/sopt-fx -I <reshade-shaders>/Shaders -o out <dir or .fx>... [--isa --sass]`
  (`--list --skips` shows regions, facts and why statements were skipped)
  (`--region F[:L]` searches only matching regions, e.g. long runs: `--region ASCII.fx:254 --time 600`;
  the report's "search limit hit (levels complete to X of Y)": exhaustive up to order-model level X of Y)
- Bench: `build/sopt-bench --examples examples [--cost-model rdna3]` and
  `build/sopt-bench --planted 12 --size 3 --inputs 3 --time 30`
- ISA ranking: `SOPT_FXSTAT=... SOPT_RGA=... build/sopt examples/factor.sopt --isa`,
  Backend normalization (M4): `sopt-fx ... --backends` ($SOPT_FXSTAT; $SOPT_FXC =
  sopt-fxc.exe, tools/fxc, run under Wine with Microsoft's d3dcompiler_47.dll off Windows).
  NVIDIA: `SOPT_PTXAS=... SOPT_NVDISASM=... build/sopt ... --sass` (pip:
  nvidia-cuda-nvcc-cu12 for ptxas, nvidia-cuda-nvdisasm for nvdisasm).
  Harness (M4, Windows): `sopt-host --api dx11|vulkan --bench` with ReShade (full add-on
  support) and `sopt-timer.addon64`; one click: `run-bench.bat` (tools/windows/README.md: ReShade64.dll or
  the ReShade add-on setup exe + a test package next to it; DX11 + Vulkan, screenshots, results zip);
  CI artifact sopt-windows-tools (flat folder).
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
  `backends` (M4 normalization): the same effect through fxstat's optimized SPIR-V
  (pixel "alu" count, canonical spirv-dis text) and ReShade's HLSL through Microsoft's fxc
  -O3 (sopt-fxc; instruction lines of the disassembly); identical code to the original =
  "same" (the compiler already does it on that backend). sopt-fx report columns spirv / dxbc
  and variant comments. Corpus 2026-09-26 (45 regions, 87 variants): DXBC after fxc
  identical to the original for 16 (Daltonize's 0 * x / 1 * x terms, PerfectPerspective
  y * 16 / 9 folding, qUINT_dof floor: fxc does them, they only help the SPIR-V path),
  fewer for 58, more for 2 (Temporal_AA rational forms, faster in AMD/NVIDIA ISA). SPIR-V
  identity is too strict (inputs read from the test texture differently, e.g. Daltonize
  folds to 0 alu in both): counts only; DXBC identity is meaningful.
- `tools/windows` (Windows bench, owner 2026-10-01: one folder, documented in its README.md;
  `run-bench.ps1` / `.bat`: own run folder and ReShade.ini, ReShade as dxgi.dll for DX11 and as a
  Vulkan layer via VK_ADD_LAYER_PATH / VK_INSTANCE_LAYERS, sopt-timer Screenshots=1).
  `tools/windows/timer` (sopt-timer, ReShade add-on): passive per-technique GPU timestamps (median,
  p10-p90, 60-frame mean like ReShade's statistics); bench walks the bundle's sopt-*.ini
  presets, renders each X_orig / X_sopt pair itself on the frame before any effect (A B B A /
  B A A B, per-frame paired difference), writes sopt-timer.csv. ReShade does not send an
  add-on the events its own render_technique causes: validity = the chain rendered the
  technique this frame. D3D11 frames bracketed by TIMESTAMP_DISJOINT (ReShade never checks).
  `tools/windows/host` (sopt-host): DX11 / Vulkan window, fixed image, vsync off; Vulkan loaded at
  run time; synthetic depth (owner's go 2026-09-27): z prepass of a procedural scene,
  reversed Z, 256x144 grid in 144 draws (generic depth skips <= 3 vertices / <= 8 draws),
  shaders depth.hlsl -> depth_dxbc.h (Microsoft D3DCompile) and depth.vert -> depth_spv.h;
  D3D11 SV_VertexID excludes the start vertex, so ids come from a vertex buffer. Checked
  under Wine with ReShade + DisplayDepth (DX11) and a Vulkan run. CI builds sopt-timer.addon32 too. Headers vendored: third_party/reshade-addon (ReShade 6.8.0 API, ImGui 1.92.5),
  third_party/vulkan. Add-on must be built with MSVC (member functions returning small
  structs differ between MSVC and mingw ABIs); tested under Wine/DXVK/lavapipe with a mingw
  ReShade (DX11 end to end; Vulkan host presents; ReShade Vulkan layer untested there).
- `third_party/reshadefx`: ReShade 6.8.0 FX lexer/preprocessor/parser, unmodified
  except `symbolic_macros`, `\` -> `/` and case-insensitive lookup for #include names off Windows (built as C++17). `src/fx/codegen`: its codegen interface recorded as a dataflow graph
  (values with seq/block, statements Init/Store/Return, loops, samplers, uniforms).
- `src/fx/frontend`: `loadEffect` (ReShade's predefined macros; `ppLines` maps source
  lines to preprocessed text), `extractRegions`: pixel-reachable functions, statement
  text checks (alone on its lines, no macros outside fetch calls), IR building (leaves =
  variable + member chain with used components, or texture fetch call text in current
  syntax: `modernFetch` turns deprecated tex2Doffset/tex2Dlodoffset/tex2Dgather(s, c, n)
  into tex2D(s, c, o)/tex2Dlod(s, c, o)/tex2DgatherR..A (owner via crosire)), windows
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
  enumerator accepts them as hits, the driver caps them at maxLoose. Bench: same
  results and first-hit times with and without the rule (examples + 12 planted);
  verification ~9% slower (exact evaluation).
- GPU approximations (owner, 2026-09-26): profiles `gpu+` / `gpu-` (Profile::ulpStep) move
  every inexact op's result (rcp, rsqrt, div as a * rcp(b), exp, log, sin, cos, pow; not
  sqrt, which the op table counts exact) one float step up / down; verification runs 6
  profiles. Rel budgets are relative to |t| itself (`relBase`: max(|t|, 1e-30); the old
  max(1, |t|) floor made them absolute below 1). Both together reject the partial fraction
  depth rewrite (cancellation near t = 1).
- Accuracy variants (owner, 2026-09-26; `Options::accuracyVariants`, default on,
  `--no-accuracy-variants`): `Accepted::moreAccurate` = error vs exact at most 1/4 of the
  original's (rel, and abs not worse), needs the accuracy rule. Kept up to
  `accuracySlack` (8) above the target's static cost; sopt-fx keeps them when not faster if
  at most 1 instruction slower per measured vendor (`Variant::accuracyOnly`, listed last,
  "more accurate (not faster)", max 2 per region, never SOPT_AUTO). Search side:
  `SearchConfig::rational` emits the inner rcp fit p / (v + c) + q also as
  (v - r) * rcp(mad(v, 1/q, c/q)) (no final cancellation; r snapped to a zero of the target,
  near-integer constants rounded). Found: ReShade depth (t - 1) * rcp(mad(t, 1 - F, -1)).
  Bench (examples + 12 planted, vs the day before): same results and first-hit times except
  depth_far (best 28, the accuracy variant, instead of the rejected 24) and planted_1 (its
  cost-13 candidate is now "less accurate": near the target's zeros its relative error is
  3e-4 where the original's is float precision; the old floor hid it); verify ~1.5x (6
  profiles). sopt-fx (ReShade.fxh, facts file): the depth variant is written as "as
  accurate, more accurate (not faster)", amd 7 -> 8, nv 12 -> 12.
  Corpus (12 packages): 45 -> 35 regions with variants. Rightly lost: MXAO x2, Tonemap
  (partial fractions), ColorIsolation x2 (dropped a divide-by-zero guard, abs(d) < 1e-6 ?
  1e-6 : d), Flashlight x2 (were less accurate); by the rule, likely harmless: BloomingHDR,
  Flair, EyeAdaption (t * (1 - t) -> t - t * t loses relative precision near t = 1 without
  fma); falsely lost: Vignette x2 (XOR x + y - 2xy: zero crossing, both cancel). New: PD80
  Bloom 342-343, iMMERSE FILMGRAIN 287 (faster and more accurate), AstrayFX Smart_Sharp 538.
  Then (owner's go) error-scale floor: Rel budgets relative to max(|t|, S), S = the target's
  running rounding-error bound / unit roundoff (ExactEvaluator::withScale, relBase(t, s)):
  absolute-like where the original itself cancels (Vignette XOR back), relative where the
  small value is exact (depth partial fraction, MXAO, t - t * t stay rejected). Bench: examples
  identical, planted_1 recovered (0 not recovered, 1 needs-sharing).
  Noise guard (2026-09-27, found in the corpus; owner: fine for now): hashes like
  frac(sin(dot(uv, k)) * 43758.5) became 0.0 / uv.x / uv.y as "less accurate" (float32 sin
  of large arguments is chaotic, the original is off by up to 1 from exact, and the
  error-scale floor made rel budgets accept anything). `followsExact` (driver): if the
  original's max error vs exact > 10% of its range (4096 random points), Budget::vsExact
  and Budget::errorScale are off (RunResult::exactOff). Corpus: 45 -> 41 regions (ASCII,
  Common GetRandom, GrainSpread, Limbo_Mod dither gone); bench unchanged. Owner (2026-09-27):
  variants should be better in some way, faster or more accurate or both; faster usually
  matters more (8-bit output hides most error); accuracy is written next to each variant
  and the user chooses. Noise (owner, 2026-09-27): A for now = the guard above (the float32
  original is the only reference, so only same-noise rewrites pass); B later = a noise mode
  (variants must stay noise: same range, similar mean/spread, uniform histogram, no
  neighbour correlation) that could swap in cheaper hashes.
- Inexact ops (rsqrt, rcp, div, pow, exp, log, sin, cos) are never classified bit-exact.
  Div is inexact because GPUs lower it to a * rcp(b) with an approximate rcp.
- Contraction (profile `gpu`, cost model `fusedAdd`) uses one rule, `fusedArg` in
  `expr.cpp`: an add/sub over a single-use mul (or div) is one fma.
- Pure helper intrinsics (lerp, step, later smoothstep/length/...) are not enumerated
  during search (owner's rule: their expansions are tried anyway). Single-instruction
  intrinsics and modifiers (mad, clamp, saturate, rcp, rsqrt, ...) are.
- Every new search technique goes behind a flag and must improve time-to-best on the
  bench (section 6 of the design) before becoming default, unless the owner decides
  otherwise (shared leaves, subtrees and cut points: default by the owner's decision, 2026-09-27;
  top-down split and library 2026-09-29, two-phase 2026-10-01). Tests that check what the bank alone
  reaches (test_rewrites expectRewrite) turn off overflow, shared leaves, subtrees, cuts, top-down,
  two-phase and the library.

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
- V2 checks the cheapest 20 alternatives when the domain has <= 2^24 points; elsewhere V3
  proves a bound where it can (often only part of the domain); the rest is sampled.
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
  (8x on sm_86+, 4x on sm_75/80) is from memory of the CUDA guide's throughput table; 4x on
  sm_75 confirmed by opbench on the GTX 1660 (sm_86+ still unverified).
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
  variant measured fastest, `vendorPick`; default 0 = unchanged; `__DEVICE__` later, M5;
  per API too (owner, 2026-09-27): DX9-DX12 (`__RENDERER__ < 0x10000`) skip variants fxc
  compiles to the original's code or to more DXBC, a separate line only where the pick differs); `SOPT_ALL = k` uses the
  last variant where a region has fewer; constant-input regions are skipped; assumed
  ranges: track facts further (done for vertex shaders) and ask the user for missing
  ranges (done: sopt-facts.txt + `--facts`, interactive `--ask`; user ranges apply in
  the range propagation, key "<file> <function|global> <variable>"); dataflow cut
  points to split windows: try in M7.
- Problem inputs (owner 2026-09-25: keep such variants, mark them, the user decides):
  `verify/problems` (`findProblemRanges`, run on every accepted candidate in the driver)
  finds input values where a verified variant still fails, e.g. rcp(0.01 * F - 2) at
  F = 200, which sampling cannot hit: zeros / domain edges of rcp, div, rsqrt, sqrt, log
  and pow operands that depend on one scalar input, a full check at each, widened to the
  failing interval. Shown in the report, the variant comment and `sopt` output with the
  fine ranges ("fails at F = [200, 200.00002] (NaN/inf at some), fine on ..."); never
  picked by SOPT_AUTO. Not covered: operands of several inputs or vectors.
  ui_min/ui_max stay facts (owner: values forced outside them are not guaranteed).
  FAR_PLANE stays [100, 10000] (hard limits [1, int max]; the shipped ReShade.fxh depth
  rewrite fails for F in [1, 1.2342985]).
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

- Test corpus (owner): not the legacy branch (deprecated). Use packages from
  https://github.com/crosire/reshade-shaders/blob/list/EffectPackages.ini (install paths
  there). 2026-09-24: slim, SweetFX, AstrayFX, Daodan, OtisFX, Fubax, brussell,
  FXShaders, qUINT, PD80, iMMERSE: 43 regions with gains; all variant files compile
  (1535 HLSL/SPIR-V builds incl. SOPT_AUTO). iMMERSE parses since 2026-09-25 (backslash
  includes): 188 regions, 4 with gains (MXAO, SOLARIS), many search-limit hits; it found
  the chain-from-declaration bug (variant lost the declaration). OtisFX parses fully
  since includes ignore letter case off Windows (owner: ReShade assumes Windows). Test packages: unique file name per package, steps inside
  TESTING.txt AND in the chat message. Owner (2026-09-25): Marty McFly
  (martymcmodding) and originalcodr are interested; add their repos to future test
  runs, iMMERSE especially (heavy, complex code: stress test), also METEOR. CorgiFX
  (originalnicodr): 9 effects, 148 regions, 4 variants all on assumed ranges.
  Rerun 2026-09-25 (all 12 packages): 45 regions with variants, all variant files
  re-parse, no written variant has problem inputs. Flair.fx:599-602 and BeforeAfter.fx:93-95
  dropped out versus the day before with identical code (commit 53f0285 rebuilt gives the
  same): time-limited search (BeforeAfter found again with --time 10) and measurement.
- Render check (owner's suggestion): RTI Shaderlab (`rti/shaderlab/fxrender.py`, runtime
  built with build_runtime.sh; Wine + mingw + system python3.12 for PIL). Render the
  original and the variant with SOPT_ALL = 0..3 on a test image, compare pixels. Each
  effect file name must exist once on the search path (ReShade selects by name): render
  variants from a copy of the package with the changed files laid over it. Shaderlab's
  vkd3d d3dcompiler rejects [fastopt] (ReShade's codegen emits it for loops at SM >= 4):
  patched line 1874 of its effect_codegen_hlsl.cpp to emit [loop]. This found a real bug
  (constant arrays indexed at run time got the first element's range: Fubax Waveform).
- Owner's test method (Compare.fx, SweetFX): package with <Effect>-orig.fx / -sopt.fx
  side by side (techniques and the file's own non-semantic textures suffixed _orig /
  _sopt: ReShade shares textures by name; changed headers as <H>-orig/-sopt.fxh; -sopt
  defaults SOPT_ALL 1) and one preset per effect: Capture -> orig -> Restore -> sopt ->
  Compare (compare_mode 7, difference_scale 20). Techniques from code only (strip
  comments and strings). Check renders in a full install tree (packages in their
  installer folders), else headers get included twice via different paths.
  2026-09-26 owner's DX11 test: Limbo_Mod and Temporal_AA differed, everything else matched.
  Cause: SweetFX Compare.fx Capture/Restore are float3, so back buffer alpha is not
  restored and both effects read it (identical code differs too; RGBA Capture/Restore: 0 px).
  Bundle format since then (owner): everything flat in reshade-shaders/Shaders/sopt/
  (effects -orig/-sopt, every included header, includes rewritten to bare names since
  #pragma once goes by path; clashing names get a package prefix), textures in
  Textures/sopt/, sopt_Compare.fx (float4 copy, sopt_ prefixes), sopt_TintA/B between
  BeforeAfter's Before and After, one preset per effect and SOPT_ALL step
  (PreprocessorDefinitions=SOPT_ALL=k, sopt-NN-<Effect>-<k>of<n>.ini). Shaderlab renders the
  presets as chains (scratch fxrender copy accepting Name@File.fx techniques; depth from a
  synthetic slDp chunk, or Depth Anything V2 depth: scratch depth/estimate_depth.py runs the
  ONNX export (github fabio-sim/Depth-Anything-ONNX; huggingface.co is blocked) with
  ShaderLab's pre/post-processing and embeds the slDp chunk).
  Found 2026-09-26: the shipped ReShade.fxh reversed-depth variant makes DisplayDepth's
  normals noisier on the GPU path (Shaderlab: mean |laplacian| 4.71 vs 2.96; depth view
  identical). Cause: GPU rcp is approximate and the variant ends in a cancellation
  (~0.00105 - 0.00100), so its relative error at small linear depths is ~5e-6 vs 5e-7
  (CPU with exact division shows the opposite, hence "as accurate"). The rel budget's
  max(1, |t|) floor hides it. Excluded from test packages; design fix (model GPU rcp error,
  relative budgets for small values) to be decided by the owner.
  A cancellation-free form, (1.0 - d) * rcp(mad(d, F - 1.0, 1.0)), is 160x more accurate
  than the original (max rel 2.9e-7 vs 4.6e-5 with a 1-ulp rcp) at amd 8 (original 7),
  nv 12 (12): an accuracy fix, not a speedup; the search only reports cheaper ones.
  Shaderlab: isnan now emitted as (x != x) (RTI shaderlab/patches/reshade-isnan.patch);
  PerfectPerspective renders (9 presets identical). Bundle 26c: textures named through
  macros are bundled too (blueNoise64.png, NeoBloom_LensDirt.png were missing in 26b).
  RTI fixes pushed to CeeJayDK/ReShade-Testing-Initiative branch
  claude/shaderlab-loop-attribute ([fastopt] patch, stale Xvfb lock, exec bits).

## Next (per docs/design.md)
0. M3 done (2026-09-26): owner's manual test of the variants in ReShade passed on DX11
   and Vulkan (package sopt-compare-2026-09-26c, all presets black; the per-step presets
   made it much easier).
1. Optional (owner: "could"): after search, try re-writing the best candidates with pure
   helpers (mad(t, b - a, a) -> lerp(a, b, t)) for readability only.
2. M4: backend normalization done; harness written (sopt-timer + sopt-host + one-click
   run-bench.bat), waiting for the owner's first runs on Windows (AMD/NVIDIA, DX11/Vulkan;
   owner, 2026-10-01: remind them to test it, resend the artifact link and a test package). Then: new test package
   (bundle presets double as bench presets), merge sopt-timer.csv results into the report.
3. M5 rest: probe effect, facts database, `__DEVICE__` paths.
4. M7 search scaling (owner's go 2026-09-27). Default since 2026-09-27 (owner: "not too much
   extra time; we want the search that finds the best variants"), off with --no-...:
   `--shared-leaves` (SearchConfig::sharedLeaves: the target's own subexpressions, up to 16,
   are free level-0 leaves; `upgradeShared` swaps in a cheaper program of the same value;
   hits not below the target's DAG cost dropped) and `--subtrees` (Options::subtrees,
   search/subtrees.cpp: when the search hits a limit, subexpressions of cost <= 64, max 24,
   1 s each, cached per run, searched with rel 1e-6 + accuracy rule; each cheaper form put
   back and the best disjoint ones combined become candidates, verified as usual).
   Bench (examples + 12 planted, time 30), shared leaves vs none: normalize_x found (28,
   never before), rsqrt_affine 24 -> 21, planted_7 found, needs-sharing 0 (was 1), first
   hits faster; lost: length_squared (at the bank's edge anyway) and planted_1 13 -> 16
   (the extra leaves fill the bank sooner). Subtrees alone: normalize_x 44, rest the same.
   Tonemap.fxh:109 (cost 121): both 121 -> 71 (pow(abs(u), 2.0) -> u * u, mads).
   Corpus (12 packages, --isa --sass --backends --time 3), both flags vs none: 72 regions
   with variants instead of 41 (none lost; all 350 variant files parse; 49 min vs 34).
   New e.g. PD80 Sharpening x4 (saturate chains, bit-exact, amd 10 -> 7), FXShaders
   Convolution gaussian (nv 27 -> 19), Tonemap 109/173, CinematicDOF 587-588, Flair
   599-602 (back), GloomAO/RadiantGI 2 * abs(x), SnowScape, Technicolor, EyeAdaption.
   The run found two old bugs: affine hits on vector values built scalar constants
   (now typed), and a UTF-8 BOM ended up after the generated header (DisplayDepth.fx).
   Cut points (2026-09-27, default by the owner's decision, `--no-cuts`; search/cuts.cpp):
   nodes v that the rest reads the inputs below v only through (dominators); top(cut,
   other inputs) is searched over v's sampled range (box domain exact), sub as in the
   subtree search (`searchPart`, shared cache), combinations verified as candidates.
   Survey: 369 of 2365 corpus regions have such cuts (>= 2 ops each side), 202 of them
   hit the limit. Bench (time 30): identical except rsqrt_affine 21 -> 20. Corpus (same
   run settings, current defaults vs + --cuts): 72 -> 73 regions (ArtisticVignette
   153-154 2 * max(abs(uv - 0.5).x, ...), bit-exact, amd 5 -> 3), better variants in PD80
   Sharpening 246 (amd 13 -> 11), ColorIsolation / CBS (static 17 -> 14, assumed ranges),
   AspectRatioSuite, Flashlight, Limbo_Mod, ColorfulPoster (static -1); none worse;
   45 -> 47 min.
   Quantized OE (2026-09-27, flag `--quant-oe N`, off: not better): fingerprints compared
   after rounding away the low N mantissa bits (SearchConfig::quantBits); a merged value
   that is not bitwise equal is still goal-checked but not an operand. N = 8: ~10% of new
   values merge, no deeper level (levels grow 3-5x), generation ~15% slower. Bench:
   identical except planted_2 12 -> 10. Corpus (vs a baseline rerun on the same, by then
   slower machine): 73 regions either way, 8 variants worse (TripleMonitor 412 20 -> 28,
   ColorLab 118 / PD80 Color_Spaces 142 5 -> 8, ...; the stored first program of a merged
   value is often not the useful one), 2 slightly better. Corpus timings vary ~30% between
   runs on this machine: compare only runs made back to back.
   V3 (2026-09-28, default by the owner's decision, `--no-v3`; verify/bound.cpp): formal
   bound on |candidate - original| by interval subdivision. Exact difference: naive,
   first-order and second-order centered forms (interval gradients and Hessians, forward
   mode; the Hessian difference is exactly 0 for identities, so the remainder ~ width^3),
   a shared saturate/abs/neg root peeled (1-Lipschitz). Rounding: mean value theorem over
   the widened arguments, op model as the profiles (u per exact op, 2 ulp inexact, div
   4 ulp, FTZ 2^-126). A box passes within the budget of the float32 original or
   (accuracy rule) of the exact value; Rel uses |t| only (no error-scale floor, so
   stricter). Adds a proof, rejects nothing. Driver (`sopt`): cheapest 3 accepted, 2 s /
   200k boxes each; sopt-fx: only written variants, after measurement; not inside subtree /
   cut searches. Examples: rsqrt_affine, rational, sqrt_product proven; normalize_x 99.6%;
   factor not (x * (a + b) -> mad(a, x, b * x) really is outside rel 1e-6 near a + b = 0;
   sampling missed it); depth_far not (rel 1e-6, structurally different, compile-time F).
   Corpus: 126 variants checked in 63 s: 11 proven, 30 more on > 99.99% of the domain,
   42 partly, 43 not at all (2 more exhaustive by V2); same regions and variants.
   M7 done.

## Under discussion (not decided — ask before implementing)
- Speed / RAM round 2 (owner, 2026-09-28: profile, then safe changes; disk-backed bank as a
  non-default option for single regions): done hash / prefilter / fit speedups (-8..-14%
  instructions, identical results) and 20-byte entries + freeing the bank before part
  searches (peak 619 -> 383 MB at a full 2M bank); details in docs/performance-ideas.md.
  Then (owner): 16-byte packed entries (op/type codebook byte, flag bits incl. isHit,
  28-bit indices); bank sized by memory (SearchConfig::memBudget; default = RAM available
  at start minus max(1 GB, 5%) and 256 MB per concurrent search, shared by sopt-fx's
  parallel regions; --max-mem MB, --max-bank N an extra cap); bank / offsets /
  fingerprints in fixed chunks (no big reserve, which Windows would commit, no copying);
  hash table grows. 24 test points instead of 32 (same RAM: 2.48M entries): bench
  identical, corpus 74 -> 71 regions (Vignette XOR x3 lost, ~11 worse, 6 better): stays
  32. Fingerprint compression study (3 banks of 1M): delta vs an operand 72-84%, value
  codebook 75-100%, per-position codebook (owner's idea) 79-98% (only corner test points
  compress), lz4 per entry 83-93%, zstd per 64 KB block 39-58%: not worth it in RAM
  (owner agreed: skip fingerprint recompute too), zstd for the disk option.
  Disk-backed bank (`--disk DIR`, option, not default; search/diskstore.cpp): fingerprints
  beyond an eighth of the budget go to zstd tiles in a temp file; dedup by two 64-bit
  hashes; lists split into a RAM segment + per-tile segments at each level's end; tiled
  unary/binary/ternary loops with <= 4 resident tiles (LRU). normalize_x, 150 MB, 30 s:
  2.1M entries vs 936k, one more level, 53% compression, same best.
  Bench (time 30), RAM-budget default vs the old 2M-entry bank: identical results; only
  normalize_x's first hit later (5.7 s vs 4.4 s: storing more costs time per candidate).
  30-second searches are rarely memory bound; the bigger bank pays off in longer runs.
  Deep runs (owner, 2026-09-28): run nothing else heavy at the same time; every sopt
  process sizes its bank from the RAM free when it starts (two 20-minute --disk runs were
  OOM-killed next to test runs). Several processes at once: give each --max-mem.
- Search/verification speed (owner: explore all; order A1-A3, A4, B6/B7): profile,
  proposals and status in docs/performance-ideas.md. Done: inner-fit monotonicity
  prefilter (`innerPrefilter`), compare() stops at a rejected candidate's first failure,
  sopt-fx maxAlternatives 20 and one search per distinct region. Overflow (default,
  owner; `--no-overflow`): when the bank is full, keep combining stored entries and only
  check new values as hits; bench +2 found (length_squared, planted_1), none lost; every
  search runs to --time, which now covers all CEGIS iterations together (tests that
  check bank-limited behavior set overflow = false). A4 done: enumerator batches
  (`flush`: parallel prepare + lookup, serial ordered dedup/store, parallel
  goalCheck/fits, serial ordered commitHits); same results as one thread;
  `SearchConfig::threads`. Next: B6 top-down split or B7 shared leaves.
  Owner: less accurate variants stay in SOPT_ALL; --loose 100 is fine for now.
- Next (owner, 2026-09-28: "your order is fine, as long as we try them all at some point"):
  1. top-down split B6 (done, default), 2. snippet library (--library, default) /
  lerp-step rewrites before the search, 3. long --disk runs on hard regions (running),
  4. M5 rest (probe effect, facts database, __DEVICE__ paths).
- Best-so-far bound (owner, 2026-09-28: compare against the best candidate so far, not the
  original; keep only hits as fast or faster, or as fast and more accurate; the original
  only for presentation; design.md 4.3). Default, `--no-best-bound`: objLimit_ =
  min(target, best + slack + 1), `--slack` 1, 0 when the bank is full, -1 (drop ties with
  the best) when full past half the time; the best is lowered only by hits plausible on
  512 extra points, at max(obj, DAG cost); a full hit list is pruned (cheapest half kept)
  instead of ending the search. sopt-fx drops written variants that another variant of the
  region matches or beats on every measure (Pareto). Top-down split (default since
  2026-09-29 by the owner's decision, `--no-top-down`):
  sorted index of stored values at one test point; add/sub/mul/div inverted against the
  target, second-point filter, then the goal check; only strictly cheaper than the best.
  Bench (time 30): bound = none except planted_2 12 -> 10; + top-down: length_squared found
  (16), normalize_x first hit 0.002 s, step_lerp faster. Corpus (12 packages, --isa --sass
  --backends --time 3, back to back): none 71 regions / 85 variants (68 min), bound 74 / 89
  (69 min; + Smart_Sharp 479, Vignette 73-75 / 82-84), + top-down 79 / 94 (76 min; + Vignette
  101, EyeAdaption 155, PD80 Film_Grain 230 / 234 / 240, Flashlight nv -1 x2); Pareto drops
  121-136 variants per run; all variant files parse. Single-region reruns: DepthAlpha
  146-150 (44 in the unbounded corpus run, 47 in both others) gives 47 alone in all three
  modes, and Vignette 73-75 (19 bound, 20 top-down) gives 20 in both: timing noise of
  parallel runs, nothing lost.
- Rewrite library (owner, 2026-09-27 / 09-29: verified rewrites that the program, people and
  AI use; the program adds what it finds): `library/rewrites.txt`, one rule per line,
  `pattern -> replacement   where x const, t in [0, 1], x >= 0, v : float3   # comment`
  (names = pattern variables, the same name = the same subexpression, commutative operands
  either order; search/library.cpp). `sopt --check-library [--library-file F]` and the tests
  check every rule (rel 1e-6 or at least as close to exact, all 6 profiles, variables in
  [-100, 100] narrowed by the conditions); the check rejected 3 of the first 36 (lerp forms
  that cancel). Built in via cmake/embed_library.cmake; at run time $SOPT_LIBRARY or
  library/rewrites.txt next to the executable (or up to two directories up) wins.
  `--library` (default since 2026-09-29 by the owner's decision, `--no-library`; `Options::library`): up to 4 rule applications, 256 forms (constants
  folded, identities removed); forms cheaper than the target are candidates, the cheapest
  that passes stage 2 sets SearchConfig::seedBound (start of the best-so-far bound), its
  subexpressions are extra shared leaves (SearchConfig::seeds) and subtrees / cuts also
  run on it. sopt-fx writes every faster variant (written or assumed-range, not
  accuracy-only) to sopt-found.txt in the library format (inputs that are not plain
  names become in1.. with a legend; ranges as `where`); it parses and checks as a library.
  Owner: start by collecting all found variants there, then study, generalize and add.
  A seed also makes subtrees / cuts run when the search ends without a limit (the seed's
  bound can end it early; MartysMods_FILMGRAIN 451 13 -> 5 needs the subtree search).
  Bench (time 30): examples identical except length_squared 16 -> 12 (2.7 s instead of 75 s)
  and step_lerp (0.4 s instead of 34 s); planted identical. Corpus (back to back, before the
  seed / part-search fix): 78 -> 91 regions with variants, 93 -> 106 variants, 70 -> 80 min;
  new e.g. MultiTonePoster x3, EyeAdaption 148 / 167, Monochrome 115-118, PiecewiseFilmic x2,
  PD80 Color_Gamut 182 / Color_Balance 176 / Color_Spaces 56, qUINT_lightroom 717; better:
  Tonemap.fxh 109 / 173 nv 129 -> 81, PD80 Sharpening 244-246 57 -> 54; lost only FILMGRAIN
  451 (fixed since).
- Back buffer formats (owner, 2026-09-30: the buffer format gives the range; RGBA8 / RGB10A2
  sample [0, 1], scRGB FP16 goes to 80+ and below 0): sopt-fx (default, `--no-format-checks`)
  checks variants of regions that read or write the back buffer again: 10-bit (back buffer
  inputs on grid 1023; a back buffer output, budget reason "back buffer", counted in 10-bit
  codes) and scRGB (inputs from a second extraction with RegionOptions::hdrBackBuffer, back
  buffer [kScRgbLo, kScRgbHi] = [-0.5, 125]; a back buffer output within rel 2^-11). Failing
  variants get Variant::formatGuard (`BUFFER_COLOR_BIT_DEPTH == 8` / `BUFFER_COLOR_SPACE <= 1`)
  in the variant's #if; inlined statements come back under the negation of all variant
  conditions. Corpus: 44 variants checked, none 8-bit only, 5 SDR only, all rightly (they drop
  a clamp that only [0, 1] makes redundant): EyeAdaption 167 pow(saturate(c), 1/2.2), Tonemap.fxh
  173 abs(...), PD80 Color_Gamut 182 max(..., 0), PD80 Film_Grain 427 x2 saturate.
- Two-phase search (owner's idea, 2026-09-29; SearchConfig::twoPhase, default since 2026-10-01 by
  the owner's decision, `--no-two-phase`): phase 1 slack -1 (only strictly cheaper hits); once there is a hit, from the end of
  the levels or 75% of the time on, phase 2 goes over the levels again with the slack, trying
  only candidates phase 1 pruned (`p1Level_` / `p1Limit_`), plus `refitPass` (fitted hits of
  stored entries that phase 1's bound rejected); top-down and the bank-full pressure are off in
  phase 2. Bench (time 30): identical bests, searches end sooner. Corpus (library default,
  run package by package, each package with and without back to back): 94 -> 95 regions (+
  Fubax Waveform 224-226), 88 -> 84 min (enumeration -10%, subtrees -12%); worse: PD80
  Film_Grain 230 / 234 / 240 (20 -> 23) and FILMGRAIN 287 (amd 2 -> 3). Film_Grain 230 alone:
  default 20 in 2 of 2 runs, --two-phase 20 in 1 of 2 (timing dependent). Rerun after the
  top-down time check and boundBy fixes (12 packages, same build, back to back per package):
  93 / 93 regions, 105 / 108 variants, 116 / 114 min; only Vignette 73 / 82 differ (static
  14 -> 15), and Vignette 73 alone gives 14 in both modes in 3 of 3 runs (timing noise).
  Container note (2026-09-30): the cloud container restarts when the session is idle and a
  background task hits its time limit; long corpus runs go in chunks of ~20-30 min
  (scratchpad tp2/pair.sh), one background task each.
- Full corpus run (owner, 2026-10-01, not urgent): later, every package the installer can
  install (EffectPackages.ini, 45 entries; we use 12), to collect as many new variants as
  possible in sopt-found.txt for improving the library. Run in chunks (container note above).
  Rotation (owner, 2026-10-01): smaller tests should use other installer packages each time
  (both arms of an A/B comparison on the same set), so new packages get checked as a bonus and
  may turn up bugs. Fixed core in every test (owner): ReShade.fxh (used by nearly every effect:
  depth handling, the standard vertex shader) and DisplayDepth.fx (the tool for checking the
  depth buffer setup), both the owner's, from reshade-shaders; and SweetFX (popular, the
  owner's). The rest of reshade-shaders matters little.
- DXC review (owner, 2026-10-01; sparse clone of microsoft/DirectXShaderCompiler, lib/HLSL/
  HLOperationLower.cpp, DxilExpandTrigIntrinsics.cpp): intrinsic lowerings give the semantics and
  cost of ops sopt lacks. Corpus skips (12 packages, `--list --skips`): smoothstep 223, exp2 145,
  mul (matrix) 101, all 94, radians 76, ddx 51, tan 37, log2 34, cross 33, round 20, fwidth 13,
  isnan 12, atan2 10, log10 8, ceil 6, atan 6, asin 6. DXC: exp(x) = exp2(x * log2 e), log =
  log2 * ln 2, log10 = log2 * ln2/ln10, smoothstep = s * s * (3 - 2s) with s = saturate((x - a) /
  (b - a)), pow = exp2(y * log2 x) except pow(x, 2) = x * x (fxc compat mode: integer exponents as
  multiply chains), atan/asin/acos/tan as polynomial expansions. Done (owner's go "1, 2, 3"):
  ops exp2 / log2 (inexact, gpu+/- step them; rdna3 16, nvidia 32: exp = 20 = mul + exp2),
  round (to nearest even; GLSLstd450Round leaves ties to the driver) / ceil (exact), all non-base
  (enumerated only when in the target); smoothstep a pure helper evaluated as DXC lowers it
  (s * (s * (3 - 2s)), the division as the profile divides); radians / degrees / log10 / tan /
  cross written out at parse time (`buildSugarCall`, expr.cpp; parser and sopt-fx). Library:
  exp / log / pow -> exp2 / log2 with a constant folded into the multiply, exp2 / log sums, the
  smoothstep expansion. Corpus (SweetFX + 10 packages, --list): 2328 -> 2487 regions. Open: the
  exp rules need range conditions (rel 1e-6 vs the accurate CPU exp: the error grows with the
  exponent), so exp(x * 2.0) on [-4, 4] is not rewritten; the search cannot find exp2(x * c)
  itself (no inner scale fit).
- Mesa review (owner sent nir_opt_algebraic.py, nir_opcodes.py, nir_search_helpers.h,
  aco_optimizer.cpp, 2026-10-01; gitlab.freedesktop.org is blocked here): the rule file run with
  stub modules (scratchpad mesa/dump.py, translate.py) gives 3461 rules, 188 float rules in
  sopt's ops, 169 pass `--check-library`, 117 cheaper in rdna3; 57 curated (not trivial, not
  already in the library) in scratchpad mesa/curated.txt, all pass: waiting for the owner.
  Failing ones are the add reassociations and lerp <-> a + t(b - a) (the mix profile).
  ACO (RADV's AMD compiler) context effects the rdna3 model lacks: output modifier omod
  (x * 2, * 4, * 0.5, also negated, folded into the producing VALU op, needs FTZ and no
  signed-zero preservation; rcp(x) * 0.5 too), v_max3 / v_min3 (max(max(a, b), c) one
  instruction), GFX11 v_minmax / v_maxmin (min(max(a, b), c)), v_med3 (clamp to constants),
  (cond ? 1 : 0) * a -> one cndmask, clamp and neg / abs modifiers free. Owner (2026-10-01): add
  the 57 curated rules (done) and context costs in rdna3 as the default (done:
  CostModel::amdFolds, `amdFoldedNodes` in expr.cpp, Enumerator::amdFolds for the objective,
  compiledCost; `--no-amd-folds`): a mul by +-2 / +-4 / +-0.5 over a single-use, same-width
  instruction result (not contracted into an fma) and a min / max over a single-use min / max
  (not itself folded) cost 1 per component. Assumes FTZ fp32 (ACO needs it for omod). Bench (time
  30, folds vs --no-amd-folds): examples identical; planted_1 16 -> 13, planted_10's target 17 -> 14
  (same best 4); rsqrt first hit 1.4 -> 3.1 s; generation speed unchanged (rational, 1 s runs).
  ReShade internal shader patch (tools/reshade, owner's go 2026-10-01): CI workflow `reshade`
  builds ReShade 6.8.0 64-bit unchanged and patched (copy_ps Load, GL bilinear mipmaps), checks
  the patched DLL, artifact reshade-6.8.0-sopt with TESTING.md and sopt_MipTest.fx. Owner's test 1
  (sopt-host --api dx11 --msaa 4, 1920x1080, GTX 1660): screenshots SHA256-identical with Vibrance +
  Curves and with Vibrance alone; no visible performance difference (expected: a bandwidth-bound
  full-screen copy, the saving is issue slots / helper lanes, microseconds; not a technique, so
  sopt-timer does not see it; PIX / Nsight would). OpenGL mipmaps, sopt DLL (owner, GTX 1660,
  sopt_MipTest vs the 2x2 average of the level above, levels 1-5): red disappears at tolerance
  0.501 (RGBA8), 0.125 (RGB10A2), 0.063 (RGBA16F), 0.001 (R32F) 8-bit steps = half a step of each
  format (correct rounding; R32F differs only in the last bits). Unchanged DLL: the same except
  RGBA16F 0.109-0.117 (level 3 / level 1), just under one fp16 ulp near 1 (0.125): the old shader
  averages in fp32 and imageStore converts to fp16 by truncation on this driver, while the
  bilinear fetch already returns a correctly rounded fp16 value. So the patch is equal or more
  accurate. Both parts tested: next, report to crosire. Copy test also identical on the Intel Iris 540
  (owner, 2026-10-01). Write-up for crosire: tools/reshade/UPSTREAM.md. Owner: removed the info log line
  and the copy sampler (pipeline layout with only the SRV; sampler state, push and destroy gone);
  re-tested by the owner (D3D11 --msaa 4, GTX 1660): SHA256-identical again.
  Owner's idea (go 2026-10-01): sopt-opbench (tools/windows/opbench, measure-gpu.bat, in the
  sopt-windows-tools artifact): D3D11 compute tests, HLSL generated and compiled at run time with
  D3DCompile -O3 (the ReShade D3D path), steps x = mad(f(x, c), c.x, c.y) in long chains with
  per-step cbuffer constants (nothing folds; checked in the DXBC: fxc writes x * 2 as add x, x),
  cost = (time - base test's time) / mad time * 4; configs tput (8 chains, 1M threads), dep,
  lat; single ops plus omod / max3 / minmax / satmad / contract. Results in docs/opbench/.
  Intel Iris 540 (Gen9, NUC, 2026-10-01), tput, extra cost over the base (mad = 4): add / sub /
  mad / floor / ceil / round / frac / step ~4 (one op), min / max 4.5, neg / abs / saturate ~0
  (modifiers, satmad 0.0), clamp 8.3 (no med3), max3 / minmax +3 (no 3-operand form), select /
  lerp 7.4 (two ops), sign 14, rcp / sqrt / rsqrt / exp2 / log2 / cos ~11.9 (3x an fma), sin 13.4,
  exp 14.3, log 12, pow 29.7; omod2 (x + x after rcp) +2.3, x * 0.5 / x * 3 after rcp +0.8 (the
  mul pairs with the math op); a mul by a uniform before a mad was free in tput (the driver
  reassociates x * c1 * c2 with loop-invariant constants) but one op in dep / lat. Latency (lat):
  simple ops like mad, math ops ~2.9x. The fma rate was 0.31 TFLOPS (~40% of the nominal peak).
  Owner's go: cost model `intel-gen9` (`--cost-model intel-gen9`, ops.cpp kIntelGen9, search order;
  owner: Iris 540 / Gen9 differs a lot from Arc, so it is named for Gen9 only). Not used by sopt-fx's
  measured columns or SOPT_AUTO (no Intel ISA tool). GTX 1660 (Turing, owner, 2026-10-01; docs/opbench/nvidia-gtx-1660.csv), tput, extra over the base:
  add / sub / mad 4, neg / abs / saturate ~0, rcp / rsqrt / sqrt / exp2 / log2 / sin / cos / div / exp /
  log 12 (quarter rate; exp / log / div's mul hides under it), floor / ceil / round / frac 12 too
  (quarter rate on Turing), pow 28, lerp 8, sign 8.4; min / max / step ~0.6 and clamp / select / a second
  max ~4: FP32 min / max / compare / select run on the ALU pipe beside the FMAs (one is free next to a
  mad, two cost one op). Owner (2026-10-01): one profile per family / generation where results group,
  not one per vendor. Cost model `nvidia-turing` (ops.cpp kNvidiaTuring, search order): measured values,
  min / max / step / compares / select 2 each (additive approximation of the dual pipe). Community runs (owner's call
  for tests, 2026-10-02; docs/opbench/): RTX 2060 Super = GTX 1660 (nvidia-turing holds for RTX 20 too);
  RTX 5080 (DXGI reported a 4090 ID, spoofed: its fma rate 57 TFLOPS is a 5080's) and RTX 5090
  practically identical (Blackwell: MUFU and floor / ceil / round / frac ~23, add 3.4, min / max / step
  3.3, clamp / select ~8, lerp ~8, sign 18, pow 50); RTX 3050 (Ampere) close except min / max 4.5,
  clamp / select ~10, sign 28; Intel UHD 630 (Gen9.5, owner's iGPU) = Iris 540 within ~0.5 (intel-gen9 holds
  for Gen9.5); RTX 2070 = Turing; RTX 4070 (Ada, ray_st) is unreliable: its mad base ran slow (neg / abs
  came out -1.8, mul -1.4) and exp2 / log2 / sin / exp doubled while cos did not, i.e. the GPU clock changed
  during the run; rescaled to neg it matches the RTX 3050 (min 4.5, clamp 9.9, floor / rcp 23.6, sign 31).
  Rerun with locked clocks (2 runs, nvidia-rtx-4070-locked*.csv): tput = RTX 3050 within ~0.6 on every
  test (add 3.4, min 4.5, clamp 10, select 11, lerp 7.9, MUFU / floor 23.6, sign 27.7, pow 52), so Ada
  groups with Ampere (nvidia-ampere covers RTX 30 / 40); run to run tput within 0.2 except single spikes
  (sqrt 2.2, omod3 2.2, max3 1.1, min 0.55); dep / lat noisier (up to 3.8). Groups follow the
  architecture names. Waiting for more reports (AMD, Intel Arc wanted). Vulkan / SPIR-V path not covered (would need SPIR-V compiled in CI).
- Fast forms of expensive ops (owner, 2026-10-02: "put sopt and you to the task"; go for all five: Ampere /
  Blackwell models, targeted searches, library rules, opbench tests, precise in sopt-fx).
  sign: fxc lowers it to lt, lt, iadd, itof (the int->float conversion is quarter rate on Ampere /
  Blackwell: sign 18-28). Conversion-free exact forms (fxc output checked): saturate(x * 1e38) -
  saturate(x * -1e38) (mul_sat + add), clamp(x * 1e38, -1, 1) (mul, max, min; AMD med3),
  x > 0 ? 1 : (x < 0 ? -1 : 0) (lt, lt, and, movc); exact given FTZ (D3D10+ flushes fp32 denormals).
  x >= 0 ? 1 : -1 (ge + movc) is NOT sign (1 at 0); owner: check it as a context-dependent
  replacement when sopt runs (exact where the result is multiplied by something 0 at x = 0, e.g.
  sign(x) * pow(abs(x), g)). round: (x + 12582912.0) - 12582912.0 (RNE, |x| < 2^22) but fxc -O3 folds
  (x + c) - c to x, even with a uniform c; `precise` (supported by ReShade FX) keeps it ([precise]
  adds in DXBC). So fxc reassociates float math: sopt variants relying on rounding need `precise`.
  Done: cost models `nvidia-ampere` / `nvidia-blackwell` (provisional, kNvidiaAmpere / kNvidiaBlackwell,
  search order); library rules (sign x5 incl. sopt's own find mad(saturate(mad(x, 1e38, 0.5)), 2, -1) =
  mad_sat + mad, the signed-pow two-way select, the add-round with |x| < 2^22); opbench tests signmad,
  signsat, signclamp, signsel, signsel2, roundadd (roundAdd helper with precise); `needsPrecise` (expr.cpp:
  (v + c) - c, (v + c) + -c, mad(a, b, c) - c with |c| >= 2^22): sopt-fx writes such variants as
  `precise floatN __sopt_p<line>_<k> = ...;` and the measurement effects (emitEffect) as precise. fxc
  checked: without precise it folds mad(uv.x, 1000, 1.5 * 2^23) - 1.5 * 2^23 to uv.x * 1000 (wrong);
  precise propagates backwards to the ops feeding the value (no contraction there). Targeted searches
  (scratchpad ff/, 20 s, 5 models): round 24 / 23 / 12 -> 8 (library); sign: rdna3 16 -> 8 (clamp),
  intel 14 -> 10 (mad_sat form); floor / frac / ceil: nothing cheaper; signed pow: Turing finds the
  two-way select. Then floor / ceil / frac rules from the add-round (r - saturate((r - x) * 1e38), r +
  saturate((x - r) * 1e38), d + saturate(d * -1e38); 5 instructions as precise, fxc checked): Ampere /
  Blackwell 23-24 -> 20-21. Batch 2 (sign rerun, ceil, clamp, select, lerp, pow, exp, sin): sign 28 / 18
  -> 10 (mad_sat form) on Ampere / Blackwell; clamp, select, lerp, pow, exp, sin: nothing cheaper
  (Blackwell min / max raised 3 -> 4 so clamp = min + max: fxc writes clamp as max + min).
  Exhaustive float check (scratchpad ff/exh.c, every float in [-2^22, 2^22], FTZ): the round, floor,
  ceil, frac and mad_sat sign forms are exact; sopt's own Blackwell ceil x + C - (x + C - (x + 0.5)) was
  wrong for x in (0, 6e-8) (x + 0.5 rounds to 0.5, the tie goes to even) but passed sampling: a
  verification gap. Fix: specialValues adds tiny magnitudes (+-FLT_MIN, 1e-30, 1e-20, 1e-10, 1e-7, 1e-4)
  inside the range, not for compile-time / library `const` variables; it rejects that ceil; it also
  showed frac(frac(a)) -> frac(a) (Mesa) wrong for tiny negative a (now `where a >= 0`). Bench (examples,
  time 30): identical.
- docs/inexact-tricks.md (owner, 2026-10-02): every not-exact or conditional trick we find, with what is
  wrong with it and when it is safe (to avoid them when found again, and for programmers who can rule
  the limitation out). Add new ones there as they turn up.
- opbench version 2 WISHLIST (owner, 2026-10-02: collect more reports first; build only when the owner
  says so): (1) warm-up before measuring and a fresh mad base right before every test, so a GPU clock change
  only hits one test, with a warning when the base drifts (the RTX 4070 run); (2) the new
  fast-form tests (signmad, signsat, signclamp, signsel, signsel2, roundadd, flooradd, fracadd, already
  in the code); (3) CSV: gpu / vendor / device / driver once in header lines at the top instead of on every
  row (owner; the table script must read both formats); (4) a VERSIONINFO resource (product, version, description) and a manifest for
  sopt-opbench.exe: Windows 11 Defender flags the static-CRT build as Trojan:Win32/Sabsik.FL.A!ml (an ML
  heuristic false positive on an unsigned exe); the dynamic-CRT build is not flagged (owner, 2026-10-02:
  keep that one; CMake back to the default runtime, README names the VC++ redistributable). Report false positives at microsoft.com/en-us/wdsi/filesubmission (per
  build); code signing (Azure Trusted Signing, or SignPath if the repo is public) is the real fix, later.
  (5) (owner) after the run, print a readable summary in the console: tput cost per op ("rcp costs 23.6 =
  ~6 mads"), grouped (free / cheap / one op / expensive), with colors (ANSI via
  ENABLE_VIRTUAL_TERMINAL_PROCESSING, plain text fallback) and some ASCII / terminal art; warnings for a
  drifting base shown there too. Layout (owner): header banner with program name and the measured card,
  below it "Also detected in system:" with the other adapters (as --list shows them); then one row per
  test in aligned columns: test name, cost, bar, comment (free / cheap / one op / expensive, "≈6 mads").
- Precomputing equivalent instruction forms per input domain to prune the search
  (only one representative per equivalence class needs to be enumerated).
