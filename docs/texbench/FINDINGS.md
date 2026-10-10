# TexBench findings

What the TexBench reports show, as recommendations. Each point says for which GPUs it holds and gives the
numbers behind it. Costs are in fma units (1.0 = one fused multiply-add at full rate, throughput: many reads in
parallel), so they compare directly with the cost models. Lower is better.

Cards so far (16): NVIDIA Maxwell (GTX 860M, Quadro M5000M), Pascal (GTX 1050), Turing (GTX 1660, GTX 1660 Ti, RTX
2060), Ampere / Ada (RTX 3050 Laptop, RTX 4060 Ti, RTX 4090 Laptop), Blackwell (RTX 5080); AMD GCN 5 (Vega 8), RDNA 2 (Ryzen 7000 iGPU), RDNA 4
(RX 9070 XT); Intel Gen7.5 (HD Graphics 4600), Gen9 (Iris 540, UHD 630), Gen12 (Iris Xe). Laptop runs are often throttled: their
absolute numbers are low, their ratios still match their desktop family.

TexBench 0.6.0 measures the comparisons directly (the same result read different ways, side by side). So far it has
run on the GTX 1660 (Turing), the RTX 5080 (Blackwell), the UHD 630 (Gen9.5) and the Iris 540 (Gen9; within ~2% of the
UHD 630 in every shader test, so the Intel numbers below hold for both); for other cards the advice is *estimated* from single-read costs until
their reports come in. The direct tests already corrected two estimates: several reads in a row do not cost
the sum of single reads, so only the side-by-side tests decide.

### Averaging 2 × 2 texels (downsampling, box blurs): one bilinear read

One bilinear read at the shared corner of four texels returns their average. Measured directly:

| fma units | one bilinear | 3 gathers (RGB) | 4 Loads + math |
|---|---|---|---|
| GTX 1660, RGBA8 | 11.4 | 29.1 | 57.5 |
| GTX 1660, RGBA16F | 22.1 | 50.7 | 58.1 |
| GTX 1660, RGBA32F | 26.1 | 58.2 | 58.6 |
| RTX 5080, RGBA8 | 18.2 | 46.9 | 35.3 |
| RTX 5080, RGBA16F | 39.4 | 90.2 | 53.6 |
| RTX 5080, RGBA32F | 46.4 | 88.3 | 170.2 |
| UHD 630, RGBA8 | 8.8 | 24.1 | 54.0 |
| UHD 630, RGBA16F | 20.2 | 45.8 | 53.5 |
| UHD 630, RGBA32F | 52.7 | 92.1 | 119.5 |

Two to six times cheaper than four reads, for every format. Four point samples cost the same as four Loads. *Estimated*
exceptions: AMD GCN 5 (Vega) filters wide formats slowly, so four reads may win there for RGBA16F and RGBA32F, and RDNA 2
for RGBA32F.

As a whole half-size downsample pass (3840 × 2160 to 1920 × 1080) both ways take the same time on the GTX 1660 and the
UHD 630 (GTX 1660 RGBA8 0.25 ms either way): a pass that only downsamples is limited by memory, so the cheaper read
shows only when the pass does more work. On the Iris 540 (slower memory) the bilinear pass is clearly faster: RGBA8
1.44 vs 1.67 ms, RGB10A2 1.37 vs 1.87 ms, RGBA32F 12.8 vs 14.0 ms.

### A 1:1 copy: Load or point sample?

- *Estimated:* **AMD RDNA and NVIDIA Ada / Ampere: use Load (`tex2Dfetch`) for formats of up to 32 bits per texel.**
  A single Load is much cheaper there than a sample: RDNA 2 2.9 vs 7.1, RX 7900 GRE (RDNA 3) 9.0 vs 30, RX 9070 XT 10.5 vs
  22, RTX 4060 Ti 14.3 vs 20.
- *Estimated:* **but not for 64- and 128-bit formats on Ampere / Ada:** there a Load is slower than a point sample (RTX
  4060 Ti RGBA16F 25.6 vs 20.1, RGBA32F 45.7 vs 22.5).
- Elsewhere (NVIDIA Maxwell, Pascal, Turing; AMD GCN 5; Intel Gen7.5, Gen9) a Load and a point sample cost the same.
- *Estimated:* **Intel Gen12 (Iris Xe): use a point sample, not Load,** for formats of up to 64 bits: in dependent reads
  a Load costs ~1.7x a point sample (RGBA8 17 vs 10 fma units). Whole copy passes take the same time either way there
  (RGBA8 2.28 ms both, RGBA16F 5.07 vs 5.13: memory bound).

Measured as whole full-screen copy passes (3840 × 2160): the GTX 1660 copies in the same time with Sample and with Load
in every format (RGBA8 0.42 ms, RGBA16F 0.85 ms); the UHD 630 too, except RGBA16F, where Load is 11% slower (5.65 vs
5.09 ms).

**What this means for ReShade's copy shader (our Load patch):** no change on GTX 16 / RTX 20 and Intel Gen9 (measured),
nor on GTX 900 / 10 and Vega (estimated); the expected win is on AMD RDNA and NVIDIA RTX 30 / 40 with 8- and 10-bit back
buffers, and a possible loss with HDR (RGBA16F) back buffers on RTX 30 / 40 and Intel Gen9. A copy pass is limited by
memory, so even there the gain is shader time, not necessarily frame time. Reports from RDNA and RTX 30 / 40 cards
running TexBench 0.6.0 will settle it.

### Reading 2 × 2 texels each on their own (min / max / median filters, edge detection): three gathers

Measured directly (here: the maximum of the four texels' RGB):

| fma units | 3 gathers | 4 Loads | 4 point samples |
|---|---|---|---|
| GTX 1660, RGBA8 | 28.7 | 57.9 | 58.1 |
| GTX 1660, RGBA16F | 50.5 | 59.5 | 58.2 |
| GTX 1660, RGBA32F | 58.1 | 58.5 | 63.4 |
| RTX 5080, RGBA8 | 46.7 | **35.3** | 87.5 |
| RTX 5080, RGBA16F | 87.7 | **51.7** | 87.0 |
| RTX 5080, RGBA32F | **88.1** | 169.9 | 129.3 |
| UHD 630, RGBA8 | 23.8 | 53.6 | 52.4 |
| UHD 630, RGBA16F | 45.5 | 53.2 | 52.0 |
| UHD 630, RGBA32F | 93.9 | 118.2 | 120.3 |

Three gathers (`tex2DgatherR` / `G` / `B`) are twice as fast as four reads for RGBA8 and still ahead for the wider
formats on the GTX 1660 and Intel. (The estimate from single reads said the opposite for 64-bit formats; the direct test
wins.) **Not on Blackwell:** on the RTX 5080 four Loads (`tex2Dfetch`) beat three gathers for RGBA8 and RGBA16F (35 vs 47,
52 vs 88); gathers win only for RGBA32F, and four point samples are the slowest way everywhere there. *Estimated:* AMD
RDNA and NVIDIA Ada (cheap Loads) likely follow Blackwell.

### Trilinear or anisotropic 2x: the same cost only on round footprints

Measured with the same gradients for both filters:

| fma units | trilinear, round | aniso 2x, round | trilinear, 2:1 | aniso 2x, 2:1 |
|---|---|---|---|---|
| GTX 1660, RGBA8 | 26.0 | 26.0 | 25.8 | 58.0 |
| GTX 1660, RGBA16F | 41.6 | 41.6 | 25.9 | 94.8 |
| UHD 630, RGBA8 | 23.1 | 23.3 | 23.1 | 52.9 |
| UHD 630, RGBA16F | 52.4 | 52.2 | 51.9 | 110.9 |

Where the footprint is round, anisotropic 2x costs exactly what trilinear costs (it has nothing to add). Where the
footprint is stretched, it takes its second tap: about 2× trilinear (up to 3.7× for RGBA16F on the GTX 1660) and a
sharper result than trilinear's blur. So anisotropic filtering is not free: use it where the sharpness on surfaces seen
at an angle is worth it. (The earlier "same cost" came from a test that read a smaller mip for the anisotropic case.)

### Color lookup tables: a 3D texture

A 3D LUT texture is never slower than the 2D-slices layout (two reads and a lerp, as in LUT.fx), and clearly faster at
64³: GTX 1660 36 vs 46, RX 9070 XT 58 vs 90, UHD 630 32 vs 41. At 32³ they cost the same.

### Skipping work: stencil or discarding whole tiles, not single pixels

Masking half the pixels with the stencil test, or discarding in whole 8 × 8 tiles, halves the time of a heavy pass on
every card. Discarding every other pixel saves nothing (GPUs shade 2 × 2 quads).

### Derivatives on NVIDIA: `ddx_fine` / `ddy_fine`

On NVIDIA (every generation tested) the fine derivatives cost half of the plain (coarse) ones: GTX 1660 3.0 vs 7.0, RTX
4060 Ti 5.9 vs 12.7, RTX 5080 6.0 vs 12.9. On AMD and Intel they cost the same, so `ddx_fine` is never worse.

### Clears are free

`ClearRenderTargets` costs nothing measurable on every card except Intel Gen7.5 (1.2 ms at 3840 × 2160): GPUs mark
the target as cleared instead of writing it. Clear instead of drawing a flat color.

## How formats and filters cost

- **Point and bilinear cost the same** for formats of up to 32 bits per texel on every card: filtering is free.
  **Exception: NVIDIA Blackwell in compute shaders** (RTX 5080, dependent reads): RGBA8 point 5.3, Load 12.8, bilinear
  19 fma units; in the pixel shader point and bilinear cost the same there too.
  **Exception: Intel Gen9** filters RGBA8 sRGB, RGB10A2 and RG11B10F at about a third of the rate (UHD 630: point 8.7,
  bilinear 23); its 8-bit, R16F, RG16F and R32F formats filter for free like everywhere else. Intel Gen12 (Iris Xe)
  no longer has this: sRGB, RGB10A2 and RG11B10F filter at full rate there.
- **64-bit formats (RGBA16F, RGBA16, RG32F):** bilinear and gather run at half rate on NVIDIA and Intel Gen9 (about 2.5×
  a point read), at full rate on AMD RDNA, and slowly on AMD GCN 5 (about 5×). Point reads and Loads stay at full rate
  everywhere.
- **RGBA32F:** bilinear about 2× point on NVIDIA and Intel; GCN 5 about 10×.

## Caches

- Reads within about 8 texels around a pixel cost no more than reading the pixel's own texel; from about 16 texels on
  they get slower (every card). Pixel shader and compute shader measure the same within about 10% on the GTX 1660 and
  UHD 630 (knee at 8–16 texels on both), so the pixel order (4 × 8 blocks on NVIDIA, 4 × 4 on Intel) makes no visible
  difference at these spreads.
- Rows and columns: on most cards reads spread 256 texels vertically cost more than the same spread horizontally
  (GTX 1050 3.6×, UHD 630 2.4×, RDNA 2 2.8×, GTX 1660 1.4×), so a separable blur's vertical pass is the dearer one.
  NVIDIA Ada is the reverse (RTX 4060 Ti: horizontal 1.4× vertical).

## Blending: blend state or shader

The 0.5.0 test was flawed (the blend source equalled the destination): lerp and min blending looked free on the GTX
1660. With the fix they cost what add does (RGBA8: 0.41 ms against 0.32 ms without blending). Measured again:

- **GTX 1660:** the blend state is 2–3% faster than reading the image in the shader and doing the math, for formats of up
  to 64 bits per pixel; RGBA32F blending is about 10% slower than the shader.
- **RTX 5080:** the same pattern, stronger: RGBA8 add by blend state 0.06 ms vs 0.08 ms in the shader, RGBA16F equal,
  RGBA32F blending 0.41 vs 0.34 ms (20% slower).
- **UHD 630:** within ±10% either way (RG11B10F blending about 10% faster, RGBA8 min blending 11% slower): no rule.

So the choice rarely matters on these cards; avoid RGBA32F blending on NVIDIA. From the flawed 0.5.0 test (to be
remeasured): Iris 540 showed the blend state clearly faster, the RX 9070 XT the shader (add and multiply up to 1.9× slower
as blending for RGBA16F).
