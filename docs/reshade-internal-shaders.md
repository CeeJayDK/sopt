# ReShade's own shaders (6.8.0)

ReShade ships a few shaders of its own in `res/shaders/`. They are written per API (HLSL for
D3D, GLSL / SPIR-V for OpenGL / Vulkan), not in ReShade FX, so sopt-fx cannot read them; this
is a manual review with sopt's measuring tools (fxc -O3 for DXBC, RGA for RDNA3 ISA).
Owner (2026-10-01): an optimization is anything that runs faster or is more accurate.

| shader | used for | when it runs |
|---|---|---|
| `fullscreen_vs.hlsl` + `copy_ps.hlsl` | copy of the resolved back buffer into the real one (`runtime.cpp`, `_copy_pipeline`) | every frame on D3D10/11/12 when the back buffer is MSAA or needs a format conversion |
| `mipmap_cs_5_0.hlsl` | `generate_mipmaps` on D3D12 | after effects write textures that have mipmaps |
| `mipmap_cs_430.glsl` | `generate_mipmaps` on OpenGL (formats usable as images; others use `glGenerateMipmap`) | same |
| `imgui_*` | the overlay | only while the overlay is drawn |

Vulkan generates mipmaps with `vkCmdBlitImage`, D3D11 with `GenerateMips`: no ReShade shader.

## 1. copy_ps: Load instead of Sample (faster, identical result)

The copy is 1:1 (viewport = back buffer size) with a point sampler and clamp, so the sample
at the interpolated pixel centre reads exactly the texel at `int2(vpos.xy)`:

```hlsl
Texture2D t0 : register(t0);

void main(float4 vpos : SV_POSITION, out float4 col : SV_TARGET)
{
	col = float4(t0.Load(int3(vpos.xy, 0)).rgb, 1.0); // alpha cleared as before
}
```

- RDNA3 (gfx1100, RGA): 26 instructions -> 11. Gone: the attribute interpolation
  (`lds_param_load`, `v_interp_*`), the whole-quad mode setup (`s_wqm_b64`; Sample needs helper
  lanes for derivatives, Load does not) and the sampler descriptor load.
- DXBC (fxc -O3): `sample` + 2 `mov` -> `ftoi` + `mov` + `ld` + 2 `mov`; the sampler and the
  TEXCOORD input are no longer needed (the vertex shader can stay as it is).
- The copy is a full-screen pass every frame in that configuration; it is bandwidth bound,
  so the gain is in issue slots and helper lanes, not memory.
- The sampler state and its descriptor push could then be dropped on the C++ side.

## 2. mipmap_cs_430.glsl: one bilinear fetch per output texel (faster)

```glsl
void main()
{
	// One bilinear fetch at the shared corner of the 2x2 source texels averages them.
	const vec2 src_size = vec2(textureSize(src, src_level));
	const vec2 uv = vec2(gl_GlobalInvocationID.xy * 2u + 1u) / src_size;
	imageStore(dest, ivec2(gl_GlobalInvocationID.xy), textureLod(src, uv, float(src_level)));
}
```

- RDNA3: 4 `image_load_mip` -> 1 `image_sample_l` (46 -> 43 instructions; the size query and
  divide add ALU). Mipmap generation is limited by texture throughput, so 4x fewer texture
  operations is the point.
- Needs a sampler with a linear min filter on `src` (a sampler object bound to unit 0) and only
  works for filterable formats: the integer formats in the list (`*UI`, `*I`) keep the
  texelFetch path. 32-bit float filtering is supported on desktop GL.
- Accuracy: the hardware filter weights are exactly 0.5 here, so for 8/10/16-bit unorm and
  half float the result matches the float average up to the format's own rounding; to be
  confirmed on hardware.
- D3D12 (`mipmap_cs_5_0.hlsl`) could do the same for its first level (`load_and_reduce`), but it
  reads mip 0 through a UAV; a bilinear fetch needs an SRV and a sampler in the root signature,
  a larger change.

## 3. imgui HDR conversion per vertex (faster, overlay only)

`imgui_ps_4_0.hlsl` converts the vertex colour to scRGB / HDR10 (`pow`, a 3x3 matrix, the PQ
curve with two more `pow`) in the pixel shader, for every pixel of the overlay, although the
input is an interpolated vertex colour. Converting in the vertex shader (the push constants
with `color_space` are visible there too) does it once per vertex. ImGui's quads are almost all
one colour, where this is exact; gradients (colour pickers, multi-colour rects) would then
interpolate in the converted space instead. Only matters with the overlay open in HDR.

## 4. Nothing to gain

- `fullscreen_vs.hlsl`: 3 vertices per pass.
- `mipmap_cs_5_0.hlsl` `reduce`: `(v0 + v1 + v2 + v3) * 0.25` is already what the compiler wants.

## Accuracy notes (by design, for the record)

- Both mipmap shaders are 2x2 box filters: for an odd size (e.g. 1080 -> 540 -> 270 -> 135 ->
  67) the last row / column of the larger level is never read.
- Mipmaps are generated through the texture's linear view (`tex.srv[0]`), so textures written
  with `SRGBWriteEnable` are averaged in their encoded values, on every API.

## sopt on a ReShade FX port

`examples/fx/ReShadeInternal.fx` ports `copy_ps` and the mipmap reduce to ReShade FX.
- `copy_ps` has no arithmetic (a sample and a constant alpha): nothing for the search.
- The reduce `(v0 + v1 + v2 + v3) * 0.25` (4 x float4 = 16 components, above sopt-fx's 8, so
  searched per channel with `sopt`, inputs [0, 1]): no cheaper form for exact, rel 1e-6 or
  8-bit budgets (20 s each). Four values need three additions and the scale; a mad can fold the
  scale into one of them but not save an operation.

So the gains above are all in the choice of texture operation (Load instead of Sample, one
bilinear fetch or Gathers instead of four loads), which sopt treats as fixed inputs.

## Gather (owner's question, 2026-10-01)

- `copy_ps`: no help; each pixel needs one texel and a Gather returns four.
- Mipmaps: `GatherRed/Green/Blue/Alpha` at the shared corner return one channel of all four
  texels. For RGBA that is 4 fetches, as many as the 4 loads now (alpha is needed: effect
  textures carry it), and the bilinear fetch above is 1. Gathers win for one- and two-channel
  formats (R8, R16F, R32F, R32UI, RG...: 1 or 2 fetches instead of 4) and for integer formats,
  which cannot be filtered; formats without alpha (R11G11B10) need 3. Like the bilinear
  fetch, it needs an SRV and a sampler: the GL path has them, D3D12 reads through UAVs.

## Next

The proposals need a hardware check (sopt-timer can time the copy pass with an MSAA or
format-converting back buffer) before they go to crosire.
