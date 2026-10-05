# Changelog

What is new in each release, newest first. The release workflow copies the section of the version it
builds (`## <version>` up to the next `## `) into the release notes, so add to the top section as things land.

## 0.6.0

### Web page
- **GPU Blueprint online:** the cost models SweetOpt uses, per architecture and per operation, the cards measured and the pixel order pictures, rebuilt automatically as reports come in.
- **Pages at ceejay.dk/sopt/:** SweetOpt (what it does, with the faster `sign()` per architecture), the rewrite library (every verified rewrite with its saving on each architecture), GPU Blueprint (every DirectX 11 architecture, the ones without a cost model yet greyed out) and the TexBench findings (formats x filters, caches, writes, blending, and every card's pixel order pictures).

### All downloads
- **Tidy main folder:** each zip now has only its launcher (`SweetOpt.bat`, `GPU-Blueprint.bat`, `run-bench.bat`) and its guide (`README.html`) in the main folder; the programs are in `bin`, the other pages and the licenses in `Docs`.
- **SweetOpt has an HTML guide** (`README.html`), replacing QUICKSTART.txt. The bench tools zip no longer includes GPU Blueprint (it has its own download).

### SweetOpt (was sopt)
- **New cost model `intel-gen7.5`** (Intel HD Graphics 4600, Haswell), from a GPU Blueprint report: its math unit (rcp, sqrt, exp2, sin ...) costs about as much as an add, unlike Gen9's ~3x. In the menu as key J.
- **New name:** sopt is now **SweetOpt**, the super sweet shader optimizer. The programs keep their short names `sopt-fx` and `sopt`, and variant files keep their `SOPT_*` switches; the download is `SweetOpt-<version>-windows-x64.zip`.
- **Menu for Windows:** double-click `SweetOpt.bat`: choose the Shaders folder and what to optimize (folder and file dialogs, no typing of paths), the time per statement and the graphics card family, start, open the results, edit value ranges. It remembers the choices.

### GPU Blueprint (was GPU-Bench)
- **New name:** GPU-Bench is now **GPU Blueprint** (`GPU-Blueprint.bat`, `GPU-Blueprint-<version>.zip`): it maps out what each part of a graphics card costs. OpBench, TexBench and ShaderInfo keep their names.
- **One batch file:** `GPU-Blueprint.bat` is now the only one, a menu that does it all. New: **another graphics card** (shows the cards, you type the number). The separate measure / shader-info batch files are gone.
- **Reports folder:** everything to send goes to `Reports\` (shader dumps to `Reports\Shaders\`), and after every run a zip named after the cards in it (e.g. `Reports-GTX-1660+UHD-630.zip`) holds the reports, ready to send.
- **HTML guide:** `README.html` is the hub, with one page per program in `Docs\` (what every test measures, how to read the results), replacing the text files.
- **Send the reports:** at the end of a run it asks whether to send them (or menu key 9 later, blinking while there are new reports): yes opens CeeJay's upload page (a Dropbox file request, no account needed) and shows the zip, ready to drag onto the page.
- **Key 1 measures every graphics card** in the PC (with one card: that card); 2 the main card only, 3 another card.
- **Beep** when a run is done, before the "press a key" pause (it used to come after the key press).
- **Results start sooner:** OpBench and TexBench compile the shaders in the background, section by section, while the previous section is measured (no compiling pause at the start).
- **Clearer headings:** the three ways of measuring are called "Cost, many in parallel", "Cost, one dependent chain" and "Latency, one at a time" (every number is a cost: lower is better).
- **fp32 score on AMD GCN:** a new test, `fma1` (one fma with a single constant), gives the score its real rate where the reference fma's two constants cost a second instruction (Radeon Vega APUs showed about a third of their fp32 rate).
- **TexBench: "do this, not that" tests:** the same result read different ways, to choose between them: a 2 × 2 average as one bilinear read, 4 fetches, 4 point samples or 3 gathers; 2 × 2 texels each on their own as 4 fetches, 4 point samples or 3 gathers; full-screen copies with Sample against Load, and half-size downsamples as one bilinear read against 4 fetches (RGBA8, RGB10A2, RGBA16F, RGBA32F).
- **TexBench: fairer comparisons:** trilinear and anisotropic 2x on the same footprints; the cache spread test also in a pixel shader (it follows the pixel order); the blend test's source no longer equals the destination (lerp and min blending looked free on some cards because of it).
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
