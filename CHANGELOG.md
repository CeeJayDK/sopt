# Changelog

What is new in each release, newest first. The release workflow copies the section of the version it
builds (`## <version>` up to the next `## `) into the release notes, so add to the top section as things land.

## 0.6.0

### sopt / sopt-fx
- **Menu for Windows:** double-click `sopt-menu.bat`: choose the Shaders folder and what to optimize (folder and file dialogs, no typing of paths), the time per statement and the graphics card family, start, open the results, edit value ranges. It remembers the choices.

### GPU-Bench
- **One batch file:** `GPU-Bench.bat` is now the only one, a menu that does it all. New: **another graphics card** (shows the cards, you type the number). The separate measure / shader-info batch files are gone.
- **Reports folder:** everything to send goes to `Reports\` (shader dumps to `Reports\Shaders\`), and after every run `GPU-Bench-Reports.zip` holds the reports, ready to send.
- **HTML guide:** `README.html` is the hub, with one page per program in `Docs\` (what every test measures, how to read the results), replacing the text files.
- **Beep** when a run is done, before the "press a key" pause (it used to come after the key press).
- **Results start sooner:** OpBench and TexBench compile the shaders in the background, section by section, while the previous section is measured (no compiling pause at the start).
- **Clearer headings:** the three ways of measuring are called "Cost, many in parallel", "Cost, one dependent chain" and "Latency, one at a time" (every number is a cost: lower is better).
- **TexBench is shorter:** the formats × filtering table no longer runs the dependent-chain mode (it matched the first mode on every card tested), about 25-30 seconds less.

## 0.5.0

### GPU-Bench (new: one zip for testers)
- **TexBench (new):** what texture work costs next to math. Every ReShade format × Load / point / bilinear / gather / trilinear / anisotropic, texture functions (offsets, gathers, grad, 1D / 3D, size queries, address modes), color lookup tables (2D vs 3D), cache sizes and cache use, random reads, render target writes (noise / smooth / flat), blending (hardware blend vs shader math), pass states (render targets, clears, mipmaps, stencil, discard), storage writes and atomics, pixel shader derivatives, and how the GPU orders pixels (images included).
- **ShaderInfo (new):** what the Vulkan driver reports about compiled shaders (register counts, instruction / cycle counts on Intel, hardware counters, extensions).
- **OpBench:** the remaining ReShade FX / HLSL functions, compute ops (groupshared memory, barriers, atomics, arrays, branches) and parallel issue tests (an fma beside integer, min/max, conversion, rcp or fp16 work).
- **Both benchmarks:** a score box at the end (TFLOPS, GTexels/s, GPixels/s, GB/s), results shown section by section while the rest is measured, progress in the window title (the console no longer jumps while you scroll), GPU name in the title and "done" at the end.
- **Reliability:** bad timer readings are retried instead of skewing the calibration (fixes a stall on Intel UHD 630), a clear message if the GPU stops responding, each GPU tested once even if Windows lists it twice.
- **GPU-Bench.bat:** a menu for everything; `measure-main-gpu.bat` runs all three programs on the main GPU, `measure-all-gpus.bat` on every GPU in the PC.

### sopt / sopt-fx
- **Compute shaders:** ReShade FX compute passes and HLSL `[numthreads]` entry points; stores to textures and buffers are regions.
- **Intel instruction counts** from the real Intel driver (`--export-spirv`, `ShaderInfo --batch`, `--driver-stats`): Intel becomes a measured vendor with its own `SOPT_AUTO` pick.
- **Fewer registers:** variants that need fewer registers without being faster are kept, labeled "fewer registers (not faster)".
- **Cost model** `nvidia-maxwell` (GTX 900 / 800M series).
- **Terminal output:** title box, section headings and progress bars with time left.
