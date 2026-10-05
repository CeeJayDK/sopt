ShaderInfo - what your graphics driver reports about the shaders it compiles
============================================================================

Part of sopt, a shader superoptimizer for ReShade effects (https://github.com/CeeJayDK/sopt,
GPL-3.0). Graphics drivers turn shaders into the GPU's own instructions. Some drivers can report
what they produced (instruction counts, registers, estimated cycles) and which hardware counters
the GPU offers. ShaderInfo asks every Vulkan GPU in the PC and writes it all to a text file, so
sopt can learn what the real drivers do with its shader variants.

How to run
----------
1. Double-click shader-info.bat. It takes a few seconds.
2. Send the file(s) shaderinfo-<your GPU>.txt that appear next to it.

It needs a Vulkan driver (every current NVIDIA, AMD and Intel driver has one) and the Microsoft
Visual C++ 2015-2022 redistributable (x64): https://aka.ms/vs/17/release/vc_redist.x64.exe

Options (ShaderInfo.exe in a command prompt)
--------------------------------------------
  --batch folder  compile every *.ps.spv in the folder (sopt-fx --export-spirv writes them) and
                  write the driver's statistics to shaderinfo-batch-<GPU>.csv in that folder
  --spv file      also compile a compute shader of your own (SPIR-V, entry point main)
  --all           include software renderers

What it reports (where the driver offers it)
--------------------------------------------
  VK_KHR_pipeline_executable_properties  statistics of a compiled shader (instructions, registers,
                                         spills, ...) and sometimes its disassembly
  VK_AMD_shader_info                     AMD: registers and the disassembly
  VK_KHR_performance_query               the hardware counters the GPU exposes (listed, not read)
  VK_AMD_gpa_interface, VK_INTEL_performance_query   whether these vendor interfaces exist
  and every Vulkan extension the driver supports.

Nothing is sent anywhere; the file stays on your PC until you send it.
