@echo off
rem SweetOpt menu (owner, 2026-10-05: a menu helps users, as GPU-Blueprint.bat did): pick the shaders, then start
rem sopt-fx with a number key. The programs and the settings (SweetOpt.ini) are in bin\ (owner, 2026-10-05: only the
rem menu and the guide in the main folder); results go to sopt-out\.
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
rem Output (owner, 2026-10-08): easy = ready files with our picks, easy-switches = the same with switches, expert = every
rem variant behind switches. Easy settings default to the safe choices.
set "MODE=easy"
set "TOOEXACT=no"
set "REWRITES=safe"
if exist "bin\SweetOpt.ini" for /f "usebackq eol=# tokens=1,* delims==" %%a in ("bin\SweetOpt.ini") do set "%%a=%%b"

:menu
cls
echo.
echo   %E%[96m====================================================================%E%[0m
echo   %E%[1;97m  SweetOpt  -  the super sweet shader optimizer  -  by CeeJay.dk%E%[0m
echo   %E%[90m  finds faster ways to write the math in ReShade, HLSL and GLSL shaders%E%[0m
echo   %E%[96m====================================================================%E%[0m
echo.
if "%SHADERS%"=="" echo   Shaders folder   %E%[91mnot chosen yet - press 1%E%[0m
if not "%SHADERS%"=="" echo   Shaders folder   %E%[97m%SHADERS%%E%[0m
if "%TARGET%"=="" echo   Optimize         %E%[97mevery effect in the Shaders folder%E%[0m
if not "%TARGET%"=="" echo   Optimize         %E%[97m%TARGET%%E%[0m
echo   Time             %E%[97m%SECONDS% seconds per statement%E%[0m
echo   Graphics card    %E%[97m%MODELNAME%%E%[0m
if "%MODE%"=="easy" echo   Mode             %E%[97mEasy: ready files with our recommended changes, no switches%E%[0m
if "%MODE%"=="easy-switches" echo   Mode             %E%[97mEasy: ready files, each change behind a switch%E%[0m
if "%MODE%"=="expert" echo   Mode             %E%[97mExpert: every variant behind switches, you choose%E%[0m
if exist "sopt-facts.txt" echo   Value ranges     %E%[97msopt-facts.txt%E%[0m
echo.
echo   %E%[1;93m1%E%[0m  Choose the Shaders folder %E%[90m(the reshade-shaders\Shaders folder with ReShade.fxh)%E%[0m
echo   %E%[1;93m2%E%[0m  Choose what to optimize %E%[90m(everything, one folder or one effect)%E%[0m
echo   %E%[1;93m3%E%[0m  Choose the time per statement %E%[90m(more time can find more)%E%[0m
echo   %E%[1;93m4%E%[0m  Choose the graphics card family %E%[90m(what counts as faster)%E%[0m
echo   %E%[1;93mM%E%[0m  Choose the mode %E%[90m(easy: ready files, or expert: every variant)%E%[0m
echo   %E%[1;93mE%E%[0m  Easy mode settings %E%[90m(which changes easy mode may make)%E%[0m
echo.
echo   %E%[1;93m5%E%[0m  %E%[1;97mStart%E%[0m
echo   %E%[1;93m6%E%[0m  Quick look: what SweetOpt would work on %E%[90m(seconds, no search)%E%[0m
echo.
echo   %E%[1;93m7%E%[0m  Open the results %E%[90m(sopt-out folder and sopt-report.md)%E%[0m
echo   %E%[1;93m8%E%[0m  Edit value ranges %E%[90m(better results: sopt-facts.txt)%E%[0m
echo   %E%[1;93m9%E%[0m  Open the guide %E%[90m(README.html)%E%[0m
echo   %E%[1;93m0%E%[0m  Quit
echo.
choice /c 1234567890ME /n /m "  Press a number (or M, E): "
set "n=%errorlevel%"
if "%n%"=="11" call :chooseMode
if "%n%"=="12" call :easySettings
if "%n%"=="10" goto :eof
if "%n%"=="1" call :chooseShaders
if "%n%"=="2" call :chooseTarget
if "%n%"=="3" call :chooseTime
if "%n%"=="4" call :chooseModel
if "%n%"=="5" call :start
if "%n%"=="6" call :look
if "%n%"=="7" call :results
if "%n%"=="8" call :facts
if "%n%"=="9" start "" "%~dp0README.html"
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
cls
echo.
echo   What should SweetOpt optimize?
echo.
echo   %E%[1;93m1%E%[0m  Every effect in the Shaders folder
echo   %E%[1;93m2%E%[0m  One folder %E%[90m(for example SweetFX)%E%[0m
echo   %E%[1;93m3%E%[0m  One effect file
echo   %E%[1;93m4%E%[0m  A GLSL or HLSL shader file %E%[90m(not ReShade: .frag, .glsl, .hlsl ...)%E%[0m
echo   %E%[1;93m0%E%[0m  Back
echo.
choice /c 12340 /n /m "  Press a number: "
if errorlevel 5 exit /b
if errorlevel 4 goto targetOther
if errorlevel 3 goto targetFile
if errorlevel 2 goto targetFolder
set "TARGET="
exit /b
:targetFolder
if "%SHADERS%"=="" call :chooseShaders
if "%SHADERS%"=="" exit /b
call :pickFolder "Choose the folder of effects to optimize" "%SHADERS%"
if not "%PICKED%"=="" set "TARGET=%PICKED%"
exit /b
:targetFile
if "%SHADERS%"=="" call :chooseShaders
if "%SHADERS%"=="" exit /b
call :pickFile "%SHADERS%"
if not "%PICKED%"=="" set "TARGET=%PICKED%"
exit /b
:targetOther
call :pickFile "%TARGET%"
if not "%PICKED%"=="" set "TARGET=%PICKED%"
exit /b

:chooseTime
cls
echo.
echo   How long may SweetOpt search each statement?
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
echo   %E%[90m(SweetOpt also measures AMD and NVIDIA code when their tools are installed; this picks what it searches for)%E%[0m
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
echo   %E%[1;93mK%E%[0m  Intel Gen12     %E%[90mIris Xe, UHD Graphics 700%E%[0m
echo   %E%[1;93mI%E%[0m  Intel Gen9      %E%[90mHD / UHD Graphics 500 and 600%E%[0m
echo   %E%[1;93mJ%E%[0m  Intel Gen7.5    %E%[90mHD Graphics 4200 - 5200 (Haswell)%E%[0m
echo.
choice /c 123456789IJK /n /m "  Press a number (or I, J, K): "
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
if "%k%"=="11" set "MODEL=intel-gen7.5" & set "MODELNAME=Intel Gen7.5 (HD Graphics 4600)"
if "%k%"=="12" set "MODEL=intel-gen12" & set "MODELNAME=Intel Gen12 (Iris Xe)"
exit /b

:chooseMode
cls
echo.
echo   How do you want the results?
echo.
echo   %E%[1;93m1%E%[0m  Easy %E%[90m(ready files: our recommended changes are put in, nothing to choose)%E%[0m
echo   %E%[1;93m2%E%[0m  Expert %E%[90m(every variant behind a switch with its accuracy and cost: you choose)%E%[0m
echo.
choice /c 12 /n /m "  Press a number: "
if errorlevel 2 set "MODE=expert" & exit /b
echo.
echo   Keep a switch for each change? %E%[90m(lets you turn single changes off again later)%E%[0m
choice /c YN /n /m "  Y = with switches, N = plain ready files: "
if errorlevel 2 set "MODE=easy" & exit /b
set "MODE=easy-switches"
exit /b

:easySettings
cls
echo.
echo   Easy mode settings %E%[90m(the defaults are the safe choices)%E%[0m
echo.
if "%TOOEXACT%"=="no" echo   %E%[1;93m1%E%[0m  "Too exact" changes      %E%[97mno%E%[0m %E%[90m(closer to exact math than the original; can differ where an effect relies on rounding)%E%[0m
if "%TOOEXACT%"=="yes" echo   %E%[1;93m1%E%[0m  "Too exact" changes      %E%[97myes%E%[0m %E%[90m(closer to exact math than the original; can differ where an effect relies on rounding)%E%[0m
if "%REWRITES%"=="safe" echo   %E%[1;93m2%E%[0m  Bigger rewrites          %E%[97mtables and vertex shader moves%E%[0m %E%[90m(default)%E%[0m
if "%REWRITES%"=="all" echo   %E%[1;93m2%E%[0m  Bigger rewrites          %E%[97mall, blends to the blend stage too%E%[0m %E%[90m(check those in ReShade)%E%[0m
if "%REWRITES%"=="none" echo   %E%[1;93m2%E%[0m  Bigger rewrites          %E%[97mnone%E%[0m
echo   %E%[1;93m3%E%[0m  Back to the safe defaults
echo   %E%[1;93m0%E%[0m  Back
echo.
choice /c 1230 /n /m "  Press a number: "
set "k=%errorlevel%"
if "%k%"=="4" exit /b
if "%k%"=="3" set "TOOEXACT=no" & set "REWRITES=safe"
if "%k%"=="1" goto toggleTooExact
if "%k%"=="2" goto toggleRewrites
goto easySettings
:toggleTooExact
if "%TOOEXACT%"=="no" (set "TOOEXACT=yes") else (set "TOOEXACT=no")
goto easySettings
:toggleRewrites
if "%REWRITES%"=="safe" (set "REWRITES=all") else if "%REWRITES%"=="all" (set "REWRITES=none") else (set "REWRITES=safe")
goto easySettings

rem ---------------------------------------------------------------------------------------------------
:start
if "%SHADERS%%TARGET%"=="" call :chooseShaders
if "%SHADERS%%TARGET%"=="" exit /b
set "WHAT=%TARGET%"
if "%WHAT%"=="" set "WHAT=%SHADERS%"
set "INC="
if not "%SHADERS%"=="" set INC=-I "%SHADERS%"
set "FACTS="
if exist "sopt-facts.txt" set FACTS=--facts "%~dp0sopt-facts.txt"
set "EASY="
if "%MODE%"=="easy" set "EASY=--easy"
if "%MODE%"=="easy-switches" set "EASY=--easy-switches"
if not "%MODE%"=="expert" if "%TOOEXACT%"=="yes" set "EASY=%EASY% --easy-too-exact"
if not "%MODE%"=="expert" set "EASY=%EASY% --easy-rewrites %REWRITES%"
cls
"%~dp0bin\sopt-fx.exe" %INC% -o "%~dp0sopt-out" --time %SECONDS% --cost-model %MODEL% %FACTS% %EASY% "%WHAT%"
echo.
if exist "sopt-out\sopt-report.md" echo   %E%[92mDone.%E%[0m The results are in the sopt-out folder: press 7 in the menu to open them.
<nul set /p "=%BEL%"
pause
exit /b

:look
if "%SHADERS%%TARGET%"=="" call :chooseShaders
if "%SHADERS%%TARGET%"=="" exit /b
set "WHAT=%TARGET%"
if "%WHAT%"=="" set "WHAT=%SHADERS%"
set "INC="
if not "%SHADERS%"=="" set INC=-I "%SHADERS%"
cls
"%~dp0bin\sopt-fx.exe" %INC% -o "%~dp0sopt-out" --list "%WHAT%"
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
echo   %E%[93mNo sopt-facts.txt yet: press 5 first.%E%[0m A run lists the inputs whose value range SweetOpt could not work out.
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

rem :pickFile "start folder" -> PICKED (an .fx / .hlsl / GLSL file)
:pickFile
set "PICKED="
set "PSTART=%~1"
for /f "usebackq delims=" %%p in (`powershell -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; $d.Title = 'Choose the effect to optimize'; $d.Filter = 'Shaders (*.fx;*.fxh;*.hlsl;*.hlsli;*.frag;*.fs;*.glsl)|*.fx;*.fxh;*.hlsl;*.hlsli;*.frag;*.fs;*.glsl|All files|*.*'; $d.InitialDirectory = $env:PSTART; if ($d.ShowDialog() -eq 'OK') { $d.FileName }" 2^>nul`) do set "PICKED=%%p"
if not "%PICKED%"=="" exit /b
echo.
set /p "PICKED=  The effect file to optimize - type or paste the path (Enter = cancel): "
if defined PICKED set "PICKED=%PICKED:"=%"
exit /b

:save
if not exist "bin" mkdir "bin"
>"bin\SweetOpt.ini" echo # SweetOpt.bat settings
>>"bin\SweetOpt.ini" echo SHADERS=%SHADERS%
>>"bin\SweetOpt.ini" echo TARGET=%TARGET%
>>"bin\SweetOpt.ini" echo SECONDS=%SECONDS%
>>"bin\SweetOpt.ini" echo MODEL=%MODEL%
>>"bin\SweetOpt.ini" echo MODELNAME=%MODELNAME%
>>"bin\SweetOpt.ini" echo MODE=%MODE%
>>"bin\SweetOpt.ini" echo TOOEXACT=%TOOEXACT%
>>"bin\SweetOpt.ini" echo REWRITES=%REWRITES%
exit /b
