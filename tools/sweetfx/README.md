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

## BlendModes.fxh 1.0 (2026-10-08, new)

27 blend modes for any effect to include (W3C compositing formulas plus the image editor extras), picked by
number for a combo uniform (`BLENDMODES_LIST`), and the 8 modes the GPU's blend stage can do as pass state macros
with their pixel shader sources (the shader then does not read the back buffer). Several modes use forms SweetOpt
found (overlay and hard light without a select, pin light as one clamp); all are within one 8-bit step of the
W3C formulas.

## Layer 1.0 (2026-10-08)

- All 27 blend modes; `LAYER_BLEND_STATE` 1 - 9 for the blend stage modes.
- Rotation and pivot; the layer is drawn as its own rectangle (6 vertices), so it can be small and pixels outside it
  cost nothing.
- Place with mouse (D3D10+): drag, rotate, scale with the overlay open; the values are shown next to the mouse to be
  typed into the sliders (an effect cannot save them, see tools/reshade/FEATURE-REQUESTS.md).
- Copies work side by side: Layer2.fx loads Layer2.png, texture names carry the file's hash, preprocessor
  definitions are per effect file in ReShade 6.
- Old presets look the same: rendered under Wine (ShaderLab, ReShade D3D11) against version 0.2 with the same
  position and scale, 7 pixels differ by one step. Blend stage modes vs the shader: within one step for normal,
  multiply, screen and exclusion; add / linear burn / subtract differ below full opacity (Fill vs Opacity, see above).
  Compiles for SPIR-V, DXBC (SM5) and GLSL in every mode; the D3D9 pixel and vertex shaders with Microsoft's fxc.
