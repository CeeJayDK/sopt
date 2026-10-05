# TexBench findings

What the TexBench reports show, as advice: **do this, not that**. Each point says for which GPUs it holds and gives the
numbers behind it. Costs are in fma units (1.0 = one fused multiply-add at full rate, throughput: many reads in
parallel), so they compare directly with the cost models. Lower is better.

Cards so far (15): NVIDIA Maxwell (GTX 860M, Quadro M5000M), Pascal (GTX 1050), Turing (GTX 1660, GTX 1660 Ti, RTX
2060), Ampere / Ada (RTX 3050 Laptop, RTX 4060 Ti, RTX 4090 Laptop); AMD GCN 5 (Vega 8), RDNA 2 (Ryzen 7000 iGPU), RDNA 4
(RX 9070 XT); Intel Gen7.5 (HD Graphics 4600), Gen9 (Iris 540, UHD 630). Laptop runs are often throttled: their
absolute numbers are low, their ratios still match their desktop family.

Points marked *preliminary* rest on tests that were not quite fair; TexBench 0.6.0 measures them properly, and they
will be updated when its reports come in.

## Do this, not that

### Averaging 2 × 2 texels (downsampling, box blurs): one bilinear read

One bilinear read at the shared corner of four texels returns their average. It costs the same as one point read for
formats of up to 32 bits per texel on every card tested, so it is up to 4 times cheaper than four reads and the math.

| | one bilinear | four reads |
|---|---|---|
| GTX 1660, RGBA8 | 9.9 | 39.5 |
| Iris 540, RGBA8 | 8.7 | 34.9 |
| RX 9070 XT, RGBA8 | 23.1 | 41.9 |

For 64-bit formats (RGBA16F) bilinear filtering runs at half rate on NVIDIA and Intel Gen9, and still wins (about
1.5×). **Exception: AMD GCN 5 (Vega)** filters wide formats slowly: four reads beat one bilinear for RGBA16F (24 vs 27)
and clearly for RGBA32F (27 vs 69). RDNA 2 also prefers four reads for RGBA32F (26 vs 29).

### A 1:1 copy: Load or point sample?

- **AMD RDNA and NVIDIA Ada / Ampere: use Load (`tex2Dfetch`) for formats of up to 32 bits per texel.** It is much cheaper there
  than a sample: RDNA 2 2.9 vs 7.1, RX 9070 XT 10.5 vs 22, RTX 4060 Ti 14.3 vs 20.
- **But not for 64- and 128-bit formats on Ampere / Ada:** there Load is slower than a point sample (RTX 4060 Ti
  RGBA16F 25.6 vs 20.1, RGBA32F 45.7 vs 22.5).
- Everywhere else (NVIDIA Maxwell, Pascal, Turing; AMD GCN 5; Intel Gen7.5, Gen9) Load and point sample cost the same.

**What this means for ReShade's copy shader (our Load patch):** the biggest win is on AMD RDNA (the read costs less
than half) and NVIDIA RTX 30 / 40 (about 30% less) with 8- and 10-bit back buffers; no change on GTX 900 / 10 / 16, RTX
20, Vega and Intel; possibly a loss with HDR (RGBA16F scRGB) back buffers on RTX 30 / 40. A full-screen copy is mostly
limited by memory bandwidth, so the gain shows as less shader time per pixel, not always as less frame time. TexBench
0.6.0's copy passes (Sample against Load, per format) measure the whole pass.

### Reading 2 × 2 texels each on their own (min / max / median filters, edge detection)

- **8-bit formats (RGBA8) on NVIDIA Maxwell to Turing, Intel and AMD GCN: three gathers (`tex2DgatherR` / `G` / `B`)**
  for the RGB of the four texels beat four reads by about 25% (GTX 1660: 29.6 vs 39.5).
- **On AMD RDNA and NVIDIA Ada: four Loads** are cheaper than both (RDNA 2: 11.7 vs 21.3 for the gathers).
- **64-bit and wider formats: four point reads or Loads,** never gathers: a gather is as slow as a bilinear read there
  (GTX 1660 RGBA16F: three gathers 77, four points 40). Exception: Intel Gen7.5 (HD 4600), where gathers are faster for
  every format.

### Trilinear or anisotropic 2x (*preliminary*)

On NVIDIA (Maxwell to Ada), Intel Gen9 and AMD RDNA, anisotropic 2x costs the same as trilinear for formats of up to 32 bits per
texel and is never blurrier, so prefer it. **Exceptions:** AMD GCN 5 (Vega 8: trilinear 13.9, aniso 2x 17.6) and
Intel Gen7.5, where trilinear is about as cheap as bilinear (HD 4600: 9.7 vs 9.6) and aniso 2x costs 24.8. Not fair yet:
the aniso test read a smaller mip level than the trilinear test.

### Color lookup tables: a 3D texture

A 3D LUT texture is never slower than the 2D-slices layout (two reads and a lerp, as in LUT.fx), and clearly faster at
64³: GTX 1660 36 vs 46, RX 9070 XT 58 vs 90, UHD 630 32 vs 41. At 32³ they cost the same.

### Skipping work: stencil or discarding whole tiles, not single pixels

Masking half the pixels with the stencil test, or discarding in whole 8 × 8 tiles, halves the time of a heavy pass on
every card. Discarding every other pixel saves nothing (GPUs shade 2 × 2 quads).

### Derivatives on NVIDIA: `ddx_fine` / `ddy_fine`

On NVIDIA (every generation tested) the fine derivatives cost half of the plain (coarse) ones: GTX 1660 3.0 vs 7.0, RTX
4060 Ti 5.9 vs 12.7. On AMD and Intel they cost the same, so `ddx_fine` is never worse.

### Clears are free

`ClearRenderTargets` costs nothing measurable on every card except Intel Gen7.5 (1.2 ms at 3840 × 2160): GPUs mark
the target as cleared instead of writing it. Clear instead of drawing a flat color.

## How formats and filters cost

- **Point and bilinear cost the same** for formats of up to 32 bits per texel on every card: filtering is free.
  **Exception: Intel Gen9** filters RGBA8 sRGB, RGB10A2 and RG11B10F at about a third of the rate (UHD 630: point 8.7,
  bilinear 23); its 8-bit, R16F, RG16F and R32F formats filter for free like everywhere else.
- **64-bit formats (RGBA16F, RGBA16, RG32F):** bilinear and gather run at half rate on NVIDIA and Intel Gen9 (about 2.5×
  a point read), at full rate on AMD RDNA, and slowly on AMD GCN 5 (about 5×). Point reads and Loads stay at full rate
  everywhere.
- **RGBA32F:** bilinear about 2× point on NVIDIA and Intel; GCN 5 about 10×.

## Caches

- Reads within about 4–8 texels around a pixel cost no more than reading the pixel's own texel on every card; from about
  16 texels on they get slower. Measured in compute shaders so far; TexBench 0.6.0 repeats it in a pixel shader, to see
  whether it follows each GPU's pixel order (4 × 8 pixel blocks on NVIDIA, 8 × 8 on AMD, 4 × 4 on Intel).
- Rows and columns: on most cards reads spread 256 texels vertically cost more than the same spread horizontally
  (GTX 1050 3.6×, UHD 630 2.4×, RDNA 2 2.8×, GTX 1660 1.4×), so a separable blur's vertical pass is the dearer one.
  NVIDIA Ada is the reverse (RTX 4060 Ti: horizontal 1.4× vertical).

## Blending (*preliminary*)

In the 0.5.0 test the blend source equalled the destination, which seems to have made lerp and min blending free on
some cards (GTX 1660: exactly the time of no blending at all). Add and multiply were less affected:

- **NVIDIA:** the blend state costs the same as reading the image in the shader and doing the math. RGBA32F blending
  is slower than the shader (up to 1.75×).
- **Intel Gen9 (Iris 540):** the blend state is clearly faster (3.3 vs 4.8 ms for an RGBA8 add); the UHD 630 showed no
  difference.
- **AMD RDNA 4 (RX 9070 XT):** the shader is faster (add and multiply blending up to 1.9× slower for RGBA16F).
