@echo off
rem ShaderInfo: what each Vulkan GPU driver reports about the shaders it compiles
rem (writes shaderinfo-<gpu>.txt next to this file; please send those files).
"%~dp0ShaderInfo.exe" %*
pause
