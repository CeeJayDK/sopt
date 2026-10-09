Two small changes to ReShade's internal shaders (patch against v6.8.0 attached,
`internal-shaders.patch`, 11 files, +100/-24). Both were found while running a shader
superoptimizer over ReShade's own shaders, and both were tested on a GTX 1660; the copy change
also on an Intel Iris 540.

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
  Vibrance + Curves and with Vibrance alone: the screenshots have identical SHA256 hashes, on a
  GTX 1660 and on an Intel Iris 540.
- The pass is bandwidth bound, so there is no visible FPS difference. The saving is issue slots
  and helper lanes.
- The copy sampler is gone on the C++ side too: the copy pipeline layout has only the source view,
  and the sampler state, its creation, descriptor push and destruction are removed. This final
  version was tested again on the GTX 1660: screenshots SHA256-identical to the unchanged build.

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
- Non-square textures: the old shader reads past the edge once the level above is 1 texel wide
  or high, and averages with what comes back there (zeros), e.g. levels 6-8 of a 256 x 32 texture
  are wrong. The bilinear fetch clamps to the edge, which gives the right box filter there. Checked
  with `sopt_MipTest.fx` (its "RGBA8 256x32" format) under Wine / Mesa: unchanged DLL red on levels
  6-8, patched none. (Mirror instead of clamp gives the same values: the sample sits exactly on the
  edge.)
- Integer formats: unpatched 6.8.0 sends them (R8UI ... RGBA32I) to the same compute shader, whose
  float `sampler2D` / `image2D` bindings are undefined in GL for integer textures; Mesa returns
  zeros (`sopt_MipTestInt.fx`). The patch leaves them on the old shader. So the bilinear shader
  could replace the old file outright; integer textures would need their own `usampler2D` /
  `uimage2D` shader, or no generated mips at all (D3D11's GenerateMips and a linear Vulkan blit
  do not support them either).

**3. Suggestion: the same for D3D12 (`d3d12-mipmaps.patch`, on top of the first)**

Only as an illustration. `mipmap_linear_cs_5_0.hlsl` is `mipmap_cs_5_0.hlsl` with `load_and_reduce`
as one `SampleLevel` at the shared corner, from an SRV of the pass's source level, and a static
sampler (linear, clamp) in the root signature. On the C++ side each pass gets its own descriptor
block (SRV + the 7 UAVs, `mips[0]` now a null view, since it is not read), and the state
transitions are per level: the source level of a pass stays a shader resource while the levels it
writes are unordered access. Integer formats keep the old pipeline; if the new one fails to create,
it falls back with a warning. It compiles (mingw / clang, and the shader with fxc) but is untested on
hardware.

It also fixes the edge problem, which the D3D12 shader has like the old GL one: for a level whose
parent is 1 texel wide or high, half of each 2x2 block lies outside the texture (zeros from UAV
loads, or junk from threads outside it in groupshared memory). The sampler clamps for the first level
of a pass, and `reduce_clamped` replaces the outside neighbours with the texel inside for the later
ones (it only triggers on 1 texel parents; odd sizes still drop the last row / column as before).
`d3d12_mipmap_model.py` is a CPU model of both shaders (dispatch, morton order, groupshared steps)
against a reference box filter: the original gets levels wrong on non-square textures (256 x 32:
levels 6-8, 128 x 8: 4-7, 100 x 60: 6, 32 x 512: 6-9, 300 x 7: 3-8), the patched one none.

**4. The `(v0 + v1 + v2 + v3) * 0.25` pattern**

Checked whether `((v0 + v1) * 0.5 + (v2 + v3) * 0.5) * 0.5` (AMD's output modifier makes `* 0.5`
free), a tree `((v0 + v1) + (v2 + v3)) * 0.25` or a mad chain does better. fxc keeps the source
structure (the `* 0.5` form is 5 DXBC instructions, the others 4). AMD's driver compiler (RGA,
Vulkan, gfx1100) turns all of them into the same 3 adds and a multiply by 0.25 per component (it
folds the halves back into 0.25 and reorders the adds), except the mad chain (a mul and 3 fmas: also
4). NVIDIA (ptxas): the `* 0.5` form is 5, the others 4. So for the count the current form is as good
as any; the bilinear fetch is the real saving.

Where the values come from texture reads, the mad chain (weight each sample as it arrives, add the sum
so far: `s = v0 * w; s = mad(v1, w, s); ...`) has a shorter tail: the compiler issues all four loads
and then waits for them in order, so the chain starts on v0 while the rest are in flight. In AMD's
ISA (RGA) the sum form has 8 instructions (4 adds + 4 muls for a float4) after the last load lands,
the mad chain 4 (one fma per component); same total.

**Not tested yet:** AMD hardware, the OpenGL change on Intel, and timings beyond "no visible
difference".
