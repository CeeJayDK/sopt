TexBench - what texture reads and render target writes cost on your graphics card
==================================================================================

Part of sopt, a shader superoptimizer for ReShade effects (https://github.com/CeeJayDK/sopt,
GPL-3.0). The numbers show how expensive texture operations are next to math (when a lookup
table beats computing a value, and the other way round) and how the texture formats ReShade
supports really perform on your GPU.

How to run
----------
1. Close games and other programs that use the GPU. On a laptop, plug it in.
2. Double-click measure-textures.bat. It takes a few minutes and keeps the window open at the end.
3. Send the file texbench-<your GPU>.csv that appears next to it.

It needs the Microsoft Visual C++ 2015-2022 redistributable (x64), which almost every gaming PC
already has: https://aka.ms/vs/17/release/vc_redist.x64.exe

Options (TexBench.exe in a command prompt)
------------------------------------------
  --list         show the GPUs Windows reports, with their numbers
  --adapter N    measure GPU number N instead of the one with the most video memory
  --filter text  only tests whose name contains text (quick checks)

What it does
------------
It runs small shaders on the GPU (Direct3D 11) and times them, like OpBench: each test repeats
one texture read (or derivative) in long chains, where every read's coordinate depends on the
result of the one before, and its cost is reported relative to one multiply-add (4 = one fma).
Each test is read at least twice against a fresh reference and more often when the readings
disagree. Texture reads depend a lot on caches and memory, so read the numbers as ballpark
figures; TexBench-TESTS.txt explains every test.

It writes only texbench-<GPU>.csv and the folder texbench-dxbc (the shader code of each test)
next to itself. It does not use the network.
