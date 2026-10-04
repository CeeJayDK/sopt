@echo off
rem Double-click: runs OpBench and TexBench on every graphics card in this PC, one card after the
rem other, without stopping in between (each card once, no software renderer), and keeps the window
rem open at the end. Close games and other GPU-heavy programs first. Send all the CSV files it writes.
for /f "usebackq" %%i in (`call "%~dp0OpBench.exe" --adapters`) do (
  "%~dp0OpBench.exe" --adapter %%i %*
  "%~dp0TexBench.exe" --adapter %%i %*
)
pause
