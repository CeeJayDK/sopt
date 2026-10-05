@echo off
rem GPU-Bench (owner, 2026-10-05): one batch file with a menu; pick what to measure with a number key.
rem Reports go to Reports\, and after every run GPU-Bench-Reports.zip holds them (without the shader dumps).
setlocal EnableExtensions
cd /d "%~dp0"
rem ESC for colors (Windows 10 and later; older consoles show the codes as text); BEL (a raw 0x07 byte below) beeps.
for /f %%e in ('echo prompt $E^| cmd') do set "E=%%e"
set "BEL="
:menu
cls
echo.
echo   %E%[96m=====================================================%E%[0m
echo   %E%[1;97m  GPU-Bench  -  by CeeJay.dk%E%[0m
echo   %E%[90m  measures your graphics card for sopt%E%[0m
echo   %E%[96m=====================================================%E%[0m
echo.
echo   %E%[1;93m1%E%[0m  Main graphics card: ShaderInfo, OpBench and TexBench %E%[90m(5-15 min)%E%[0m
echo   %E%[1;93m2%E%[0m  Another graphics card: the same, you pick the card
echo   %E%[1;93m3%E%[0m  Every graphics card in this PC, one after the other
echo.
echo   %E%[1;93m4%E%[0m  OpBench only     %E%[90mwhat math instructions cost%E%[0m
echo   %E%[1;93m5%E%[0m  TexBench only    %E%[90mwhat texture reads, writes and blending cost%E%[0m
echo   %E%[1;93m6%E%[0m  ShaderInfo only  %E%[90mwhat the driver reports about compiled shaders (seconds)%E%[0m
echo.
echo   %E%[1;93m7%E%[0m  Open the Reports folder
echo   %E%[1;93m8%E%[0m  Open the guide %E%[90m(README.html)%E%[0m
echo   %E%[1;93m0%E%[0m  Quit
echo.
echo   %E%[90mClose games and other GPU-heavy programs first. When a run is done, send GPU-Bench-Reports.zip.%E%[0m
echo.
choice /c 123456780 /n /m "  Press a number: "
set "n=%errorlevel%"
if "%n%"=="9" goto :eof
if "%n%"=="1" call :run "" all
if "%n%"=="2" call :pick
if "%n%"=="3" call :every
if "%n%"=="4" call :run "" opbench
if "%n%"=="5" call :run "" texbench
if "%n%"=="6" call :run "" shaderinfo
if "%n%"=="7" call :reports
if "%n%"=="8" start "" "%~dp0README.html"
goto menu

:reports
if not exist "Reports" mkdir "Reports"
start "" explorer "%~dp0Reports"
exit /b

rem :run "<adapter number or empty>" all|opbench|texbench|shaderinfo
:run
set "adapter="
if not "%~1"=="" set "adapter=--adapter %~1"
if /i "%~2"=="all" call :one ShaderInfo.exe ""
if /i "%~2"=="all" call :one OpBench.exe "%adapter%"
if /i "%~2"=="all" call :one TexBench.exe "%adapter%"
if /i "%~2"=="opbench" call :one OpBench.exe "%adapter%"
if /i "%~2"=="texbench" call :one TexBench.exe "%adapter%"
if /i "%~2"=="shaderinfo" call :one ShaderInfo.exe ""
goto :finish

rem Another card: the list from OpBench, then its number.
:pick
cls
echo.
"%~dp0OpBench.exe" --list
echo.
set "card="
set /p "card=  Number of the graphics card to measure (Enter = back to the menu): "
if "%card%"=="" exit /b
set /a "num=card" 2>nul
if "%num%"=="%card%" goto picked
echo   %E%[91mNot a number.%E%[0m
pause
goto pick
:picked
call :run %card% all
exit /b

rem Every card: ShaderInfo once (it covers every Vulkan GPU), then OpBench and TexBench per card.
:every
call :one ShaderInfo.exe ""
for /f "usebackq" %%i in (`call "%~dp0OpBench.exe" --adapters`) do (
  call :one OpBench.exe "--adapter %%i"
  call :one TexBench.exe "--adapter %%i"
)
goto :finish

:one
if exist "%~dp0%~1" goto oneRun
echo   %E%[91m%~1 is missing next to GPU-Bench.bat%E%[0m
exit /b
:oneRun
"%~dp0%~1" %~2
exit /b

rem After a run: zip the reports (not Reports\Shaders), beep, then wait for a key.
:finish
if exist "Reports" powershell -NoProfile -Command "$f = Get-ChildItem -LiteralPath 'Reports' -File; if ($f) { Compress-Archive -LiteralPath $f.FullName -DestinationPath 'GPU-Bench-Reports.zip' -Force }" >nul 2>&1
echo.
if exist "GPU-Bench-Reports.zip" goto zipped
echo   %E%[92mDone.%E%[0m The reports are in the Reports folder.
goto beep
:zipped
echo   %E%[92mDone.%E%[0m Send %E%[1;97mGPU-Bench-Reports.zip%E%[0m, next to GPU-Bench.bat: it holds every report in the Reports folder.
:beep
<nul set /p "=%BEL%"
pause
exit /b
