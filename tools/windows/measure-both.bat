@echo off
rem Double-click: runs OpBench (instruction costs) and then TexBench (texture costs) on this PC's GPU
rem one after the other, without stopping in between, and keeps the window open at the end.
rem Close games and other GPU-heavy programs first. Send the opbench-*.csv and texbench-*.csv files.
"%~dp0OpBench.exe" %*
"%~dp0TexBench.exe" %*
pause
