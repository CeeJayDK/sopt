# sopt: shader superoptimizer (M0 + M1)

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
build/sopt-bench --examples examples
build/sopt-bench --planted 12 --size 3 --inputs 3 --time 30
```

## Input format (`.sopt`)

```
# expect: lerp(a, 1.0, b)        (optional, used by tests/bench)
input a : float in [0, 1] grid 255
input b : float in [0, 1] grid 255
output r = 1.0 - (1.0 - a) * (1.0 - b)
budget r : color8                 # exact | color8 [maxdiff N] | abs EPS | rel EPS
```

`rel EPS` means `|candidate - target| <= EPS * max(1, |target|)`.

## Output

Every verified alternative cheaper than the target, sorted by cost, with class
(bit-exact / 8-bit identical / within budget), max error, max 8-bit code difference
and the fraction of sample points whose 8-bit code changed. Verification is dense
sampling (1M points) under three semantic profiles: `ref` (HLSL lerp, unfused mad),
`mix` (GLSL mix formula) and `fma` (fused mad).
