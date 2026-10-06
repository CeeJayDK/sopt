# Test Host and the Windows tools

The Test Host (owner, 2026-10-06: one test host for every API ReShade supports, with a menu) runs ReShade in front of
`sopt-host`, a window with a fixed test image and a synthetic depth buffer, on Direct3D 9, 10, 11 and 12, Vulkan and
OpenGL. It runs the IEEE 754 test effect (tools/reshade/sopt_IEEE754.fx, findings in tools/reshade/IEEE754.md) or any
effect with a screenshot per API, opens the host with ReShade on a chosen API for testing by hand, and times every
`X_orig` / `X_sopt` pair of a SweetOpt test package (M4 harness).

## Quick start

1. Download the `Test-Host` artifact of the latest CI run (GitHub, Actions, the run's Artifacts) or the
   release's `Test-Host-<version>.zip`, and extract it into a folder. It contains everything, `ReShade64.dll` included
   (ReShade 6.8.0 with full add-on support, built by CI from crosire's unchanged source; an official add-on build or
   `ReShade_Setup_*_Addon.exe` next to `Test-Host.bat` works too).
2. Double-click `Test-Host.bat` and press a number: 1 / 2 IEEE 754 test on every / one API, 3 an effect from
   `Effects\` on every API, 4 the host with ReShade on a chosen API (Home opens the overlay), 5 the benchmark (extract
   the test package, `sopt-compare-*.zip`, into the same folder first), 6 / 7 / 8 the Results / Effects folders and
   the guide.
3. Results go to `Results\`: per test `<effect>-<date>\` (a PNG per API, `ReShade-<api>.log`, `gpu.txt`,
   `summary.txt`) and its zip; the benchmark writes `Results\bench-<date>\` (`sopt-timer-<api>.csv`, the screenshots
   per API, ReShade's logs) and its zip.

Every run uses its own folder (`run\`) with its own `ReShade.ini`, so no ReShade installation and no game is
touched. ReShade goes next to sopt-host as `d3d9.dll` (Direct3D 9), `dxgi.dll` (Direct3D 10 / 11 / 12),
`opengl32.dll` (OpenGL), or as a Vulkan layer through `VK_ADD_LAYER_PATH` / `VK_INSTANCE_LAYERS` for sopt-host only
(`bin\common.ps1`). Test screenshots come from the sopt-timer add-on's shot mode (`SOPT_TIMER_SHOT=N`): after N frames
with a rendered technique and 7 s (ReShade's banner is gone by then) it saves one screenshot and closes the window;
after 60 s without a rendered technique (an effect that does not compile) the screenshot is taken anyway.

Scripts (in `bin\`; Windows PowerShell 5.1): `run-test.ps1 [-Apis all|dx9,dx10,dx11,dx12,vulkan,gl] [-Effect F.fx |
-Effect ?] [-Interactive] [-Width 1920] [-Height 1080] [-Frames 30] [-ReShade dll]`, `run-bench.ps1 [-Apis dx11,vulkan]
[-Frames 300] [-Width 3840] [-Height 2160] [-ReShade dll] [-Package folder]`, `common.ps1` (shared).

## Contents

Since 0.6.0 (owner, 2026-10-05) every Windows download has only its launcher and `README.html` in the main
folder, the programs in `bin\` and the other pages / licenses in `Docs\` (`tools/windows/stage.ps1` stages all
three for CI and the release). This one (CI artifact `Test-Host`, release `Test-Host-<version>.zip`):

| file | what |
|---|---|
| `Test-Host.bat` | the menu |
| `README.html` | the guide (source: `tools/windows/docs/TestHost.html`) |
| `Effects\` | the test effects (`sopt_IEEE754.fx`, `sopt_MipTest.fx`); users add their own |
| `bin\sopt-host.exe` | the test window: fixed image, synthetic depth, vsync off, `--api dx9\|dx10\|dx11\|dx12\|vulkan\|gl` |
| `bin\sopt-timer.addon64` / `.addon32` | the ReShade add-on: technique timings, the benchmark, the test screenshots |
| `bin\run-test.ps1`, `bin\run-bench.ps1`, `bin\common.ps1` | the scripts behind the menu |
| `bin\sopt-fxc.exe` | Microsoft's fxc -O3 on one HLSL entry point (for `sopt-fx --backends`, see tools/fxc) |
| `bin\timings.py` | merges several CSVs into one Markdown table |
| `bin\ReShade64.dll`, `Docs\ReShade-LICENSE.md` | ReShade 6.8.0, full add-on support, unchanged (CI build) |
| `Docs\IEEE754.md` | what the IEEE 754 test found in ReShade 6.8.0 |

GPU Blueprint (OpBench, TexBench, ShaderInfo, `GPU-Blueprint.bat`) is its own download (CI artifact `GPU-Blueprint`,
release `GPU-Blueprint-<version>.zip`): `GPU-Blueprint.bat`, `README.html`, `bin\` with the three programs (their
`Reports\` goes next to the batch file: benchkit `reportsDir` steps out of a folder named bin), `Docs\` with one
page per program. SweetOpt (CI artifact `SweetOpt-windows`, release `SweetOpt-<version>-windows-x64.zip`):
`SweetOpt.bat`, `README.html` (docs/sweetopt/README.html), `bin\sopt.exe`, `bin\sopt-fx.exe` (and the menu's
`SweetOpt.ini`), `Docs\`.

Sources: `tools/windows/timer` (add-on, timings.py), `tools/windows/host` (sopt-host),
`tools/windows/opbench` (OpBench), `tools/windows/texbench` (TexBench), `tools/windows/shaderinfo` (ShaderInfo), `benchkit.hpp` (shared), `tools/windows/docs` (README.html, Docs\\*.html), this folder (scripts).

# sopt-timer and sopt-host (details)

**sopt-timer** is a ReShade add-on (`sopt-timer.addon64`). It needs ReShade *with full add-on
support*: the standard build skips `.addon` files.

**sopt-host** (`sopt-host.exe`) is a window that shows one fixed image with vsync off (DX11 or
Vulkan). It lets you run the bench without a game.

Both are built by CMake on Windows. CI uploads them as the `Test-Host` artifact of the
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
- The format is D24S8 (typeless) on Direct3D 9 / 10 / 11 / 12 and D32 on Vulkan; OpenGL has no depth pass yet.
- `--no-depth` leaves it out.

The shaders are `tools/windows/host/depth.hlsl`, `depth9.hlsl` (the same in float math for shader model 3, with a
ps_3_0 that Direct3D 9 needs next to a vs_3_0) and `depth.vert`; keep them in sync. They are embedded as
`depth_dxbc.h` (vs_5_0: Direct3D 11 and 12), `depth10_dxbc.h` (vs_4_0), `depth9_dxbc.h` (vs_3_0 + ps_3_0), all
compiled with Microsoft's `D3DCompile` (O3), and `depth_spv.h` (`glslangValidator -V depth.vert`). In D3D11,
`SV_VertexID` does not include a draw's start vertex, so the vertex index comes from a
vertex buffer.

**Closing:** `--bench` closes the window when the CSV is written. Shift+Esc closes it by hand.

## OpBench (instruction costs; sopt-opbench until 0.2.0)

`GPU-Blueprint.bat` (or `OpBench.exe [--adapter N] [--list] [--filter text] [--reps N] [--groups N]`)
measures what single instructions and instruction patterns cost on this PC's GPU, to calibrate
sopt's cost models (rdna3, nvidia). No ReShade or game needed; close GPU-heavy programs first.
It takes a few minutes and writes `Reports\opbench-<gpu>.csv` (send that) and `Reports\Shaders\OpBench\` (the HLSL
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
rcp: AMD output modifier, expected ~0; `omod4` likewise; `omod8`, `omod0.25`, `omod0.125` are
DX9-era scales, expected to cost a multiply now; `omod3` is the control), `max3`, `minmax`, `satmad`,
`contract`. The GPU's clock is not known, so latency is relative too.

Version 2 (0.1.0): a 2-second warm-up, then every test is measured twice, forward and backward
through the list, each time with a fresh reference mad right before it, so a GPU clock change only
moves the tests around it. The summary at the end shows the throughput costs in a fixed order (the
same on every GPU) with a bar and a comment per test, the other GPUs Windows reports (with
`--adapter N`), and warnings when the reference drifted more than 5% or the two passes disagree.
The CSV has the GPU, vendor, device, driver and the drift once in `#` header lines, then one row
per configuration and test (`units_vs_base` is the average of `vs_base_fwd` and `vs_base_bwd`).

Version 3 (0.2.0, owner 2026-10-03: "the test itself does not take long, so we might as well include
a lot"): chains can be `float2..4` (dot2..4, cross, length, distance, normalize, reflect against 2-4
fmas), `uint` (integer / bit ops and int <-> float conversions; random odd 32-bit constants, step
`(x ^ a) * b`) or `min16float` (half precision; the CSV header and the summary say whether the driver
runs it at 16 bits); scalar tests of intrinsics fxc writes out (smoothstep, fmod, sincos, tan, atan,
atan2, asin, acos). The other GPUs are listed right after the `GPU:` line, the three modes are
explained as they start, and the summary has sections. `Docs\OpBench.html` (in the zip) describes
every test.

Version 4 (0.3.0, owner 2026-10-03): renamed OpBench (`OpBench.exe`, zip `OpBench-<version>.zip`).
Block graphics use only the full block and the half blocks (the 1/8 blocks of 0.2.0 are missing in
the Windows console fonts): a title box ("OpBench <version> - by CeeJay.dk"), bars with 4 levels per cell (bright / dark color pairs) and a
progress bar with 6 levels per cell. The summary has an Ops column (Cost / 4). A test whose two
readings disagree (more than 0.75 units or 15%) is measured again, up to 6 passes, until more than
half of its readings agree (their mean is the result; "no consensus" otherwise); the CSV adds
`passes`, `consensus` and `readings`.

Version 5 (0.4.0, owner 2026-10-03): more output modifier scales after `rcp` (`omod4`; `omod8`,
`omod0.25`, `omod0.125`: free on DX9-era GPUs, measured to know whether they still are) and `trunc`
(round toward zero, suggested by a tester).

Version 6 (0.5.0, owner 2026-10-04): the rest of ReShade FX's math (hyperbolic functions, ldexp / frexp /
modf, isnan / isinf, f16 conversions, refract, faceforward, matrices, determinant, integer divide / modulo);
"(shorter is better)" under each section, aligned with the graphs; GPU names in their vendor's color; the
progress bar stays within 70 characters. Also a percentage scale (0% 25% 50% 75% 100%) above each progress bar.
100% is the end of the two passes over every test; extra passes for tests whose readings disagree
continue past it in yellow (`+` without colors).
Results per section (owner, 2026-10-04: users can read while the rest is measured): OpBench measures section
by section (all three configurations, the bases with the first section that needs them, forward then
backward within the section) and prints each section's table as soon as it is done, with the progress bar
below; graphs on a fixed scale (a full bar = 100 = 25 mads, longer costs fill it). TexBench prints each
section once its last test is measured. At the end: the GPU box, drift / consensus warnings, the footer.
The exe asks NVIDIA / AMD drivers for the discrete GPU on laptops with switchable graphics
(`NvOptimusEnablement`, `AmdPowerXpressRequestHighPerformance`). Releases (since 0.5.0, owner: one zip for
testers): `GPU-Blueprint-<version>.zip` (GPU-Bench before 0.6.0) on https://github.com/CeeJayDK/sopt/releases with OpBench, TexBench and
ShaderInfo and GPU-Blueprint.bat. Since 0.6.0 (testers' notes, owner 2026-10-05): one batch file (the menu, with
"another graphics card"), reports in `Reports\` and zipped into `Reports-<cards>.zip` (bin\zip-reports.ps1: the card models, e.g. Reports-GTX-1660+UHD-630.zip) after every run, the
guide as HTML (README.html, a hub, and `Docs\<Program>.html`), a beep when a run ends (before the pause).

## TexBench (texture costs)

`GPU-Blueprint.bat` (or `TexBench.exe [--adapter N] [--list] [--filter text] [--reps N] [--groups N]`)
writes `Reports\texbench-<gpu>.csv` and `Reports\Shaders\TexBench\` next to the exe. Owner's idea (2026-10-04): ballpark
costs of texture operations next to math (when a lookup table beats computing), and every format ReShade
supports measured instead of assumed; a separate program because it about doubles OpBench's run time.
Same method as OpBench (chains, a fresh reference mad before every reading, extra readings until they
agree; shared code in `benchkit.hpp`), but each test is measured in all configurations in a row and its
texture freed afterwards (19 formats at 4096 x 4096 would not fit together). Sections: formats with
coherent reads (bilinear, 1024 x 1024, each thread within one texel of its pixel) and random reads
(Load, 4096 x 4096); access and filtering on RGBA8 (Load, point, bilinear, gather, trilinear,
anisotropic via SampleGrad); a 1D LUT (256 x 1), a 3D LUT (32^3) and random reads from 512^2 to 8192^2;
pixel shader derivatives (ddx / ddy, fine / coarse, fwidth) and Sample with automatic mip selection
(bilinear, trilinear, 4:1 anisotropic); render target writes per format (GB/s, 3840 x 2160).
`tools/windows/docs/TexBench.html` explains each test. Releases: in `GPU-Blueprint-<version>.zip`.

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
