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
echo   %E%[1;93m1%E%[0m  Every graphics card in this PC: ShaderInfo, OpBench and TexBench %E%[90m(5-15 min per card)%E%[0m
echo   %E%[1;93m2%E%[0m  Main graphics card only
echo   %E%[1;93m3%E%[0m  Another graphics card: you pick the card
echo.
echo   %E%[1;93m4%E%[0m  OpBench only     %E%[90mwhat math instructions cost%E%[0m
echo   %E%[1;93m5%E%[0m  TexBench only    %E%[90mwhat texture reads, writes and blending cost%E%[0m
echo   %E%[1;93m6%E%[0m  ShaderInfo only  %E%[90mwhat the driver reports about compiled shaders (seconds)%E%[0m
echo.
echo   %E%[1;93m7%E%[0m  Open the Reports folder
echo   %E%[1;93m8%E%[0m  Open the guide %E%[90m(README.html)%E%[0m
rem New reports not sent yet (the zip without the hidden marker Reports\.sent): key 9 blinks (owner, 2026-10-05).
if defined ZIP if not exist "Reports\.sent" goto menuNew
echo   %E%[1;93m9%E%[0m  Send the reports to CeeJay %E%[90m(opens an upload page in your browser)%E%[0m
goto menuQuit
:menuNew
echo   %E%[5;1;92m9%E%[0m  %E%[5;1;92mSend the reports to CeeJay%E%[0m %E%[92m(new reports are ready)%E%[0m
:menuQuit
echo   %E%[1;93m0%E%[0m  Quit
echo.
echo   %E%[90mClose games and other GPU-heavy programs first. When a run is done, it offers to send the reports.%E%[0m
echo.
choice /c 1234567890 /n /m "  Press a number: "
set "n=%errorlevel%"
if "%n%"=="10" goto :eof
if "%n%"=="1" call :every
if "%n%"=="2" call :run "" all
if "%n%"=="3" call :pick
if "%n%"=="4" call :run "" opbench
if "%n%"=="5" call :run "" texbench
if "%n%"=="6" call :run "" shaderinfo
if "%n%"=="7" call :reports
if "%n%"=="8" start "" "%~dp0README.html"
if "%n%"=="9" call :send
goto menu

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
echo   %E%[91mNo reports yet.%E%[0m Run a test first (1 to 6); the reports are zipped when it is done.
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
echo   Drag the zip onto the page. You do not need a Dropbox account; the page asks for a name and an email
echo   (any name will do) so CeeJay can tell reports apart.
echo.
echo   The zip holds the reports in the Reports folder: your graphics card's name, driver version and the
echo   measurements. Nothing else from your PC is sent, and nothing is sent unless you upload it.
echo.
choice /c YN /n /m "  Send the reports now, open the upload page? (Y/N) "
if errorlevel 2 exit /b
start "" "%UPLOAD%"
start "" explorer /select,"%~dp0%ZIP%"
if not exist "Reports\.sent" type nul > "Reports\.sent"
attrib +h "Reports\.sent" >nul 2>&1
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

rem Every card: ShaderInfo once (it covers every Vulkan GPU), then OpBench and TexBench per card.
:every
call :one ShaderInfo.exe ""
for /f "usebackq" %%i in (`call "%BIN%OpBench.exe" --adapters`) do (
  call :one OpBench.exe "--adapter %%i"
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
