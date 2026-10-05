@echo off
rem Double-click: ShaderInfo (every Vulkan GPU, a few seconds), then OpBench and TexBench on every
rem graphics card in this PC, one card after the other, without stopping in between (each card once,
rem no software renderer); keeps the window open at the end. Close games and other GPU-heavy programs
rem first. Send all the .txt and .csv files it writes.
if exist "%~dp0ShaderInfo.exe" "%~dp0ShaderInfo.exe"
for /f "usebackq" %%i in (`call "%~dp0OpBench.exe" --adapters`) do (
  "%~dp0OpBench.exe" --adapter %%i %*
  "%~dp0TexBench.exe" --adapter %%i %*
)
pause
