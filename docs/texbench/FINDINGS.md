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

### `SampleLevel(…, 0)` is never slower than `Sample`

Measured first on a GTX 1660 and a UHD 630 (TexBench 0.9.2, 2026-10-22): in throughput a bilinear `Sample` and
`SampleLevel` at level 0 or 1 cost the same (39.1 / 39.1 / 39.2 and 35.3 / 34.9 / 35.3); in a chain of reads that
depend on each other `Sample` is about 3 units slower (42.1 vs 39.3 and 42.7 vs 39.6). Where the level is known, give
it.

### Reads hide some math; `tex2D` costs nothing

Timed through the Test Host at 3840 x 2160 (sopt-timer, ad hoc effect, one test image, 2026-11-03). Extra mads that
add no time, on top of a shader's own math (D3D11):

| Reads in the pass | 0 | 1 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| GTX 1660 (1 mad = 4.7 us) | 37 | 44 | 46 | 78 | 132 |
| UHD 630 (1 mad = 50 us) | 9 | 10 | 22 | 44 | 94 |

So a pass has a floor (about 470 us on the 1660, even with no read) and past 4 reads each read hides about 6 more
mads. A read costs about 11 mads on both. Math the pass can hide does not show up in time: a cheaper 4-tap B-spline
gains 0 us alone on the 1660, 10% alone on the 630, 2.7% on both once 400 mads make the pass math-limited.

`tex2D` against `tex2Dlod` at level 0 (4 reads): no measurable difference, D3D9 and D3D11 on the 1660, D3D11 on the
630, also math-limited (+/- 0.5 us on the 1660, under 10 us of 24 ms on the 630). The 4 extra `mul`s fxc writes for
`tex2Dlod` on D3D9 would show as about 19 us.

### Sampling precision: 8 bits, rounded to the nearest

By 2026-11-03 ten reports (7 cards: GT 530, GT 1030, GTX 1660, UHD 630, HD 3000, Arc B580, AMD Renoir) have this test:
all exactly 257 positions, none finer than 8 bits. HD 3000 keeps only 9 levels inside one colour step in RGBA8, and it
and Renoir round a half step down (first change at 129 / 65536). No RTX or RDNA report has it yet.

The same two cards place a bilinear read in 1/256 steps between two texels (257 results from 65536 reads) and round a
position to the nearest step (the first change at 1/512 texel), in RGBA8, RGBA16F and R32F alike: an offset of 1/257
texel reads as 1/256. Inside one 8-bit colour step they keep all 257 levels.

### Interpolators: one or two values are free, many are not

GTX 1660 and UHD 630, a full-screen pass busy with math: a texture coordinate from the vertex shader costs the same
as working it out in the pixel shader (one mad), and a float4 of which two values are read costs the same as a float2.
In bulk each interpolated value costs about 0.5 fma per pixel on the GTX 1660 and about 2 on the UHD 630 (8 float4:
+0.05 ms and +2.7 ms at 3840 × 2160); `nointerpolation` values cost far less.

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

## Small lookup tables: texture or something else

Can anything beat a texture for a small dither table or LUT? Fma per lookup at 8 lookups per pixel, 3840 x 2160, into R8,
GTX 1660 / UHD 630 (2 to 4 clean runs, within 0.1 fma on the 1660 and about 1 on the 630; AMD has no timing yet). Runs where the 1660 fell to
990-1200 MHz partway through (every time about 1.6x, while the run's reference drift still read below 1%) were thrown out.

| Dither by pixel position | GTX 1660 | UHD 630 |
|---|---|---|
| texture, 4 x 4 to 64 x 64 (Sample or Load) | 11.5 | 11.9 |
| Bayer 4 x 4, from tools/bitsearch: `((y * 18) & 20) + ((a * 36) & 40)`, x and y masked, a = x ^ y, scale 1/64 | **2.0** | **6.7** |
| Bayer 8 x 8, from tools/bitsearch: `((y * 73) & 84) + ((a * 146) & 168)`, x and y masked, a = x ^ y, scale 1/256 | **2.0** | **9.9** |
| Bayer 4 x 4, 2 multiplies: `((a * 9) & 10) \| ((y * 9 >> 1) & 5)`, a = x ^ y | **2.8** | **7.2** |
| Bayer 8 x 8, 2 multiplies: `((a * 73 >> 1) & 42) \| ((y * 73 >> 2) & 21)` | **3.7** | 11.7 |
| Bayer 4 x 4 as a table in 2 uints | **3.9** | **9.7** |
| Bayer 4 x 4 / 8 x 8, shifts and masks | **5.0** / **9.8** | 11.3 / 19.0 |
| Bayer 4 x 4 / 8 x 8, floor and frac (D3D9 can) | 19.9 / 31.7 | 14.7 / 22.9 |
| Bayer 4 x 4 / 8 x 8, a float for each bit (D3D9 can) | 14.9 / 23.0 | 22.4 / 33.9 |
| packed 4 x 4, 16 levels in 2 uints, only the masks needed | **3.9** | **9.7** |
| packed 8 x 8, 16 levels, a uint per row, only the masks needed | **5.8** | 13.9 |
| packed 8 x 8, all 64 levels in 16 uints, only the masks needed | 12.8 | 28.1 |
| any 4 x 4: rows selected as vectors, on floats (D3D9 can) | 12.8 | 25.7 |
| any 4 x 4: a cubic in x and y (D3D9 can) | 49 | 51 |
| select tree, 4 x 4 / 8 x 8 / 16 x 16 | 10.4 / 42.5 / 176 | 12.4 / 75 / 306 |
| static const, 4 x 4 / 8 x 8 / 16 x 16 / 32 x 32 / 64 x 64 | 59 / 122 / 204 / 613 / 1393 | 26 / 27 / 26 / 28 / 108 |
| constant buffer, the same sizes | 60 / 123 / 203 / 588 / 1335 | 26 / 26 / 26 / 26 / 108 |

| Curve (1D), 16 / 32 / 64 entries | GTX 1660 | UHD 630 |
|---|---|---|
| texture, nearest or interpolated | 11.0 | 10.5 |
| static const, nearest | 14 / 30 / 53 | 25 / 6 / 25 (see below) |
| static const, 64, index smooth over the screen | **4.0** | 4.5 to 27 (see below) |
| select tree, nearest | 6.9 / 18 / 39 | 19 / 38 / 73 |
| packed, 16 entries | 7.9 | 12.7 |
| static const + lerp | 32 / 63 / 109 | 54 / 17 / 55 |
| select tree + lerp | 19 / 41 / 82 | 42 / 79 / 149 |

| Color (3D), 4 x 4 x 4 | GTX 1660 | UHD 630 |
|---|---|---|
| texture, nearest / trilinear | 19.9 / 22.5 | 18.6 / 19.8 |
| static const, nearest, color changes slowly | **12.0** | 18.7 |
| static const, nearest, color differs per pixel | 116 | 34 |
| select tree, nearest | 117 | 214 |
| static const, trilinear | 117 | 222 |

- **Textures cost the same at every size** from 4 x 4 to 64 x 64, and interpolating costs (almost) nothing extra. For anything
  interpolated the texture wins everywhere by far: about 2 to 14 times cheaper than doing it in the shader.
- **NVIDIA can beat the texture:** almost 6 times with bitsearch's Bayer forms, 3 times with a packed 4 x 4, 2 times with a packed
  8 x 8 of 16 levels; also a select tree up to 16 entries, and a const table whose index changes slowly.
- **Intel can, for Bayer and at 4 x 4:** bitsearch's Bayer is 43% cheaper than the texture at 4 x 4 and 16% at 8 x 8, any packed
  4 x 4 table 18%. Any other 8 x 8 loses.
- **The form of the decoder matters.** Bayer 8 x 8 went from 9.8 to 2.0 (1660) and 19 to 9.9 (630): a multiply spreads the bits
  instead of shifts and masks, `+` replaces `|` (the bits never overlap), and the value stays high so that the float's scale
  replaces the right shift. OpBench says why: on the 1660 multiply, left shift, and, add cost about 0, right shift, or, xor
  about 4; on the 630 and, add 3.7 and the rest 7.2. Dropping masks a shift does not need (it uses only 5 bits of the amount)
  helps Intel at 4 x 4 (11.2 to 9.7) and NVIDIA at 8 x 8; it hurt Bayer on Intel (shifts and masks, 11.3 to 13.1 at 4 x 4).
  Writing bits straight into a float instead of converting was slower on both; itof and utof cost the same.
- **A const table on NVIDIA depends on the index, not only on its size.** The same 64 entries cost 4 fma with a smooth index
  and 53 with a noisy one; in 3D, 12 against 116. fxc writes the same read for all of them (`icb[r0.x]`), and a real constant
  buffer, whose values the driver cannot know, costs the same at every size. So values known to the compiler are not the
  reason. It looks like cost per different index that neighbouring pixels read (a guess at the cause; the measured part is
  the index and the size). Select trees and textures do not depend on the index.
- **A const table on Intel depends on its contents.** The same code with other numbers in the table moved 1D 32 from 27 to 6
  fma and the smooth 1D 64 from 27 to 4.5 (three levels, about 5, 12 and 26). Why is not known; one run says little there.
- 16 levels are too few for real 8 x 8 blue noise (it has 64 thresholds): with all 64 levels, packing loses to the texture on
  both (12.8 against 11.5 on the 1660, close). Packing pays for 4 x 4 (16 thresholds fit exactly) and for 8 x 8 only if 16
  levels are good enough.
- **D3D9 (ps_3_0, checked with fxc):** no integers or bit operations, and an indexed const or uniform array compiles to a chain
  of `cmp`. Of the forms it can compile, none beat the texture here, measured through D3D11: the best are 12.8 on the 1660
  (rows selected as vectors) and 14.7 on the 630 (Bayer floor and frac). A pattern that need not be a table is far cheaper:
  R2 1.6 / 4.0, IGN 4.0 / 6.0 (see noise sources). Not yet measured on D3D9 itself.
- sopt found nothing here for the float forms on these cards' models (only `g - floor(g)` to `frac(g)`), and does not search
  integer math. The decoders above are by hand and from `tools/bitsearch` (a small search over integer formulas).
- At 1 lookup per pixel the cheap forms all read about 0: the pass is limited by writing the pixels, so a single lookup in
  a very small shader hides them. Const tables on NVIDIA show anyway (4 x 4: 29 fma, 8 x 8: 92).

**Where the texture starts to win** (any table, not Bayer; fma per lookup, texture 11.5 / 12):

| Tile | GTX 1660, best form | UHD 630, best form |
|---|---|---|
| 4 x 4, all 16 levels | **3.9** | **9.7** |
| 5 x 5, all 25 levels (6 bits a value, a uint per row) | **9.1** (integer %) | 17.0 |
| 6 x 6, 32 of 36 levels (5 bits) | **9.8** (integer %) | 17.9 |
| 6 x 6, all levels | 13.9 | 23.3 |
| 7 x 7, 16 levels / all levels | 11.9 / 16.3 | 18.4 / 26.4 |
| 8 x 8, 16 levels / all levels | **5.8** / 12.8 | 13.9 / 28.1 |

So with all levels the last win is 5 x 5 on NVIDIA and 4 x 4 on Intel. A tile that is not a power of two pays for its index: on the
1660 integer % beats a fixed point fraction and a float's frac (5 x 5: 9.1, 11.5, 16.3), on the 630 the fixed point fraction
wins and % is worst (17.0, 18.4 for frac, 22.7).

**Blue noise against the procedural dithers.** Cost as above, quality from flat grays between the table's levels, dithered to 1 bit:
RMS after a blur of 1 and 2 pixels, and the error of the mean gray (too few levels shows here as banding). Lower is better.

| Pattern | 1660 | 630 | blur 1 | blur 2 | mean gray |
|---|---|---|---|---|---|
| R2 | 1.7 | 3.8 | 0.040 | 0.011 | 0.0001 |
| IGN | 4.1 | 5.6 | 0.042 | 0.013 | 0.0003 |
| Hoskins hash12 | 12.7 | 13.7 | 0.116 | 0.058 | 0.0026 |
| Bayer 4 x 4 (bitsearch); also the best blue noise 4 x 4 | 2.0 | 6.7 | 0.031 | 0.018 | 0.0157 |
| Bayer 8 x 8 (bitsearch) | 2.0 | 9.9 | 0.038 | 0.008 | 0.0035 |
| Blue noise 5 x 5, packed | 9.1 | 17.0 | 0.036 | 0.012 | 0.0100 |
| Blue noise 6 x 6, 32 levels, packed | 9.8 | 17.9 | 0.040 | 0.015 | 0.0116 |
| Blue noise 8 x 8, 16 levels, packed | 5.8 | 13.9 | 0.039 | 0.019 | 0.0157 |
| Blue noise 8 x 8, texture | 11.5 | 11.8 | 0.037 | 0.009 | 0.0035 |
| Blue noise 64 x 64, texture | 11.5 | 11.8 | 0.038 | 0.010 | 0.0001 |

- These numbers do not show what makes blue noise look good (no visible structure): Bayer scores as well as blue noise here and
  plainly looks worse. Judge the look by eye. Void-and-cluster tiles; up to 8 x 8 the best of 160 by these scores and by stripes (one
  picked on the 2-pixel blur alone gave a badly striped 6 x 6). The best 4 x 4 one is exactly Bayer 4 x 4; other tries gave horizontal lines at mid gray. On small tiles the 2-pixel blur mostly measures the number of levels (any tile of 16 levels: 0.018).
- A faster small table closes the cost gap: 4 x 4 beats IGN on NVIDIA (2.0 against 4.1) and comes close on Intel (6.7 against 5.6); R2 stays the cheapest. But
  the tiles it helps are too small to have blue noise's character or enough levels; a texture costs the same at 64 x 64.

**Registers (AMD, offline with RGA, GLSL):** scratch 0 in every const form, also as a local array; VGPRs do not grow with
the table (RDNA 3 / 4: 6 at 1 lookup, 10-11 at 8, 34 with lerp at 8, 242 with trilinear at 8). RDNA 3 / 4 read the table from
memory (ISA size does not grow with it). GCN 5 on the older RGA writes the table's values into the code: 2.6 KB for 8 x 8 at
8 lookups, 76 KB for trilinear. No result for RDNA 2. Not the D3D11 path: AMD's D3D11 compiler has shown scratch for local
arrays before (Monochrome), and that needs a card or an AMD driver to measure.

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
