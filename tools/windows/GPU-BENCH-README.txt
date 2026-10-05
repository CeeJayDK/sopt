GPU-Bench - measure your graphics card for sopt
==============================================

Part of sopt, a shader superoptimizer for ReShade effects (https://github.com/CeeJayDK/sopt, GPL-3.0).
Three small programs measure what your GPU and its driver really do, so sopt can pick the fastest
shader code for each kind of card:

  OpBench     what math instructions cost (OpBench-README.txt, OpBench-TESTS.txt)
  TexBench    what texture reads, render target writes and blending cost (TexBench-README.txt, TexBench-TESTS.txt)
  ShaderInfo  what the driver reports about the shaders it compiles (ShaderInfo-README.txt)

How to run
----------
1. Close games and other programs that use the GPU. On a laptop, plug it in.
2. Double-click GPU-Bench.bat: a menu, press the number of what you want. Option 1 runs all three on
   your main graphics card (about 5-15 minutes). With more than one graphics card (for example a laptop
   with built-in and dedicated graphics), option 2 measures every card. The same without the menu:
   measure-main-gpu.bat and measure-all-gpus.bat; one program alone: measure-gpu.bat (OpBench),
   measure-textures.bat (TexBench), shader-info.bat (ShaderInfo).
3. Send the files it writes next to the programs: shaderinfo-*.txt, opbench-*.csv, texbench-*.csv
   (and the texbench-*-order.png images if you like).

The results appear section by section while the rest is measured, and each program ends with a score
box. Feedback and results: https://github.com/CeeJayDK/sopt/issues

It needs the Microsoft Visual C++ 2015-2022 redistributable (x64), which almost every gaming PC already
has: https://aka.ms/vs/17/release/vc_redist.x64.exe
