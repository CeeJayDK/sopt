@echo off
rem Double-click: measures instruction costs on this PC's GPU (sopt-opbench) and keeps the window open.
rem Close games and other GPU-heavy programs first. Send the opbench-*.csv file it writes.
"%~dp0sopt-opbench.exe" %*
pause
