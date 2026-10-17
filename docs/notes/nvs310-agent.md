# Task for an agent on the NVS 310 PC: why GPU Blueprint stops on this card

You run on the owner's test PC with an NVIDIA NVS 310. Find out why OpBench, StageBench and TexBench stop on this
card, and fix it in the tools without changing their results on other cards. Work in the repo
https://github.com/CeeJayDK/sopt (clone it; read CLAUDE.md first, then grep docs/notes/history.md for "NVS 310").

## The card and what is known
- NVIDIA NVS 310: Fermi GF119, sm_21, 48 cores, 512 MB, device id 0x107D, about 0.08 TFLOPS (66x slower than a
  GTX 1660). Old PC with a BIOS (no UEFI). The owner has 4 of these cards (swapping one rules out a bad card).
- Driver now 392.68 (Windows shows 23.21.13.9268: the last 5 digits are NVIDIA's number), the newest for Fermi.
  The NVIDIA control panel setting graphics only / graphics and compute changes nothing.
- Every timed run stops with "the GPU has not finished a measurement in 60 seconds":
  OpBench and StageBench (pixel) in "the 2-second warm-up (mad)", TexBench at its first test "mad". The screen went
  blank for a second during the warm-up (a Windows driver reset, TDR, is the suspicion).
- On the older driver 353.62 (10.18.13.5362): StageBench pixel and vertex ran through (GPU Blueprint 0.7.11,
  docs/stagebench/nvidia-nvs-310-*.csv: mad 4.0 ms per iteration over 1024 x 1024 pixels); OpBench and TexBench
  (compute) stopped the same way. So compute shaders failed even then.
- 0.7.17 sized the warm-up to the card (8x a calibrated run, at most the old 256 iterations): still stops, even
  OpBench. So the long warm-up batches were not (or not the only) cause.
- 0.7.17 also writes the device removed reason into the stop line ("# stopped: ... (device removed reason 0x...)",
  the last line of Reports\opbench-NVIDIA_NVS_310.csv). Read it first: 0x00000000 = no reset; 0x887A0006 / 0x887A0007
  = Windows reset the driver.

## The code
- tools/windows/benchkit.hpp: `Timer` (about line 750): every measurement is a D3D11 timestamp pair inside a
  timestamp-disjoint query; `wait` polls `GetData` and gives up after 60 s (`fail`, which appends the stop line).
  `gCurrent` names what is being measured.
- tools/windows/opbench/sopt_opbench.cpp: OpBench and StageBench (SOPT_STAGEBENCH, the same source); `calibrate`
  (doubles iterations from 2 until a dispatch takes 2 ms), the warm-up (search "Warming up"), `kGroupsFull` 16384
  groups of 64 threads, `kConfigs`.
- tools/windows/texbench/texbench.cpp: its own `calibrate` (starts at 1024 groups / 16 pixel rows) and warm-up.
- Options: `OpBench.exe [--adapter N] [--list] [--adapters] [--filter text] [--reps N] [--out file.csv] [--groups N]`
  (StageBench and TexBench the same). ShaderInfo.exe times nothing (does the card work at all?).

## How to work
1. No compiler needed: push a branch to GitHub, CI (.github/workflows/ci.yml) builds it with MSVC; download the
   Windows tools with `gh run download <run id> -n GPU-Blueprint-<version>` (artifact `GPU-Blueprint` in older
   runs). Find the run with `gh run list --branch <branch>`.
2. Run one tool at a time with a narrow filter (`--filter mad`) and look at its CSV and Reports\Results\*.html.
3. Ideas, cheapest first (test, do not assume):
   - Does ShaderInfo run? Does `OpBench.exe --filter mad --groups 64` (a tiny dispatch) still stop?
   - Is it the timing, not the work? Try a build that waits on a D3D11_QUERY_EVENT after the work (no timestamps)
     and times on the CPU: if that finishes, the timestamp or disjoint queries never complete on this driver.
   - Is it compute only? StageBench (pixel) ran on 353.62: does it run now with a tiny filter?
   - A first dispatch that is slow on Fermi: try smaller first calibration steps.
   - If nothing in the tools helps, say so with the evidence, and suggest the owner try driver 353.62 again or
     another of the 4 cards.
4. A fix must leave fast cards unchanged (same numbers on a GTX 1660 / UHD 630: say what you changed for them).

## Rules (from the owner)
- Ask before design changes that are not needed for this fix; brief, direct messages.
- Branch + pull request; the owner merges. Raise the last digit of `project(sopt VERSION ...)` in CMakeLists.txt for
  a build the owner should run, and add a plain-English line to the top section of CHANGELOG.md.
- Never install drivers, change registry, TDR or other system settings yourself: tell the owner what to change.
- No personal data in the repo (names of people next to reports or notes). No AI model names or IDs in commits.
- Batch files keep CRLF line ends (.gitattributes).
- Write what you found (measurements, what failed, the cause) in docs/notes/history.md, short, near the other
  NVS 310 notes.
