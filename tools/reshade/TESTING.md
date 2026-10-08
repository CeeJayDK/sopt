# Testing the ReShade internal shader changes

`internal-shaders.patch` (against ReShade 6.8.0) changes two of ReShade's own shaders
(docs/reshade-internal-shaders.md):

1. `copy_ps.hlsl` (D3D10/11/12): `Load` of the texel under the pixel instead of a point-sampled
   `Sample` (and no copy sampler on the C++ side). It runs every frame when the back buffer is
   multisampled (MSAA) or has no alpha channel (B8G8R8X8 / R8G8B8X8): ReShade then renders into its
   own texture and copies the result back with this shader. Result must be bit-identical.
2. OpenGL mipmap generation (`GenerateMipMaps` in effects): one bilinear fetch at the shared corner
   of each 2x2 block instead of four texel fetches, for filterable formats (integer formats keep the
   old shader). Result must match to within the format's rounding.

`d3d12-mipmaps.patch` (applies on top of it, a suggestion) does the same for D3D12's mipmap shader:
the first level of each pass (up to 6 levels per dispatch) comes from one bilinear fetch per texel
from an SRV of the source level instead of four UAV loads.

The CI artifact `reshade-6.8.0-sopt` has three DLLs built from the same source with the same compiler:
`ReShade64-6.8.0-unchanged.dll`, `ReShade64-6.8.0-sopt.dll` and `ReShade64-6.8.0-sopt-d3d12.dll`
(the second plus the D3D12 patch; 64-bit, full add-on support).
Compare them with each other, not with the official signed build. Rename the one under test to what
the API needs next to the game's exe: `dxgi.dll` (D3D10/11/12), `opengl32.dll` (OpenGL),
`d3d9.dll` (D3D9).

Screenshots: ReShade's screenshot key (Print Screen) saves lossless PNGs; take the same shot with both
DLLs (same preset, same frame content) and compare them (or send them for a pixel diff).

## 1. D3D11 copy path (sopt-host --msaa)

sopt-host.exe (CI artifact `Test-Host`) shows a fixed image; `--msaa 4` gives it a
4x multisampled back buffer, so ReShade's copy shader runs every frame.

1. Folder with `sopt-host.exe`, the DLL under test as `dxgi.dll` and a `reshade-shaders` folder
   (set the effect search path in the ReShade overlay).
2. `sopt-host.exe --api dx11 --msaa 4 --width 1920 --height 1080 --no-depth`
3. Turn on one effect that changes the picture (e.g. SweetFX Vibrance), take a screenshot; turn all
   effects off, take another.
4. Repeat with the other DLL. Expected: identical screenshots; ReShade.log without "Failed to create
   copy pipeline".
5. Optional: a D3D10/11 game with an in-game MSAA setting that multisamples the back buffer.

## 2. OpenGL mipmaps (sopt_MipTest.fx)

Simplest: `sopt-host.exe --api gl --width 1920 --height 1080` with the DLL as `opengl32.dll` next
to it (Test-Host artifact). Any other OpenGL 4.3 program works too (GZDoom with the OpenGL
renderer, RetroArch with the `gl` video driver, ...).

1. ReShade.log must not contain "Failed to compile bilinear mipmap generation shader" (that warning
   means it fell back to the old shader: report it).
2. Put `sopt_MipTest.fx` in the effect folder and turn it on alone. Mode "Over tolerance (red)"
   (Tolerance 1): for every Format (RGBA8, RGBA16F, R32F, RGB10A2) and Level 1-5 the screen must be
   dark gray with no red, with both DLLs. Format "RGBA8 256x32" (Level 1-8): its levels 6-8 are 1
   texel high, so the old shader reads past the edge there (red with the unchanged DLL: it averages
   with zeros); the bilinear fetch clamps to the edge (dark gray with the sopt DLL). Each level is
   compared with the 2x2 average of the level above, which is what ReShade's mipmap shaders compute.
   Under Wine (Mesa llvmpipe) the sopt DLL's RGBA8 levels are within 1 step instead of 1/2: llvmpipe's
   filter rounds less exactly than the GTX 1660 (red at Tolerance 1.0 on 3.5%, none at 1.01). (On D3D11 and Vulkan the driver generates
   the mips, unchanged by the patch: there red appears once a level above has an odd size, e.g. from
   level 4 at 1080 lines, because the driver then filters differently.)
3. Mode "Difference (amplified)": the noise should look the same with both DLLs (equal or within one
   8-bit step per level). A screenshot per DLL of Format RGBA8 / Level 3 in this mode is useful.
4. A real effect that uses mipmaps (e.g. qUINT or iMMERSE MXAO, a bloom with a mip chain): screenshots
   with both DLLs should look the same.
5. Speed (optional): ReShade's statistics (GPU time per technique) for that effect with each DLL; the
   mipmap generation is counted in its technique.

## 3. Integer formats (sopt_MipTestInt.fx)

Integer textures (R32U, RGBA32I) with mip levels: what does ReShade generate on each API? Turn
`sopt_MipTestInt.fx` on alone, Mode "Classification", Level 1, both formats: green = the exact
average, blue = one of the four texels (point filter), gray = 0, red = anything else. Note the
colour per API (and whether the effect loads at all: the log says why if not). Unchanged by the
OpenGL patch (integer formats keep the old shader), so one DLL is enough. Under Wine (Mesa
llvmpipe, OpenGL) both DLLs give gray (zeros).

## 4. D3D12 mipmaps (sopt-d3d12 DLL)

`sopt-host.exe --api dx12` with the DLL as `dxgi.dll`. ReShade.log must not contain "Failed to
create bilinear mipmap generation pipeline". `sopt_MipTest.fx` as in section 2, with the unchanged
and the sopt-d3d12 DLL: Formats RGBA8 / RGBA16F / R32F / RGB10A2, Level 1-5, Tolerance 1 (expected:
no red with both), and "RGBA8 256x32" Level 1-8 (note where red appears with each DLL: the
D3D12 shader makes levels 2-6 of a pass from the level before in groupshared memory, where a 1 texel
high level still averages with values from outside the texture, patch or not).

## 5. Unchanged paths (sanity)

D3D9 and Vulkan run the same code as before (D3D12 uses copy_ps only for multisampled or X8 back
buffers, which flip-model swap chains cannot have). Start one game per API with the sopt DLL and
check that effects and the overlay work.
