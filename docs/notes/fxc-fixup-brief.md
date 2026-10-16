# Brief: an fxc fix-up project (owner's idea, 2026-10-16)

A prompt for a separate project, kept here because sopt / GPU Blueprint collect the evidence. Update the evidence
section as reports come in; hand the whole file to the new project's first session.

## Prompt

Build a fix-up step for DXBC shader bytecode produced by Microsoft's fxc (`d3dcompiler_47.dll`, `D3DCompile`, -O3).
fxc is closed source and frozen since DXC replaced it, and its optimizer still follows a 2000s cost model (fewest DXBC
instructions, fewest constants). Some of its rewrites cost speed on today's GPUs, because the drivers recompile DXBC and
cannot undo them. The step reads DXBC, rewrites known bad patterns into what the driver handles best, re-signs the
container (DXBC checksum) and passes it on. Results must be bit-identical to fxc's output wherever the shader is not
`precise` and the rewrite is exact (e.g. `add r, r` -> `mul r, l(2.0)` is exact for every float).

Delivery, in order of reach:
1. A library: `bytes in -> bytes out` (DXBC SM4 / SM5 first; D3D9 SM3 bytecode later or never).
2. A ReShade add-on: ReShade's `create_pipeline` event lets add-ons replace shader bytecode, which covers a game's own
   precompiled shaders (most games ship bytecode compiled offline and never call d3dcompiler at run time) and
   ReShade's effects alike.
3. A proxy `d3dcompiler_47.dll` next to a program's exe that loads the real one from System32, forwards every export
   and fixes up `D3DCompile` / `D3DCompile2` / `D3DCompileFromFile` results: covers programs that compile at run
   time. Do not patch Microsoft's DLL itself (signed, closed, license). Online games' anti-cheat may reject proxy DLLs.

Also worth proposing to crosire (ReShade): use DXC for Direct3D 12 (it accepts DXIL; DXC keeps `fmul x, 2.0`).

Measure every rewrite before shipping it: GPU Blueprint's OpBench (in the sopt repo, tools/windows/opbench) times a
step per GPU family; compiling a test with fxc `/Od` (D3DCOMPILE_SKIP_OPTIMIZATION) shows what the driver does with
the untouched form. A rewrite ships for a GPU family only where it measured faster and never slower elsewhere.

References: Microsoft's tokenized program format (`d3d11TokenizedProgramFormat.hpp`, Windows Driver Kit); DXBC readers
and writers in DXVK (dxbc), vkd3d-shader (Wine, LGPL, also writes DXBC and SM1-3) and DXC's dxilconv (DXBC -> DXIL,
MIT); the DXBC checksum (a modified MD5, reimplemented in those projects). Prior art for patching bytecode after fxc:
Aras Pranckevicius, "Solving DX9 Half-Pixel Offset" (aras-p.info, 2016): Unity rewrote compiled vs_3_0 bytecode
(oPos writes redirected to a free temp, then mad / mov into oPos with a constant in c255). D3D9 bytecode has no
checksum (format: MSDN "Direct3D Shader Codes"); the limits he names apply here too: a free temp, a free constant,
free instruction slots (SM3: 512).

## Evidence (from sopt / GPU Blueprint; details in docs/notes/history.md)

1. **x * 2 (measured, costs speed).** fxc writes `x * 2.0` as `add x, x`, `a * b * 2.0` as `dp2` (a * b + a * b),
   a float4 `a * b * 2.0` as `mul` + `add`, `rcp(x) * 2.0` as `rcp` + `add`; `* 4.0` stays a mul. No option avoids it
   except `/Od` (all optimization off) and `/Gis` (marks everything precise, which stops the driver's free scales:
   precise x4 cost 0.95 on the GTX 1660). NVIDIA (Maxwell to Ada) and AMD (output modifier) multiply a result by
   2^-3 .. 2^3 for free. GTX 1660, OpBench scale tests: with fxc -O3 x2 costs 0.96 fma, x4 / x8 / x0.5 / x0.25 /
   x0.125 cost 0; compiled with `/Od` x2 costs 0.00. Fix: `add r, a, a` -> `mul r, a, l(2.0)` (and `dp2 r, a.xx,
   b.xx` -> `mul` + `mul l(2.0)`) where the driver folds the scale into the instruction before.
   NVIDIA's own CUDA compiler has the same problem (NVIDIA developer forum, "The curious case of the missing
   multiply-by-2", 2026-08): nvcc's front end turns * 2 into an add in PTX, and ptxas folds 2^k into FMUL (.M4 .M8
   .D2 .D4 .D8 on sm_89) but not the add: a * b * 2 becomes FMUL + FFMA(a, b, a * b). Workarounds there: __fmul_rn(a * b,
   2.0f) or inline PTX mul. The thread argues FFMA(a, b, FMUL(a, b)) is bit-identical to FMUL.M2(a, b), so fxc's dp2
   form can be turned back into mul + mul l(2.0) without changing any result. It also recalls the scale factor came with
   sm_52 (Maxwell 2); nouveau's GM107 emitter (sm_50) encodes it too, and no Maxwell 1 report has the fixed scale tests
   yet (OpBench >= 0.7.3): sopt's nvidia-maxwell1 model assumes it, unverified.
2. **sign (measured, costs speed).** fxc writes `sign(x)` as `lt`, `lt`, `iadd`, `itof`; the int -> float conversion
   is quarter rate on Ampere / Blackwell (sign 18-28 quarter units). `mad(saturate(mad(x, 1e38, 0.5)), 2.0, -1.0)` is
   cheaper on every family measured so far (9 on gen12 against 18; see docs/inexact-tricks.md for its limits at 0 /
   tiny values).
3. **clamp (candidate).** fxc writes `clamp` as `max` + `min`; only AMD's driver turns it back into one v_med3.
4. **!(x < 1) (correctness, not speed).** fxc rewrites it to `ge`, wrong for NaN (IEEE strictness keeps `lt` + movc).
5. **dot products (candidate).** fxc uses `dp2` / `dp3` for sums of products; drivers split them again. Check whether
   any split costs more than the plain mul / mad chain.
6. **Constant folding through saturate (harmless, note only).** fxc tracks value ranges across unrolled code and
   folds what becomes constant; keep in mind when writing tests.
8. **Integer shift + add -> imul (candidate, from Emil Persson's DX11 talk, slides 53-55).** fxc writes `(x << 2) + x`
   as `imul x, 5` and `(x << 3) - x` as `imul x, 7` (checked with fxc -O3). Integer multiplies are quarter rate on AMD
   (shl + add is one v_lshl_add); NVIDIA Turing+ has a full-rate IMAD. OpBench pair shladd5 / shladd5-od (0.7.11).
9. **Rotates (candidate).** fxc writes a constant rotate as `ushr` + `bfi`; NVIDIA has a one-instruction funnel shift
   (SHF). OpBench pair irot / irot-od (0.7.11). GTX 1660 (measured): fxc's ushr + bfi 1.78 fma, the plain shl / shr /
   or compiled with /Od 0.88: the driver makes one funnel shift of the plain form, not of fxc's. shladd5 is free on
   the 1660 both ways (IMAD); AMD reports will tell. UHD 630 (0.7.11): irot 4.0 -> 2.7 fma in compute, 3.6 -> 2.7 in pixel
   shaders; shladd5 0.9 both ways. The rotate loss is on both vendors, in both stages.
10. **Checked, not fxc's doing (2026-10-16, from a list another assistant suggested):** `a * b + a * c` is not factored
   (it becomes `dp2`, like x * 2); bit select `(a & m) | (b & ~m)`, min / max / median networks and bool-to-float
   arithmetic stay as written; saturate keeps its place before a scale. Stephen Hill's "Righting Wrap, Part 2" (fxc
   misoptimising when it knows constant values, DX9 / Xbox 360) is not reproduced yet.
7. **D3D9 round (behavior).** For SM3 fxc writes `round(x)` as `floor(x + 0.5)` (halves up); SM4+ uses `round_ne`.

OpBench 0.7.8 runs pairs (section "fxc's rewrites": sqmax, scale2, scale4, sign, clamp, lerp, smoothstep, fmod,
atan2, each also as "<name>-od" compiled with /Od). GTX 1660 (fma units over the base, -O3 / -od): scale2 0.96 /
0.12, sign 2.13 / 1.96, the others equal within noise (sqmax 1.09 / 0.96, clamp 1.02 / 1.06, lerp 2.0 / 1.98,
smoothstep 5.0 / 5.0, fmod 3.5 / 3.6, atan2 15.1 / 15.4): NVIDIA's driver undoes everything but x2 there. Other
families (AMD clamp / med3, Ampere / Blackwell sign) come with the reports.

Idea to keep (owner, 2026-10-16): the ReShade add-on could also dump a game's shaders, optimize them with sopt and put
the faster ones back. A driver's shader cache keys on the shader's bytes, so a replaced shader is compiled once more
and cached under its new bytes.

Mesa (2026-10-17, src/compiler/nir/nir_opt_algebraic.py on main, the rules every Mesa driver runs, so DXVK / vkd3d on
Linux): `fadd a, a -> fmul a, 2.0` (line ~274) undoes fxc's x2 rewrite before the back ends pick output modifiers;
`fadd a, fadd(a, b) -> b + a * 2`. D3D patterns it recognises, worth copying: D3D Booleans (0 / -1 masks: bcsel(a, -1,
0) -> -b2i(a), and of a mask -> bcsel, imax / umax of masks = and / or, shifts of masks), b2f(compare) * x -> x or a
select (fxc's `and r, cmp, 0x3f800000` then mul), D3D9 trunc (floor + compare fix-ups -> ftrunc), DXVK's (a >> b) << b
-> and. Windows drivers apparently lack the x2 one (GTX 1660: x2 not free after fxc).
Tools that show the GPU's own instructions (2026-10-17): NVIDIA Nsight Graphics shows SASS only in its Pro build
(NDA); the public build shows DXIL / SPIR-V. Intel GPA reached end of life (2025.1 final, downloads ended 2025); its
shader profiler needed Iris Xe or newer. AMD: RGA. So measure (OpBench pairs) rather than read the ISA.

## Collecting more

Candidates to test with OpBench pairs (fxc -O3 against `/Od`, same step): every intrinsic fxc writes out (smoothstep,
fmod, sincos, tan, atan, atan2, asin, acos, pow with constant exponents, lerp, saturate on integer math), integer
`* 2` / `* 2^n` (does fxc turn them into shifts?), `x * -2`, `abs(x) * 2`, divisions by constants, select chains.
