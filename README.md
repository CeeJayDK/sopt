# sopt: shader superoptimizer (M0-M2, ISA ranking)

Finds cheaper, verified alternatives to small arithmetic expressions from shaders.
Design and roadmap: [docs/design.md](docs/design.md) (Danish).

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Usage

```
build/sopt examples/screen.sopt [--stats] [--top N] [--time S] [--max-bank N]
build/sopt examples/screen.sopt --isa [--cost-model rdna3|generic] [--order-model M]
           [--no-affine] [--no-inner] [--helpers]
build/sopt-bench --examples examples
build/sopt-bench --planted 12 --size 3 --inputs 3 --time 30
```

## Input format (`.sopt`)

```
# expect: lerp(a, 1.0, b)        (optional, used by tests/bench)
input a : float in [0, 1] grid 255
input b : float in [0, 1] grid 255
output r = 1.0 - (1.0 - a) * (1.0 - b)
budget r : color8                 # see budgets below
```

Inputs and the output can be `float`, `float2`, `float3` or `float4`; every component of
an input has the same domain. Expressions use HLSL syntax: swizzles (`c.rgb`, `v.x`),
constructors (`float3(a, b, c)`, `float3(s)`), scalar broadcast (`v * s`), `dot`,
`length`, `normalize`, `distance`. Comparisons are scalar.

```
input v : float3 in [-1, 1]
output r = normalize(v) * length(v)
budget r : rel 1e-5
```

Budgets (per output component):

| budget | meaning |
|---|---|
| `exact`, `condition`, `temporal`, `depth` | bit-exact |
| `color8 [maxdiff N]`, `color10 [maxdiff N]` | 8/10-bit code values differ by at most N (default 1) |
| `texcoord [PX]` | at most PX pixels at 3840 wide (default 0.25) |
| `abs EPS` | `|candidate - target| <= EPS` |
| `rel EPS` | `|candidate - target| <= EPS * max(1, |target|)` |

`dot`, `length`, `normalize` and `distance` are pure helpers (no GPU has an FP32 dot
instruction): they are evaluated and costed as their expansions, and the search builds
the expansions (components of vector inputs are free leaves), not the helpers.

## Output

Every verified alternative cheaper than the target, sorted by cost, with class
(bit-exact / 8-bit identical / within budget), max error, max 8-bit code difference
and the fraction of sample points whose 8-bit code changed. Verification is dense
sampling (1M points) under four semantic profiles: `ref` (HLSL lerp, unfused mad),
`mix` (GLSL mix formula), `fma` (fused mad) and `gpu` (what drivers emit: a single-use
product under +/- contracted to fma, `a / b` as `a * rcp(b)`).

## Exhaustive verification (V2)

When the input domain is small (every grid value, or every float32 of a component in a
narrow interval) and has at most `--v2-max` points (default 2^24, e.g. an 8-bit RGB
input or a D24 depth value), the cheapest `--v2` (20) alternatives are checked on every
point after sampling; the `ver` column shows `all` for those, `smp` for sampled only.

## Cost models

`nvidia` uses NVIDIA Ada SASS costs (same units, transcendentals at 8x, clamp = two
FMNMX), searched in the same `search` order. `rdna3` (default) uses AMD RDNA3 ISA costs in quarter-VALU units (calibrated with RGA,
see `src/ir/ops.cpp`), with free modifiers and contraction. `--cost-model generic` uses
the M1 placeholder weights. Under `rdna3` alone the search does not reach deep
candidates (bank limit), so it enumerates in `search` order
(same cheap ops, transcendentals at half cost) while rdna3 still decides hits and
ranking. `--order-model M` picks another order (e.g. `rdna3`, `generic`).

## Symbolic constants (default; `--no-affine` to disable)

Constants are solved instead of enumerated for the outer affine map: every bank entry
`v` is fitted as `target ~ p * v + q` (least squares on the fingerprint points, then
the budget check), and entries that are only an affine map of another entry are not
stored. This finds e.g. `mad(rcp(t + 0.001001001), 0.001002003, -0.0010009935)` for
ReShade's depth linearization (`examples/depth_reversed.sopt`), which plain
enumeration does not reach.

Pure helper intrinsics (`lerp`, `step`) are not enumerated during search: their
expansions (`mad(t, b - a, a)`, `x >= e ? 1 : 0`) are, at the same cost. `--helpers`
enumerates them too.

It also solves one inner constant (`--no-inner` to disable): `target ~ p * u(v + c) + q` for
`u = rcp, sqrt, rsqrt` (each is linear in a reparametrization, refined by Gauss-Newton).
It finds e.g. `rsqrt(t + 0.6666667) * 0.57735026` for `1 / sqrt(3t + 2)`, where 2/3 is
not in the constant pool (`examples/rational.sopt`, `examples/rsqrt_affine.sopt`).

## Real ISA cost (`--isa`)

Ranks the shown alternatives by the pixel shader's ISA cost on AMD RDNA3: each one
is emitted as a small ReShade FX effect and compiled with `fxstat` from
[ReShade Testing Initiative](https://github.com/CeeJayDK/ReShade-Testing-Initiative)
and AMD's [Radeon GPU Analyzer](https://github.com/GPUOpen-Tools/radeon_gpu_analyzer)
(about 0.25 s each, run in parallel). The `isa` column is fxstat's COST
(VALU + 3 x TRANS); `!` marks alternatives that are not cheaper than the target in
ISA, i.e. the driver already produces the same code.

```
export SOPT_FXSTAT=/path/to/rti/bin/fxstat SOPT_RGA=/path/to/rga
build/sopt examples/factor.sopt --isa [--asic gfx1100] [--isa-keep DIR]
```

The ISA test in `sopt-tests` runs only when both variables are set.

## NVIDIA SASS cost (`--sass`)

Each shown alternative is also emitted as a PTX kernel (approximate transcendentals and
`a / b` as `a * rcp(b)`, as graphics drivers do; mul/add left for ptxas to contract),
compiled by `ptxas` for one GPU generation and disassembled by `nvdisasm`. The `nv`
column is ALU + MOV + 8 x MUFU (FP32 : MUFU throughput 128 : 16 per SM, 4x on sm_75/80;
from the CUDA programming guide, to be verified). With `--isa` too, rows are ranked by
AMD then NVIDIA, and `!` per column shows a rewrite that does not help that vendor.

```
pip install nvidia-cuda-nvcc-cu12 nvidia-cuda-nvdisasm   # ptxas, nvdisasm; no GPU needed
export SOPT_PTXAS=.../nvidia/cuda_nvcc/bin/ptxas SOPT_NVDISASM=.../nvidia/cu13/bin/nvdisasm
build/sopt examples/step_lerp.sopt --isa --sass [--sm 75|86|89|120] [--sass-keep DIR]
```

This is the CUDA compiler, not the graphics driver's shader compiler (believed to share
the backend); Nsight Graphics on real hardware is the ground truth.
