# CLAUDE.md

SweetOpt (sopt / sopt-fx): a superoptimizer for ReShade FX, plain HLSL (SM5 pixel and compute) and GLSL fragment shaders.
It finds cheaper, verified alternatives to small pure arithmetic regions and writes them as user-selectable variants.
Also in this repo: GPU Blueprint (OpBench, TexBench, ShaderInfo: GPU measurement tools for Windows), Test Host (ReShade
on every API without a game), the web page (site/) and ReShade patches (tools/reshade). Design and milestones:
`docs/design.md` (Danish).

**History:** every decision, measurement and report up to 2026-10-09 is in `docs/notes/history.md` (the former, much
longer CLAUDE.md). grep it for a topic before re-deriving anything. New detailed notes (reports, bench numbers, findings)
go there or in the topic's own doc; CLAUDE.md keeps only rules, commands, layout, invariants and open items, in short form.

## Working with the owner
- Christian (CeeJay, SweetFX / ReShade). Communicates in Danish; wants brief, direct answers. Hardware: NVIDIA GTX 1660
  and an Intel NUC (Iris 540); no AMD card (AMD numbers come from RGA and testers' reports).
- **Tokens are limited** (owner, 2026-10-09: weekly usage ran out fast). Keep turns lean: no long explanations, avoid
  re-reading large files, prefer GitHub Actions for heavy or repetitive work (they cost no tokens), batch tool calls.
- Do not implement your own improvisations or design changes without asking first. The agreed plan is fine; flag the rest.
- No personal data in the repository (GDPR, 2026-10-05): no names, handles or other details of testers next to reports,
  results or notes; a report is described by its hardware and driver only. Telling the owner in chat who uploaded a
  report is welcome (so he can thank them), never anywhere else.
- Fewer instructions at equal measured speed are still better (less power; faster once the bottleneck moves).
- When asking the owner to test or download something, repeat the links in that message (CI run / artifact links).
- Versions: raise the last digit of `project(sopt VERSION ...)` in CMakeLists.txt for every build sent to the owner or
  testers (now 0.6.10); the release sets the next minor version (0.7.0 is next).
- CHANGELOG.md: one `## <version>` section per release, add to the top section as things land (release.yml copies it).
- PNGs sent to the owner or committed: ECT -5 first (`ect -5 -strip -quiet`, oxipng -o 4 as fallback).
- Commits: no model IDs anywhere in the repo. End commit messages with the session's attribution trailer.

## Commands
- Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8`
- Tests: `ctest --test-dir build --output-on-failure` (or `build/sopt-tests [filter]`)
- CLI: `build/sopt examples/screen.sopt --stats`; library check: `build/sopt --check-library [--library-file F]`
- FX: `build/sopt-fx -I <reshade-shaders>/Shaders -o out <dir or .fx>... [--isa --sass --backends]`
  (`--list --skips` shows regions / facts / skip reasons; `--region F[:L]` searches matching regions only)
- Bench (run before / after any search or cost change, back to back, same machine):
  `build/sopt-bench --examples examples --time 30` and `build/sopt-bench --planted 12 --size 3 --inputs 3 --time 30`
- Measurement tools (live in the session scratchpad; rebuild / fetch when missing): `SOPT_FXSTAT` (RTI fxstat),
  `SOPT_RGA` (RGA 2.14 Linux), `SOPT_PTXAS` / `SOPT_NVDISASM` (pip nvidia-cuda-nvcc-cu12 / nvidia-cuda-nvdisasm),
  `SOPT_FXC` (sopt-fxc.exe under Wine with Microsoft's d3dcompiler_47.dll). OpBench / TexBench shaders can be compiled
  with fxc under Wine through a harness that #includes the .cpp (history: "f16/harness.cpp").
- Windows tools cross-build with mingw for Wine checks; the real Windows builds come from CI (MSVC).

## Releases and CI
- CI (ci.yml) on every push: Linux/MSVC builds, tests, artifacts SweetOpt-windows, GPU-Blueprint, Test-Host.
- Release: PR to main (check for a PR template), owner merges, run release.yml manually on main = draft release, owner
  publishes (this session cannot push tags). Run the steam-survey workflow on main before each release.
- pages.yml builds the site (tools/site/build.py) from docs/opbench / texbench; optimize-pngs.yml compresses report PNGs.
- After adding OpBench reports: rerun `tools/windows/gen_expected.py` (writes tools/windows/expected.hpp).
- Release 0.7.0 waits for the owner's test of the current build.

## GPU Blueprint report intake (only when the owner says "check Dropbox")
The scheduled check is disabled (trigger trig_01RggLdp3vQTNdxhpVXZhXFa, owner 2026-10-09: tokens). Procedure:
1. Dropbox list_folder "/Uploads/GPU Blueprint" (recursive); touch no other folder. Uploads: "<uploader> - <file>".
2. Per zip: Dropbox download_link (single use, never preview) + curl, unzip in a fresh folder. Fallback: Dropbox fetch
   (text files concatenated, each after a line with its name; no PNGs).
3. Valid: OpBench / TexBench CSVs with "# OpBench <v>" / "# TexBench <v>" headers and numeric rows; ShaderInfo .txt
   starting with "GPU: "; TexBench order PNGs next to a valid TexBench CSV. Everything else is junk.
4. Save to docs/opbench, docs/texbench (+ PNGs), docs/shaderinfo as `<vendor>-<gpu>.csv|.txt|-order*.png` (never
   overwrite: -2, -3 ...), a short note per GPU in docs/notes/history.md (family match, drift, consensus, new / odd;
   TexBench time and scores), gen_expected.py if OpBench reports came in, commit, push.
5. Only after the push delete every file in the folder. 6. Tell the owner per GPU what came in (uploader name in chat).
- Action (owner's wish, no tokens): .github/workflows/report-intake.yml + tools/site/report_intake.py (every 6 h on main,
  commits reports + docs/notes/intake-log.md, regenerates expected.hpp, then deletes the uploads). Needs the repo secrets
  DROPBOX_APP_KEY / DROPBOX_APP_SECRET / DROPBOX_REFRESH_TOKEN and a Full Dropbox app; runs once it is on main.

## Layout
- `src/ir`: ops (`ops.cpp`: op table, exactness, cost models, `CostModel::opCost` / `divCost` / `binaryCost`, HalfCosts),
  float32 evaluator (`evalNode`), hash-consed Expr DAG (`expr.cpp`: dagCost / nodeCosts / divCosts / fusedArg /
  amdFoldedNodes / scheduleMetrics, printers incl. `toGlsl`), `.sopt` parser.
- `src/verify`: sample points (incl. special values, threshold points), block evaluation, budgets, accuracy rule
  (`verify/exact`), V2 exhaustive, V3 interval bounds (`bound.cpp`), problem inputs (`problems.cpp`).
- `src/search`: `enumerator` (bottom-up by cost, observational equivalence, affine / inner fits, top-down split, two-phase,
  best-so-far bound, overflow, disk bank), `driver` (CEGIS, stage-2 filter, V1 verification, library pre-pass, cache),
  `library.cpp` (rewrite rules from library/rewrites.txt), `subtrees.cpp`, `cuts.cpp`, `generalize.cpp`, `reshape.cpp`.
- `src/measure`: `isa` (fxstat + RGA, or RGA GLSL mode), `sass` (ptxas + nvdisasm), `backends` (SPIR-V alu, fxc DXBC).
- `src/fx`: `frontend` (loadEffect, extractRegions, ranges, facts, budgets; HLSL / GLSL modes), `variants` (compiledCost,
  variant files, report, easyPicks, sopt-found.txt), `platforms` (12 GPU families, Steam shares), `classic.cpp` /
  `hoist.cpp` / `blend.cpp` (classical rewrites: tables, vertex shader moves, blend stage).
- `src/cli`: `main.cpp` (sopt), `fx_main.cpp` (sopt-fx), `console` (titles, progress bars, window title progress).
- `third_party/reshadefx`: ReShade 6.8.0 FX lexer / preprocessor / parser with small hooks (symbolic macros, HLSL / GLSL
  modes, named expressions); `src/fx/codegen` records its codegen as a dataflow graph.
- `tools/windows`: opbench (OpBench), texbench (TexBench), shaderinfo (ShaderInfo), benchkit.hpp (shared console /
  timing), host (sopt-host), timer (sopt-timer ReShade add-on), GPU-Blueprint.bat / SweetOpt.bat / Test-Host.bat menus,
  docs/*.html (shipped guides), stage.ps1, expected.hpp. `tools/reshade`: ReShade patches, IEEE754 test effect.
  `tools/site`: site build, Steam share script. `tools/sweetfx`: proposed SweetFX changes, BlendModes.fxh.
- Data: docs/opbench, docs/texbench (+ FINDINGS.md), docs/shaderinfo; data/steam-gpu-share.txt; library/rewrites.txt
  (+ library/found/); docs/inexact-tricks.md (every not-exact / conditional trick with its limits: add new ones there).
- `bench/bench.cpp` + `examples/*.sopt` (`# expect:`).

## Invariants (do not break)
- CPU evaluation is the float32 reference: never FP contraction or fast-math (`-ffp-contract=off`, MSVC `/fp:precise`),
  `f` suffixes, no double inside evaluation. `eval_golden_exact_ops` hashes must match on MSVC, GCC and Clang.
- Every non-leaf op costs >= 1 in every cost model (fusedAdd too); bank entries in cost order, operands at lower index.
- Undefined inputs are don't-care (points where the target is not finite are skipped).
- Accuracy rule: a candidate passes a point within the budget of the float32 original or at least as close to the exact
  value (double, metrics only); not for Exact budgets. Less accurate candidates are listed as such (`--loose`).
  Rel budgets are relative to max(|t|, error scale). Noise guard: if the original itself is far from exact math (hashes),
  exact-based rules are off.
- Six verification profiles incl. gpu+ / gpu- (inexact ops one float step off). Inexact ops (rcp, rsqrt, div, exp, log,
  sin, cos, pow, exp2, log2) are never bit-exact.
- Contraction rule: `fusedArg` in expr.cpp (an add/sub over a single-use mul or div is one fma).
- Divisions cost as compilers emit them: one reciprocal per distinct divisor (shared with rcp(b)), a multiply per
  component, nothing for a compile-time divisor (`divCosts`, checked with RGA and ptxas).
- Pure helpers (lerp, step, smoothstep, dot, length, normalize, distance) are not enumerated; their expansions are.
- Every new search technique goes behind a flag and must improve the bench before becoming default (unless the owner
  decides). Tests of what the bank alone reaches turn off overflow, shared leaves, subtrees, cuts, top-down, two-phase,
  library.
- `precise` regions keep float semantics; rounding-dependent variants are written as `precise` (`needsPrecise`).
- Variants of regions with assumed (unproven) ranges are not written; facts files / `--ask` supply ranges.

## Facts worth keeping in mind
- Cost models (all from OpBench, docs/opbench): rdna3 (default; RX 7900 GRE), amd-rdna2, amd-rdna4, amd-gcn5,
  amd-terascale2, nvidia-maxwell / pascal / turing / ampere (RTX 30 + 40) / blackwell, intel-gen7.5 / gen9 / gen12,
  plus rdna3-rga (old instruction counts), nvidia (ptxas), generic, search (enumeration order). New cards that differ
  get their own model, identical ones share. GCN 4 (RX 590) would be its own model (owner's go needed).
- min16float: ReShade writes it as min16float only on D3D10-12 (D3D9 float, OpenGL mediump = ignored, Vulkan
  RelaxedPrecision = ignored by AMD's compiler). fp16 is faster only on AMD GCN 5 / RDNA 2 (packed) and Intel Gen9 / Gen12
  (HalfCosts in ops.cpp; provisional until OpBench mix16 / mad16v3 reports).
- ReShade FX has no wave intrinsics / SM6 features (owner: ReShade adds features once widely supported).
- fxc -O3 folds (x + c) - c and assumes no NaN / inf on inputs; drivers turn x / const into a multiply and share
  reciprocals.

## Open items (details: docs/notes/history.md)
1. Release 0.7.0 after the owner's test of build 0.6.10 (GPU Blueprint + SweetOpt CI artifacts of commit 99b8634).
2. fp16 variants (owner's go): step 1 done (OpBench max16 / log2_16 / mad16v3 / mix16, HalfCosts). Next: step 2 fp16
   evaluator (half / float / mixed profiles), 3 half variants with conversions at region edges in the GPU-family table,
   4 output only for D3D10-12, 5 corpus A/B + sopt-timer test on the owner's NUC (Gen9 should gain, GTX 1660 control).
3. Uniform zero guard (owner: on the list): `if (Strength != 0)` around work a uniform scales, for performance mode off;
   a classical rewrite measured per effect.
4. STOKE-style stochastic search (owner's go to try later, behind a flag, bench decides).
5. After 0.7.0: code review / cleanup with the ponytail plugin (needs a new session; plugins load at session start).
6. Waiting on others: IEEE 754 test results for crosire (tools/reshade/IEEE754.md); ReShade patches (UPSTREAM.md).
7. Planned / parked: `--poly` approximation mode; test package from the full corpus run; M5 rest (probe effect, facts
   database, `__DEVICE__` paths); M4 harness results merged into reports; Layer.fx 2.0 (questions asked); noise mode;
   GCN 4 model; driver-statistics (ShaderInfo) as a measured source (wait for reports); pruning sopt options (later).
