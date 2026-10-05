@echo off
rem GPU-Bench menu (owner, 2026-10-05): pick what to measure with a number key.
setlocal
cd /d "%~dp0"
rem ESC for colors (Windows 10 and later; older consoles show the codes as text).
for /f %%e in ('echo prompt $E^| cmd') do set "E=%%e"
:menu
cls
echo.
echo   %E%[96m=====================================================%E%[0m
echo   %E%[1;97m  GPU-Bench  -  by CeeJay.dk%E%[0m
echo   %E%[90m  measures your graphics card for sopt%E%[0m
echo   %E%[96m=====================================================%E%[0m
echo.
echo   %E%[1;93m1%E%[0m  Main graphics card: ShaderInfo, OpBench and TexBench %E%[90m(5-15 min)%E%[0m
echo   %E%[1;93m2%E%[0m  Every graphics card in this PC: the same for each %E%[90m(laptops with two GPUs)%E%[0m
echo.
echo   %E%[1;93m3%E%[0m  OpBench only     %E%[90mwhat math instructions cost%E%[0m
echo   %E%[1;93m4%E%[0m  TexBench only    %E%[90mwhat texture reads, writes and blending cost%E%[0m
echo   %E%[1;93m5%E%[0m  ShaderInfo only  %E%[90mwhat the driver reports about compiled shaders (seconds)%E%[0m
echo.
echo   %E%[1;93m6%E%[0m  List the graphics cards in this PC
echo   %E%[1;93m7%E%[0m  Open README.txt
echo   %E%[1;93m0%E%[0m  Quit
echo.
echo   %E%[90mClose games and other GPU-heavy programs first. Send the .csv and .txt files it writes.%E%[0m
echo.
choice /c 12345670 /n /m "  Press a number: "
set "n=%errorlevel%"
if "%n%"=="8" goto :eof
if "%n%"=="1" call "%~dp0measure-main-gpu.bat"
if "%n%"=="2" call "%~dp0measure-all-gpus.bat"
if "%n%"=="3" call "%~dp0measure-gpu.bat"
if "%n%"=="4" call "%~dp0measure-textures.bat"
if "%n%"=="5" call "%~dp0shader-info.bat"
if "%n%"=="6" ("%~dp0OpBench.exe" --list & pause)
if "%n%"=="7" start "" notepad "%~dp0README.txt"
goto menu
