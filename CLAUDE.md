# CLAUDE.md

Shader superoptimizer for ReShade FX shaders. Finds cheaper, verified alternatives
to small pure arithmetic regions and presents them as user-selectable variants.
Full design and milestones: `docs/design.md` (Danish). Status: M0, M1, M2 done (CI green on
MSVC/GCC/Clang, golden hashes match); M3 done (`sopt-fx`: FX front end, regions,
facts, budgets, variant .fx; owner's ReShade test passed on DX11 and Vulkan); plus RDNA3 cost model, `gpu` semantic profile, ISA
ranking via fxstat + RGA, solved outer and inner constants (affine + inner, default),
a separate enumeration order model (`--order-model`; rdna3 and nvidia default to
`search`), no pure helper intrinsics (lerp, step) during search (default), an `nvidia`
cost model and NVIDIA SASS ranking (`--sass`, ptxas + nvdisasm). Default cost model: rdna3.

## Working with the owner
- Owner's principle (2026-09-26): fewer instructions at equal measured speed are still
  better (less power; faster once the bottleneck moves). Timings (M4 harness) inform, they
  do not veto such variants. ReShade's own performance statistics need a look too (owner is
  not sure they are consistent).
- Christian (CeeJay, SweetFX/ReShade). Communicates in Danish; prefers brief, direct answers.
- Do not implement your own improvisations or design changes without asking first.
  Implementing the agreed milestone plan is fine; flag anything beyond it.

## Commands
- Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`
- Tests: `ctest --test-dir build --output-on-failure` (or `build/sopt-tests [filter]`)
- CLI: `build/sopt examples/screen.sopt --stats`
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
  support) and `sopt-timer.addon64` (tools/timer/README.md); CI artifact sopt-windows-tools.
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
- `tools/timer` (sopt-timer, ReShade add-on): passive per-technique GPU timestamps (median,
  p10-p90, 60-frame mean like ReShade's statistics); bench walks the bundle's sopt-*.ini
  presets, renders each X_orig / X_sopt pair itself on the frame before any effect (A B B A /
  B A A B, per-frame paired difference), writes sopt-timer.csv. ReShade does not send an
  add-on the events its own render_technique causes: validity = the chain rendered the
  technique this frame. D3D11 frames bracketed by TIMESTAMP_DISJOINT (ReShade never checks).
  `tools/host` (sopt-host): DX11 / Vulkan window, fixed image, vsync off; Vulkan loaded at
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
  Noise guard (2026-09-27, found in the corpus; to confirm with the owner): hashes like
  frac(sin(dot(uv, k)) * 43758.5) became 0.0 / uv.x / uv.y as "less accurate" (float32 sin
  of large arguments is chaotic, the original is off by up to 1 from exact, and the
  error-scale floor made rel budgets accept anything). `followsExact` (driver): if the
  original's max error vs exact > 10% of its range (4096 random points), Budget::vsExact
  and Budget::errorScale are off (RunResult::exactOff). Corpus: 45 -> 41 regions (ASCII,
  Common GetRandom, GrainSpread, Limbo_Mod dither gone); bench unchanged. Owner (2026-09-27):
  variants should be better in some way, faster or more accurate or both; faster usually
  matters more (8-bit output hides most error); accuracy is written next to each variant
  and the user chooses. Suggested: an option to ignore accuracy where inaccuracy is the
  point (noise) - not decided how (see chat).
- Inexact ops (rsqrt, rcp, div, pow, exp, log, sin, cos) are never classified bit-exact.
  Div is inexact because GPUs lower it to a * rcp(b) with an approximate rcp.
- Contraction (profile `gpu`, cost model `fusedAdd`) uses one rule, `fusedArg` in
  `expr.cpp`: an add/sub over a single-use mul (or div) is one fma.
- Pure helper intrinsics (lerp, step, later smoothstep/length/...) are not enumerated
  during search (owner's rule: their expansions are tried anyway). Single-instruction
  intrinsics and modifiers (mad, clamp, saturate, rcp, rsqrt, ...) are.
- Every new search technique goes behind a flag and must improve time-to-best on the
  bench (section 6 of the design) before becoming default, unless the owner decides
  otherwise (shared leaves + subtrees: default by the owner's decision, 2026-09-27).
  Tests that check what the bank alone reaches (test_rewrites expectRewrite) turn off
  overflow, shared leaves and subtrees.

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
2. M4: backend normalization done; harness written (sopt-timer + sopt-host), waiting for
   the owner's first runs on Windows (AMD/NVIDIA, DX11/Vulkan). Then: new test package
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
   Still open in M7: cut points, V3, quantized OE.

## Under discussion (not decided — ask before implementing)
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
- Library of small verified snippets/rewrites that humans, AI or the tool can reuse.
- Precomputing equivalent instruction forms per input domain to prune the search
  (only one representative per equivalence class needs to be enumerated).
