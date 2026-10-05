@echo off
rem sopt menu (owner, 2026-10-05: a menu helps users, as GPU-Blueprint.bat did): pick the shaders, then start
rem sopt-fx with a number key. Settings are kept in sopt-menu.ini next to this file; results go to sopt-out\.
setlocal EnableExtensions
cd /d "%~dp0"
rem ESC for colors (Windows 10 and later; older consoles show the codes as text); BEL (a raw 0x07 byte below) beeps.
for /f %%e in ('echo prompt $E^| cmd') do set "E=%%e"
set "BEL="
set "SHADERS="
set "TARGET="
set "SECONDS=5"
set "MODEL=rdna3"
set "MODELNAME=AMD RDNA 3 (Radeon RX 7000)"
if exist "sopt-menu.ini" for /f "usebackq eol=# tokens=1,* delims==" %%a in ("sopt-menu.ini") do set "%%a=%%b"

:menu
cls
echo.
echo   %E%[96m=====================================================%E%[0m
echo   %E%[1;97m  sopt  -  by CeeJay.dk%E%[0m
echo   %E%[90m  finds faster ways to write the math in ReShade shaders%E%[0m
echo   %E%[96m=====================================================%E%[0m
echo.
if "%SHADERS%"=="" echo   Shaders folder   %E%[91mnot chosen yet - press 1%E%[0m
if not "%SHADERS%"=="" echo   Shaders folder   %E%[97m%SHADERS%%E%[0m
if "%TARGET%"=="" echo   Optimize         %E%[97mevery effect in the Shaders folder%E%[0m
if not "%TARGET%"=="" echo   Optimize         %E%[97m%TARGET%%E%[0m
echo   Time             %E%[97m%SECONDS% seconds per statement%E%[0m
echo   Graphics card    %E%[97m%MODELNAME%%E%[0m
if exist "sopt-facts.txt" echo   Value ranges     %E%[97msopt-facts.txt%E%[0m
echo.
echo   %E%[1;93m1%E%[0m  Choose the Shaders folder %E%[90m(the reshade-shaders\Shaders folder with ReShade.fxh)%E%[0m
echo   %E%[1;93m2%E%[0m  Choose what to optimize %E%[90m(everything, one folder or one effect)%E%[0m
echo   %E%[1;93m3%E%[0m  Choose the time per statement %E%[90m(more time can find more)%E%[0m
echo   %E%[1;93m4%E%[0m  Choose the graphics card family %E%[90m(what counts as faster)%E%[0m
echo.
echo   %E%[1;93m5%E%[0m  %E%[1;97mStart%E%[0m
echo   %E%[1;93m6%E%[0m  Quick look: what sopt would work on %E%[90m(seconds, no search)%E%[0m
echo.
echo   %E%[1;93m7%E%[0m  Open the results %E%[90m(sopt-out folder and sopt-report.md)%E%[0m
echo   %E%[1;93m8%E%[0m  Edit value ranges %E%[90m(better results: sopt-facts.txt)%E%[0m
echo   %E%[1;93m9%E%[0m  Open the quick start guide
echo   %E%[1;93m0%E%[0m  Quit
echo.
choice /c 1234567890 /n /m "  Press a number: "
set "n=%errorlevel%"
if "%n%"=="10" goto :eof
if "%n%"=="1" call :chooseShaders
if "%n%"=="2" call :chooseTarget
if "%n%"=="3" call :chooseTime
if "%n%"=="4" call :chooseModel
if "%n%"=="5" call :start
if "%n%"=="6" call :look
if "%n%"=="7" call :results
if "%n%"=="8" call :facts
if "%n%"=="9" start "" notepad "%~dp0QUICKSTART.txt"
call :save
goto menu

rem ---------------------------------------------------------------------------------------------------
:chooseShaders
call :pickFolder "Choose the reshade-shaders\Shaders folder (the one with ReShade.fxh)" "%SHADERS%"
if "%PICKED%"=="" exit /b
if exist "%PICKED%\ReShade.fxh" goto shadersOk
echo.
echo   %E%[93mThere is no ReShade.fxh in that folder.%E%[0m Most effects need it: choose reshade-shaders\Shaders.
choice /c YN /n /m "  Use it anyway? (Y/N) "
if errorlevel 2 exit /b
:shadersOk
set "SHADERS=%PICKED%"
set "TARGET="
exit /b

:chooseTarget
if "%SHADERS%"=="" call :chooseShaders
if "%SHADERS%"=="" exit /b
cls
echo.
echo   What should sopt optimize?
echo.
echo   %E%[1;93m1%E%[0m  Every effect in the Shaders folder
echo   %E%[1;93m2%E%[0m  One folder %E%[90m(for example SweetFX)%E%[0m
echo   %E%[1;93m3%E%[0m  One effect file
echo   %E%[1;93m0%E%[0m  Back
echo.
choice /c 1230 /n /m "  Press a number: "
if errorlevel 4 exit /b
if errorlevel 3 goto targetFile
if errorlevel 2 goto targetFolder
set "TARGET="
exit /b
:targetFolder
call :pickFolder "Choose the folder of effects to optimize" "%SHADERS%"
if not "%PICKED%"=="" set "TARGET=%PICKED%"
exit /b
:targetFile
call :pickFile "%SHADERS%"
if not "%PICKED%"=="" set "TARGET=%PICKED%"
exit /b

:chooseTime
cls
echo.
echo   How long may sopt search each statement?
echo.
echo   %E%[1;93m1%E%[0m  5 seconds  %E%[90m(a first look; a big folder takes a few minutes)%E%[0m
echo   %E%[1;93m2%E%[0m  20 seconds %E%[90m(finds more)%E%[0m
echo   %E%[1;93m3%E%[0m  60 seconds %E%[90m(for one effect)%E%[0m
echo.
choice /c 123 /n /m "  Press a number: "
if errorlevel 3 set "SECONDS=60" & exit /b
if errorlevel 2 set "SECONDS=20" & exit /b
set "SECONDS=5"
exit /b

:chooseModel
cls
echo.
echo   Which graphics cards should the variants be fastest on?
echo   %E%[90m(sopt also measures AMD and NVIDIA code when their tools are installed; this picks what it searches for)%E%[0m
echo.
echo   %E%[1;93m1%E%[0m  AMD RDNA 3      %E%[90mRadeon RX 7000%E%[0m
echo   %E%[1;93m2%E%[0m  AMD RDNA 2      %E%[90mRadeon RX 6000, Steam Deck, Radeon 680M%E%[0m
echo   %E%[1;93m3%E%[0m  AMD RDNA 4      %E%[90mRadeon RX 9000%E%[0m
echo   %E%[1;93m4%E%[0m  AMD GCN 5       %E%[90mRadeon Vega, Ryzen APUs before RDNA%E%[0m
echo   %E%[1;93m5%E%[0m  NVIDIA Turing   %E%[90mGTX 16, RTX 20%E%[0m
echo   %E%[1;93m6%E%[0m  NVIDIA Ampere   %E%[90mRTX 30, RTX 40%E%[0m
echo   %E%[1;93m7%E%[0m  NVIDIA Blackwell %E%[90mRTX 50%E%[0m
echo   %E%[1;93m8%E%[0m  NVIDIA Pascal   %E%[90mGTX 10%E%[0m
echo   %E%[1;93m9%E%[0m  NVIDIA Maxwell  %E%[90mGTX 900, GTX 800M%E%[0m
echo   %E%[1;93mI%E%[0m  Intel Gen9      %E%[90mHD / UHD Graphics 500 and 600%E%[0m
echo.
choice /c 123456789I /n /m "  Press a number (or I): "
set "k=%errorlevel%"
if "%k%"=="1" set "MODEL=rdna3" & set "MODELNAME=AMD RDNA 3 (Radeon RX 7000)"
if "%k%"=="2" set "MODEL=amd-rdna2" & set "MODELNAME=AMD RDNA 2 (Radeon RX 6000, Steam Deck)"
if "%k%"=="3" set "MODEL=amd-rdna4" & set "MODELNAME=AMD RDNA 4 (Radeon RX 9000)"
if "%k%"=="4" set "MODEL=amd-gcn5" & set "MODELNAME=AMD GCN 5 (Radeon Vega)"
if "%k%"=="5" set "MODEL=nvidia-turing" & set "MODELNAME=NVIDIA Turing (GTX 16, RTX 20)"
if "%k%"=="6" set "MODEL=nvidia-ampere" & set "MODELNAME=NVIDIA Ampere (RTX 30, RTX 40)"
if "%k%"=="7" set "MODEL=nvidia-blackwell" & set "MODELNAME=NVIDIA Blackwell (RTX 50)"
if "%k%"=="8" set "MODEL=nvidia-pascal" & set "MODELNAME=NVIDIA Pascal (GTX 10)"
if "%k%"=="9" set "MODEL=nvidia-maxwell" & set "MODELNAME=NVIDIA Maxwell (GTX 900)"
if "%k%"=="10" set "MODEL=intel-gen9" & set "MODELNAME=Intel Gen9 (HD / UHD Graphics)"
exit /b

rem ---------------------------------------------------------------------------------------------------
:start
if "%SHADERS%"=="" call :chooseShaders
if "%SHADERS%"=="" exit /b
set "WHAT=%TARGET%"
if "%WHAT%"=="" set "WHAT=%SHADERS%"
set "FACTS="
if exist "sopt-facts.txt" set FACTS=--facts "%~dp0sopt-facts.txt"
cls
"%~dp0sopt-fx.exe" -I "%SHADERS%" -o "%~dp0sopt-out" --time %SECONDS% --cost-model %MODEL% %FACTS% "%WHAT%"
echo.
if exist "sopt-out\sopt-report.md" echo   %E%[92mDone.%E%[0m The results are in the sopt-out folder: press 7 in the menu to open them.
<nul set /p "=%BEL%"
pause
exit /b

:look
if "%SHADERS%"=="" call :chooseShaders
if "%SHADERS%"=="" exit /b
set "WHAT=%TARGET%"
if "%WHAT%"=="" set "WHAT=%SHADERS%"
cls
"%~dp0sopt-fx.exe" -I "%SHADERS%" -o "%~dp0sopt-out" --list "%WHAT%"
echo.
pause
exit /b

:results
if not exist "sopt-out" echo   %E%[93mNo results yet: press 5 first.%E%[0m& pause& exit /b
start "" explorer "%~dp0sopt-out"
if exist "sopt-out\sopt-report.md" start "" notepad "%~dp0sopt-out\sopt-report.md"
exit /b

rem Value ranges: sopt-facts.txt next to this file (a copy of the one a run writes), used by every later run.
:facts
if exist "sopt-facts.txt" goto factsEdit
if exist "sopt-out\sopt-facts.txt" copy /y "sopt-out\sopt-facts.txt" "sopt-facts.txt" >nul
if exist "sopt-facts.txt" goto factsEdit
echo.
echo   %E%[93mNo sopt-facts.txt yet: press 5 first.%E%[0m A run lists the inputs whose value range sopt could not work out.
pause
exit /b
:factsEdit
echo.
echo   Notepad opens sopt-facts.txt. Remove the # in front of the lines you know and correct the ranges,
echo   save, and start again (5). Delete sopt-facts.txt to stop using it.
start "" notepad "%~dp0sopt-facts.txt"
pause
exit /b

rem ---------------------------------------------------------------------------------------------------
rem :pickFolder "title" "start folder" -> PICKED (a folder dialog; typing the path if PowerShell is missing)
:pickFolder
set "PICKED="
set "PTITLE=%~1"
set "PSTART=%~2"
for /f "usebackq delims=" %%p in (`powershell -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.FolderBrowserDialog; $d.Description = $env:PTITLE; $d.SelectedPath = $env:PSTART; if ($d.ShowDialog() -eq 'OK') { $d.SelectedPath }" 2^>nul`) do set "PICKED=%%p"
if not "%PICKED%"=="" exit /b
echo.
set /p "PICKED=  %PTITLE% - type or paste the path (Enter = cancel): "
if defined PICKED set "PICKED=%PICKED:"=%"
exit /b

rem :pickFile "start folder" -> PICKED (an .fx / .hlsl file)
:pickFile
set "PICKED="
set "PSTART=%~1"
for /f "usebackq delims=" %%p in (`powershell -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; $d.Title = 'Choose the effect to optimize'; $d.Filter = 'Effects (*.fx;*.fxh;*.hlsl)|*.fx;*.fxh;*.hlsl|All files|*.*'; $d.InitialDirectory = $env:PSTART; if ($d.ShowDialog() -eq 'OK') { $d.FileName }" 2^>nul`) do set "PICKED=%%p"
if not "%PICKED%"=="" exit /b
echo.
set /p "PICKED=  The effect file to optimize - type or paste the path (Enter = cancel): "
if defined PICKED set "PICKED=%PICKED:"=%"
exit /b

:save
>"sopt-menu.ini" echo # sopt-menu.bat settings
>>"sopt-menu.ini" echo SHADERS=%SHADERS%
>>"sopt-menu.ini" echo TARGET=%TARGET%
>>"sopt-menu.ini" echo SECONDS=%SECONDS%
>>"sopt-menu.ini" echo MODEL=%MODEL%
>>"sopt-menu.ini" echo MODELNAME=%MODELNAME%
exit /b
