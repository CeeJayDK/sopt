Two small changes to ReShade's internal shaders (patch against v6.8.0 attached,
`internal-shaders.patch`, 9 files, +98/-5). Both were found while running a shader
superoptimizer over ReShade's own shaders, and both were tested on a GTX 1660.

**1. copy_ps.hlsl: `Load` instead of `Sample` (D3D10/11/12)**

This shader copies the resolve texture back into the back buffer every frame when the back
buffer is multisampled or has no alpha (B8G8R8X8 / R8G8B8X8). The copy is 1:1 with a point
sampler, so the texel under the pixel centre can be read directly:

```hlsl
col = t0.Load(int3(vpos.xy, 0));
col.a = 1.0;
```

- It no longer needs the interpolated TEXCOORD, the sampler, or helper lanes for derivatives.
  On RDNA3 (RGA, gfx1100) it goes from 26 to 11 instructions; the attribute interpolation,
  `s_wqm` and the sampler descriptor load are gone. The DXBC is `ftoi` + `ld` instead of `sample`.
- The result is bit-identical. Tested with D3D11 and a 4x MSAA back buffer at 1920x1080, with
  Vibrance + Curves and with Vibrance alone: the screenshots have identical SHA256 hashes.
- The pass is bandwidth bound, so there is no visible FPS difference. The saving is issue slots
  and helper lanes.
- Not in the patch, to keep it minimal: the copy sampler and its descriptor push could now be
  dropped on the C++ side.

**2. OpenGL mipmap generation: one bilinear fetch per output texel**

A second compute shader (`mipmap_linear_cs_430.glsl`) does one `textureLod` at the shared corner
of each 2x2 block, where the filter weights are exactly 0.5. It uses a sampler object with
linear filtering and clamp to edge, bound to unit 0 only while generating mips. This replaces
four `texelFetch`.

- Integer formats keep the existing shader, since they cannot be filtered. If the new program
  fails to link, it falls back to the existing shader with a warning in the log.
- 4 texture operations become 1. On RDNA3 that is 4 `image_load_mip` becoming 1 `image_sample_l`.
- Accuracy was checked with a test effect (`sopt_MipTest.fx`, attached) that compares every level
  with the exact 2x2 average of the level above, for levels 1-5. The new shader's maximum error
  is half a step of the format, i.e. correct rounding, for RGBA8, RGB10A2 and RGBA16F; R32F
  differs only in the last bits. The current shader gives the same results except for RGBA16F,
  where it is off by almost one fp16 step on this driver. It averages in fp32, and `imageStore`
  appears to truncate when converting to fp16, while the filtered fetch comes back already
  rounded. So the new path is equal or more accurate.
- The patch also adds an info log line ("Using bilinear mipmap generation shader ...") to confirm
  which path runs during testing. It can be removed.
- D3D12 (`mipmap_cs_5_0.hlsl`) could do the same, but it reads through a UAV; a bilinear fetch
  would need an SRV and a sampler in the root signature. That is not part of this patch.

**Not tested yet:** AMD and Intel hardware, and timings beyond "no visible difference".
