# Windows bench tools (M4)

Measures the real speed of sopt's variants on a Windows PC: every `X_orig` / `X_sopt` pair of a
test package is timed on the GPU, on DX11 and Vulkan, with a screenshot per preset.

## Quick start (one click)

1. Download the `sopt-windows-tools` artifact of the latest CI run (GitHub, Actions, the run's
   Artifacts) and extract it into a folder. It contains everything, `ReShade64.dll` included
   (ReShade 6.8.0 with full add-on support, built by CI from crosire's unchanged source; an
   official add-on build next to the script works too).
2. Extract the test package (`sopt-compare-*.zip`, a folder with `sopt-presets\` and
   `reshade-shaders\`) into the same folder.
3. Double-click `run-bench.bat`. A 4K window opens and closes by itself, first for DX11, then
   for Vulkan; each preset warms up, is timed for 300 frames and gets a screenshot.
4. Send the `results-<date>.zip` it writes next to the script: `sopt-timer-dx11.csv`,
   `sopt-timer-vulkan.csv`, the screenshots per API (`screenshots\dx11`, `screenshots\vulkan`;
   the bundle's presets end in the Compare effect, so a black image means orig and sopt render
   the same), ReShade's logs and the GPU name.

Options (`run-bench.bat -Apis dx11 -Frames 600 -Width 2560 -Height 1440`): `-Apis`, `-Frames`,
`-Width`, `-Height`, `-ReShade <dll>`, `-Package <folder>`. The run uses its own folder (`run\`)
with its own `ReShade.ini`, so no ReShade installation and no game is touched. Vulkan uses the
DLL as a layer through `VK_ADD_LAYER_PATH` / `VK_INSTANCE_LAYERS`, for sopt-host only.

## Contents

| file | what |
|---|---|
| `sopt-host.exe` | a window with a fixed test image and a synthetic depth buffer, vsync off (DX11 or Vulkan) |
| `sopt-timer.addon64` / `.addon32` | the ReShade add-on that times the techniques and runs the bench |
| `run-bench.bat`, `run-bench.ps1` | the one-click bench above |
| `sopt-fxc.exe` | Microsoft's fxc -O3 on one HLSL entry point (for `sopt-fx --backends`, see tools/fxc) |
| `timings.py` | merges several CSVs into one Markdown table |
| `ReShade64.dll`, `ReShade-LICENSE.md` | ReShade 6.8.0, full add-on support, unchanged (CI build) |
| `sopt-opbench.exe`, `measure-gpu.bat` | instruction costs on this GPU, for sopt's cost models (below) |

Sources: `tools/windows/timer` (add-on, timings.py), `tools/windows/host` (sopt-host),
`tools/windows/opbench` (sopt-opbench), this folder (scripts).

# sopt-timer and sopt-host (details)

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
| `Screenshots` (one per preset after the warm-up, ReShade's screenshot path) | 0 |

Several CSVs (DX11 / Vulkan, AMD / NVIDIA) merge into one Markdown table:
`python3 tools/windows/timer/timings.py dx11-amd.csv vulkan-nv.csv > timings.md`. Each cell reads
orig -> sopt in µs, followed by the median difference in % and a verdict. The verdict comes
from the difference's p10 and p90:
- faster: both are below 0;
- slower: both are above 0;
- same: anything else, meaning the difference is within the noise.

## sopt-host

```
sopt-host [--api dx11|vulkan|gl] [--width 3840] [--height 2160] [--image file.png] [--frames N] [--bench]
          [--no-depth] [--msaa N]
```

**DX11:** put ReShade (with full add-on support) as `dxgi.dll` and `sopt-timer.addon64` next to
`sopt-host.exe`.

**MSAA (DX11):** `--msaa 2|4|8` gives the back buffer that many samples (blt-model swap chain,
no tearing); the image is drawn into it with a small shader (`host/blit.hlsl`, embedded as
`blit_dxbc.h`). ReShade then renders into a resolve texture and copies back with its internal
copy shader every frame (tools/reshade/TESTING.md).

**OpenGL (`--api gl`):** a compatibility context showing the image with `glDrawPixels` (no
depth), for checking ReShade's OpenGL path: put ReShade as `opengl32.dll` next to `sopt-host.exe`.

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

The shaders are `tools/windows/host/depth.hlsl` and `tools/windows/host/depth.vert`; keep them in sync.
They are embedded as `depth_dxbc.h`, compiled with Microsoft's `D3DCompile` (vs_5_0,
entry VS, O3), and `depth_spv.h` (`glslangValidator -V depth.vert`). In D3D11,
`SV_VertexID` does not include a draw's start vertex, so the vertex index comes from a
vertex buffer.

**Closing:** `--bench` closes the window when the CSV is written. Shift+Esc closes it by hand.

## sopt-opbench (instruction costs)

`measure-gpu.bat` (or `sopt-opbench.exe [--adapter N] [--list] [--filter text] [--reps N] [--groups N]`)
measures what single instructions and instruction patterns cost on this PC's GPU, to calibrate
sopt's cost models (rdna3, nvidia). No ReShade or game needed; close GPU-heavy programs first.
It takes a few minutes and writes `opbench-<gpu>.csv` (send that) and `opbench-dxbc\` (the HLSL
and DXBC of every test).
It needs the Microsoft Visual C++ 2015-2022 redistributable (x64), which almost every gaming PC already
has: https://aka.ms/vs/17/release/vc_redist.x64.exe

How: each test is a step `x = f(x, c)` repeated in long chains in a D3D11 compute shader,
compiled at run time with Microsoft's D3DCompile -O3 (what ReShade does on D3D9-12), then by the
driver. The constants come from a constant buffer and differ per step, so nothing folds. Every
step ends in `mad(y, c.x, c.y)`; the plain mad step is the reference, and a test's cost is its
time per step minus its base test's, in sopt's units (4 = one fma). Configurations: `tput`
(8 independent chains per thread, 1M threads: throughput), `dep` (1 chain per thread, 1M
threads), `lat` (1 chain, one thread group: latency relative to mad's). Besides single ops it
tests the context effects the rdna3 model assumes: `omod2` / `omodhalf` (x * 2, x * 0.5 after
rcp: AMD output modifier, expected ~0; `omod3` is the control), `max3`, `minmax`, `satmad`,
`contract`. The GPU's clock is not known, so latency is relative too.

Version 2 (0.1.0): a 2-second warm-up, then every test is measured twice, forward and backward
through the list, each time with a fresh reference mad right before it, so a GPU clock change only
moves the tests around it. The summary at the end shows the throughput costs in a fixed order (the
same on every GPU) with a bar and a comment per test, the other GPUs Windows reports (with
`--adapter N`), and warnings when the reference drifted more than 5% or the two passes disagree.
The CSV has the GPU, vendor, device, driver and the drift once in `#` header lines, then one row
per configuration and test (`units_vs_base` is the average of `vs_base_fwd` and `vs_base_bwd`).
The exe asks NVIDIA / AMD drivers for the discrete GPU on laptops with switchable graphics
(`NvOptimusEnablement`, `AmdPowerXpressRequestHighPerformance`). Releases: the
`sopt-opbench-<version>.zip` on https://github.com/CeeJayDK/sopt/releases (exe, measure-gpu.bat,
README.txt).

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
