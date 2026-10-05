@echo off
rem Double-click: ShaderInfo (what the driver reports, a few seconds), then OpBench (instruction costs)
rem and TexBench (texture costs) on this PC's GPU, one after the other without stopping in between;
rem keeps the window open at the end. Close games and other GPU-heavy programs first.
rem Send the shaderinfo-*.txt, opbench-*.csv and texbench-*.csv files.
if exist "%~dp0ShaderInfo.exe" "%~dp0ShaderInfo.exe"
"%~dp0OpBench.exe" %*
"%~dp0TexBench.exe" %*
pause
