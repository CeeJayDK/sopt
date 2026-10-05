@echo off
rem Double-click: measures texture read and render target write costs on this PC's GPU (TexBench) and
rem keeps the window open. Close games and other GPU-heavy programs first. Send the texbench-*.csv file.
"%~dp0TexBench.exe" %*
pause
