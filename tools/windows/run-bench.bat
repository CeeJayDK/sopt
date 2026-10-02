@echo off
rem Double-click: runs the sopt bench (run-bench.ps1) on DX11 and Vulkan and zips the results.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-bench.ps1" %*
pause
