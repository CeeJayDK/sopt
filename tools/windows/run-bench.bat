@echo off
rem Double-click: runs the sopt bench (run-bench.ps1) on DX11 and Vulkan and zips the results.
rem run-bench.ps1 is in bin\ in the release zip (next to this file in the repository).
set "PS1=%~dp0bin\run-bench.ps1"
if not exist "%PS1%" set "PS1=%~dp0run-bench.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
pause
