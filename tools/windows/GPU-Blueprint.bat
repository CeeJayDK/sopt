@echo off
rem GPU Blueprint (owner, 2026-10-05): one batch file with a menu; pick what to measure with a number key.
rem Reports go to Reports\, and after every run Reports-<cards>.zip holds them (without the shader dumps;
rem bin\zip-reports.ps1 names it after the graphics cards in the reports, owner 2026-10-05).
setlocal EnableExtensions
rem Upload page for the reports (a Dropbox file request: anyone can upload, nobody can see the folder).
set "UPLOAD=https://www.dropbox.com/request/jmwykzlbulwo8ltkyule"
cd /d "%~dp0"
rem The programs are in bin\ (owner, 2026-10-05: only the menu and the guide in the main folder).
set "BIN=%~dp0bin\"
rem ESC for colors (Windows 10 and later; older consoles show the codes as text); BEL (a raw 0x07 byte below) beeps.
for /f %%e in ('echo prompt $E^| cmd') do set "E=%%e"
set "BEL="
rem The cards by name, once (owner, 2026-10-16: a PC rarely has more than two; one or two cards are named on keys 2
rem and 3, with one card key 3 is not shown; more cards keep "main card" and "pick a card").
set "cards=0"
if exist "%BIN%OpBench.exe" for /f "usebackq tokens=1,*" %%a in (`call "%BIN%OpBench.exe" --adapters`) do call :addCard %%a "%%b"
set "run2="
set "run3="
if "%cards%"=="1" set "run2=%cardIdx1%"
if "%cards%"=="2" set "run2=%cardIdx1%"
if "%cards%"=="2" set "run3=%cardIdx2%"
:menu
set "ZIP="
for %%z in (Reports*.zip) do set "ZIP=%%~nxz"
cls
echo.
echo   %E%[96m=====================================================%E%[0m
echo   %E%[1;97m  GPU Blueprint  -  by CeeJay.dk%E%[0m
echo   %E%[90m  measures your graphics card for sopt%E%[0m
echo   %E%[96m=====================================================%E%[0m
echo.
echo   %E%[1;93m1%E%[0m  %E%[1;92mEvery graphics card in this PC: ShaderInfo, OpBench, StageBench and TexBench%E%[0m %E%[90m(10-25 min per card)%E%[0m %E%[92m^<- recommended%E%[0m
if "%cards%"=="2" goto menuCards
if "%cards%"=="1" goto menuOne
echo   %E%[1;93m2%E%[0m  Main graphics card only
echo   %E%[1;93m3%E%[0m  Another graphics card: you pick the card
goto menuTools
:menuOne
echo   %E%[1;93m2%E%[0m  Only the %cardName1%
goto menuTools
:menuCards
echo   %E%[1;93m2%E%[0m  Only the %cardName1%
echo   %E%[1;93m3%E%[0m  Only the %cardName2%
:menuTools
echo.
echo   %E%[1;93m4%E%[0m  OpBench only     %E%[90mwhat math instructions cost%E%[0m
echo   %E%[1;93mS%E%[0m  StageBench only  %E%[90mwhat math costs in pixel and vertex shaders%E%[0m
echo   %E%[1;93m5%E%[0m  TexBench only    %E%[90mwhat texture reads, writes and blending cost%E%[0m
echo   %E%[1;93m6%E%[0m  ShaderInfo only  %E%[90mwhat the driver reports about compiled shaders (seconds)%E%[0m
echo.
echo   %E%[1;93m7%E%[0m  Open the Reports folder
echo   %E%[1;93m8%E%[0m  Open the guide %E%[90m(README.html)%E%[0m
echo   %E%[1;93mR%E%[0m  See the results again %E%[90m(opens them in your browser)%E%[0m
rem New reports not sent yet (the zip without the hidden marker Reports\.sent): key 9 blinks (owner, 2026-10-05).
if defined ZIP if not exist "Reports\.sent" goto menuNew
echo   %E%[1;93m9%E%[0m  Send the reports to CeeJay %E%[90m(opens an upload page in your browser)%E%[0m
goto menuQuit
:menuNew
echo   %E%[5;1;92m9%E%[0m  %E%[5;1;92mSend the reports to CeeJay%E%[0m %E%[92m(new reports are ready)%E%[0m
:menuQuit
echo   %E%[1;93m0%E%[0m  Quit
rem The programs need Microsoft's Visual C++ runtime (not bundled: antivirus programs flagged packages that bundle
rem it, owner 2026-10-16). Without it Windows reports a missing VCRUNTIME140 / MSVCP140 dll; key V opens the download.
set "VCMISSING="
for %%d in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll) do if not exist "%SystemRoot%\System32\%%d" set "VCMISSING=1"
if not defined VCMISSING goto menuVcOk
echo.
echo   %E%[1;91mThe Microsoft Visual C++ runtime is missing: the tests will not start without it.%E%[0m
echo   %E%[1;93mV%E%[0m  Open Microsoft's download page %E%[90m(install "X64", then run this menu again)%E%[0m
:menuVcOk
echo.
echo   %E%[90mClose games and other GPU-heavy programs first. When a run is done, it offers to send the reports.%E%[0m
echo.
choice /c 1234567890VRS /n /m "  Press a number: "
set "n=%errorlevel%"
if "%n%"=="10" goto :eof
if "%n%"=="11" start "" "https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist"
if "%n%"=="1" call :every
if "%n%"=="2" if defined run2 (call :run %run2% all) else (call :run "" all)
if "%n%"=="3" if defined run3 (call :run %run3% all) else if not "%cards%"=="1" call :pick
if "%n%"=="4" call :run "" opbench
if "%n%"=="5" call :run "" texbench
if "%n%"=="6" call :run "" shaderinfo
if "%n%"=="7" call :reports
if "%n%"=="12" call :results
if "%n%"=="13" call :run "" stagebench
if "%n%"=="8" start "" "%~dp0README.html"
if "%n%"=="9" call :send
goto menu

rem :addCard <adapter number> "<name>" (from OpBench --adapters)
:addCard
set /a cards+=1
set "cardIdx%cards%=%~1"
set "cardName%cards%=%~2"
exit /b

rem R: the pages the tests save of what they showed (Reports\Results, owner 2026-10-16: see the results again).
:results
if exist "Reports\Results\*.html" goto resultsOpen
echo.
echo   %E%[91mNo results yet.%E%[0m Run a test first (1 to 6 or S).
echo.
pause
exit /b
:resultsOpen
for %%f in ("Reports\Results\*.html") do start "" "%%~ff"
exit /b

:reports
if not exist "Reports" mkdir "Reports"
start "" explorer "%~dp0Reports"
exit /b

rem Send: only when the user picks it (owner, 2026-10-05). Opens the upload page and shows the zip in Explorer;
rem the user drags it onto the page (file requests take uploads through the web page only).
:send
cls
echo.
if defined ZIP goto sendAsk
echo   %E%[91mNo reports yet.%E%[0m Run a test first (1 to 6 or S); the reports are zipped when it is done.
echo.
pause
exit /b
:sendAsk
if defined UPLOAD goto sendInfo
echo   %E%[91mThe upload page is not set up in this version.%E%[0m Send %ZIP% to CeeJay another way.
echo.
pause
exit /b
rem Also called at the end of a run (no cls: the results stay on screen).
:sendInfo
echo   This opens CeeJay's upload page (Dropbox) in your browser and shows %E%[1;97m%ZIP%%E%[0m in a folder.
echo   Drag the zip onto the page. You do not need a Dropbox account; the page asks for a name and an email.
echo   %E%[93mThey do not have to be real: any name and any email will do. CeeJay never sees the email.%E%[0m
echo.
echo   The zip holds the reports in the Reports folder: your graphics card's name, driver version and the
echo   measurements. Nothing else from your PC is sent, and nothing is sent unless you upload it.
echo.
for /f "usebackq tokens=1,2* delims=|" %%a in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%BIN%drift-check.ps1" "%~dp0."`) do call :drift %%a %%b "%%c"
echo.
choice /c YN /n /m "  Send the reports now, open the upload page? (Y/N) "
if errorlevel 2 exit /b
start "" "%UPLOAD%"
start "" explorer /select,"%~dp0%ZIP%"
if not exist "Reports\.sent" type nul > "Reports\.sent"
attrib +h "Reports\.sent" >nul 2>&1
rem Sent reports move to Reports\Sent, so the next zip holds only new ones (owner, 2026-10-16); the zip itself stays.
if not exist "Reports\Sent" mkdir "Reports\Sent"
for %%f in (Reports\*.csv Reports\*.txt Reports\*.png) do move /y "%%f" "Reports\Sent\" >nul
echo.
echo   %E%[90mThe sent reports are now in Reports\Sent; the next zip holds only new ones.%E%[0m
exit /b

rem :drift <percent> <program> "<GPU>": how much the GPU clock moved during a run (owner, 2026-10-11/12: say it
rem before sending, highlighted). 20%% is normal even at maximum performance (owner's GTX 1660); some runs with 150%%
rem and more were off, others with 175%% still fine: "may be".
:drift
if %~1 GTR 50 goto driftHigh
echo   %~2 on %~3: clock drift %~1%% %E%[92m(fine)%E%[0m
exit /b
:driftHigh
echo   %E%[1;93m%~2 on %~3: clock drift %~1%%: the numbers may be less reliable.%E%[0m
echo   %E%[93mA rerun can help: close other programs, plug in a laptop, and set "Prefer maximum performance"%E%[0m
echo   %E%[93m(NVIDIA Control Panel) or the "High performance" power plan in Windows.%E%[0m
exit /b

rem :run "<adapter number or empty>" all|opbench|stagebench|texbench|shaderinfo
:run
set "adapter="
if not "%~1"=="" set "adapter=--adapter %~1"
if /i "%~2"=="all" call :one ShaderInfo.exe ""
if /i "%~2"=="all" call :one OpBench.exe "%adapter%"
if /i "%~2"=="all" call :one StageBench.exe "%adapter%"
if /i "%~2"=="all" call :one TexBench.exe "%adapter%"
if /i "%~2"=="opbench" call :one OpBench.exe "%adapter%"
if /i "%~2"=="stagebench" call :one StageBench.exe "%adapter%"
if /i "%~2"=="texbench" call :one TexBench.exe "%adapter%"
if /i "%~2"=="shaderinfo" call :one ShaderInfo.exe ""
goto :finish

rem Another card: the list from OpBench, then its number.
:pick
cls
echo.
"%BIN%OpBench.exe" --list
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

rem Every card: ShaderInfo once (it covers every Vulkan GPU), then OpBench, StageBench (owner, 2026-10-16: pixel
rem shaders are most of what sopt optimizes) and TexBench per card.
:every
call :one ShaderInfo.exe ""
for /f "usebackq" %%i in (`call "%BIN%OpBench.exe" --adapters`) do (
  call :one OpBench.exe "--adapter %%i"
  call :one StageBench.exe "--adapter %%i"
  call :one TexBench.exe "--adapter %%i"
)
goto :finish

:one
if exist "%BIN%%~1" goto oneRun
echo   %E%[91m%~1 is missing from the bin folder%E%[0m
exit /b
:oneRun
"%BIN%%~1" %~2
exit /b

rem After a run: zip the reports (not Reports\Shaders; Get-ChildItem leaves out the hidden marker), beep, then offer
rem to send them (owner, 2026-10-05).
:finish
if exist "Reports\.sent" del /f /q /a:h "Reports\.sent" >nul 2>&1
set "ZIP="
if exist "Reports" for /f "usebackq delims=" %%z in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%BIN%zip-reports.ps1" "%~dp0."`) do set "ZIP=%%z"
echo.
if defined ZIP goto zipped
echo   %E%[92mDone.%E%[0m The reports are in the Reports folder.
goto beep
:zipped
echo   %E%[92mDone.%E%[0m %E%[1;97m%ZIP%%E%[0m, next to GPU-Blueprint.bat, holds every report in the Reports folder.
echo.
<nul set /p "=%BEL%"
if not defined UPLOAD goto later
call :sendInfo
exit /b
:later
pause
exit /b
:beep
<nul set /p "=%BEL%"
pause
exit /b
