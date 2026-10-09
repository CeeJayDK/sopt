@echo off
rem Test Host (owner, 2026-10-06: one test host for every graphics API ReShade supports, with a menu for ease of use).
rem sopt-host shows a fixed test image on Direct3D 9, 10, 11, 12, Vulkan or OpenGL with ReShade in front of it, so
rem effects can be tested and benchmarked without a game. Results go to Results\, test effects live in Effects\.
setlocal EnableExtensions
cd /d "%~dp0"
rem The programs and scripts are in bin\ (only the menu and the guide in the main folder).
set "BIN=%~dp0bin\"
rem ESC for colors (Windows 10 and later); BEL (a raw 0x07 byte below) beeps when a run is done.
for /f %%e in ('echo prompt $E^| cmd') do set "E=%%e"
set "BEL="
:menu
cls
echo.
echo   %E%[96m=====================================================%E%[0m
echo   %E%[1;97m  Test Host  -  by CeeJay.dk%E%[0m
echo   %E%[90m  ReShade on every graphics API, no game needed%E%[0m
echo   %E%[96m=====================================================%E%[0m
echo.
echo   %E%[1;93m1%E%[0m  IEEE 754 test on every API   %E%[90mfloat rules as ReShade compiles them (about 2 minutes)%E%[0m
echo   %E%[1;93m2%E%[0m  IEEE 754 test on one API
echo   %E%[1;93m3%E%[0m  Test an effect from the Effects folder on every API %E%[90m(a screenshot per API)%E%[0m
echo.
echo   %E%[1;93m4%E%[0m  Open the test host with ReShade %E%[90m(you pick the API; press Home in it for ReShade)%E%[0m
echo   %E%[1;93m5%E%[0m  Benchmark a SweetOpt test package %E%[90m(Direct3D 11 and Vulkan)%E%[0m
echo.
echo   %E%[1;93m6%E%[0m  Open the Results folder
echo   %E%[1;93m7%E%[0m  Open the Effects folder %E%[90m(put your own .fx files or a reshade-shaders folder here)%E%[0m
echo   %E%[1;93m8%E%[0m  Open the guide %E%[90m(README.html)%E%[0m
echo   %E%[1;93m0%E%[0m  Quit
echo.
echo   %E%[90mThe test host opens its own window and closes it by itself; nothing outside this folder is changed.%E%[0m
echo.
choice /c 123456780 /n /m "  Press a number: "
set "n=%errorlevel%"
if "%n%"=="9" goto :eof
if "%n%"=="1" call :test all sopt_IEEE754.fx
if "%n%"=="2" call :ieeeOne
if "%n%"=="3" call :test all ?
if "%n%"=="4" call :hostOne
if "%n%"=="5" call :bench
if "%n%"=="6" call :open Results
if "%n%"=="7" call :open Effects
if "%n%"=="8" start "" "%~dp0README.html"
goto menu

rem %API% is read on lines after :pickApi (cmd expands a line's variables before running it).
:ieeeOne
call :pickApi
if errorlevel 1 exit /b
call :test %API% sopt_IEEE754.fx
exit /b

:hostOne
call :pickApi
if errorlevel 1 exit /b
call :interactive %API%
exit /b

rem Sets API; exits with errorlevel 1 when the user goes back.
:pickApi
cls
echo.
echo   %E%[1;97mWhich graphics API?%E%[0m
echo.
echo   %E%[1;93m1%E%[0m  Direct3D 9
echo   %E%[1;93m2%E%[0m  Direct3D 10
echo   %E%[1;93m3%E%[0m  Direct3D 11
echo   %E%[1;93m4%E%[0m  Direct3D 12
echo   %E%[1;93m5%E%[0m  Vulkan
echo   %E%[1;93m6%E%[0m  OpenGL
echo   %E%[1;93m0%E%[0m  Back
echo.
choice /c 1234560 /n /m "  Press a number: "
set "k=%errorlevel%"
set "API="
if "%k%"=="1" set "API=dx9"
if "%k%"=="2" set "API=dx10"
if "%k%"=="3" set "API=dx11"
if "%k%"=="4" set "API=dx12"
if "%k%"=="5" set "API=vulkan"
if "%k%"=="6" set "API=gl"
if not defined API exit /b 1
exit /b 0

rem :test <apis> <effect file or ?>
:test
cls
powershell -NoProfile -ExecutionPolicy Bypass -File "%BIN%run-test.ps1" -Apis %1 -Effect "%~2"
goto done

rem :interactive <api>
:interactive
cls
powershell -NoProfile -ExecutionPolicy Bypass -File "%BIN%run-test.ps1" -Apis %1 -Interactive
exit /b

:bench
cls
powershell -NoProfile -ExecutionPolicy Bypass -File "%BIN%run-bench.ps1"
goto done

:open
if not exist "%~1" mkdir "%~1"
start "" explorer "%~dp0%~1"
exit /b

:done
echo.
<nul set /p "=%BEL%"
choice /c YN /n /m "  Open the Results folder? (Y/N) "
if errorlevel 2 exit /b
call :open Results
exit /b
