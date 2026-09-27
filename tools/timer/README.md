# sopt-timer and sopt-host (M4 benchmark harness)

**sopt-timer** is a ReShade add-on (`sopt-timer.addon64`). It needs ReShade *with full add-on
support*: the standard build skips `.addon` files.

**sopt-host** (`sopt-host.exe`) is a window that shows one fixed image with vsync off (DX11 or
Vulkan). It lets you run the bench without a game.

Both are built by CMake on Windows. CI uploads them as the `sopt-windows-tools` artifact of the
MSVC job.

## What sopt-timer measures

**Passive (always on).** It writes a GPU timestamp before the first technique and after every
technique ReShade renders. The add-on's settings in ReShade's Add-ons tab show, per technique:
- the median over the last 600 frames;
- the 10th to 90th percentile;
- the mean of the last 60 frames, which is what ReShade's Statistics tab computes.

**Bench.** Click "Run bench" in the add-on settings, or start `sopt-host --bench`. It then:
1. Loads every preset whose name starts with `sopt-` from the current preset's folder, one
   after the other. These are the test bundle's presets, one per effect and `SOPT_ALL` step.
2. Warms up for 120 frames once both techniques have rendered. Effects compile after a preset
   switch.
3. For every enabled pair `X_orig` / `X_sopt`, renders both itself on the same input: the frame
   before any effect. It alternates the order, A B B A in one frame and B A A B in the next,
   with one timestamp pair per run.
4. Takes the difference of the means (sopt - orig) within each frame, which cancels clock
   changes between frames.

After 300 frames it writes `sopt-timer.csv` to ReShade's base folder, one row per preset:
- the median µs for orig and for sopt, each with p10 and p90;
- the median difference (µs) with its p10 and p90, and as a percentage;
- the share of frames in which sopt was faster.

The header line gives the API, vendor, GPU and resolution. The displayed frame is restored, so
the screen shows the normal chain. When it is done, the original preset is loaded again.

Settings go in the `[SOPT_TIMER]` section of `ReShade.ini`:

| key | default |
|---|---|
| `WarmupFrames` | 120 |
| `Frames` | 300 |
| `Repeats` (runs per technique per frame) | 2 |
| `PresetPattern` (name prefix) | `sopt-` |
| `AutoRun` | 0 |
| `ExitWhenDone` | 0 |

Several CSVs (DX11 / Vulkan, AMD / NVIDIA) merge into one Markdown table:
`python3 tools/timer/timings.py dx11-amd.csv vulkan-nv.csv > timings.md`. Each cell reads
orig -> sopt in µs, followed by the median difference in % and a verdict. The verdict comes
from the difference's p10 and p90:
- faster: both are below 0;
- slower: both are above 0;
- same: anything else, meaning the difference is within the noise.

## sopt-host

```
sopt-host [--api dx11|vulkan] [--width 3840] [--height 2160] [--image file.png] [--frames N] [--bench]
```

**DX11:** put ReShade (with full add-on support) as `dxgi.dll` and `sopt-timer.addon64` next to
`sopt-host.exe`.

**Vulkan:** ReShade is a Vulkan layer, installed by the ReShade setup for the exe. The Vulkan
window's client area must be the full size; larger than the screen is allowed. The DX11 swap
chain is 4K regardless of the window.

**Image:** without `--image`, sopt-host shows a procedural test image: a hue sweep, a grey ramp,
colour patches, and smooth content with noise. It is the same on every run.

**Depth:** every frame draws a procedural scene into a depth buffer, the way a game's z
prepass does, so ReShade's generic depth picks it up and depth effects do real work:
- The scene is a ground plane up to a horizon, sky at the far plane, and three spheres.
- Depth is reversed Z (ReShade's default `RESHADE_DEPTH_INPUT_IS_REVERSED = 1`), near 0.1,
  far 1000.
- It is drawn as a 256 x 144 grid in 144 draw calls, because generic depth ignores depth
  buffers with 3 or fewer vertices or 8 or fewer draw calls.
- The format is D24S8 (typeless) on DX11 and D32 on Vulkan.
- `--no-depth` leaves it out.

The shaders are `tools/host/depth.hlsl` and `tools/host/depth.vert`; keep them in sync.
They are embedded as `depth_dxbc.h`, compiled with Microsoft's `D3DCompile` (vs_5_0,
entry VS, O3), and `depth_spv.h` (`glslangValidator -V depth.vert`). In D3D11,
`SV_VertexID` does not include a draw's start vertex, so the vertex index comes from a
vertex buffer.

**Closing:** `--bench` closes the window when the CSV is written. Shift+Esc closes it by hand.

## ReShade's own statistics (6.8.0 source, `runtime.cpp` / `runtime_gui.cpp`)

These are the reasons its numbers can be inconsistent:

- **Mean, not median.** Per technique it shows `moving_average<uint64_t, 60>`, which always
  divides by 60. After a reload or a clear, the value ramps up from 0 over 60 frames. A single
  hitch (driver, shader cache, another app) moves the mean for a whole second.
- **Only collected while the Statistics tab is drawn.** Collection turns on when the tab is
  drawn and off every GUI frame otherwise. After reopening, the mean mixes old and new samples
  for 60 frames. The first reads can come from query slots written long before, because the
  ring is 4 frames and it reads the oldest slot.
- **The overlay is open while you read it.** ImGui rendering and input handling run at the same
  time, and change the GPU load and clocks.
- **D3D10/11 timestamps.** The frequency is read once at start-up. Timestamp queries are never
  bracketed by a `TIMESTAMP_DISJOINT` query and disjoint intervals are never detected. D3D11
  defines timestamps only inside such a query. After a power-state or clock change, the values
  can be wrong. sopt-timer brackets each frame and drops disjoint frames.
- **Chain position and input.** A technique's time depends on the techniques before it (caches,
  clocks) and on its input image (branches). The bench measures both variants on the same
  input, interleaved.
- **Clock speed.** In a frame-capped game the GPU clocks down, and every time scales up. Run
  benches with vsync off and no frame cap (sopt-host does this) and compare only within one run.

## Tested

In the cloud, under Wine with DXVK on lavapipe (CPU):
- the DX11 path end to end: bundle presets load, pairs are found, 30 frames each, no dropped
  frames, CSV written, window closed;
- the Vulkan host presenting the test image.

ReShade as a Vulkan layer could not be tested there, because Wine's `vulkan-1` has no layer
support. Real timings need a real GPU.
