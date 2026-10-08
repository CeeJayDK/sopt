# SweetFX changes found with SweetOpt

Proposed changes to CeeJay's SweetFX effects (github.com/CeeJayDK/SweetFX), found while building SweetOpt's
classical optimizer passes. Each comes as the whole file and as a patch against SweetFX as installed by ReShade.

## LumaSharpen 1.6.0 (2026-10-08)

The sample patterns come from tables indexed by `pattern` instead of one `if` block per pattern. Without
performance mode the compiler flattened the four blocks and ran every pattern's texture fetches.

| | shipped | tables |
|---|---|---|
| AMD RDNA 3 (RGA, Vulkan), performance mode off | 96 VALU, 15 fetches | 58 VALU, 5 fetches |
| Direct3D 11 bytecode (fxc ps_5_0), performance mode off | 67 slots, 15 samples | 39 slots, 5 samples |
| performance mode on | 25 VALU, 5 fetches | the same |

Direct3D 9 keeps the `if` blocks (`__RENDERER__ < 0xa000`): ps_3_0 cannot index constants in a pixel shader,
so fxc turns each table lookup into selects (73 -> 136 slots). Rendered under Wine (OpenGL, llvmpipe,
performance mode off): patterns 1-3 bit-identical to the shipped version, pattern 0 within one 8-bit step on
0.05% of the pixels (its two fetches are now averaged as four).
