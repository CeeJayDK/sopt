# Testing the ReShade internal shader changes

`internal-shaders.patch` (against ReShade 6.8.0) changes two of ReShade's own shaders
(docs/reshade-internal-shaders.md):

1. `copy_ps.hlsl` (D3D10/11/12): `Load` of the texel under the pixel instead of a point-sampled
   `Sample`. It runs every frame when the back buffer is multisampled (MSAA) or has no alpha
   channel (B8G8R8X8 / R8G8B8X8): ReShade then renders into its own texture and copies the result
   back with this shader. Result must be bit-identical.
2. OpenGL mipmap generation (`GenerateMipMaps` in effects): one bilinear fetch at the shared corner
   of each 2x2 block instead of four texel fetches, for filterable formats (integer formats keep the
   old shader). Result must match to within the format's rounding; ReShade.log says
   "Using bilinear mipmap generation shader for filterable formats." when it is active.

The CI artifact `reshade-6.8.0-sopt` has two DLLs built from the same source with the same compiler:
`ReShade64-6.8.0-unchanged.dll` and `ReShade64-6.8.0-sopt.dll` (64-bit, full add-on support).
Compare them with each other, not with the official signed build. Rename the one under test to what
the API needs next to the game's exe: `dxgi.dll` (D3D10/11/12), `opengl32.dll` (OpenGL),
`d3d9.dll` (D3D9).

Screenshots: ReShade's screenshot key (Print Screen) saves lossless PNGs; take the same shot with both
DLLs (same preset, same frame content) and compare them (or send them for a pixel diff).

## 1. D3D11 copy path (sopt-host --msaa)

sopt-host.exe (CI artifact `sopt-windows-tools`) shows a fixed image; `--msaa 4` gives it a
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
to it (sopt-windows-tools artifact). Any other OpenGL 4.3 program works too (GZDoom with the OpenGL
renderer, RetroArch with the `gl` video driver, ...).

1. ReShade.log: the sopt DLL logs "Using bilinear mipmap generation shader for filterable formats.",
   the unchanged DLL does not. (A "Failed to compile bilinear mipmap generation shader" warning means
   it fell back to the old shader: report it.)
2. Put `sopt_MipTest.fx` in the effect folder and turn it on alone. Mode "Over tolerance (red)"
   (Tolerance 1): for every Format (RGBA8, RGBA16F, R32F, RGB10A2) and Level 1-5 the screen must be
   dark gray with no red, with both DLLs. Each level is compared with the 2x2 average of the level
   above, which is what ReShade's mipmap shaders compute. (On D3D11 and Vulkan the driver generates
   the mips, unchanged by the patch: there red appears once a level above has an odd size, e.g. from
   level 4 at 1080 lines, because the driver then filters differently.)
3. Mode "Difference (amplified)": the noise should look the same with both DLLs (equal or within one
   8-bit step per level). A screenshot per DLL of Format RGBA8 / Level 3 in this mode is useful.
4. A real effect that uses mipmaps (e.g. qUINT or iMMERSE MXAO, a bloom with a mip chain): screenshots
   with both DLLs should look the same.
5. Speed (optional): ReShade's statistics (GPU time per technique) for that effect with each DLL; the
   mipmap generation is counted in its technique.

## 3. Unchanged paths (sanity)

D3D9, D3D12 and Vulkan run the same code as before (D3D12 uses copy_ps only for multisampled or
X8 back buffers, which flip-model swap chains cannot have). Start one game per API with the sopt
DLL and check that effects and the overlay work.
