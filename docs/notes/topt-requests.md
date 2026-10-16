# TexBench / OpBench requests from topt (blur optimizer), 2026-10-16

Passed on by the owner from the topt session. Not started. Priority order as given; all passes full-screen pixel shaders
at 3840x2160 unless stated, ms per pass (median of several runs).

- P1 Per-pass overhead: chains of K = 1, 2, 4, 8, 16, 32 dependent passes (each reads the previous output) into 1/64
  (480x270) and 1/256 (240x135) RGBA8 targets, 1 point read + write; slope = overhead (topt assumes 5 us). Variant:
  ping-pong between two targets (A->B->A) instead of K distinct ones.
- P2 Derivative mode per API: ddx / ddy and ddx_fine / ddy_fine of f = x + 1000*y, check the bottom-right pixel of each
  quad (fine: own row / column; coarse: repeats top row / left column), on D3D9, 10, 11, 12, OpenGL, Vulkan. Needs
  Test Host (ReShade on every API), not TexBench (D3D11 only).
- P3 "Free" fetches in a memory-bound pass: RGBA8 full-res source -> RGBA8 target, N = 1, 2, 3, 4, 6, 8, 10, 12, 16, 24
  bilinear reads at fractional offsets within +-3 texels; ms per N (knee: topt assumes ~7).
- P4 Pass time vs target size: 4-tap 2x2 average (1 bilinear read) into 1/4, 1/16, 1/64, 1/256, 1/1024 of the screen
  (source read at the matching stride); where small targets stop scaling.
- P5 Quad exchange vs a fetch: in a 16-read pass replace 3 of 4 reads by 2x2 quad neighbours via ddx_fine / ddy_fine
  (v_x = v + s*ddx_fine(v), v_y = v + s*ddy_fine(v), v_xy = v_x + s*ddy_fine(v_x), s = +-1 by pixel parity, float3);
  ms for 16 reads vs 4 + exchange, and the fma cost of one float3 exchange.
- P6 Noise sources (1 and 8 per pixel): blue-noise texture point reads (WRAP) at 4^2 .. 512^2 (RG8; at 64^2 also R8,
  RGBA8, R8 with two 4-bit values; tex2D vs tex2Dfetch), static const float2 table[16] by (x&3) + 4*(y&3), ALU:
  interleaved gradient noise, R2 (frac(dot(p, (0.7548777, 0.5698403)))), Dave Hoskins' hash without sine; fma units per
  lookup, does texture size change the cost.
- P7 Fetch order: 25 taps (5x5, 2 texels apart) and 81 taps (x16 downsample, 18x18 texels) in row, serpentine, Morton,
  spiral and random order; ms per order (check the ISA keeps the order).
- P8 Uniform vs divergent branches: loop 0..16 taps with [branch] if (i < n), n from a uniform vs from the pixel
  position, vs the loop fully unrolled.
- P9 Compute (lower): dispatch overhead (chains of K small dispatches) and groupshared float4 read rate. In ReShade a
  compute pass must be followed by a pixel pass.
- P10 Partial-screen draws (lower): 25% of the screen (two bands) with a heavy shader: custom vertex shader drawing only
  the bands vs full-screen + stencil vs full-screen + discard.

Also open (owner's go 2026-10-16): a site table of all OpBench tests per family (rows = tests in kDisplayOrder's
sections, columns = families, family medians of the newest clean report per card, heat colors like the group table).
