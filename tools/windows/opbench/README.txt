sopt-opbench - what GPU instructions cost on your graphics card
================================================================

Part of sopt, a shader superoptimizer for ReShade effects (https://github.com/CeeJayDK/sopt,
GPL-3.0). The numbers help sopt pick faster shader code for your kind of GPU.

How to run
----------
1. Close games and other programs that use the GPU. On a laptop, plug it in.
2. Double-click measure-gpu.bat. It takes a few minutes and keeps the window open at the end.
3. Send the file opbench-<your GPU>.csv that appears next to it.

It needs the Microsoft Visual C++ 2015-2022 redistributable (x64), which almost every gaming PC
already has: https://aka.ms/vs/17/release/vc_redist.x64.exe

Options (sopt-opbench.exe in a command prompt)
----------------------------------------------
  --list         show the GPUs Windows reports, with their numbers
  --adapter N    measure GPU number N instead of the one with the most video memory
  --filter text  only tests whose name contains text (quick checks)

Laptops with two GPUs: sopt-opbench asks the NVIDIA / AMD driver for the fast GPU. On older AMD
laptops, set sopt-opbench.exe to "High Performance" in the Switchable Graphics settings if it still
runs on the integrated GPU. Windows may keep showing the integrated GPU's name even then.

What it does
------------
It runs small compute shaders on the GPU (Direct3D 11) and times them. Each test repeats one
instruction pattern in long chains; its cost is reported relative to one multiply-add (4 = one
fma). Every test runs twice, forward and backward through the list, with a fresh reference right
before it, so a GPU clock change shows up as a warning instead of wrong numbers.

TESTS.txt explains what each test measures and why.

It writes only opbench-<GPU>.csv and the folder opbench-dxbc (the shader code of each test) next
to itself. It does not use the network.
